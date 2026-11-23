#ifndef TRACKER_CLIENT_HPP
#define TRACKER_CLIENT_HPP

/*
TrackerClient - BitTorrent tracker protocol on top of HttpTransport (phase 2, plan step 2 in phase2.cpp).

Responsibilities:
    - builds the announce URL with a percent-encoded query string (BEP-3)
    - performs one synchronous HTTP GET via HttpTransport and checks for HTTP 200
    - parses the bencoded tracker response into an AnnounceResponse
    - surfaces "failure reason" cleanly
    - async single-tracker announce on a caller-owned io_context (async_announce)
    - concurrent multi-tracker fan-out with worker threads, per-URL results and aggregated peers (announce_all)

Header-only: all definitions are inline. Supports compact IPv4 "peers", non-compact dictionary-list
"peers" (BEP-3) and 18-byte compact IPv6 "peers6" (BEP-7). Retries/fallback tiers are intentionally not implemented yet.

Usage:
    TrackerClient client; // default HttpTransport config
    AnnounceRequest req;
    req.announce_url = "http://tracker.opentrackr.org/announce";
    ...
    AnnounceResponse resp = client.announce(req);
    // or concurrently against several trackers (phase 5):
    MultiAnnounceResult multi = client.announce_all({url1, url2}, req);
*/

#include "HttpTransport.hpp"
#include "../phase1/BencodeParser.hpp"

#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/address_v6.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Hard cap on the number of peers accepted per announce response (across "peers" and "peers6").
// Real trackers return at most a few thousand; this bounds heap growth from an attacker-controlled
// compact peer string (each 6-byte group becomes a ~50-100 B Peer object) to ~6.5 MB worst case.
static constexpr std::size_t kMaxPeersPerResponse = 65536;

// Hard cap on the number of distinct tracker URLs processed by announce_all() (H2). The URL list comes from
// the .torrent file and is attacker-controlled; without this cap a malicious torrent could spawn thousands
// of concurrent connections/threads. Real torrents use at most a handful of trackers.
static constexpr std::size_t kMaxTrackersPerAnnounce = 25;

// Absolute cap on announce_all() worker threads (H2): bounds thread/socket exhaustion even when the caller
// passes an attacker-influenced num_threads value.
static constexpr std::size_t kMaxAnnounceThreads = 8;

// M1 sanity bound on "interval" (seconds, BEP-3): negative values would cause immediate re-announce hot loops
// or timer underflow in callers, and huge values (e.g. INT64_MAX) an infinite stall or overflow in downstream
// int conversions. Reject < 0 and cap at ~7 days so the value can be used directly for scheduling.
static constexpr int64_t kMaxIntervalSeconds = 7 * 24 * 60 * 60; // 604800 s (7 days)

// A single peer from a tracker response: IP address as a canonical string (dotted-quad IPv4 or compressed IPv6
// literal; list-form values are validated and normalized at parse time, see normalize_peer_ip()) + port in host byte order.
struct Peer
{
	std::string ip; // e.g. "192.168.1.1"
	uint16_t port = 0; // host byte order
};

enum class AnnounceEvent
{
	None,
	Started,
	Stopped,
	Completed,
};

struct AnnounceRequest
{
	std::string announce_url; // e.g. "http://tracker.opentrackr.org/announce" (no query string expected)
	std::array<uint8_t, 20> info_hash{};
	std::array<uint8_t, 20> peer_id{};
	uint16_t port = 0;
	uint64_t uploaded = 0;
	uint64_t downloaded = 0;
	uint64_t left = 0;
	AnnounceEvent event = AnnounceEvent::None;
};

struct AnnounceResponse
{
	int64_t interval = 0;
	std::vector<Peer> peers;
	std::optional<std::string> failure_reason; // set when the tracker reports a "failure reason" (M3: control bytes escaped for safe log/UI display)
};

// Per-tracker outcome of announce_all(): one entry per input URL, in input order.
struct TrackerResult
{
	std::string announce_url; // the exact URL that was announced to
	std::optional<AnnounceResponse> response; // set when HTTP 200 + bencode parse succeeded; check failure_reason for tracker-level errors
	std::exception_ptr error; // transport/timeout/non-200/parse failure (null on success)
};

// Aggregated outcome of announce_all() across all trackers.
struct MultiAnnounceResult
{
	std::vector<TrackerResult> results; // one per input URL, in input order
	std::vector<Peer> peers;            // deduplicated union over successful non-failure responses (discovery order preserved)
	int64_t min_interval = 0;           // smallest positive interval among successful non-failure responses, else 0
};

namespace
{
// bencode dict keys are byte strings: build one from a C string for lookup.
Bytes make_key(const char* key)
{
	Bytes k;

	for (const char * p = key; *p != '\0'; ++p)
	{
		k.push_back(static_cast<std::byte>(static_cast<unsigned char>(*p)));
	}

	return k;
}

// Lookup of a dict entry by string key; nullptr if absent.
const BencodeValue* dict_get(const Dict& d, const char* key)
{
	const auto it = d.find(make_key(key));
	return (it == d.end()) ? nullptr : &it->second;
}

// std::string -> vector<std::byte> for the bencode Parser.
Bytes to_bytes(const std::string& s)
{
	Bytes out(s.size());

	for (std::size_t i = 0; i < s.size(); ++i)
	{
		out[i] = static_cast<std::byte>(static_cast<unsigned char>(s[i]));
	}

	return out;
}

// H3 (SSRF hardening): validate a list-form peer "ip" as a real IP literal and normalize its textual form.
// Rejects empty strings, hostnames and arbitrary byte blobs: if downstream code resolves or logs them, an
// attacker would otherwise control which names get resolved (DNS steering) and what reaches logs/UIs.
// IPv4-mapped IPv6 forms ("::ffff:a.b.c.d") are normalized to plain dotted-quad so equivalent addresses
// deduplicate. Private/loopback literals are intentionally KEPT: BEP-3 trackers may legitimately return LAN
// peers (local swarms); the SSRF trade-off is an explicit, documented decision and filtering belongs in the
// connection layer, which knows the local interfaces.
std::optional<std::string> normalize_peer_ip(const std::string& ip)
{
	if (ip.empty())
	{
		return std::nullopt;
	}

	boost::system::error_code ec;
	const auto addr = boost::asio::ip::make_address(ip, ec); // accepts IPv4 and IPv6 literals only, never hostnames

	if (ec)
	{
		return std::nullopt; // hostname / arbitrary blob -> reject
	}

	// IPv4 literals are already canonical dotted-quad; to_v6() would throw bad_address_cast on them.
	if (!addr.is_v6())
	{
		return addr.to_string();
	}

	const auto v6 = addr.to_v6(); // safe here: is_v6() checked above

	if (v6.is_v4_mapped())
	{
		// This Boost version has no to_v4()/to_v4_mapped(): extract the embedded IPv4 bytes manually.
		const auto v6_bytes = v6.to_bytes();

		boost::asio::ip::address_v4::bytes_type v4_bytes{}; // std::array<unsigned char, 4> (no push_back)

		for (std::size_t i = 0; i < 4; ++i)
		{
			v4_bytes[i] = v6_bytes[12 + i]; // last 4 bytes of an IPv4-mapped address
		}

		return boost::asio::ip::address_v4(v4_bytes).to_string(); // "::ffff:a.b.c.d" -> "a.b.c.d"
	}

	return addr.to_string(); // canonical form: dotted-quad for IPv4, compressed for IPv6
}
}

class TrackerClient
{
	public:
		explicit TrackerClient(HttpTransport http = {}) : m_http(std::move(http)) {}

		/*
		Percent-encode raw bytes for a URL query string (BEP-3 sends info_hash/peer_id as the raw
		bytes, percent-encoded byte-by-byte). RFC 3986 unreserved characters (A-Z, a-z, 0-9, '-', '_', '.', '~')
		are kept as-is; everything else becomes %XX with uppercase hex digits.
		*/
		static std::string percent_encode(const uint8_t* data, std::size_t len)
		{
			static constexpr char kHex[] = "0123456789ABCDEF";
			std::string out;
			out.reserve(len * 3);

			for (std::size_t i = 0; i < len; ++i)
			{
				const auto c = data[i];

				if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
				{
					out += static_cast<char>(c);
				}
				else
				{
					out += '%';
					out += kHex[c >> 4];
					out += kHex[c & 0x0F];
				}
			}

			return out;
		}

		/*
		Build the full announce URL:
		    <announce_url>?info_hash=..&peer_id=..&port=..&uploaded=..&downloaded=..&left=..&compact=1[&event=..]
		If announce_url already contains a query string, parameters are appended with '&'.

		SSRF note (H3): the tracker host is attacker-controlled by design (BEP-3 names it in the .torrent); no
		internal-endpoint filtering is applied here. That is an explicit, documented trade-off of BEP-3.
		*/
		static std::string build_announce_url(const AnnounceRequest& req)
		{
			std::string query = "info_hash=" + percent_encode(req.info_hash.data(), 20);
			query += "&peer_id=" + percent_encode(req.peer_id.data(), 20);
			query += "&port=" + std::to_string(req.port);
			query += "&uploaded=" + std::to_string(req.uploaded);
			query += "&downloaded=" + std::to_string(req.downloaded);
			query += "&left=" + std::to_string(req.left);
			query += "&compact=1";

			if (req.event != AnnounceEvent::None)
			{
				std::string event;

				switch (req.event)
				{
				case AnnounceEvent::Started:
					event = "started";
					break;

				case AnnounceEvent::Stopped:
					event = "stopped";
					break;

				case AnnounceEvent::Completed:
					event = "completed";
					break;

				default:
					break; // None is excluded above
				}

				query += "&event=" + event;
			}

			const char sep = (req.announce_url.find('?') != std::string::npos) ? '&' : '?';
			return req.announce_url + sep + query;
		}

		/*
		M3: escape control characters in tracker/torrent-derived text before it is embedded in exceptions or stored
		for log/UI display, so a malicious "failure reason" or announce_url cannot forge log lines or inject terminal
		escapes. \r -> "\\r", \n -> "\\n", any other byte < 0x20 or == 0x7F -> "\xNN"; printable bytes are unchanged.
		*/
		static std::string sanitize_for_logging(const std::string& s)
		{
			std::string out;
			out.reserve(s.size());

			for (const char ch : s)
			{
				const unsigned char c = static_cast<unsigned char>(ch);

				if (c == '\r')
				{
					out += "\\r";
				}
				else if (c == '\n')
				{
					out += "\\n";
				}
				else if (c < 0x20 || c == 0x7F)
				{
					static constexpr char kHex[] = "0123456789ABCDEF";
					out += "\\x";
					out += kHex[c >> 4];
					out += kHex[c & 0xF];
				}
				else
				{
					out += ch;
				}
			}

			return out;
		}

		/*
		Parse a raw bencoded tracker response body into an AnnounceResponse:
		    - the entire body must be consumed (no trailing bytes after the top-level value)
		    - "failure reason" is checked first and surfaced in AnnounceResponse.failure_reason, with control bytes
		      escaped via sanitize_for_logging() so it is safe to write to logs or UIs (M3)
		    - "interval" (optional integer, must be within [0, kMaxIntervalSeconds]; M1 sanity bound)
		    - "peers" (optional): compact byte string (length must be a multiple of 6, each group of 6 bytes is
		      4-byte big-endian IPv4 + 2-byte big-endian port) or a list of dictionaries with required
		      "ip" (byte string holding a real IP literal; empty values, hostnames and other non-IP blobs are
		      rejected, IPv4-mapped forms are normalized to dotted-quad) and "port" (integer in [0, 65535]);
		      "peer id" is ignored
		    - "peers6" (optional, BEP-7): compact byte string, length must be a multiple of 18; each group
		      of 18 bytes is a 16-byte IPv6 address + 2-byte big-endian port (address formatted canonically)

		SSRF note (H3): peer addresses and tracker hosts are attacker-controlled by design. Private/loopback
		peer literals are intentionally kept because BEP-3 trackers may legitimately return LAN peers; filtering
		belongs in the connection layer, which knows the local interfaces. Tracker hosts named by the .torrent
		are likewise not filtered (see build_announce_url). This is an explicit, documented trade-off.
		*/
		static AnnounceResponse parse_response(const std::string& body)
		{
			const Bytes input = to_bytes(body);
			Parser parser(input);
			BencodeValue value = parser.parse();

			// BencodeValue.raw spans exactly the bytes consumed by the top-level value (which starts at offset 0):
			// if it does not cover the whole input, trailing unparsed bytes remain -> reject.
			if (value.raw.size() != input.size())
			{
				throw std::invalid_argument("#dd3de5: malformed tracker response: " + std::to_string(input.size() - value.raw.size()) + " trailing byte(s) after top-level value");
			}

			if (!value.is_dict())
			{
				throw std::invalid_argument("#1551f3: malformed tracker response: expected a bencoded dictionary");
			}

			const auto& dict = value.get_dict();
			AnnounceResponse resp;

			// 1) "failure reason" first: surface it cleanly and stop here.
			if (const BencodeValue * fr = dict_get(dict, "failure reason"))
			{
				if (!fr->is_bytes())
				{
					throw std::invalid_argument("#26f981: malformed tracker response: \"failure reason\" is not a byte string");
				}

				const auto& raw = fr->get_bytes();

				// M3: the tracker controls this string; escape control bytes so it cannot forge log lines or inject
				// terminal escapes when displayed.
				resp.failure_reason = sanitize_for_logging(std::string(reinterpret_cast<const char*>(raw.data()), raw.size()));
				return resp;
			}

			// 2) "interval" (optional, must be an integer in [0, kMaxIntervalSeconds] when present; M1).
			if (const BencodeValue * iv = dict_get(dict, "interval"))
			{
				if (!iv->is_int())
				{
					throw std::invalid_argument("#4edb5b: malformed tracker response: \"interval\" is not an integer");
				}

				const int64_t interval = iv->get_int();

				if (interval < 0 || interval > kMaxIntervalSeconds)
				{
					throw std::invalid_argument("#a3c7d9: malformed tracker response: \"interval\" " + std::to_string(interval) + " is out of range [0, " + std::to_string(kMaxIntervalSeconds) + "]");
				}

				resp.interval = interval;
			}

			// 3) "peers" (optional): either a compact byte string of 6-byte IPv4+port groups, or a list of
			//    dictionaries with required "ip" (byte string) and "port" (integer in [0, 65535]); "peer id" is ignored.
			if (const BencodeValue * pv = dict_get(dict, "peers"))
			{
				if (pv->is_bytes())
				{
					const auto& raw = pv->get_bytes();

					if (raw.size() % 6 != 0)
					{
						throw std::invalid_argument("#544212: malformed tracker response: compact peers length " + std::to_string(raw.size()) + " is not a multiple of 6");
					}

					const std::size_t peer_count = raw.size() / 6;

					if (peer_count > kMaxPeersPerResponse)
					{
						throw std::invalid_argument("#3f8a19: malformed tracker response: compact peers count " + std::to_string(peer_count) + " exceeds limit of " + std::to_string(kMaxPeersPerResponse));
					}

					resp.peers.reserve(peer_count);

					for (std::size_t i = 0; i < raw.size(); i += 6)
					{
						const auto a = static_cast<unsigned int>(raw[i]);
						const auto b = static_cast<unsigned int>(raw[i + 1]);
						const auto c = static_cast<unsigned int>(raw[i + 2]);
						const auto d = static_cast<unsigned int>(raw[i + 3]);
						const uint16_t port = static_cast<uint16_t>((static_cast<unsigned int>(raw[i + 4]) << 8) | static_cast<unsigned int>(raw[i + 5]));

						resp.peers.push_back(Peer{std::to_string(a) + "." + std::to_string(b) + "." + std::to_string(c) + "." + std::to_string(d), port});
					}
				}
				else if (pv->is_list())
				{
					const auto& list = pv->get_list();

					if (list.size() > kMaxPeersPerResponse)
					{
						throw std::invalid_argument("#c0d5f6: malformed tracker response: peers list size " + std::to_string(list.size()) + " exceeds limit of " + std::to_string(kMaxPeersPerResponse));
					}

					resp.peers.reserve(list.size());

					for (const BencodeValue & entry : list)
					{
						if (!entry.is_dict())
						{
							throw std::invalid_argument("#eab63e: malformed tracker response: \"peers\" list element is not a dictionary");
						}

						const auto& d = entry.get_dict();

						const BencodeValue * ip = dict_get(d, "ip");

						if (ip == nullptr || !ip->is_bytes())
						{
							throw std::invalid_argument("#a54ff2: malformed tracker response: \"peers\" list element is missing a byte-string \"ip\"");
						}

						const auto& raw_ip = ip->get_bytes();

						const BencodeValue * port_v = dict_get(d, "port");

						if (port_v == nullptr || !port_v->is_int())
						{
							throw std::invalid_argument("#f6bc0d: malformed tracker response: \"peers\" list element is missing an integer \"port\"");
						}

						const int64_t port = port_v->get_int();

						if (port < 0 || port > 65535)
						{
							throw std::invalid_argument("#ff6184: malformed tracker response: \"peers\" list element has out-of-range port " + std::to_string(port));
						}

						const std::string ip_str = std::string(reinterpret_cast<const char*>(raw_ip.data()), raw_ip.size());

						// H3: list-form "ip" must be a real IP literal (no hostnames/empty/arbitrary blobs) and is normalized.
						const auto norm_ip = normalize_peer_ip(ip_str);

						if (!norm_ip.has_value())
						{
							throw std::invalid_argument("#7c2e91: malformed tracker response: \"peers\" list element has a non-IP-literal \"ip\"");
						}

						resp.peers.push_back(Peer{*norm_ip, static_cast<uint16_t>(port)});
					}
				}
				else
				{
					throw std::invalid_argument("#2d4e22: malformed tracker response: \"peers\" is neither a byte string nor a list");
				}
			}

			// 4) "peers6" (optional, BEP-7): compact byte string of 18-byte IPv6+port groups.
			if (const BencodeValue * p6 = dict_get(dict, "peers6"))
			{
				if (!p6->is_bytes())
				{
					throw std::invalid_argument("#3dda13: malformed tracker response: \"peers6\" is not a byte string");
				}

				const auto& raw = p6->get_bytes();

				if (raw.size() % 18 != 0)
				{
					throw std::invalid_argument("#77e284: malformed tracker response: peers6 length " + std::to_string(raw.size()) + " is not a multiple of 18");
				}

				const std::size_t p6_count = raw.size() / 18;

				if (p6_count > kMaxPeersPerResponse || resp.peers.size() > kMaxPeersPerResponse - p6_count)
				{
					throw std::invalid_argument("#9e2b7a: malformed tracker response: total peer count exceeds limit of " + std::to_string(kMaxPeersPerResponse));
				}

				resp.peers.reserve(resp.peers.size() + p6_count);

				for (std::size_t i = 0; i < raw.size(); i += 18)
				{
					boost::asio::ip::address_v6::bytes_type addr{};

					for (std::size_t j = 0; j < 16; ++j)
					{
						addr[j] = static_cast<uint8_t>(raw[i + j]);
					}

					const uint16_t port = static_cast<uint16_t>((static_cast<unsigned int>(raw[i + 16]) << 8) | static_cast<unsigned int>(raw[i + 17]));

					resp.peers.push_back(Peer{boost::asio::ip::make_address_v6(addr).to_string(), port});
				}
			}

			return resp;
		}

		// Build the announce URL, perform one synchronous HTTP GET via HttpTransport, require HTTP 200 and parse the body.
		AnnounceResponse announce(const AnnounceRequest& req) const
		{
			const std::string url = build_announce_url(req);
			const HttpResponse http_resp = m_http.get(url);

			if (http_resp.status_code != 200)
			{
				// M3: announce_url comes from the .torrent; escape control bytes before embedding it in the message.
				throw std::runtime_error("#d56990: tracker returned HTTP " + std::to_string(http_resp.status_code) + " for " + sanitize_for_logging(url));
			}

			return parse_response(http_resp.body);
		}

		// Asynchronous announce on a caller-owned io_context: builds the URL, performs one async GET via HttpTransport,
		// requires HTTP 200 and parses the body. on_complete is invoked at most once with a non-null exception_ptr on any
		// failure (transport error, non-200 status, or malformed bencode) and the parsed AnnounceResponse on success.
		// M4: if the user-supplied on_complete itself throws, that exception is caught by invoke_on_complete() and logged
		// (#c4e8a2 for std::exception types, #4a7f2b for non-std::exception types) instead of escaping the Asio
		// completion handler into std::terminate().
		void async_announce(boost::asio::io_context& io, const AnnounceRequest& req, std::function<void(std::exception_ptr, AnnounceResponse)> on_complete) const
		{
			const std::string url = build_announce_url(req);

			m_http.async_get(io, url, [on_complete = std::move(on_complete), url](std::exception_ptr ex, const HttpResponse & http_resp)
			{
				if (ex)
				{
					invoke_on_complete(std::move(on_complete), std::move(ex), AnnounceResponse{});
					return;
				}

				if (http_resp.status_code != 200)
				{
					// M3: announce_url comes from the .torrent; escape control bytes before embedding it in the message.
					invoke_on_complete(std::move(on_complete), std::make_exception_ptr(std::runtime_error("tracker returned HTTP " + std::to_string(http_resp.status_code) + " for " + sanitize_for_logging(url))), AnnounceResponse{});
					return;
				}

				std::exception_ptr parse_ex;
				AnnounceResponse resp;

				try
				{
					resp = parse_response(http_resp.body);
				}
				catch (...)
				{
					parse_ex = std::current_exception();
				}

				if (parse_ex)
				{
					invoke_on_complete(std::move(on_complete), std::move(parse_ex), AnnounceResponse{});
				}
				else
				{
					invoke_on_complete(std::move(on_complete), nullptr, std::move(resp));
				}
			});
		}

		/*
		Concurrent multi-tracker announce (phase 5): announces to every URL in announce_urls on a local
		io_context with worker threads and collects the per-URL outcomes plus an aggregated peer list:
		    - empty announce_urls -> empty MultiAnnounceResult (no I/O at all)
		    - H2 hardening: announce_urls comes from an attacker-controlled .torrent file, so duplicate URLs are
		      announced only once (first occurrence wins) and at most kMaxTrackersPerAnnounce distinct URLs are
		      processed; any remaining URLs are ignored. results[i] corresponds to the i-th distinct URL in
		      first-occurrence order
		    - each callback writes into its own disjoint slot, so no synchronization is needed between workers
		    - peers: deduplicated union of identical (ip, port) pairs over responses that succeeded without a
		      "failure reason", preserving discovery order (results in input URL order, peers in response order)
		    - min_interval: smallest positive interval among successful non-failure responses, else 0
		num_threads is the number of io_context worker threads; clamped to the processed URL count and to the
		absolute constant kMaxAnnounceThreads, at least 1.
		*/
		MultiAnnounceResult announce_all(const std::vector<std::string>& announce_urls, const AnnounceRequest& base_req, std::size_t num_threads = 4) const
		{
			MultiAnnounceResult out;

			if (announce_urls.empty())
			{
				return out; // nothing to do: no I/O, no threads
			}

			// H2: deduplicate (first occurrence wins) and cap at kMaxTrackersPerAnnounce distinct URLs so a
			// malicious torrent cannot spawn thousands of concurrent connections/threads or result slots.
			std::vector<std::string> urls;
			urls.reserve(std::min(announce_urls.size(), kMaxTrackersPerAnnounce));

			for (const std::string& url : announce_urls)
			{
				if (urls.size() >= kMaxTrackersPerAnnounce)
				{
					break; // cap reached: ignore the remaining URLs
				}

				if (std::find(urls.begin(), urls.end(), url) == urls.end())
				{
					urls.push_back(url); // first occurrence wins; duplicates are skipped
				}
			}

			boost::asio::io_context io; // local executor: all async work below runs on the worker threads spawned here
			out.results.resize(urls.size());

			for (std::size_t i = 0; i < urls.size(); ++i)
			{
				out.results[i].announce_url = urls[i];
			}

			for (std::size_t i = 0; i < urls.size(); ++i)
			{
				AnnounceRequest req = base_req; // copy: each tracker gets its own URL
				req.announce_url = urls[i];

				async_announce(io, req, [&out, i](std::exception_ptr ex, AnnounceResponse resp)
				{
					if (ex)
					{
						out.results[i].error = std::move(ex); // transport/timeout/non-200/parse failure
					}
					else
					{
						out.results[i].response = std::move(resp); // HTTP 200 + bencode parse succeeded (disjoint slot: no mutex needed)
					}
				});
			}

			const std::size_t workers = std::min({(num_threads == 0) ? 1 : num_threads, urls.size(), kMaxAnnounceThreads}); // H2: absolute thread cap as well
			std::vector<std::thread> pool;
			pool.reserve(workers);

			for (std::size_t t = 0; t < workers; ++t)
			{
				pool.emplace_back([&io]()
				{
					io.run();
				}); // each thread exits once no work remains
			}

			for (std::thread & th : pool)
			{
				th.join(); // all announces are complete before we aggregate below
			}

			std::set<std::pair<std::string, uint16_t>> seen;

			for (const TrackerResult & tr : out.results)
			{
				if (!tr.response.has_value() || tr.response->failure_reason.has_value())
				{
					continue; // only successful non-failure responses contribute peers/interval
				}

				for (const Peer & p : tr.response->peers)
				{
					if (seen.insert({p.ip, p.port}).second)
					{
						out.peers.push_back(p); // first discovery wins; order preserved
					}
				}

				if (tr.response->interval > 0 && (out.min_interval == 0 || tr.response->interval < out.min_interval))
				{
					out.min_interval = tr.response->interval;
				}
			}

			return out;
		}

	private:
		// M4: on_complete runs inside a Boost.Asio completion handler; an exception escaping that path calls
		// std::terminate(). This wrapper guarantees a throwing user callback can never propagate out of
		// async_announce(): any exception is caught, logged with #c4e8a2, and swallowed.
		static void invoke_on_complete(const std::function<void(std::exception_ptr, AnnounceResponse)>& on_complete, std::exception_ptr ex, AnnounceResponse resp)
		{
			try
			{
				on_complete(std::move(ex), std::move(resp));
			}
			catch (const std::exception& e)
			{
				std::cerr << "#c4e8a2: async_announce user callback threw, exception swallowed: " << e.what() << std::endl;
			}
			catch (...)
			{
				std::cerr << "#4a7f2b: async_announce user callback threw a non-std::exception type, exception swallowed" << std::endl;
			}
		}

		HttpTransport m_http;
};

#endif // TRACKER_CLIENT_HPP
