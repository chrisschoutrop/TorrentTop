#ifndef HTTP_TRANSPORT_HPP
#define HTTP_TRANSPORT_HPP

/*
HttpTransport - generic, reusable synchronous HTTP client (phase 2, plan step 1 in phase2.cpp).

Responsibilities (no knowledge of BitTorrent at all):
    - DNS resolve -> connect (try each endpoint) -> send request -> read full response
    - parses the HTTP status line + headers
    - returns body bytes
    - timeouts for connect/read/write via SO_RCVTIMEO/SO_SNDTIMEO, plus a max-response-size guard

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
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>

#ifdef __unix__
#   include <sys/socket.h> // SOL_SOCKET, SO_RCVTIMEO, SO_SNDTIMEO
#   include <sys/time.h>   // timeval
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

timeval to_timeval(std::chrono::milliseconds ms)
{
	const auto secs = std::chrono::duration_cast<std::chrono::seconds>(ms);
	const auto usecs = std::chrono::duration_cast<std::chrono::microseconds>(ms - secs).count();
	return timeval{secs.count(), usecs};
}

// Returns the expected body size if a Content-Length header is present in the head, else nullopt (read until EOF).
std::optional<std::size_t> content_length_of_head(const std::string& head)
{
	const auto lowered = to_lower(head);
	const auto pos = lowered.find("content-length:");

	if (pos == std::string::npos)
	{
		return std::nullopt;
	}

	try
	{
		return static_cast<std::size_t>(std::stoull(trim(lowered.substr(pos + 15))));
	}
	catch (const std::exception&)
	{
		return std::nullopt;
	}
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
		throw std::runtime_error("malformed HTTP response: missing header terminator");
	}

	const std::string head = response.substr(0, head_end);
	HttpResponse resp;
	resp.body = response.substr(head_end + 4);

	// --- status line -----------------------------------------------------
	const auto nl = head.find('\n');

	if (nl == std::string::npos)
	{
		throw std::runtime_error("malformed HTTP response: missing status line");
	}

	const std::string status_line = trim(head.substr(0, nl));

	if (status_line.rfind("HTTP/", 0) != 0)
	{
		throw std::runtime_error("malformed HTTP status line: " + status_line);
	}

	const auto sp1 = status_line.find(' ');

	if (sp1 == std::string::npos)
	{
		throw std::runtime_error("malformed HTTP status line: " + status_line);
	}

	const std::string after_version = status_line.substr(sp1 + 1); // e.g. "200 OK" or just "200"
	std::size_t parsed = 0;

	try
	{
		resp.status_code = static_cast<int>(std::stoul(after_version, &parsed));
	}
	catch (const std::exception&)
	{
		throw std::runtime_error("malformed HTTP status line: " + status_line);
	}

	if (parsed == 0)
	{
		throw std::runtime_error("malformed HTTP status line: " + status_line);
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
			std::chrono::milliseconds connect_timeout{5000}; // also bounds the blocking connect()
			std::chrono::milliseconds read_timeout{10000};   // per socket read, not total
			std::size_t max_response_size = 8 * 1024 * 1024; // hard cap on total response bytes
			std::string user_agent{"TorrentTop/0.1"};
		};

		HttpTransport() : m_config() {}
		explicit HttpTransport(const Config& config) : m_config(config) {}

		// GET path_and_query (e.g. "/announce?info_hash=..") against host:port and read the full response.
		HttpResponse get(const std::string& host, const std::string& port, const std::string& path_and_query) const;

		// Convenience overload for "http://host[:port]/path[?query]" (no https in this basic version).
		HttpResponse get(const std::string& url) const;

	private:
		Config m_config;
};

inline HttpResponse HttpTransport::get(const std::string& host, const std::string& port, const std::string& path_and_query) const
{
	const std::string path = (path_and_query.empty() || path_and_query.front() != '/') ? "/" + path_and_query : path_and_query;

	// --- build the request ------------------------------------------------------
	const std::string request =
	    std::string("GET ") + path + " HTTP/1.1\r\n" +
	    "Host: " + host + "\r\n" +
	    "User-Agent: " + m_config.user_agent + "\r\n" +
	    "Accept: */*\r\n" +
	    "Connection: close\r\n" +
	    "\r\n";

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
		socket.open(ep.endpoint().protocol(), ec);

		if (ec)
		{
			last_ec = ec;
			continue;
		}

#ifdef __unix__
		// SO_RCVTIMEO/SO_SNDTIMEO bound the blocking connect() as well as every read/write below.
		const timeval tv_recv = to_timeval(m_config.read_timeout);
		const timeval tv_send = to_timeval(m_config.connect_timeout);
		::setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv_recv, sizeof(tv_recv));
		::setsockopt(socket.native_handle(), SOL_SOCKET, SO_SNDTIMEO, &tv_send, sizeof(tv_send));
#endif

		socket.connect(ep.endpoint(), ec);

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

	// --- send the full request ------------------------------------------------------
	boost::asio::write(socket, boost::asio::buffer(request), ec);

	if (ec)
	{
		throw std::system_error(ec, "send failed for GET " + path);
	}

	// --- read until we have a complete response ---------------------------------------
	std::string response;
	char buf[8192];

	while (true)
	{
		const auto n = socket.read_some(boost::asio::buffer(buf), ec);

		if (ec)
		{
			throw std::system_error(ec, "read failed");
		}

		if (n == 0) // peer closed the connection: response complete
		{
			break;
		}

		response.append(buf, n);

		if (response.size() > m_config.max_response_size)
		{
			throw std::runtime_error("response exceeds max_response_size (" + std::to_string(m_config.max_response_size) + " bytes)");
		}

		const auto head_end = response.find("\r\n\r\n");

		if (head_end != std::string::npos)
		{
			const auto content_length = content_length_of_head(response.substr(0, head_end));
			const auto body_start = head_end + 4;

			// Done once the declared Content-Length is fully received.
			// Without one (e.g. chunked), we rely on "Connection: close" and read until EOF above.
			if (!content_length || response.size() >= body_start + *content_length)
			{
				break;
			}
		}
	}

	boost::system::error_code ignored;
	socket.close(ignored); // best-effort, ignore errors

	return parse_http_response(response);
}

inline HttpResponse HttpTransport::get(const std::string& url) const
{
	const auto scheme_end = url.find("://");

	if (scheme_end == std::string::npos || to_lower(url.substr(0, scheme_end)) != "http")
	{
		throw std::invalid_argument("only http:// URLs are supported: " + url);
	}

	const std::string rest = url.substr(scheme_end + 3);
	const auto slash = rest.find('/');
	const std::string hostport = (slash == std::string::npos) ? rest : rest.substr(0, slash);
	const std::string path = (slash == std::string::npos) ? "/" : rest.substr(slash);

	std::string host = hostport;
	std::string port = "80"; // default HTTP port

	if (const auto colon = host.rfind(':'); colon != std::string::npos)
	{
		port = host.substr(colon + 1);
		host.resize(colon);
	}

	return get(host, port, path);
}

#endif // HTTP_TRANSPORT_HPP
