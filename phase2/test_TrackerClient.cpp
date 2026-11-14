#include "TrackerClient.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <thread>

// Minimal single-shot local HTTP server so the announce() end-to-end tests need no external network (shared with test_HttpTransport).
#include "local_test_server.hpp"

class test_TrackerClient
{
	public:
		test_TrackerClient()
		{
			test_percent_encode();
			test_percent_encode_unreserved_punctuation();
			test_build_announce_url();
			test_parse_response_valid();
			test_parse_response_failure_reason();
			test_parse_response_malformed_peers();
			test_parse_response_dict_list_peers();
			test_parse_response_dict_list_peers_malformed();
			test_parse_response_peers6_valid();
			test_parse_response_combined_peers_and_peers6();
			test_parse_response_peers6_malformed();
			test_parse_response_invalid();
			test_announce_local_server();
			test_announce_failure_reason();
			test_announce_non_200();
		}

		static void test_percent_encode_unreserved_punctuation()
		{
			/*
			    RFC 3986 unreserved characters (A-Z, a-z, 0-9, '-', '_', '.', '~') are kept as-is;
			    everything else is percent-encoded. This covers the four punctuation unreserveds
			    that plain alphanumerics do not exercise.
			*/
			const uint8_t input[] = {'-', '_', '.', '~', 'a', 'Z', '7'};

			assert(TrackerClient::percent_encode(input, sizeof(input)) == "-_.~aZ7");
		}

		static void test_percent_encode()
		{
			/*
			    0x00            -> %00 (NUL byte)
			    '/' '?' '&' '='  -> reserved URI characters, all percent-encoded
			    'a' 'Z' '7'      -> unreserved alphanumerics kept as-is
			    0xFF            -> high byte -> %FF (uppercase hex digits)
			*/
			const uint8_t input[] = {0x00, 0x2F, 0x3F, 0x26, 0x3D, 'a', 'Z', '7', 0xFF};

			assert(TrackerClient::percent_encode(input, sizeof(input)) == "%00%2F%3F%26%3DaZ7%FF");
		}

		static void test_build_announce_url()
		{
			/*
			    - full parameter set in the expected order: info_hash, peer_id, port, uploaded, downloaded, left, compact=1
			    - binary info_hash bytes are percent-encoded byte-by-byte (0x00, reserved chars, alphanumerics)
			    - event is appended only when not None
			    - announce_url that already has a query string gets '&' as separator
			*/
			std::string ff_12; // 12 x "%FF" for the 0xFF tail bytes of info_hash (indices 8..19)
			std::string nul_14; // 14 x "%00" for the value-initialized (0x00) tail bytes of peer_id

			for (int i = 0; i < 12; ++i)
			{
				ff_12 += "%FF";
			}

			for (int i = 0; i < 14; ++i)
			{
				nul_14 += "%00";
			}

			AnnounceRequest req;
			req.announce_url = "http://tracker.example.org/announce";

			std::array<uint8_t, 20> ih{}; // value-initialized to 0x00
			ih[1] = 0x2F; // '/'
			ih[2] = 0x3F; // '?'
			ih[3] = 0x26; // '&'
			ih[4] = 0x3D; // '='
			ih[5] = 'a';
			ih[6] = 'Z';
			ih[7] = '7';

			for (std::size_t i = 8; i < 20; ++i)
			{
				ih[i] = 0xFF;
			}

			req.info_hash = ih; // first byte stays 0x00 -> must be encoded as %00
			req.peer_id = {'T', 'T', '0', '0', '0', '1'}; // remaining 14 bytes are 0x00 (value-initialized)
			req.port = 6881;
			req.uploaded = 12345;
			req.downloaded = 67890;
			req.left = 111222333;

			const std::string expected_info_hash = "%00%2F%3F%26%3DaZ7" + ff_12;
			const std::string expected_peer_id = "TT0001" + nul_14;

			// event == None: no event parameter at all.
			assert(TrackerClient().build_announce_url(req) ==
			       "http://tracker.example.org/announce?info_hash=" + expected_info_hash +
			       "&peer_id=" + expected_peer_id +
			       "&port=6881&uploaded=12345&downloaded=67890&left=111222333&compact=1");

			// event == Started: appended as the last parameter.
			req.event = AnnounceEvent::Started;
			assert(TrackerClient().build_announce_url(req) ==
			       "http://tracker.example.org/announce?info_hash=" + expected_info_hash +
			       "&peer_id=" + expected_peer_id +
			       "&port=6881&uploaded=12345&downloaded=67890&left=111222333&compact=1&event=started");

			// event == Stopped / Completed.
			req.event = AnnounceEvent::Stopped;
			assert(TrackerClient().build_announce_url(req).ends_with("&event=stopped"));
			req.event = AnnounceEvent::Completed;
			assert(TrackerClient().build_announce_url(req).ends_with("&event=completed"));

			// announce_url that already carries a query string: parameters are appended with '&'.
			req.announce_url = "http://tracker.example.org/announce?min_interval=30";
			req.event = AnnounceEvent::None;
			assert(TrackerClient().build_announce_url(req) ==
			       "http://tracker.example.org/announce?min_interval=30&info_hash=" + expected_info_hash +
			       "&peer_id=" + expected_peer_id +
			       "&port=6881&uploaded=12345&downloaded=67890&left=111222333&compact=1");
		}

		static void test_parse_response_valid()
		{
			/*
			    d8:intervali3600e5:peers12:<12 bytes>ee
			    12-byte compact peers string = exactly two 6-byte groups (4-byte big-endian IPv4 + 2-byte big-endian port):
			        C0 A8 01 01 1A E1   -> 192.168.1.1:6881
			        0A 00 00 FF 01 BB   -> 10.0.0.255:443
			*/
			std::string body = "d8:intervali3600e5:peers12:";
			const char peers_bytes[] = {static_cast<char>(0xC0), static_cast<char>(0xA8), '\x01', '\x01', static_cast<char>(0x1A), static_cast<char>(0xE1),
			                            '\x0A', '\x00', '\x00', static_cast<char>(0xFF), '\x01', static_cast<char>(0xBB)
			                           };
			body.append(peers_bytes, sizeof(peers_bytes));
			body += "e"; // single 'e' closes the dictionary

			const AnnounceResponse resp = TrackerClient::parse_response(body);

			assert(!resp.failure_reason.has_value());
			assert(resp.interval == 3600);
			assert(resp.peers.size() == 2);
			assert(resp.peers[0].ip == "192.168.1.1");
			assert(resp.peers[0].port == 6881); // host byte order, not big-endian
			assert(resp.peers[1].ip == "10.0.0.255");
			assert(resp.peers[1].port == 443);
		}

		static void test_parse_response_failure_reason()
		{
			/*
			    d14:failure reason7:pausedee   -> {"failure reason": "paused"}
			    The failure reason is surfaced cleanly; interval/peers keep their defaults.
			*/
			// d 14:"failure reason" 6:"paused" e   ("paused" is 6 characters; one 'e' closes the dict)
			const AnnounceResponse resp = TrackerClient::parse_response("d14:failure reason6:pausede");

			assert(resp.failure_reason.has_value());
			assert(*resp.failure_reason == "paused");
			assert(resp.interval == 0);
			assert(resp.peers.empty());
		}

		static void test_parse_response_malformed_peers()
		{
			/*
			    Compact peers length must be a multiple of 6:
			        d5:peers5:12345ee          -> 5 bytes, reject
			        d5:peers7:1234567ee        -> 7 bytes, reject
			        d5:peers0:eee              -> 0 bytes is valid (empty peer list)
			*/
			bool exceptionThrown = false;

			try
			{
				const AnnounceResponse resp = TrackerClient::parse_response("d5:peers5:12345e");
				std::cout << "#e29e5d: 5-byte peers string was not rejected" << std::endl;
			}
			catch (const std::invalid_argument& e)
			{
				exceptionThrown = true;
				std::cout << "#0a9e8b: expected failure: " << e.what() << std::endl;
			}

			assert(exceptionThrown);

			exceptionThrown = false;

			try
			{
				const AnnounceResponse resp = TrackerClient::parse_response("d5:peers7:1234567e");
				std::cout << "#e29e5d: 7-byte peers string was not rejected" << std::endl;
			}
			catch (const std::invalid_argument& e)
			{
				exceptionThrown = true;
				std::cout << "#0a9e8b: expected failure: " << e.what() << std::endl;
			}

			assert(exceptionThrown);

			const AnnounceResponse resp_empty = TrackerClient::parse_response("d5:peers0:e");
			assert(resp_empty.peers.empty());
			assert(!resp_empty.failure_reason.has_value());
		}

		static void test_parse_response_dict_list_peers()
		{
			/*
			    Non-compact "peers" as a list of dictionaries (BEP-3): d5:peersl <dict> <dict> ee
			    Each dict requires "ip" (byte string) and "port" (integer); the optional "peer id" is ignored.
			    Dict keys are sorted ("ip" < "peer id" < "port"):
			        entry 1: d2:ip11:192.168.1.17:peer id4:abcde4:porti6881ee   -> ip "192.168.1.1", port 6881, peer id ignored
			        entry 2: d2:ip12:peer.example4:porti51413ee                  -> non-numeric ip stored as-is, port 51413
			*/
			const AnnounceResponse resp = TrackerClient::parse_response("d5:peersld2:ip11:192.168.1.17:peer id4:abcd4:porti6881eed2:ip12:peer.example4:porti51413eeee");

			assert(!resp.failure_reason.has_value());
			assert(resp.peers.size() == 2);
			assert(resp.peers[0].ip == "192.168.1.1");
			assert(resp.peers[0].port == 6881); // host byte order, not big-endian
			assert(resp.peers[1].ip == "peer.example");
			assert(resp.peers[1].port == 51413);
		}

		static void test_parse_response_dict_list_peers_malformed()
		{
			/*
			    Malformed dictionary-list "peers" must be rejected with std::invalid_argument:
			        d5:peersl i1e ee                        -> list element is not a dictionary
			        d5:peersl d4:porti6881ee ee             -> missing required "ip"
			        d5:peersl d2:ip11:192.168.1.1e ee       -> missing required "port"
			        d5:peersl d2:ipi1e4:porti6881ee ee      -> "ip" is not a byte string
			        d5:peersl d2:ip11:192.168.1.14:porti-1ee ee     -> port out of range (-1)
			        d5:peersl d2:ip11:192.168.1.14:porti70000ee ee  -> port out of range (70000)
			*/
			bool exceptionThrown = false;

			try
			{
				const AnnounceResponse resp = TrackerClient::parse_response("d5:peersli1eee");
				std::cout << "#e29e5d: non-dict peers list element was not rejected" << std::endl;
			}
			catch (const std::invalid_argument& e)
			{
				exceptionThrown = true;
				std::cout << "#0a9e8b: expected failure: " << e.what() << std::endl;
			}

			assert(exceptionThrown);

			exceptionThrown = false;

			try
			{
				const AnnounceResponse resp = TrackerClient::parse_response("d5:peersld4:porti6881eeee");
				std::cout << "#e29e5d: peers dict entry without \"ip\" was not rejected" << std::endl;
			}
			catch (const std::invalid_argument& e)
			{
				exceptionThrown = true;
				std::cout << "#0a9e8b: expected failure: " << e.what() << std::endl;
			}

			assert(exceptionThrown);

			exceptionThrown = false;

			try
			{
				const AnnounceResponse resp = TrackerClient::parse_response("d5:peersld2:ip11:192.168.1.1eee");
				std::cout << "#e29e5d: peers dict entry without \"port\" was not rejected" << std::endl;
			}
			catch (const std::invalid_argument& e)
			{
				exceptionThrown = true;
				std::cout << "#0a9e8b: expected failure: " << e.what() << std::endl;
			}

			assert(exceptionThrown);

			exceptionThrown = false;

			try
			{
				const AnnounceResponse resp = TrackerClient::parse_response("d5:peersld2:ipi1e4:porti6881eeee");
				std::cout << "#e29e5d: non-bytes \"ip\" was not rejected" << std::endl;
			}
			catch (const std::invalid_argument& e)
			{
				exceptionThrown = true;
				std::cout << "#0a9e8b: expected failure: " << e.what() << std::endl;
			}

			assert(exceptionThrown);

			exceptionThrown = false;

			try
			{
				const AnnounceResponse resp = TrackerClient::parse_response("d5:peersld2:ip11:192.168.1.14:porti-1eeee");
				std::cout << "#e29e5d: negative port was not rejected" << std::endl;
			}
			catch (const std::invalid_argument& e)
			{
				exceptionThrown = true;
				std::cout << "#0a9e8b: expected failure: " << e.what() << std::endl;
			}

			assert(exceptionThrown);

			exceptionThrown = false;

			try
			{
				const AnnounceResponse resp = TrackerClient::parse_response("d5:peersld2:ip11:192.168.1.14:porti70000eeee");
				std::cout << "#e29e5d: port above 65535 was not rejected" << std::endl;
			}
			catch (const std::invalid_argument& e)
			{
				exceptionThrown = true;
				std::cout << "#0a9e8b: expected failure: " << e.what() << std::endl;
			}

			assert(exceptionThrown);
		}

		static void test_parse_response_peers6_valid()
		{
			/*
			    d6:peers636:<36 bytes>e   -> BEP-7 compact IPv6 peers, two 18-byte groups (16-byte address + 2-byte big-endian port):
			        group 1: 20 01 0D B8 ... 00 01 (2001:db8::1) + 1A E1 (port 6881)
			        group 2: 00 x15, 01       (::1)         + C8 D5 (port 51413)
			*/
			std::string body = "d6:peers636:";
			const char peers6_bytes[] = {static_cast<char>(0x20), '\x01', static_cast<char>(0x0D), static_cast<char>(0xB8),
			                             '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', static_cast<char>(0x01),
			                             static_cast<char>(0x1A), static_cast<char>(0xE1),
			                             '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00',
			                             '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', static_cast<char>(0x01),
			                             static_cast<char>(0xC8), static_cast<char>(0xD5)
			                            };
			body.append(peers6_bytes, sizeof(peers6_bytes));
			body += "e"; // single 'e' closes the dictionary

			const AnnounceResponse resp = TrackerClient::parse_response(body);

			assert(!resp.failure_reason.has_value());
			assert(resp.peers.size() == 2);
			assert(resp.peers[0].ip == "2001:db8::1"); // canonical compressed form via boost::asio
			assert(resp.peers[0].port == 6881);        // host byte order, not big-endian
			assert(resp.peers[1].ip == "::1");
			assert(resp.peers[1].port == 51413);
		}

		static void test_parse_response_combined_peers_and_peers6()
		{
			/*
			    Both "peers" and "peers6" may coexist; peers are appended from both (compact first, then IPv6):
			        d5:peers6:<6 bytes>6:peers618:<18 bytes>e
			        compact group : C0 A8 01 02 + 1A E1 -> 192.168.1.2:6881
			        peers6 group  : 2001:db8::1         + C8 D5 -> [2001:db8::1]:51413
			*/
			std::string body = "d5:peers6:";
			const char peers_bytes[] = {static_cast<char>(0xC0), static_cast<char>(0xA8), '\x01', '\x02', static_cast<char>(0x1A), static_cast<char>(0xE1)};
			body.append(peers_bytes, sizeof(peers_bytes));
			body += "6:peers618:";
			const char peers6_bytes[] = {static_cast<char>(0x20), '\x01', static_cast<char>(0x0D), static_cast<char>(0xB8),
			                             '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', static_cast<char>(0x01),
			                             static_cast<char>(0xC8), static_cast<char>(0xD5)
			                            };
			body.append(peers6_bytes, sizeof(peers6_bytes));
			body += "e"; // single 'e' closes the dictionary

			const AnnounceResponse resp = TrackerClient::parse_response(body);

			assert(resp.peers.size() == 2);
			assert(resp.peers[0].ip == "192.168.1.2");
			assert(resp.peers[0].port == 6881);
			assert(resp.peers[1].ip == "2001:db8::1");
			assert(resp.peers[1].port == 51413);
		}

		static void test_parse_response_peers6_malformed()
		{
			/*
			    d6:peers617:<17 bytes>e   -> length not a multiple of 18, reject
			    d6:peers6i1ee             -> "peers6" is an integer, not a byte string, reject
			*/
			bool exceptionThrown = false;

			try
			{
				const AnnounceResponse resp = TrackerClient::parse_response("d6:peers617:01234567890123456e");
				std::cout << "#e29e5d: 17-byte peers6 string was not rejected" << std::endl;
			}
			catch (const std::invalid_argument& e)
			{
				exceptionThrown = true;
				std::cout << "#0a9e8b: expected failure: " << e.what() << std::endl;
			}

			assert(exceptionThrown);

			exceptionThrown = false;

			try
			{
				const AnnounceResponse resp = TrackerClient::parse_response("d6:peers6i1ee");
				std::cout << "#e29e5d: non-bytes peers6 was not rejected" << std::endl;
			}
			catch (const std::invalid_argument& e)
			{
				exceptionThrown = true;
				std::cout << "#0a9e8b: expected failure: " << e.what() << std::endl;
			}

			assert(exceptionThrown);
		}

		static void test_parse_response_invalid()
		{
			/*
			    i42e                  top level is not a dictionary -> reject
			    d8:intervali3600ee    "interval" present but peers missing is fine (defaults)
			    d5:peersi1eee         "peers" is an integer, not a byte string -> reject
			*/
			bool exceptionThrown = false;

			try
			{
				const AnnounceResponse resp = TrackerClient::parse_response("i42e");
				std::cout << "#e29e5d: non-dict top level was not rejected" << std::endl;
			}
			catch (const std::invalid_argument& e)
			{
				exceptionThrown = true;
				std::cout << "#0a9e8b: expected failure: " << e.what() << std::endl;
			}

			assert(exceptionThrown);

			// d 8:"interval" i3600e e   (the integer's 'e' and the dict-closing 'e' are distinct)
			const AnnounceResponse resp_interval_only = TrackerClient::parse_response("d8:intervali3600ee");
			assert(resp_interval_only.interval == 3600);
			assert(resp_interval_only.peers.empty());

			exceptionThrown = false;

			try
			{
				const AnnounceResponse resp = TrackerClient::parse_response("d5:peersi1ee");
				std::cout << "#e29e5d: non-bytes peers was not rejected" << std::endl;
			}
			catch (const std::invalid_argument& e)
			{
				exceptionThrown = true;
				std::cout << "#0a9e8b: expected failure: " << e.what() << std::endl;
			}

			assert(exceptionThrown);

			// Valid bencode followed by trailing bytes -> the whole body must be consumed, reject.
			exceptionThrown = false;

			try
			{
				const AnnounceResponse resp = TrackerClient::parse_response("d8:intervali60eextra");
				std::cout << "#e29e5d: trailing bytes after top-level value were not rejected" << std::endl;
			}
			catch (const std::invalid_argument& e)
			{
				exceptionThrown = true;
				std::cout << "#0a9e8b: expected failure: " << e.what() << std::endl;
			}

			assert(exceptionThrown);
		}

		static void test_announce_local_server()
		{
			/*
			End-to-end against a local single-shot server (no external network):
			    200 OK + bencoded body -> parsed AnnounceResponse, and the request line must carry
			    the percent-encoded query string built by build_announce_url().
			*/
			std::string body = "d8:intervali3600e5:peers6:"; // 6-byte compact peers string (one peer)
			const char peers_bytes[] = {static_cast<char>(0xC0), static_cast<char>(0xA8), '\x01', '\x01', static_cast<char>(0x1A), static_cast<char>(0xE1)};
			body.append(peers_bytes, sizeof(peers_bytes)); // one peer: 192.168.1.1:6881 (6 bytes is a valid multiple of 6)
			body += "e"; // single 'e' closes the dictionary

			local_test_server server("HTTP/1.1 200 OK\r\n"
			                         "Content-Type: text/plain\r\n"
			                         "Content-Length: " + std::to_string(body.size()) + "\r\n"
			                         "\r\n" + body);

			AnnounceRequest req;
			req.announce_url = "http://127.0.0.1:" + server.port() + "/announce";
			req.info_hash[0] = 0x00; // NUL byte must be percent-encoded in the request line
			req.peer_id = {'T', 'T'};
			req.port = 6881;

			const AnnounceResponse resp = TrackerClient().announce(req);

			assert(resp.interval == 3600);
			assert(resp.peers.size() == 1);
			assert(resp.peers[0].ip == "192.168.1.1");
			assert(resp.peers[0].port == 6881);

			// The raw request line must contain the percent-encoded info_hash (NUL -> %00).
			const std::string& request = server.last_request();
			assert(request.rfind("GET /announce?info_hash=%00", 0) != std::string::npos);
			assert(request.find("&compact=1") != std::string::npos);
		}

		static void test_announce_failure_reason()
		{
			/*
			End-to-end against a local single-shot server (no external network):
			    200 OK + bencoded "failure reason" body -> announce() must NOT throw; the reason is
			    surfaced in AnnounceResponse.failure_reason and interval/peers keep their defaults.
			*/
			const std::string body = "d14:failure reason16:invalid infohashe";

			local_test_server server("HTTP/1.1 200 OK\r\n"
			                         "Content-Type: text/plain\r\n"
			                         "Content-Length: " + std::to_string(body.size()) + "\r\n"
			                         "\r\n" + body);

			AnnounceRequest req;
			req.announce_url = "http://127.0.0.1:" + server.port() + "/announce";

			const AnnounceResponse resp = TrackerClient().announce(req); // must not throw on a failure reason

			assert(resp.failure_reason.has_value());
			assert(*resp.failure_reason == "invalid infohash");
			assert(resp.interval == 0);
			assert(resp.peers.empty());
		}

		static void test_announce_non_200()
		{
			/*
			Tracker answers with a non-200 status -> announce() must throw, even if the body is valid bencode.
			*/
			local_test_server server("HTTP/1.1 503 Service Unavailable\r\n"
			                         "Content-Length: 2\r\n"
			                         "\r\n"
			                         "de");

			AnnounceRequest req;
			req.announce_url = "http://127.0.0.1:" + server.port() + "/announce";

			bool exceptionThrown = false;

			try
			{
				const AnnounceResponse resp = TrackerClient().announce(req);
				std::cout << "#46188c: HTTP 503 was not rejected" << std::endl;
			}
			catch (const std::runtime_error& e)
			{
				exceptionThrown = true;
				std::cout << "#0a9e8b: expected failure: " << e.what() << std::endl;
			}

			assert(exceptionThrown);
		}
};



#ifndef FUZZER
int main()
{
	std::cout << "test_TrackerClient.cpp" << std::endl;
	test_TrackerClient test;
	return 0;
}
#else
// fuzz_target.cc
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size)
{
	try
	{
		std::string input(reinterpret_cast<const char*>(Data), Size);
		const AnnounceResponse resp = TrackerClient::parse_response(input);
		volatile auto sink = resp.interval + static_cast<int64_t>(resp.peers.size());
	}
	catch(...)
	{

	}

	return 0;
}
#endif
