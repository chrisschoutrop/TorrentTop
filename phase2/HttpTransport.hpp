#ifndef HTTP_TRANSPORT_HPP
#define HTTP_TRANSPORT_HPP

/*
HttpTransport - generic, reusable synchronous HTTP client (phase 2, plan step 1 in phase2.cpp).

Responsibilities (no knowledge of BitTorrent at all):
    - DNS resolve -> connect (try each endpoint) -> send request -> read full response
    - parses the HTTP status line + headers
    - returns body bytes
    - on unix, connect/read/write are each bounded by explicit poll(2) deadlines (Config::connect_timeout /
      Config::read_timeout); SO_RCVTIMEO/SO_SNDTIMEO are deliberately not used because Asio's synchronous recv/send
      convert their EAGAIN into an infinite internal poll and would not bound a silent peer. The whole request is
      additionally bounded by Config::request_timeout as an overall wall-clock budget (M2), plus a max-response-size
      guard. On non-unix platforms the same bounds are enforced portably with Asio timers and non-blocking IO (L3):
      connect is driven by async_connect + a timer deadline, and read/write use bounded non-blocking probes.

Header-only: all definitions are inline. Built on Boost.Asio (already used in exp_boost_asio.cpp).

Usage:
    HttpTransport http;
    auto resp = http.get("tracker.opentrackr.org", "80", "/announce?info_hash=...");
    // or with a full URL:
    auto resp2 = http.get("http://example.com/path?query=1");
*/

#include <boost/asio.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <exception>
#include <iostream> // M5: report a swallowed user-callback exception on stderr
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#ifdef __unix__
#   include <arpa/inet.h>  // htons (M3 raw connect address)
#   include <cerrno>       // errno, EINTR, EINPROGRESS (M3 connect deadline)
#   include <poll.h>       // poll(2), pollfd (M3/M2 bounded read/write deadlines)
#   include <sys/socket.h> // sockaddr_storage, recv/send, MSG_NOSIGNAL
#   include <sys/types.h>  // ssize_t
#endif

namespace
{
std::string to_lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c)
	{
		return static_cast<char>(std::tolower(c));
	});
	return s;
}

std::string trim(const std::string& s)
{
	const auto b = s.find_first_not_of(" \t\r\n");

	if (b == std::string::npos)
	{
		return "";
	}

	const auto e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

// Escapes control characters in text before it is embedded in exceptions or logs, so that
// malicious remote strings cannot forge log lines or inject terminal escapes.
std::string sanitize_for_logging(const std::string& s)
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

// Returns the expected body size if a Content-Length header is present in the head, else nullopt (read until EOF).
std::optional<std::size_t> content_length_of_head(const std::string& head)
{
	const auto lowered = to_lower(head);
	std::size_t pos = 0;

	while (true)
	{
		pos = lowered.find("content-length:", pos);

		if (pos == std::string::npos)
		{
			return std::nullopt;
		}

		// Must start a header line, not be part of another name (e.g. "x-content-length:").
		if (pos != 0 && lowered[pos - 1] != '\n')
		{
			pos += 1; // skip this occurrence and keep searching
			continue;
		}

		// Take only that single header line, up to the next \r or \n.
		const auto value_start = pos + 15; // strlen("content-length:")
		auto eol = lowered.find_first_of("\r\n", value_start);

		if (eol == std::string::npos)
		{
			eol = lowered.size();
		}

		const auto val = trim(lowered.substr(value_start, eol - value_start));
		std::size_t idx = 0;

		try
		{
			const auto n = std::stoull(val, &idx);

			if (idx != val.size()) // trailing garbage like "123abc"
			{
				return std::nullopt;
			}

			return static_cast<std::size_t>(n);
		}
		catch (const std::exception&)
		{
			return std::nullopt;
		}
	}
}

// H1: request components come from the .torrent (attacker-controlled) and are spliced verbatim into the
// wire request. Reject anything that could inject headers or split the request: control characters, DEL,
// space and non-ASCII bytes are all forbidden in a path/query by RFC 3986 §2.2 (they must be percent-encoded).
bool is_safe_request_component(const std::string& s)
{
	return std::none_of(s.begin(), s.end(), [](char ch)
	{
		const unsigned char c = static_cast<unsigned char>(ch);
		return c < 0x21 || c > 0x7E; // no control chars, no DEL, no space, ASCII only
	});
}

// H1: host must be a non-empty hostname or IPv6 literal with no whitespace/control bytes (RFC 3986 reg-name /
// IPv6address). Rejecting empty hosts also prevents getaddrinfo from silently resolving to localhost.
bool is_safe_host(const std::string& host)
{
	if (host.empty())
	{
		return false;
	}

	return std::none_of(host.begin(), host.end(), [](char ch)
	{
		const unsigned char c = static_cast<unsigned char>(ch);
		return c < 0x21 || c > 0x7E; // no control chars, no DEL, no space, ASCII only
	});
}

// H1/M4: port must be numeric and in range 1-65535. Digits-only rejects IANA service names so an attacker cannot
// steer the destination port via e.g. ":sqlservr"; the range check additionally rejects "0", "99999" etc., which
// are not valid TCP destination ports (M4).
bool is_safe_port(const std::string& port)
{
	if (port.empty() || port.size() > 5)
	{
		return false;
	}

	int value = 0; // at most 99999: cannot overflow int

	for (char ch : port)
	{
		if (ch < '0' || ch > '9') // digits only
		{
			return false;
		}

		value = value * 10 + (ch - '0');
	}

	return value >= 1 && value <= 65535;
}
}

// Parsed HTTP response: status line, headers (keys lowercased) and the raw body bytes.
struct HttpResponse
{
	int status_code = 0;
	std::string reason_phrase;
	std::map<std::string, std::string> headers; // keys are stored lowercase
	std::string body;

	// Case-insensitive header lookup; returns nullptr if absent.
	const std::string* header(const std::string& name) const
	{
		const auto it = headers.find(to_lower(name));
		return it == headers.end() ? nullptr : &it->second;
	}
};

namespace
{
// Splits a raw "status line + headers + body" response into an HttpResponse.
HttpResponse parse_http_response(const std::string& response)
{
	const auto head_end = response.find("\r\n\r\n");

	if (head_end == std::string::npos)
	{
		throw std::runtime_error("#894d0d: malformed HTTP response: missing header terminator");
	}

	const std::string head = response.substr(0, head_end);
	HttpResponse resp;
	resp.body = response.substr(head_end + 4);

	// --- status line -----------------------------------------------------
	const auto nl = head.find('\n');

	if (nl == std::string::npos)
	{
		throw std::runtime_error("#e2d45c: malformed HTTP response: missing status line");
	}

	const std::string status_line = trim(head.substr(0, nl));

	if (status_line.rfind("HTTP/", 0) != 0)
	{
		throw std::runtime_error("#2af545: malformed HTTP status line: " + sanitize_for_logging(status_line));
	}

	const auto sp1 = status_line.find(' ');

	if (sp1 == std::string::npos)
	{
		throw std::runtime_error("#e79bc8: malformed HTTP status line: " + sanitize_for_logging(status_line));
	}

	const std::string after_version = status_line.substr(sp1 + 1); // e.g. "200 OK" or just "200"
	std::size_t parsed = 0;

	try
	{
		resp.status_code = static_cast<int>(std::stoul(after_version, &parsed));
	}
	catch (const std::exception&)
	{
		throw std::runtime_error("#8ffb68: malformed HTTP status line: " + sanitize_for_logging(status_line));
	}

	if (parsed == 0)
	{
		throw std::runtime_error("#1196a5: malformed HTTP status line: " + sanitize_for_logging(status_line));
	}

	if (parsed < after_version.size())
	{
		resp.reason_phrase = trim(after_version.substr(parsed));
	}

	// --- headers ----------------------------------------------------------
	const std::string headers_part = head.substr(nl + 1);
	std::size_t line_start = 0;

	while (line_start < headers_part.size())
	{
		const auto eol = headers_part.find('\n', line_start);
		const auto len = (eol == std::string::npos) ? headers_part.size() - line_start : eol - line_start;
		const std::string line = trim(headers_part.substr(line_start, len));

		line_start = (eol == std::string::npos) ? headers_part.size() : eol + 1;

		if (line.empty())
		{
			continue; // skip blank lines
		}

		const auto colon = line.find(':');

		if (colon == std::string::npos)
		{
			continue; // skip malformed header lines
		}

		resp.headers[to_lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1)); // last one wins
	}

	// --- reject unsupported framing ------------------------------------------
	if (const auto * te = resp.header("transfer-encoding"); te && to_lower(*te).find("chunked") != std::string::npos)
	{
		throw std::runtime_error("#37a52a: chunked transfer-encoding is not supported");
	}

	// --- body: clamp to Content-Length if declared -------------------------
	// L5: bytes beyond the declared Content-Length are discarded (a malicious server can concatenate a second
	// response after a complete one — response splitting). That is fail-safe for this client, but the tail must not
	// vanish silently: log how many bytes were dropped so stricter reusers of HttpResponse can detect it. Note that
	// with multiple Content-Length headers the FIRST valid one wins (content_length_of_head stops at the first
	// parseable value); a garbage first value degrades to read-until-EOF, which is acceptable here and documented.
	if (const auto cl = content_length_of_head(head); cl && resp.body.size() > *cl)
	{
		std::cerr << "#e5a1c7: response contained " << (resp.body.size() - *cl)
		          << " trailing bytes beyond the declared Content-Length; they were discarded" << std::endl;
		resp.body.resize(*cl);
	}

	return resp;
}
}

class HttpTransport
{
	public:
		struct Config
		{
			std::chrono::milliseconds connect_timeout{5000}; // M3: bounds each connect attempt (poll(2) deadline), never the OS SYN-retry window
			std::chrono::milliseconds read_timeout{10000};   // bounds a single read/write operation, not the whole request
			std::chrono::milliseconds request_timeout{30000}; // M2: overall wall-clock budget for the whole request (resolve+connect+write+read)
			// L2: real tracker announce replies are < 100 KB, so the old 8 MiB default let a single malicious peer — or an
			// uncapped fan-out of them — allocate hundreds of MB from small network input (parse_http_response() transiently
			// doubles that while copying the body). The tracker-sized default below bounds per-response memory; raise it
			// explicitly when larger responses are expected, and cap the number of concurrent in-flight requests at the
			// caller level (this class is stateless across requests and cannot count them itself).
			std::size_t max_response_size = 1 * 1024 * 1024; // hard cap on total response bytes per request
			std::string user_agent{"TorrentTop/0.1"};
		};

		HttpTransport() : m_config() {}
		explicit HttpTransport(const Config& config) : m_config(config) {}

		// M4/SSRF trade-off (documented decision): host/port come from the .torrent (attacker-controlled). This class
		// deliberately does NOT filter resolved addresses to public ranges: BEP-3 tracker URLs are part of the torrent
		// format, and a user who adds a torrent has implicitly accepted its tracker endpoints. What IS enforced here:
		//   - empty hosts are rejected (an empty nodename would make getaddrinfo resolve to localhost);
		//   - ports must be numeric 1-65535 (IANA service names like ":sqlservr" cannot steer the destination port).
		// Callers that need a private/loopback/metadata-range deny-list should filter the resolved endpoints themselves.
		// GET path_and_query (e.g. "/announce?info_hash=..") against host:port and read the full response.
		HttpResponse get(const std::string& host, const std::string& port, const std::string& path_and_query) const;

		// Convenience overload for "http://host[:port]/path[?query]" (no https in this basic version).
		HttpResponse get(const std::string& url) const;

		// Asynchronous GET on a caller-owned io_context (Boost.Asio, callback-based; no threads added here):
		// resolve -> connect (try each endpoint) -> write request -> read full response. The resolve/connect phase is
		// bounded by Config::connect_timeout and the write/read phase by Config::read_timeout (re-armed on every read
		// progress); expiry delivers std::system_error(boost::asio::error::timed_out). In addition, M2: the whole request
		// is bounded by Config::request_timeout as an overall wall-clock budget, so a slow-drip server that keeps each
		// individual read under read_timeout cannot pin the session indefinitely. on_complete is invoked at most once, with
		// a non-null exception_ptr on failure (URL/parse errors included) and the parsed HttpResponse on success.
		void async_get(boost::asio::io_context& io, const std::string& host, const std::string& port, const std::string& path_and_query, std::function<void(std::exception_ptr, HttpResponse)> on_complete) const;

		// Convenience overload for "http://host[:port]/path[?query]"; delegates to the (io, host, port, ...) form.
		void async_get(boost::asio::io_context& io, const std::string& url, std::function<void(std::exception_ptr, HttpResponse)> on_complete) const;

	private:
		Config m_config;

		// L1: incremental scan state for response_complete(): the "\r\n\r\n" search resumes where it left off and the
		// Content-Length is parsed exactly once, so each byte of a large header block is scanned O(1) times instead of
		// being re-scanned from zero on every read chunk (which made completion detection quadratic in head size).
		struct ResponseScanState
		{
			std::size_t search_from = 0; // resume the "\r\n\r\n" search here (a match starting earlier was already ruled out)
			bool head_found = false;
			std::size_t head_end = 0;    // position of "\r\n\r\n", valid once head_found
			std::optional<std::size_t> content_length{}; // parsed once the head is complete (nullopt = absent/malformed)
		};

		// Per-request state shared by all in-flight callbacks so none of them can dangle.
		struct AsyncSession
		{
			explicit AsyncSession(boost::asio::io_context& io) : resolver(io), socket(io), timer(io) {}

			boost::asio::ip::tcp::resolver resolver;
			boost::asio::ip::tcp::socket socket;
			boost::asio::steady_timer timer;

			std::string host; // for error messages only
			std::string port; // for error messages only
			std::string request;
			std::string response;
			ResponseScanState scan; // L1: incremental completion-scan state for this session's response buffer
			char buf[8192] {};
			std::vector<boost::asio::ip::tcp::endpoint> endpoints;
			std::size_t endpoint_index = 0;
			boost::system::error_code last_error{boost::asio::error::not_connected};

			enum class Phase { Resolving, Writing, Reading };
			Phase phase = Phase::Resolving;

			bool completed = false; // at-most-once guard for on_complete
			std::size_t timer_generation = 0; // invalidates stale async_wait handlers after a re-arm

			std::chrono::milliseconds connect_timeout{5000};
			std::chrono::milliseconds read_timeout{10000};
			std::chrono::steady_clock::time_point deadline{}; // M2: overall wall-clock budget for the whole request
			std::size_t max_response_size = 0;

			std::function<void(std::exception_ptr, HttpResponse)> on_complete;
		};

		// Parsed "http://host[:port]/path[?query]" components (no https in this basic version).
		struct UrlParts
		{
			std::string host; // bare hostname or IPv6 literal without brackets
			std::string port; // decimal string, "80" when omitted
			std::string path; // always starts with '/'
		};

		static std::string make_request(const std::string& host, const std::string& port, const std::string& path_and_query, const std::string& user_agent);
		static UrlParts parse_http_url(const std::string& url);
		static void async_deliver(const std::shared_ptr<AsyncSession>& session, std::exception_ptr ex, HttpResponse resp = {});
		static void async_arm_timer(const std::shared_ptr<AsyncSession>& session, std::chrono::milliseconds timeout);
		static void async_start_resolve(const std::shared_ptr<AsyncSession>& session);
		static void async_start_connect(const std::shared_ptr<AsyncSession>& session);
		static void async_start_write(const std::shared_ptr<AsyncSession>& session);
		static void async_start_read(const std::shared_ptr<AsyncSession>& session);
		static void async_finish(const std::shared_ptr<AsyncSession>& session);
		static boost::system::error_code connect_with_timeout(boost::asio::ip::tcp::socket& socket, const boost::asio::ip::tcp::endpoint& ep, std::chrono::milliseconds timeout);

		// Throws once the accumulated response exceeds max_response_size or declares an unsatisfiable Content-Length;
		// returns true when a declared Content-Length has been fully received (a response without one completes at EOF).
		static bool response_complete(const std::string& response, const std::size_t max_response_size, ResponseScanState& scan);

};

inline HttpResponse HttpTransport::get(const std::string& host, const std::string& port, const std::string& path_and_query) const
{
	// L3: use the portable async_get implementation to ensure timeouts are enforced on all platforms.
	boost::asio::io_context io;
	std::exception_ptr ex;
	std::optional<HttpResponse> resp;

	async_get(io, host, port, path_and_query, [&](std::exception_ptr e, HttpResponse r)
	{
		ex = e;
		resp = std::move(r);
	});

	io.run();

	if (ex)
	{
		std::rethrow_exception(ex);
	}

	return std::move(*resp);
}


// Shared by the sync read loops (unix and non-unix) and the async read handler: throws once the accumulated response
// exceeds max_response_size or declares an unsatisfiable Content-Length; returns true when a declared Content-Length
// has been fully received. A response without one is only complete at EOF, which the caller detects.
// L1: scan carries incremental state across calls so each byte of the buffer is scanned O(1) times overall: the
// "\r\n\r\n" search resumes at scan.search_from (a 4-byte match starting before that offset was fully inside an
// earlier buffer and would already have been found, so resuming can never miss one), and the Content-Length is parsed
// exactly once when the head first becomes complete instead of re-copying/re-lowercasing the whole head on every chunk.
inline bool HttpTransport::response_complete(const std::string& response, const std::size_t max_response_size, ResponseScanState& scan)
{
	if (response.size() > max_response_size)
	{
		throw std::runtime_error("#d4211d: response exceeds max_response_size (" + std::to_string(max_response_size) + " bytes)");
	}

	if (!scan.head_found)
	{
		const auto head_end = response.find("\r\n\r\n", scan.search_from);

		if (head_end == std::string::npos)
		{
			// No terminator yet: only the last 3 bytes could be the start of a "\r\n\r\n" that completes in future data.
			scan.search_from = (response.size() > 3) ? response.size() - 3 : 0;
			return false; // header not complete yet
		}

		scan.head_found = true;
		scan.head_end = head_end;
		scan.content_length = content_length_of_head(response.substr(0, head_end)); // parsed exactly once (L1)
	}

	const auto body_start = scan.head_end + 4;

	// M1: a declared Content-Length above the configured cap can never be satisfied within the read budget (and
	// would overflow size_t arithmetic), so reject it up front instead of reading on.
	if (scan.content_length.has_value() && *scan.content_length > max_response_size)
	{
		throw std::runtime_error("#6fff34: Content-Length exceeds max_response_size");
	}

	// Done once the declared Content-Length is fully received. Checked arithmetic: subtracting body_start
	// (guarded by response.size() >= body_start) can never overflow, unlike the old `body_start + *content_length`
	// which wrapped for huge declared lengths and broke out early. Without one we rely on "Connection: close" and
	// keep reading until EOF (bounded by max_response_size).
	return scan.content_length.has_value() && response.size() >= body_start && *scan.content_length <= response.size() - body_start;
}


inline HttpResponse HttpTransport::get(const std::string& url) const
{
	const auto parts = parse_http_url(url);
	return get(parts.host, parts.port, parts.path);
}

// Splits "http://host[:port]/path[?query]" into host/port/path. Throws std::invalid_argument for a non-http
// scheme or a malformed IPv6 literal; the port defaults to "80" and the path always starts with '/'.
inline HttpTransport::UrlParts HttpTransport::parse_http_url(const std::string& url)
{
	const auto scheme_end = url.find("://");

	if (scheme_end == std::string::npos || to_lower(url.substr(0, scheme_end)) != "http")
	{
		throw std::invalid_argument("#89b33e: only http:// URLs are supported: " + sanitize_for_logging(url));
	}

	const std::string rest = url.substr(scheme_end + 3);
	const auto slash = rest.find('/');
	const std::string hostport = (slash == std::string::npos) ? rest : rest.substr(0, slash);
	const std::string path = (slash == std::string::npos) ? "/" : rest.substr(slash);

	std::string host = hostport;
	std::string port = "80"; // default HTTP port

	if (!host.empty() && host.front() == '[')
	{
		// Bracketed IPv6 literal, e.g. "[::1]" or "[2001:db8::1]:6969".
		const auto close_bracket = host.find(']');

		if (close_bracket == std::string::npos)
		{
			throw std::invalid_argument("#3e78d5: malformed IPv6 literal in URL: " + sanitize_for_logging(url));
		}

		host = host.substr(1, close_bracket - 1); // strip brackets for resolver.resolve

		if (hostport.size() > close_bracket + 1)
		{
			if (hostport[close_bracket + 1] != ':')
			{
				throw std::invalid_argument("#3c2f78: malformed IPv6 literal in URL: " + sanitize_for_logging(url));
			}

			port = hostport.substr(close_bracket + 2);
		}
	}
	else if (const auto colon = host.rfind(':'); colon != std::string::npos)
	{
		port = host.substr(colon + 1);
		host.resize(colon);
	}

	return UrlParts{host, port, path};
}

inline std::string HttpTransport::make_request(const std::string& host, const std::string& port, const std::string& path_and_query, const std::string& user_agent)
{
	// H1: host/port/path come from the .torrent (attacker-controlled). Reject anything that could inject
	// headers or split the request before it is spliced into the wire format. The offending value is kept out
	// of the message on purpose: remote text must not be embedded in exceptions verbatim.
	if (!is_safe_host(host))
	{
		throw std::invalid_argument("#4f8e2d: unsafe host in HTTP request");
	}

	if (!is_safe_port(port))
	{
		throw std::invalid_argument("#7c3a91: unsafe port in HTTP request (numeric 1-5 digits only)");
	}

	const std::string path = (path_and_query.empty() || path_and_query.front() != '/') ? "/" + path_and_query : path_and_query;

	if (!is_safe_request_component(path))
	{
		throw std::invalid_argument("#b56d0e: unsafe characters in HTTP request path");
	}

	// Host header per RFC 7230: bracket IPv6 literals and include the port unless it is the default.
	std::string host_header = host;

	if (host.find(':') != std::string::npos) // IPv6 literal such as "::1" or "2001:db8::1"
	{
		host_header = "[" + host + "]";
	}

	if (port != "80")
	{
		host_header += ":" + port;
	}

	return std::string("GET ") + path + " HTTP/1.1\r\n" +
	       "Host: " + host_header + "\r\n" +
	       "User-Agent: " + user_agent + "\r\n" +
	       "Accept: */*\r\n" +
	       "Connection: close\r\n" +
	       "\r\n";
}

inline void HttpTransport::async_deliver(const std::shared_ptr<AsyncSession>& session, std::exception_ptr ex, HttpResponse resp)
{
	if (session->completed)
	{
		return; // on_complete is invoked at most once
	}

	session->completed = true;

	session->resolver.cancel(); // unblock a pending async_resolve so io_context::run() can exit promptly
	session->timer.cancel(); // invalidate any outstanding async_wait (returns the number cancelled)
	boost::system::error_code ignored;
	session->socket.close(ignored); // best-effort, ignore errors

	// M5: on_complete runs inside Asio completion handlers (resolve/connect/write/read/timer). If the user callback
	// throws (bad_alloc, a bug in downstream handling of a malicious response), letting it escape would reach
	// std::terminate and crash the whole process. Swallow it here; the session is kept alive by the handler's own
	// shared_ptr copy until that handler returns.
	try
	{
		session->on_complete(std::move(ex), std::move(resp));
	}
	catch (...)
	{
		std::cerr << "#c7e2f4: HttpTransport async callback threw; exception swallowed" << std::endl;
	}
}

inline void HttpTransport::async_arm_timer(const std::shared_ptr<AsyncSession>& session, std::chrono::milliseconds timeout)
{
	// Cancel any outstanding wait first: steady_timer may accumulate multiple async_wait handlers.
	session->timer.cancel(); // returns the number of cancelled waits; ignore it

	session->timer.expires_after(timeout);
	const std::size_t generation = ++session->timer_generation; // invalidate any stale handler from a previous arm

	session->timer.async_wait([session, generation](const boost::system::error_code & ec)
	{
		if (ec == boost::asio::error::operation_aborted || session->completed || generation != session->timer_generation)
		{
			return; // the timer was re-armed, or the request already completed
		}

		const std::string what = (session->phase == AsyncSession::Phase::Resolving)
		                         ? "async_get timed out during resolve/connect"
		                         : "async_get timed out while writing the request or reading the response";

		async_deliver(session, std::make_exception_ptr(std::system_error(boost::asio::error::make_error_code(boost::asio::error::timed_out), what)));
	});
}

inline void HttpTransport::async_start_resolve(const std::shared_ptr<AsyncSession>& session)
{
	session->phase = AsyncSession::Phase::Resolving;
	async_arm_timer(session, session->connect_timeout); // bounds resolve + connect

	session->resolver.async_resolve(session->host, session->port, [session](const boost::system::error_code & ec, boost::asio::ip::tcp::resolver::results_type results)
	{
		if (ec)
		{
			async_deliver(session, std::make_exception_ptr(std::system_error(ec, "DNS resolve failed for " + sanitize_for_logging(session->host) + ":" + sanitize_for_logging(session->port))));
			return;
		}

		session->endpoints.assign(results.begin(), results.end());
		session->endpoint_index = 0;
		async_start_connect(session);
	});
}

inline void HttpTransport::async_start_connect(const std::shared_ptr<AsyncSession>& session)
{
	if (session->endpoint_index >= session->endpoints.size())
	{
		async_deliver(session, std::make_exception_ptr(std::system_error(session->last_error, "connect to " + sanitize_for_logging(session->host) + ":" + sanitize_for_logging(session->port) + " failed")));
		return;
	}

	const auto endpoint = session->endpoints[session->endpoint_index++];

	boost::system::error_code ec;
	session->socket.close(ec); // drop any socket left over from a previous attempt (ignore errors)
	session->socket.open(endpoint.protocol(), ec);

	if (ec)
	{
		session->last_error = ec;
		async_start_connect(session); // try the next endpoint
		return;
	}

	session->socket.async_connect(endpoint, [session](const boost::system::error_code & connect_ec)
	{
		if (connect_ec)
		{
			session->last_error = connect_ec;
			boost::system::error_code ignored;
			session->socket.close(ignored); // best-effort, ignore errors
			async_start_connect(session); // try the next endpoint
			return;
		}

		session->phase = AsyncSession::Phase::Writing;
		async_start_write(session);
	});
}

inline void HttpTransport::async_start_write(const std::shared_ptr<AsyncSession>& session)
{
	session->phase = AsyncSession::Phase::Writing;
	async_arm_timer(session, session->read_timeout); // bounds write + read (re-armed on every read progress)

	boost::asio::async_write(session->socket, boost::asio::buffer(session->request), [session](const boost::system::error_code & ec, std::size_t bytes_transferred)
	{
		(void)bytes_transferred;

		if (ec)
		{
			async_deliver(session, std::make_exception_ptr(std::system_error(ec, "send failed")));
			return;
		}

		session->phase = AsyncSession::Phase::Reading;
		async_start_read(session);
	});
}

inline void HttpTransport::async_start_read(const std::shared_ptr<AsyncSession>& session)
{
	session->phase = AsyncSession::Phase::Reading;
	async_arm_timer(session, session->read_timeout); // reset on every read progress

	session->socket.async_read_some(boost::asio::buffer(session->buf), [session](const boost::system::error_code & ec, std::size_t n)
	{
		if (session->completed)
		{
			return; // a timeout already delivered the result
		}

		if (ec == boost::asio::error::eof) // peer closed the connection: response complete
		{
			async_finish(session);
			return;
		}

		if (ec)
		{
			async_deliver(session, std::make_exception_ptr(std::system_error(ec, "read failed")));
			return;
		}

		session->response.append(session->buf, n);

		bool complete = false;

		try
		{
			complete = response_complete(session->response, session->max_response_size, session->scan); // L1: incremental scan
		}
		catch (...)
		{
			async_deliver(session, std::current_exception()); // over-size (#d4211d) or unsatisfiable Content-Length (M1, #6fff34)
			return;
		}

		if (complete)
		{
			async_finish(session);
			return;
		}

		// M2: a slow-drip server that keeps every individual read under read_timeout can still pin this loop
		// indefinitely; abort once the overall request budget is exceeded (complete responses finish above).
		if (std::chrono::steady_clock::now() > session->deadline)
		{
			async_deliver(session, std::make_exception_ptr(std::system_error(boost::asio::error::make_error_code(boost::asio::error::timed_out), "request exceeded the total time budget")));
			return;
		}

		async_start_read(session); // read the next chunk (re-arms the timeout)
	});
}

inline void HttpTransport::async_finish(const std::shared_ptr<AsyncSession>& session)
{
	boost::system::error_code ignored;
	session->socket.close(ignored); // best-effort, ignore errors

	try
	{
		async_deliver(session, nullptr, parse_http_response(session->response));
	}
	catch (...)
	{
		async_deliver(session, std::current_exception());
	}
}

inline void HttpTransport::async_get(boost::asio::io_context& io, const std::string& host, const std::string& port, const std::string& path_and_query, std::function<void(std::exception_ptr, HttpResponse)> on_complete) const
{
	auto session = std::make_shared<AsyncSession>(io);

	session->on_complete = std::move(on_complete);
	session->host = host;
	session->port = port;
	session->connect_timeout = m_config.connect_timeout;
	session->read_timeout = m_config.read_timeout;
	session->deadline = std::chrono::steady_clock::now() + m_config.request_timeout; // M2: overall wall-clock budget
	session->max_response_size = m_config.max_response_size;

	try
	{
		session->request = make_request(host, port, path_and_query, m_config.user_agent);
	}
	catch (...)
	{
		async_deliver(session, std::current_exception());
		return;
	}

	async_start_resolve(session);
}

inline void HttpTransport::async_get(boost::asio::io_context& io, const std::string& url, std::function<void(std::exception_ptr, HttpResponse)> on_complete) const
{
	auto session = std::make_shared<AsyncSession>(io);

	session->on_complete = std::move(on_complete);
	session->connect_timeout = m_config.connect_timeout;
	session->read_timeout = m_config.read_timeout;
	session->deadline = std::chrono::steady_clock::now() + m_config.request_timeout; // M2: overall wall-clock budget
	session->max_response_size = m_config.max_response_size;

	try
	{
		const auto parts = parse_http_url(url);
		session->host = parts.host;
		session->port = parts.port;
		session->request = make_request(parts.host, parts.port, parts.path, m_config.user_agent);
	}
	catch (...)
	{
		async_deliver(session, std::current_exception());
		return;
	}

	async_start_resolve(session);
}

#endif // HTTP_TRANSPORT_HPP
