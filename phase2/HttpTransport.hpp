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
      guard. Non-unix platforms currently have no such bounds (L3).

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
		throw std::runtime_error("#2af545: malformed HTTP status line: " + status_line);
	}

	const auto sp1 = status_line.find(' ');

	if (sp1 == std::string::npos)
	{
		throw std::runtime_error("#e79bc8: malformed HTTP status line: " + status_line);
	}

	const std::string after_version = status_line.substr(sp1 + 1); // e.g. "200 OK" or just "200"
	std::size_t parsed = 0;

	try
	{
		resp.status_code = static_cast<int>(std::stoul(after_version, &parsed));
	}
	catch (const std::exception&)
	{
		throw std::runtime_error("#8ffb68: malformed HTTP status line: " + status_line);
	}

	if (parsed == 0)
	{
		throw std::runtime_error("#1196a5: malformed HTTP status line: " + status_line);
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
	if (const auto cl = content_length_of_head(head); cl && resp.body.size() > *cl)
	{
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

#ifdef __unix__
		// M3/M2: poll(2)-deadline-bounded recv/send for the non-blocking socket left behind by connect_with_timeout().
		static std::size_t bounded_read(int fd, char* buf, std::size_t len, std::chrono::milliseconds per_op_timeout, const std::chrono::steady_clock::time_point& deadline, boost::system::error_code& ec);
		static bool bounded_write_all(int fd, const char* data, std::size_t len, std::chrono::milliseconds per_op_timeout, const std::chrono::steady_clock::time_point& deadline, boost::system::error_code& ec);
#endif
};

inline HttpResponse HttpTransport::get(const std::string& host, const std::string& port, const std::string& path_and_query) const
{
	// H1: make_request() validates host/port/path (attacker-controlled via the .torrent) and throws on any
	// byte that could inject headers or split the request. The path is re-derived for error messages below.
	const std::string request = make_request(host, port, path_and_query, m_config.user_agent);
	const std::string path = (path_and_query.empty() || path_and_query.front() != '/') ? "/" + path_and_query : path_and_query;

	// M2: overall wall-clock budget for the whole request. A slow-drip server that keeps every individual read
	// under read_timeout can still pin the read loop indefinitely, so abort once this total time is exceeded.
	const auto deadline = std::chrono::steady_clock::now() + m_config.request_timeout;

	// --- DNS resolve -------------------------------------------------------------
	boost::asio::io_context io; // executor holder only; all operations below are synchronous
	boost::system::error_code ec;
	boost::asio::ip::tcp::resolver resolver(io);
	const auto endpoints = resolver.resolve(host, port, ec);

	if (ec)
	{
		throw std::system_error(ec, "DNS resolve failed for " + host + ":" + port);
	}

	// --- connect: try each resolved endpoint in order (IPv6/IPv4 fallback) -------
	boost::asio::ip::tcp::socket socket(io);
	bool connected = false;
	boost::system::error_code last_ec{boost::asio::error::not_connected};

	for (const auto& ep : endpoints)
	{
		// M2: the whole request is budgeted; don't spend more than that on connect attempts.
		if (std::chrono::steady_clock::now() > deadline)
		{
			throw std::system_error(boost::asio::error::make_error_code(boost::asio::error::timed_out), "request exceeded the total time budget");
		}

		socket.open(ep.endpoint().protocol(), ec);

		if (ec)
		{
			last_ec = ec;
			continue;
		}

		// M3: bound this attempt by both the per-attempt connect timeout and whatever of the overall request
		// budget (M2) is still left; a black-holed SYN must not hang for the OS tcp_syn_retries window. On unix the
		// socket stays non-blocking on success so bounded_read()/bounded_write_all() can enforce their own poll(2)
		// deadlines below (Asio's synchronous recv/send would not honor SO_RCVTIMEO/SO_SNDTIMEO).
		const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
		const auto attempt_timeout = std::min(m_config.connect_timeout, (remaining > std::chrono::milliseconds(0)) ? remaining : std::chrono::milliseconds(0));
		ec = connect_with_timeout(socket, ep.endpoint(), attempt_timeout);

		if (!ec)
		{
			connected = true;
			break;
		}

		last_ec = ec;
		boost::system::error_code ignored;
		socket.close(ignored);
	}

	if (!connected)
	{
		throw std::system_error(last_ec, "connect to " + host + ":" + port + " failed");
	}

	std::string response;
	char buf[8192];
	ResponseScanState scan; // L1: incremental completion-scan state shared by the read loop below

#ifdef __unix__
	// --- send the full request (bounded by poll(2) deadlines, M3/M2) -------------------
	boost::system::error_code send_ec;

	if (!bounded_write_all(socket.native_handle(), request.data(), request.size(), m_config.read_timeout, deadline, send_ec))
	{
		throw std::system_error(send_ec, "send failed for GET " + path);
	}

	// --- read until we have a complete response (each read bounded, M3/M2; L1: incremental scan) --------------
	while (!response_complete(response, m_config.max_response_size, scan))
	{
		boost::system::error_code read_ec;
		const auto n = bounded_read(socket.native_handle(), buf, sizeof(buf), m_config.read_timeout, deadline, read_ec);

		if (read_ec == boost::asio::error::make_error_code(boost::asio::error::eof)) // peer closed the connection: response complete
		{
			break;
		}

		if (read_ec)
		{
			throw std::system_error(read_ec, "read failed for GET " + path);
		}

		response.append(buf, n);

		// M2: the overall budget is also enforced inside bounded_read(); this check keeps the loop self-documenting.
		if (std::chrono::steady_clock::now() > deadline)
		{
			throw std::system_error(boost::asio::error::make_error_code(boost::asio::error::timed_out), "request exceeded the total time budget");
		}
	}

#else
	// L3: non-unix platforms fall back to unbounded blocking Asio IO (addressed separately).
	// --- send the full request ------------------------------------------------------
	boost::asio::write(socket, boost::asio::buffer(request), ec);

	if (ec)
	{
		throw std::system_error(ec, "send failed for GET " + path);
	}

	// --- read until we have a complete response (L1: incremental scan) -------------------
	while (!response_complete(response, m_config.max_response_size, scan))
	{
		const auto n = socket.read_some(boost::asio::buffer(buf), ec);

		if (ec == boost::asio::error::eof) // peer closed the connection: response complete
		{
			break;
		}

		if (ec)
		{
			throw std::system_error(ec, "read failed");
		}

		response.append(buf, n);

		// M2: a slow-drip server that keeps every individual read under read_timeout can still pin this loop
		// indefinitely; abort once the overall request budget is exceeded (complete responses break out above).
		if (std::chrono::steady_clock::now() > deadline)
		{
			throw std::system_error(boost::asio::error::make_error_code(boost::asio::error::timed_out), "request exceeded the total time budget");
		}
	}

#endif

	boost::system::error_code ignored;
	socket.close(ignored); // best-effort, ignore errors

	return parse_http_response(response);
}

// M3: SO_RCVTIMEO/SO_SNDTIMEO do not apply to the connect() handshake — a black-holed SYN (dropped packets)
// hangs for the OS tcp_syn_retries window (~15-75 s on Linux, up to ~127 s with default settings) regardless of
// those options. This helper bounds each attempt with an explicit deadline instead: raw POSIX connect(2) on a
// non-blocking socket (guaranteed to return immediately with EINPROGRESS when the handshake cannot complete at
// once — Asio's synchronous connect() would wait internally for kernel SYN retries, defeating any user-space
// deadline), then poll(2) where completion is signalled by writability plus a clean SO_ERROR. Aborts with
// timed_out once the deadline passes. On success the socket is left in non-blocking mode: bounded_read()/
// bounded_write_all() enforce their own poll(2) deadlines on it (Asio's synchronous recv/send would not honor
// SO_RCVTIMEO/SO_SNDTIMEO — see M3).
inline boost::system::error_code HttpTransport::connect_with_timeout(boost::asio::ip::tcp::socket& socket, const boost::asio::ip::tcp::endpoint& ep, std::chrono::milliseconds timeout)
{
#ifdef __unix__
	const auto deadline = std::chrono::steady_clock::now() + timeout;

	socket.non_blocking(true);

	// Build the target address manually for raw POSIX connect(2).
	sockaddr_storage addr{};
	socklen_t addr_len = 0;

	if (ep.address().is_v4())
	{
		auto* a = reinterpret_cast<sockaddr_in*>(&addr);
		a->sin_family = AF_INET;
		a->sin_port = htons(ep.port());
		a->sin_addr.s_addr = htonl(ep.address().to_v4().to_uint()); // to_uint() is host byte order; s_addr needs network order
		addr_len = static_cast<socklen_t>(sizeof(sockaddr_in));
	}
	else
	{
		auto* a = reinterpret_cast<sockaddr_in6*>(&addr);
		a->sin6_family = AF_INET6;
		a->sin6_port = htons(ep.port());
		std::memcpy(&a->sin6_addr, ep.address().to_v6().to_bytes().data(), 16);
		addr_len = static_cast<socklen_t>(sizeof(sockaddr_in6));
	}

	const int fd = socket.native_handle();
	int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), addr_len);

	if (rc == 0)
	{
		return {}; // connected synchronously; the socket stays non-blocking for bounded_read()/bounded_write_all()
	}

	if (errno != EINPROGRESS && errno != EINTR)
	{
		return boost::system::error_code(errno, boost::system::generic_category()); // e.g. ECONNREFUSED: nothing to wait for
	}

	pollfd pfd{};
	pfd.fd = fd;
	pfd.events = POLLOUT; // a pending non-blocking connect becomes "writable" when it finishes, success or failure

	while (true)
	{
		const auto now = std::chrono::steady_clock::now();

		if (now >= deadline)
		{
			return boost::asio::error::make_error_code(boost::asio::error::timed_out);
		}

		const int remaining_ms = static_cast<int>(std::min<long long>(timeout.count(), std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count()));
		const int prc = ::poll(&pfd, 1, remaining_ms);

		if (prc < 0 && errno != EINTR)
		{
			return boost::system::error_code(errno, boost::system::generic_category()); // poll itself failed
		}

		if (prc == 0)
		{
			continue; // poll slice elapsed: re-check the deadline above and retry with a fresh slice
		}

		int so_error = 0;
		socklen_t len = sizeof(so_error);

		while (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0 && errno == EINTR)
		{
			// interrupted: retry the getsockopt
		}

		if (so_error == 0)
		{
			return {}; // connected; the socket stays non-blocking for bounded_read()/bounded_write_all()
		}

		return boost::system::error_code(so_error, boost::system::generic_category()); // e.g. ECONNREFUSED, ETIMEDOUT
	}

#else
	// L3: non-unix platforms have no poll(2) deadline here (addressed separately): plain synchronous connect,
	// unbounded by Config::connect_timeout. Restore blocking mode so the Asio-based read/write fallback in get() works.
	boost::system::error_code ec;
	socket.connect(ep, ec);

	if (!ec)
	{
		boost::system::error_code nb_ec;
		socket.non_blocking(false, nb_ec);
	}

	return ec;
#endif
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

#ifdef __unix__
// M3/M2: Asio's synchronous recv/send do not honor SO_RCVTIMEO/SO_SNDTIMEO on unix: when a blocking ::recv times
// out it returns EAGAIN, which boost::asio::detail::socket_ops::sync_recv1 treats as "would block" and retries
// after an *infinite* poll_read(-1) — so a silent peer would pin this thread forever. These helpers bound every
// read/write with explicit poll(2) deadlines instead: each operation waits at most min(per_op_timeout, remaining
// request budget), after which it reports timed_out. The socket must be non-blocking (see connect_with_timeout).
inline std::size_t HttpTransport::bounded_read(int fd, char* buf, std::size_t len, std::chrono::milliseconds per_op_timeout, const std::chrono::steady_clock::time_point& deadline, boost::system::error_code& ec)
{
	pollfd pfd{};
	pfd.fd = fd;
	pfd.events = POLLIN;

	for (;;)
	{
		const auto now = std::chrono::steady_clock::now();

		if (now >= deadline)
		{
			ec = boost::asio::error::make_error_code(boost::asio::error::timed_out); // M2: overall request budget exhausted
			return 0;
		}

		const auto wait_cap = std::min(per_op_timeout, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
		const int timeout_ms = static_cast<int>(wait_cap.count());
		const int prc = ::poll(&pfd, 1, timeout_ms);

		if (prc < 0)
		{
			if (errno == EINTR)
			{
				continue; // interrupted: re-check the deadlines and retry
			}

			ec = boost::system::error_code(errno, boost::system::generic_category());
			return 0;
		}

		if (prc == 0)
		{
			// No data within min(per_op_timeout, remaining budget): a silent peer must not pin this thread.
			ec = boost::asio::error::make_error_code(boost::asio::error::timed_out);
			return 0;
		}

		const ssize_t n = ::recv(fd, buf, len, 0); // poll(2) says data (or EOF) is ready on this non-blocking socket

		if (n > 0)
		{
			ec.clear();
			return static_cast<std::size_t>(n);
		}

		if (n == 0)
		{
			ec = boost::asio::error::make_error_code(boost::asio::error::eof); // peer closed: response complete
			return 0;
		}

		if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
		{
			continue; // spurious readiness or interrupted: re-loop under the deadlines above
		}

		ec = boost::system::error_code(errno, boost::system::generic_category());
		return 0;
	}
}

inline bool HttpTransport::bounded_write_all(int fd, const char* data, std::size_t len, std::chrono::milliseconds per_op_timeout, const std::chrono::steady_clock::time_point& deadline, boost::system::error_code& ec)
{
	std::size_t offset = 0;

	while (offset < len)
	{
		const auto now = std::chrono::steady_clock::now();

		if (now >= deadline)
		{
			ec = boost::asio::error::make_error_code(boost::asio::error::timed_out); // M2: overall request budget exhausted
			return false;
		}

		const auto wait_cap = std::min(per_op_timeout, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
		const int timeout_ms = static_cast<int>(wait_cap.count());
		pollfd pfd{};
		pfd.fd = fd;
		pfd.events = POLLOUT;
		const int prc = ::poll(&pfd, 1, timeout_ms);

		if (prc < 0)
		{
			if (errno == EINTR)
			{
				continue; // interrupted: re-check the deadlines and retry
			}

			ec = boost::system::error_code(errno, boost::system::generic_category());
			return false;
		}

		if (prc == 0)
		{
			// Peer's receive window stayed closed for min(per_op_timeout, remaining budget): report a timeout.
			ec = boost::asio::error::make_error_code(boost::asio::error::timed_out);
			return false;
		}

		const ssize_t n = ::send(fd, data + offset, len - offset, MSG_NOSIGNAL); // no SIGPIPE if the peer goes away mid-write

		if (n > 0)
		{
			offset += static_cast<std::size_t>(n);
			continue;
		}

		if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
		{
			continue; // spurious readiness or interrupted: re-loop under the deadlines above
		}

		ec = boost::system::error_code(errno, boost::system::generic_category());
		return false;
	}

	ec.clear();
	return true;
}
#endif // __unix__

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
		throw std::invalid_argument("#89b33e: only http:// URLs are supported: " + url);
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
			throw std::invalid_argument("#3e78d5: malformed IPv6 literal in URL: " + url);
		}

		host = host.substr(1, close_bracket - 1); // strip brackets for resolver.resolve

		if (hostport.size() > close_bracket + 1)
		{
			if (hostport[close_bracket + 1] != ':')
			{
				throw std::invalid_argument("#3c2f78: malformed IPv6 literal in URL: " + url);
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
			async_deliver(session, std::make_exception_ptr(std::system_error(ec, "DNS resolve failed for " + session->host + ":" + session->port)));
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
		async_deliver(session, std::make_exception_ptr(std::system_error(session->last_error, "connect to " + session->host + ":" + session->port + " failed")));
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
