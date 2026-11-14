#ifndef TRACKER_CLIENT_HPP
#define TRACKER_CLIENT_HPP

/*
TrackerClient - BitTorrent tracker protocol on top of HttpTransport (phase 2, plan step 2 in phase2.cpp).

Responsibilities:
    - builds the announce URL with a percent-encoded query string (BEP-3)
    - performs one synchronous HTTP GET via HttpTransport and checks for HTTP 200
    - parses the bencoded tracker response into an AnnounceResponse
    - surfaces "failure reason" cleanly

Header-only: all definitions are inline. Synchronous only; supports compact IPv4 "peers", non-compact
dictionary-list "peers" (BEP-3) and 18-byte compact IPv6 "peers6" (BEP-7). Async I/O and retries are intentionally not implemented yet.

Usage:
    TrackerClient client; // default HttpTransport config
    AnnounceRequest req;
    req.announce_url = "http://tracker.opentrackr.org/announce";
    ...
    AnnounceResponse resp = client.announce(req);
*/

#include "HttpTransport.hpp"
#include "../phase1/BencodeParser.hpp"

#include <boost/asio/ip/address_v6.hpp>

#include <array>
#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// A single peer from a tracker response: IP address as a string (dotted-quad IPv4, IPv6 literal, or hostname) + port in host byte order.
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
	std::optional<std::string> failure_reason; // set when the tracker reports a "failure reason"
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
		*/
		std::string build_announce_url(const AnnounceRequest& req) const
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
		Parse a raw bencoded tracker response body into an AnnounceResponse:
		    - the entire body must be consumed (no trailing bytes after the top-level value)
		    - "failure reason" is checked first and surfaced in AnnounceResponse.failure_reason
		    - "interval" (optional integer)
		    - "peers" (optional): compact byte string (length must be a multiple of 6, each group of 6 bytes is
		      4-byte big-endian IPv4 + 2-byte big-endian port) or a list of dictionaries with required
		      "ip" (byte string) and "port" (integer in [0, 65535]); "peer id" is ignored
		    - "peers6" (optional, BEP-7): compact byte string, length must be a multiple of 18; each group
		      of 18 bytes is a 16-byte IPv6 address + 2-byte big-endian port (address formatted canonically)
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
				throw std::invalid_argument("malformed tracker response: " + std::to_string(input.size() - value.raw.size()) + " trailing byte(s) after top-level value");
			}

			if (!value.is_dict())
			{
				throw std::invalid_argument("malformed tracker response: expected a bencoded dictionary");
			}

			const auto& dict = value.get_dict();
			AnnounceResponse resp;

			// 1) "failure reason" first: surface it cleanly and stop here.
			if (const BencodeValue * fr = dict_get(dict, "failure reason"))
			{
				if (!fr->is_bytes())
				{
					throw std::invalid_argument("malformed tracker response: \"failure reason\" is not a byte string");
				}

				const auto& raw = fr->get_bytes();
				resp.failure_reason = std::string(reinterpret_cast<const char*>(raw.data()), raw.size());
				return resp;
			}

			// 2) "interval" (optional, must be an integer when present).
			if (const BencodeValue * iv = dict_get(dict, "interval"))
			{
				if (!iv->is_int())
				{
					throw std::invalid_argument("malformed tracker response: \"interval\" is not an integer");
				}

				resp.interval = iv->get_int();
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
						throw std::invalid_argument("malformed tracker response: compact peers length " + std::to_string(raw.size()) + " is not a multiple of 6");
					}

					resp.peers.reserve(raw.size() / 6);

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

					resp.peers.reserve(list.size());

					for (const BencodeValue & entry : list)
					{
						if (!entry.is_dict())
						{
							throw std::invalid_argument("malformed tracker response: \"peers\" list element is not a dictionary");
						}

						const auto& d = entry.get_dict();

						const BencodeValue * ip = dict_get(d, "ip");

						if (ip == nullptr || !ip->is_bytes())
						{
							throw std::invalid_argument("malformed tracker response: \"peers\" list element is missing a byte-string \"ip\"");
						}

						const auto& raw_ip = ip->get_bytes();

						const BencodeValue * port_v = dict_get(d, "port");

						if (port_v == nullptr || !port_v->is_int())
						{
							throw std::invalid_argument("malformed tracker response: \"peers\" list element is missing an integer \"port\"");
						}

						const int64_t port = port_v->get_int();

						if (port < 0 || port > 65535)
						{
							throw std::invalid_argument("malformed tracker response: \"peers\" list element has out-of-range port " + std::to_string(port));
						}

						resp.peers.push_back(Peer{std::string(reinterpret_cast<const char*>(raw_ip.data()), raw_ip.size()), static_cast<uint16_t>(port)});
					}
				}
				else
				{
					throw std::invalid_argument("malformed tracker response: \"peers\" is neither a byte string nor a list");
				}
			}

			// 4) "peers6" (optional, BEP-7): compact byte string of 18-byte IPv6+port groups.
			if (const BencodeValue * p6 = dict_get(dict, "peers6"))
			{
				if (!p6->is_bytes())
				{
					throw std::invalid_argument("malformed tracker response: \"peers6\" is not a byte string");
				}

				const auto& raw = p6->get_bytes();

				if (raw.size() % 18 != 0)
				{
					throw std::invalid_argument("malformed tracker response: peers6 length " + std::to_string(raw.size()) + " is not a multiple of 18");
				}

				resp.peers.reserve(resp.peers.size() + raw.size() / 18);

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
				throw std::runtime_error("tracker returned HTTP " + std::to_string(http_resp.status_code) + " for " + url);
			}

			return parse_response(http_resp.body);
		}

		// Asynchronous announce on a caller-owned io_context: builds the URL, performs one async GET via HttpTransport,
		// requires HTTP 200 and parses the body. on_complete is invoked at most once with a non-null exception_ptr on any
		// failure (transport error, non-200 status, or malformed bencode) and the parsed AnnounceResponse on success.
		void async_announce(boost::asio::io_context& io, const AnnounceRequest& req, std::function<void(std::exception_ptr, AnnounceResponse)> on_complete) const
		{
			const std::string url = build_announce_url(req);

			m_http.async_get(io, url, [on_complete = std::move(on_complete), url](std::exception_ptr ex, HttpResponse http_resp)
			{
				if (ex)
				{
					on_complete(std::move(ex), AnnounceResponse{});
					return;
				}

				if (http_resp.status_code != 200)
				{
					on_complete(std::make_exception_ptr(std::runtime_error("tracker returned HTTP " + std::to_string(http_resp.status_code) + " for " + url)), AnnounceResponse{});
					return;
				}

				try
				{
					on_complete(nullptr, parse_response(http_resp.body));
				}
				catch (...)
				{
					on_complete(std::current_exception(), AnnounceResponse{});
				}
			});
		}

	private:
		HttpTransport m_http;
};

#endif // TRACKER_CLIENT_HPP
