#include "TrackerClient.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <random>
#include <string>
#include <system_error>
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
			test_parse_response_dict_list_peers_ip_validation();
			test_parse_response_dict_list_peers_ipv4_mapped_normalization();
			test_parse_response_dict_list_peers_malformed();
			test_parse_response_peers6_valid();
			test_parse_response_combined_peers_and_peers6();
			test_parse_response_peers6_malformed();
			test_parse_response_peer_limits();
			test_parse_response_invalid();
			test_parse_response_interval_bounds();
			test_announce_local_server();
			test_announce_failure_reason();
			test_announce_non_200();
			test_async_announce_local_server();
			test_async_announce_non_200();
			test_async_announce_malformed_bencode();
			test_async_announce_throwing_callback_success_path();
			test_async_announce_throwing_callback_failure_paths();
			test_sanitize_for_logging_escapes_control_bytes();
			test_parse_response_failure_reason_escaped();
			test_announce_malicious_url_rejected();
			test_announce_non200_url_sanitized();
			test_announce_all_multi_tracker_success_and_dedup();
			test_announce_all_partial_failure_isolation();
			test_announce_all_empty_url_list();
			test_announce_all_deduplicates_duplicate_urls();
			test_announce_all_caps_distinct_tracker_count();
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
				std::cout << "#58b31d: 5-byte peers string was not rejected" << std::endl;
			}
			catch (const std::invalid_argument& e)
			{
				exceptionThrown = true;
			}

			assert(exceptionThrown);

			exceptionThrown = false;

			try
			{
				const AnnounceResponse resp = TrackerClient::parse_response("d5:peers7:1234567e");
				std::cout << "#712312: 7-byte peers string was not rejected" << std::endl;
			}
			catch (const std::invalid_argument& e)
			{
				exceptionThrown = true;
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
			    Each dict requires "ip" (byte string holding a real IP literal) and "port" (integer); the
			    optional "peer id" is ignored. Dict keys are sorted ("ip" < "peer id" < "port"):
			        entry 1: d2:ip11:192.168.1.17:peer id4:abcde4:porti6881ee   -> ip "192.168.1.1", port 6881, peer id ignored
			        entry 2: d2:ip15:203.0.113.774:porti51413ee                 -> public IPv4 literal stored as-is, port 51413
			*/
			const AnnounceResponse resp = TrackerClient::parse_response("d5:peersld2:ip11:192.168.1.17:peer id4:abcd4:porti6881eed2:ip12:203.0.113.774:porti51413eeee");

			assert(!resp.failure_reason.has_value());
			assert(resp.peers.size() == 2);
			assert(resp.peers[0].ip == "192.168.1.1");
			assert(resp.peers[0].port == 6881); // host byte order, not big-endian
			assert(resp.peers[1].ip == "203.0.113.77");
			assert(resp.peers[1].port == 51413);
		}

		static void test_parse_response_dict_list_peers_ip_validation()
		{
			/*
			    H3 (SSRF hardening): list-form "ip" must be a real IP literal. Hostnames, empty strings and
			    arbitrary byte blobs are rejected with std::invalid_argument so downstream code can never be
			    steered into resolving attacker-chosen names or logging unbounded blobs:
			        d2:ip12:peer.example4:porti51413ee  -> hostname, reject
			        d2:ip0:e4:porti6881ee               -> empty string, reject
			*/
			bool exceptionThrown = false;

			try
			{
				const AnnounceResponse resp = TrackerClient::parse_response("d5:peersld2:ip12:peer.example4:porti51413eeee");
				std::cout << "#b8e0f4: hostname \"ip\" was not rejected" << std::endl;
			}
			catch (const std::invalid_argument& e)
			{
				exceptionThrown = true;
			}

			assert(exceptionThrown);

			exceptionThrown = false;

			try
			{
				const AnnounceResponse resp = TrackerClient::parse_response("d5:peersld2:ip0:e4:porti6881eeee");
				std::cout << "#d3f5a7: empty \"ip\" was not rejected" << std::endl;
			}
			catch (const std::invalid_argument& e)
			{
				exceptionThrown = true;
			}

			assert(exceptionThrown);
		}

		static void test_parse_response_dict_list_peers_ipv4_mapped_normalization()
		{
			/*
			    H3 (SSRF hardening): IPv4-mapped IPv6 literals ("::ffff:a.b.c.d") are normalized to plain
			    dotted-quad so equivalent addresses deduplicate downstream. Private/loopback literals are kept
			    intentionally (BEP-3 local swarms); filtering belongs in the connection layer.
			*/
			const AnnounceResponse resp = TrackerClient::parse_response("d5:peersld2:ip15:::ffff:10.0.0.14:porti6881eeee");

			assert(!resp.failure_reason.has_value());
			assert(resp.peers.size() == 1);
			assert(resp.peers[0].ip == "10.0.0.1"); // normalized from "::ffff:a00::1" form to dotted-quad
			assert(resp.peers[0].port == 6881);

			// A plain IPv4 literal and its mapped form must normalize identically (dedup key equivalence).
			const AnnounceResponse resp_plain = TrackerClient::parse_response("d5:peersld2:ip8:10.0.0.14:porti6881eeee");

			assert(resp_plain.peers.size() == 1);
			assert(resp_plain.peers[0].ip == "10.0.0.1");
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
				std::cout << "#45f4a4: non-dict peers list element was not rejected" << std::endl;
			}
			catch (const std::invalid_argument& e)
			{
				exceptionThrown = true;
			}

			assert(exceptionThrown);

			exceptionThrown = false;

			try
			{
				const AnnounceResponse resp = TrackerClient::parse_response("d5:peersld4:porti6881eeee");
				std::cout << "#788048: peers dict entry without \"ip\" was not rejected" << std::endl;
			}
			catch (const std::invalid_argument& e)
			{
				exceptionThrown = true;
			}

			assert(exceptionThrown);

			exceptionThrown = false;

			try
			{
				const AnnounceResponse resp = TrackerClient::parse_response("d5:peersld2:ip11:192.168.1.1eee");
				std::cout << "#57f026: peers dict entry without \"port\" was not rejected" << std::endl;
			}
			catch (const std::invalid_argument& e)
			{
				exceptionThrown = true;
			}

			assert(exceptionThrown);

			exceptionThrown = false;

			try
			{
				const AnnounceResponse resp = TrackerClient::parse_response("d5:peersld2:ipi1e4:porti6881eeee");
				std::cout << "#63100e: non-bytes \"ip\" was not rejected" << std::endl;
			}
			catch (const std::invalid_argument& e)
			{
				exceptionThrown = true;
			}

			assert(exceptionThrown);

			exceptionThrown = false;

			try
			{
				const AnnounceResponse resp = TrackerClient::parse_response("d5:peersld2:ip11:192.168.1.14:porti-1eeee");
				std::cout << "#26d195: negative port was not rejected" << std::endl;
			}
			catch (const std::invalid_argument& e)
			{
				exceptionThrown = true;
			}

			assert(exceptionThrown);

			exceptionThrown = false;

			try
			{
				const AnnounceResponse resp = TrackerClient::parse_response("d5:peersld2:ip11:192.168.1.14:porti70000eeee");
				std::cout << "#4badfa: port above 65535 was not rejected" << std::endl;
			}
			catch (const std::invalid_argument& e)
			{
				exceptionThrown = true;
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
				std::cout << "#9c1506: 17-byte peers6 string was not rejected" << std::endl;
			}
			catch (const std::invalid_argument& e)
			{
				exceptionThrown = true;
			}

			assert(exceptionThrown);

			exceptionThrown = false;

			try
			{
				const AnnounceResponse resp = TrackerClient::parse_response("d6:peers6i1ee");
				std::cout << "#692d38: non-bytes peers6 was not rejected" << std::endl;
			}
			catch (const std::invalid_argument& e)
			{
				exceptionThrown = true;
			}

			assert(exceptionThrown);
		}

		static void test_parse_response_peer_limits()
		{
			/*
			    Memory-amplification caps (H1): a tracker must not be able to force unbounded heap growth.
			    - compact "peers" with more than kMaxPeersPerResponse 6-byte groups -> reject
			    - list-form "peers" with more than kMaxPeersPerResponse entries -> reject
			    - combined "peers" + "peers6" exceeding the total cap -> reject
			    A few peers below the limit must still parse fine.
			*/
			const std::size_t limit = kMaxPeersPerResponse; // file-scope constant from TrackerClient.hpp

			// 1) Compact peers just above the cap: (limit+1) groups of 6 bytes.
			bool exceptionThrown = false;

			try
			{
				std::string body = "d5:peers" + std::to_string((limit + 1) * 6) + ":";
				body.append((limit + 1) * 6, '\x01'); // arbitrary bytes; only the count matters here
				body += "e";

				const AnnounceResponse resp = TrackerClient::parse_response(body);
				std::cout << "#a1b2c3: compact peers above limit were not rejected" << std::endl;
			}
			catch (const std::invalid_argument& e)
			{
				exceptionThrown = true;
			}

			assert(exceptionThrown);

			// 2) List-form peers just above the cap.
			exceptionThrown = false;

			try
			{
				std::string body = "d5:peersl";
				const std::string entry = "d2:ip1:14:porti6881ee"; // 19-byte dict entry

				for (std::size_t i = 0; i <= limit; ++i)
				{
					body += entry;
				}

				body += "ee";

				const AnnounceResponse resp = TrackerClient::parse_response(body);
				std::cout << "#b7a8c9: list peers above limit were not rejected" << std::endl;
			}
			catch (const std::invalid_argument& e)
			{
				exceptionThrown = true;
			}

			assert(exceptionThrown);

			// 3) Combined peers + peers6 exceeding the total cap.
			exceptionThrown = false;

			try
			{
				const std::size_t half = limit / 2 + 1; // each field alone is under the cap...
				std::string body = "d5:peers" + std::to_string(half * 6) + ":";
				body.append(half * 6, '\x01');
				body += "6:peers6" + std::to_string((half + 1) * 18) + ":"; // ...but together they exceed it
				body.append((half + 1) * 18, '\x02');
				body += "e";

				const AnnounceResponse resp = TrackerClient::parse_response(body);
				std::cout << "#c3d4e5: combined peers+peers6 above limit were not rejected" << std::endl;
			}
			catch (const std::invalid_argument& e)
			{
				exceptionThrown = true;
			}

			assert(exceptionThrown);

			// 4) A small peer list below the cap still parses.
			std::string body_ok = "d5:peers12:";
			const char peers_bytes[] = {static_cast<char>(0x01), static_cast<char>(0x02), '\x03', '\x04', static_cast<char>(0x1A), static_cast<char>(0xE1),
			                            '\x0A', '\x00', '\x00', static_cast<char>(0xFF), '\x01', static_cast<char>(0xBB)
			                           };
			body_ok.append(peers_bytes, sizeof(peers_bytes));
			body_ok += "e";

			const AnnounceResponse resp_ok = TrackerClient::parse_response(body_ok);
			assert(resp_ok.peers.size() == 2);
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
				std::cout << "#0dc596: non-dict top level was not rejected" << std::endl;
			}
			catch (const std::invalid_argument& e)
			{
				exceptionThrown = true;
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
				std::cout << "#3e4c10: non-bytes peers was not rejected" << std::endl;
			}
			catch (const std::invalid_argument& e)
			{
				exceptionThrown = true;
			}

			assert(exceptionThrown);

			// Valid bencode followed by trailing bytes -> the whole body must be consumed, reject.
			exceptionThrown = false;

			try
			{
				const AnnounceResponse resp = TrackerClient::parse_response("d8:intervali60eextra");
				std::cout << "#b3e2ad: trailing bytes after top-level value were not rejected" << std::endl;
			}
			catch (const std::invalid_argument& e)
			{
				exceptionThrown = true;
			}

			assert(exceptionThrown);
		}

		static void test_parse_response_interval_bounds()
		{
			/*
			    M1 sanity bounds on "interval" (seconds, BEP-3): a tracker can return negative values or
			    INT64_MAX. Callers that use the value directly for scheduling would get an infinite stall,
			    immediate re-announce hot loop, or overflow in downstream int conversions. parse_response()
			    must reject interval < 0 and interval > kMaxIntervalSeconds (7 days), and accept the bounds:
			        d8:intervali-1ee           negative -> reject
			        d8:intervali604801ee       just above the 7-day cap -> reject
			        d8:intervali9223372036854775807ee   INT64_MAX -> reject (bencode parser itself rejects it as overflow)
			        d8:intervali0ee            lower bound -> accept, interval == 0
			        d8:intervali604800ee       upper bound (exactly 7 days) -> accept
			*/
			bool exceptionThrown = false;

			try
			{
				const AnnounceResponse resp = TrackerClient::parse_response("d8:intervali-1ee"); // trailing 'e' closes the dict
				std::cout << "#b8e4f1: negative interval was not rejected" << std::endl;
			}
			catch (const std::invalid_argument& e)
			{
				exceptionThrown = true;
			}

			assert(exceptionThrown);

			exceptionThrown = false;

			try
			{
				const AnnounceResponse resp = TrackerClient::parse_response("d8:intervali604801ee"); // 7 days + 1 s; trailing 'e' closes the dict
				std::cout << "#e1f0a7: interval above the 7-day cap was not rejected" << std::endl;
			}
			catch (const std::invalid_argument& e)
			{
				exceptionThrown = true;
			}

			assert(exceptionThrown);

			// INT64_MAX fits in int64_t, so it is parsed by the bencode parser and then rejected here by the M1 bound;
			// values beyond INT64_MAX would already be rejected by the bencode parser (from_chars overflow).
			exceptionThrown = false;

			try
			{
				const AnnounceResponse resp = TrackerClient::parse_response("d8:intervali9223372036854775807ee"); // trailing 'e' closes the dict
				std::cout << "#2f8a4e: INT64_MAX interval was not rejected" << std::endl;
			}
			catch (const std::exception& e) // bencode parser throws runtime_error on overflow, invalid_argument otherwise
			{
				exceptionThrown = true;
			}

			assert(exceptionThrown);

			// Bounds are inclusive: 0 and exactly kMaxIntervalSeconds (604800 s) must be accepted.
			const AnnounceResponse resp_zero = TrackerClient::parse_response("d8:intervali0ee"); // trailing 'e' closes the dict
			assert(resp_zero.interval == 0);

			const AnnounceResponse resp_max = TrackerClient::parse_response("d8:intervali604800ee"); // trailing 'e' closes the dict
			assert(resp_max.interval == 604800);
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
				std::cout << "#2f0ac0: HTTP 503 was not rejected" << std::endl;
			}
			catch (const std::runtime_error& e)
			{
				exceptionThrown = true;
			}

			assert(exceptionThrown);
		}

		static void test_async_announce_local_server()
		{
			/*
			async_announce end-to-end against a local single-shot server (no external network):
			    200 OK + bencoded body -> on_complete fires exactly once with no exception and the parsed response,
			    and the request line must carry the percent-encoded query string built by build_announce_url().
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

			boost::asio::io_context io;
			std::exception_ptr ex;
			AnnounceResponse resp;
			int calls = 0;

			TrackerClient().async_announce(io, req, [&ex, &resp, &calls](std::exception_ptr e, AnnounceResponse r)
			{
				ex = std::move(e);
				resp = std::move(r);
				calls++;
			});

			io.run(); // drives the async operations to completion (no-op if already done)

			assert(calls == 1);
			assert(ex == nullptr);
			assert(resp.interval == 3600);
			assert(resp.peers.size() == 1);
			assert(resp.peers[0].ip == "192.168.1.1");
			assert(resp.peers[0].port == 6881);

			// The raw request line must contain the percent-encoded info_hash (NUL -> %00).
			const std::string& request = server.last_request();
			assert(request.rfind("GET /announce?info_hash=%00", 0) != std::string::npos);
			assert(request.find("&compact=1") != std::string::npos);
		}

		static void test_async_announce_non_200()
		{
			/*
			Tracker answers with a non-200 status -> async_announce must deliver an exception_ptr (std::runtime_error),
			even if the body is valid bencode.
			*/
			local_test_server server("HTTP/1.1 503 Service Unavailable\r\n"
			                         "Content-Length: 2\r\n"
			                         "\r\n"
			                         "de");

			AnnounceRequest req;
			req.announce_url = "http://127.0.0.1:" + server.port() + "/announce";

			boost::asio::io_context io;
			std::exception_ptr ex;
			int calls = 0;

			TrackerClient().async_announce(io, req, [&ex, &calls](std::exception_ptr e, AnnounceResponse)
			{
				ex = std::move(e);
				calls++;
			});

			io.run();

			assert(calls == 1);
			assert(ex != nullptr);

			bool exceptionThrown = false;

			try
			{
				std::rethrow_exception(ex);
			}
			catch (const std::runtime_error& e)
			{
				exceptionThrown = true;
			}

			assert(exceptionThrown);
		}

		static void test_async_announce_malformed_bencode()
		{
			/*
			200 OK but the body is not valid bencode -> async_announce must deliver a parse error via exception_ptr.
			*/
			const std::string body = "this is not bencode at all";

			local_test_server server("HTTP/1.1 200 OK\r\n"
			                         "Content-Type: text/plain\r\n"
			                         "Content-Length: " + std::to_string(body.size()) + "\r\n"
			                         "\r\n" + body);

			AnnounceRequest req;
			req.announce_url = "http://127.0.0.1:" + server.port() + "/announce";

			boost::asio::io_context io;
			std::exception_ptr ex;
			int calls = 0;

			TrackerClient().async_announce(io, req, [&ex, &calls](std::exception_ptr e, AnnounceResponse)
			{
				ex = std::move(e);
				calls++;
			});

			io.run();

			assert(calls == 1);
			assert(ex != nullptr);

			bool exceptionThrown = false;

			try
			{
				std::rethrow_exception(ex);
			}
			catch (const std::exception& e) // BencodeParser throws invalid_argument/runtime_error/out_of_range
			{
				exceptionThrown = true;
			}

			assert(exceptionThrown);
		}

		static void test_async_announce_throwing_callback_success_path()
		{
			/*
			M4: a user-supplied on_complete that throws must not escape the Asio completion handler into
			std::terminate(). Here it throws std::runtime_error on the success path (200 OK + valid bencode);
			the process must survive and io.run() must return normally. The callback is invoked exactly once.
			*/
			std::string body = "d8:intervali3600ee";

			local_test_server server("HTTP/1.1 200 OK\r\n"
			                         "Content-Type: text/plain\r\n"
			                         "Content-Length: " + std::to_string(body.size()) + "\r\n"
			                         "\r\n" + body);

			AnnounceRequest req;
			req.announce_url = "http://127.0.0.1:" + server.port() + "/announce";

			boost::asio::io_context io;
			int calls = 0;

			TrackerClient().async_announce(io, req, [&calls](std::exception_ptr, AnnounceResponse)
			{
				calls++;
				throw std::runtime_error("#f7b3d9: simulated user callback failure");
			});

			io.run(); // must return normally; a throwing callback used to call std::terminate() here

			assert(calls == 1);
		}

		static void test_async_announce_throwing_callback_failure_paths()
		{
			/*
			M4: the same guarantee on the failure paths. The user callback throws (a non-std::exception type,
			to exercise the catch(...) branch) both when a transport error is delivered and when a non-200
			status is delivered; io.run() must return normally in both cases and each callback fires once.
			*/
			local_test_server server("HTTP/1.1 503 Service Unavailable\r\n"
			                         "Content-Length: 2\r\n"
			                         "\r\n"
			                         "de");

			int calls = 0;

			// Each path gets its own io_context: once run() completes, the context is stopped and a second
			// run() would return immediately without dispatching newly posted operations.

			// Path 1: transport error (connection refused on a closed port) delivered to a throwing callback.
			{
				boost::asio::io_context io;
				AnnounceRequest req;
				req.announce_url = "http://127.0.0.1:1/announce"; // nothing listens here -> transport error

				TrackerClient().async_announce(io, req, [&calls](std::exception_ptr e, AnnounceResponse)
				{
					calls++;
					assert(e != nullptr);
					throw 42; // non-std::exception type: exercises the catch(...) branch of invoke_on_complete()
				});

				io.run();
			}

			// Path 2: non-200 status delivered to a throwing callback.
			{
				boost::asio::io_context io;
				AnnounceRequest req;
				req.announce_url = "http://127.0.0.1:" + server.port() + "/announce";

				TrackerClient().async_announce(io, req, [&calls](std::exception_ptr e, AnnounceResponse)
				{
					calls++;
					assert(e != nullptr);
					throw std::runtime_error("#8d3c91: simulated user callback failure");
				});

				io.run();
			}

			assert(calls == 2); // one delivery per path, no terminate in between
		}

		static void test_sanitize_for_logging_escapes_control_bytes()
		{
			/*
			M3: sanitize_for_logging() must escape every control byte so tracker/torrent-derived text can never forge
			log lines or inject terminal escapes; printable bytes pass through unchanged.
			    "\r\n\x1b[31mhello\x7f" -> "\\r\\n\\x1B[31mhello\\x7F"  (literal backslash sequences)
			    "abc 123"              -> "abc 123"                    (unchanged)
			*/
			const std::string escaped = TrackerClient::sanitize_for_logging("\r\n\x1b[31mhello\x7f");
			assert(escaped == "\\r\\n\\x1B[31mhello\\x7F");

			const std::string plain = TrackerClient::sanitize_for_logging("abc 123");
			assert(plain == "abc 123");
		}

		static void test_parse_response_failure_reason_escaped()
		{
			/*
			M3: a tracker-controlled "failure reason" containing CRLF and an ANSI escape must be stored with the
			control bytes escaped, so displaying it in logs or UIs cannot forge lines or reprogram the terminal.
			    payload = "FAKE\r\n\x1b[2J ok" (13 bytes) -> stored as "FAKE\\r\\n\\x1B[2J ok"
			*/
			const std::string payload = "FAKE\r\n\x1b[2J ok";
			const std::string body = "d14:failure reason" + std::to_string(payload.size()) + ":" + payload + "e";

			const AnnounceResponse resp = TrackerClient::parse_response(body);

			assert(resp.failure_reason.has_value());
			assert(*resp.failure_reason == "FAKE\\r\\n\\x1B[2J ok"); // escaped, not raw control bytes
		}

		static void test_announce_malicious_url_rejected()
		{
			/*
			H1: announce_url comes from the .torrent. A path carrying raw CRLF + an ANSI escape is rejected by
			HttpTransport before any network I/O (std::invalid_argument): no request with injected headers can be
			sent, and the exception message embeds no attacker-controlled text at all.
			*/
			const std::string malicious_path = "/ann\r\nFAKE\x1b[2J"; // log-forging CRLF + terminal escape
			AnnounceRequest req;
			req.announce_url = "http://127.0.0.1:80" + malicious_path;

			bool exceptionThrown = false;
			std::string message;

			try
			{
				TrackerClient().announce(req);
			}
			catch (const std::invalid_argument& e)
			{
				exceptionThrown = true;
				message = e.what();
			}

			assert(exceptionThrown);
			assert(message.find('\r') == std::string::npos); // no raw control byte survives into the message
			assert(message.find('\n') == std::string::npos);
			assert(message.find("\x1b") == std::string::npos);
		}

		static void test_announce_non200_url_sanitized()
		{
			/*
			M3: announce_url comes from the .torrent and is embedded in the non-200 exception message. The URL must
			appear only in escaped form in the thrown message: no raw control byte may survive into e.what().
			*/
			local_test_server server("HTTP/1.1 503 Service Unavailable\r\n"
			                         "Content-Length: 2\r\n"
			                         "\r\n"
			                         "de");

			AnnounceRequest req;
			req.announce_url = "http://127.0.0.1:" + server.port() + "/announce";

			bool exceptionThrown = false;
			std::string message;

			try
			{
				TrackerClient().announce(req);
			}
			catch (const std::runtime_error& e)
			{
				exceptionThrown = true;
				message = e.what();
			}

			assert(exceptionThrown);
			assert(message.find("503") != std::string::npos); // the non-200 status is reported
			assert(message.find('\r') == std::string::npos);  // no raw control byte survives into the message
		}

		static std::string make_announce_body(int64_t interval, const std::vector<std::array<uint8_t, 6>>& peers)
		{
			std::string body = "d8:intervali" + std::to_string(interval) + "e5:peers";
			body += std::to_string(peers.size() * 6) + ":";

			for (const auto& p : peers)
			{
				body.append(p.begin(), p.end());
			}

			body += "e"; // single 'e' closes the dictionary
			return body;
		}

		static void test_announce_all_multi_tracker_success_and_dedup()
		{
			/*
			Concurrent announce to three local trackers with overlapping peer lists (no external network):
			    results must hold one entry per URL in input order, peers must be the deduplicated union
			in discovery order and min_interval the smallest interval among successful responses.
			*/
			const std::array<uint8_t, 6> p1 = {0x01, 0x02, 0x03, 0x04, 0x1A, 0xE1}; // 1.2.3.4:6881
			const std::array<uint8_t, 6> p2 = {0x05, 0x06, 0x07, 0x08, 0x1A, 0xE2}; // 5.6.7.8:6882
			const std::array<uint8_t, 6> p3 = {0x09, 0x0A, 0x0B, 0x0C, 0x1A, 0xE3}; // 9.10.11.12:6883
			const std::array<uint8_t, 6> p4 = {0x0D, 0x0E, 0x0F, 0x10, 0x1A, 0xE4}; // 13.14.15.16:6884

			const std::string body_a = make_announce_body(1800, {p1, p2});
			const std::string body_b = make_announce_body(900, {p2, p3}); // overlaps tracker A on 5.6.7.8:6882
			const std::string body_c = make_announce_body(3600, {p1, p4}); // overlaps tracker A on 1.2.3.4:6881

			local_test_server server_a("HTTP/1.1 200 OK\r\n"
			                           "Content-Type: text/plain\r\n"
			                           "Content-Length: " + std::to_string(body_a.size()) + "\r\n"
			                           "\r\n" + body_a);
			local_test_server server_b("HTTP/1.1 200 OK\r\n"
			                           "Content-Type: text/plain\r\n"
			                           "Content-Length: " + std::to_string(body_b.size()) + "\r\n"
			                           "\r\n" + body_b);
			local_test_server server_c("HTTP/1.1 200 OK\r\n"
			                           "Content-Type: text/plain\r\n"
			                           "Content-Length: " + std::to_string(body_c.size()) + "\r\n"
			                           "\r\n" + body_c);

			const std::string url_a = "http://127.0.0.1:" + server_a.port() + "/announce";
			const std::string url_b = "http://127.0.0.1:" + server_b.port() + "/announce";
			const std::string url_c = "http://127.0.0.1:" + server_c.port() + "/announce";

			AnnounceRequest req; // base request: announce_all() fills in one URL per tracker
			req.peer_id = {'T', 'T'};
			req.port = 6881;

			const MultiAnnounceResult out = TrackerClient().announce_all({url_a, url_b, url_c}, req);

			assert(out.results.size() == 3); // one entry per input URL
			assert(out.results[0].announce_url == url_a); // input order preserved
			assert(out.results[1].announce_url == url_b);
			assert(out.results[2].announce_url == url_c);

			for (const TrackerResult& tr : out.results)
			{
				assert(tr.error == nullptr);
				assert(tr.response.has_value());
			}

			assert(out.peers.size() == 4); // deduplicated union: p1, p2 from A; p3 from B; p4 from C
			assert(out.peers[0].ip == "1.2.3.4");
			assert(out.peers[0].port == 6881);
			assert(out.peers[1].ip == "5.6.7.8");
			assert(out.peers[1].port == 6882);
			assert(out.peers[2].ip == "9.10.11.12");
			assert(out.peers[2].port == 6883);
			assert(out.peers[3].ip == "13.14.15.16");
			assert(out.peers[3].port == 6884);

			assert(out.min_interval == 900); // smallest of 1800/900/3600
		}

		static void test_announce_all_partial_failure_isolation()
		{
			/*
			Concurrent announce where one tracker fails at the transport level (HTTP 503) and another
			reports a bencoded "failure reason": neither may throw nor poison the aggregate; only the
			healthy tracker's peers/interval contribute to the merged result.
			*/
			const std::array<uint8_t, 6> p1 = {0x01, 0x02, 0x03, 0x04, 0x1A, 0xE1}; // 1.2.3.4:6881
			const std::string body_ok = make_announce_body(1800, {p1});

			local_test_server server_ok("HTTP/1.1 200 OK\r\n"
			                            "Content-Type: text/plain\r\n"
			                            "Content-Length: " + std::to_string(body_ok.size()) + "\r\n"
			                            "\r\n" + body_ok);
			local_test_server server_503("HTTP/1.1 503 Service Unavailable\r\n"
			                             "Content-Length: 2\r\n"
			                             "\r\n"
			                             "de");
			const std::string body_failure = "d14:failure reason16:invalid infohashe";

			local_test_server server_failure("HTTP/1.1 200 OK\r\n"
			                                 "Content-Type: text/plain\r\n"
			                                 "Content-Length: " + std::to_string(body_failure.size()) + "\r\n"
			                                 "\r\n" + body_failure);

			const std::string url_ok = "http://127.0.0.1:" + server_ok.port() + "/announce";
			const std::string url_503 = "http://127.0.0.1:" + server_503.port() + "/announce";
			const std::string url_failure = "http://127.0.0.1:" + server_failure.port() + "/announce";

			AnnounceRequest req;
			req.peer_id = {'T', 'T'};
			req.port = 6881;

			MultiAnnounceResult out;
			bool threw = false;

			try
			{
				out = TrackerClient().announce_all({url_ok, url_503, url_failure}, req);
			}
			catch (const std::exception& e)
			{
				threw = true;
				std::cout << "#075290: announce_all threw unexpectedly: " << e.what() << std::endl;
			}

			assert(!threw); // per-tracker failures must be isolated in TrackerResult, not thrown
			assert(out.results.size() == 3);

			// Tracker 1: healthy -> parsed response with peers, no error.
			assert(out.results[0].error == nullptr);
			assert(out.results[0].response.has_value());
			assert(!out.results[0].response->failure_reason.has_value());
			assert(out.results[0].response->peers.size() == 1);

			// Tracker 2: HTTP 503 -> error populated, no response.
			assert(out.results[1].error != nullptr);
			assert(!out.results[1].response.has_value());

			bool exceptionThrown = false;

			try
			{
				std::rethrow_exception(out.results[1].error);
			}
			catch (const std::exception& e) // HttpTransport throws runtime_error for non-200 statuses
			{
				exceptionThrown = true;
			}

			assert(exceptionThrown);

			// Tracker 3: bencoded "failure reason" -> parsed response, no error, contributes nothing.
			assert(out.results[2].error == nullptr);
			assert(out.results[2].response.has_value());
			assert(out.results[2].response->failure_reason.has_value());
			assert(*out.results[2].response->failure_reason == "invalid infohash");

			// Aggregate: only the healthy tracker's peer survives; its interval is the minimum.
			assert(out.peers.size() == 1);
			assert(out.peers[0].ip == "1.2.3.4");
			assert(out.peers[0].port == 6881);
			assert(out.min_interval == 1800);
		}

		static void test_announce_all_empty_url_list()
		{
			/*
			Empty URL list -> announce_all() must short-circuit with an empty MultiAnnounceResult:
			no I/O, no threads, nothing to aggregate.
			*/
			AnnounceRequest req;
			req.peer_id = {'T', 'T'};

			const MultiAnnounceResult out = TrackerClient().announce_all({}, req);

			assert(out.results.empty());
			assert(out.peers.empty());
			assert(out.min_interval == 0);
		}

		static void test_announce_all_deduplicates_duplicate_urls()
		{
			/*
			H2: duplicate URLs in the (attacker-controlled) torrent tracker list must be announced only once.
			The single local server is single-shot, so a second connection to it would time out and poison the
			result; exactly one result entry for the URL proves deduplication.
			*/
			const std::array<uint8_t, 6> p1 = {0x01, 0x02, '\x03', '\x04', static_cast<uint8_t>(0x1A), static_cast<uint8_t>(0xE1)}; // 1.2.3.4:6881
			const std::string body = make_announce_body(1800, {p1});

			local_test_server server("HTTP/1.1 200 OK\r\n"
			                         "Content-Type: text/plain\r\n"
			                         "Content-Length: " + std::to_string(body.size()) + "\r\n"
			                         "\r\n" + body);

			const std::string url = "http://127.0.0.1:" + server.port() + "/announce";

			AnnounceRequest req;
			req.peer_id = {'T', 'T'};
			req.port = 6881;

			const MultiAnnounceResult out = TrackerClient().announce_all({url, url, url}, req); // same URL three times

			assert(out.results.size() == 1); // duplicates collapsed to a single entry (first occurrence wins)
			assert(out.results[0].announce_url == url);
			assert(out.results[0].error == nullptr);
			assert(out.results[0].response.has_value());
			assert(out.peers.size() == 1);
			assert(out.min_interval == 1800);
		}

		static void test_announce_all_caps_distinct_tracker_count()
		{
			/*
			H2: a torrent may list far more distinct tracker URLs than the client is willing to contact.
			announce_all() must process at most kMaxTrackersPerAnnounce of them (first-occurrence order) and
			silently ignore the rest, so thread/socket exhaustion from a malicious .torrent is impossible.
			The single local server serves exactly one connection; all other URLs are unresolvable names that
			fail fast in DNS without opening sockets.
			*/
			const std::array<uint8_t, 6> p1 = {0x01, 0x02, '\x03', '\x04', static_cast<uint8_t>(0x1A), static_cast<uint8_t>(0xE1)}; // 1.2.3.4:6881
			const std::string body = make_announce_body(1800, {p1});

			local_test_server server("HTTP/1.1 200 OK\r\n"
			                         "Content-Type: text/plain\r\n"
			                         "Content-Length: " + std::to_string(body.size()) + "\r\n"
			                         "\r\n" + body);

			const std::string url_ok = "http://127.0.0.1:" + server.port() + "/announce";

			std::vector<std::string> urls;
			urls.push_back(url_ok); // first URL: the only one that can actually be served

			for (std::size_t i = 0; i < kMaxTrackersPerAnnounce * 2; ++i) // well above the cap, all distinct
			{
				urls.push_back("http://nonexistent-tracker-" + std::to_string(i) + ".invalid/announce");
			}

			AnnounceRequest req;
			req.peer_id = {'T', 'T'};
			req.port = 6881;

			const MultiAnnounceResult out = TrackerClient().announce_all(urls, req);

			assert(out.results.size() == kMaxTrackersPerAnnounce); // capped: the rest of the list is ignored
			assert(out.results[0].announce_url == url_ok);         // first-occurrence order preserved

			// The healthy tracker still contributes its peers/interval; the unresolvable ones carry errors.
			assert(out.results[0].error == nullptr);
			assert(out.results[0].response.has_value());
			assert(out.peers.size() == 1);
			assert(out.min_interval == 1800);

			for (std::size_t i = 1; i < out.results.size(); ++i)
			{
				assert(out.results[i].error != nullptr); // DNS failure, isolated per tracker
			}
		}
};



#ifndef FUZZER
int main()
{
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
