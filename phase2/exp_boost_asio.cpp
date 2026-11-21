// Minimal Boost.Asio experiment for phase 2.
//
// Connects to a public BitTorrent tracker over TCP, sends an HTTP GET
// announce request and prints whatever reply comes back. No bencode
// parsing yet - just validate DNS + connect + send + receive with Asio.
//
// Build: g++ -O0 -g3 -std=c++23 -Wall -Wextra -Wpedantic exp_boost_asio.cpp -o exp_boost_asio.out

#include <boost/asio.hpp>

#include <algorithm>
#include <cctype>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace
{
// Info hash of the Arch Linux torrent from phase 1, so we announce a real torrent.
// Per BEP-3 it must be sent as the raw 20 bytes percent-encoded byte-by-byte (not hex).
constexpr char kInfoHash[] = "%9b%52%12%19%bc%48%55%01%40%a6%66%f7%4a%29%04%8e%d7%8e%93%3b";

std::string build_request(const std::string& host)
{
	const std::string query =
	    std::string("info_hash=") + kInfoHash +
	    "&peer_id=-TT0001-000000000001" +
	    "&port=6881" +
	    "&uploaded=0&downloaded=0&left=5368709120" +
	    "&compact=1&event=started";

	return "GET /announce?" + query + " HTTP/1.1\r\n" +
	       "Host: " + host + "\r\n" +
	       "User-Agent: TorrentTop-Exp/0.1\r\n" +
	       "Connection: close\r\n" +
	       "\r\n";
}

// Returns the expected body size if a Content-Length header is present, else nullopt (read until EOF).
std::optional<std::size_t> parse_content_length(const std::string& head)
{
	std::string lowered = head;
	std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c)
	{
		return static_cast<char>(std::tolower(c));
	});

	const auto pos = lowered.find("content-length:");

	if (pos == std::string::npos)
	{
		return std::nullopt;
	}

	try
	{
		return static_cast<std::size_t>(std::stoul(lowered.substr(pos + 15))); // stoul skips the space after ':'
	}
	catch (const std::exception&)
	{
		return std::nullopt;
	}
}

// A tiny stateful session: one socket, one request, read until done.
class TrackerSession
{
	public:
		explicit TrackerSession(boost::asio::io_context& io) : socket_(io), read_buf_{} {}

		void start(const boost::asio::ip::tcp::resolver::results_type& endpoints, const std::string& request)
		{
			request_ = request;
			boost::asio::async_connect(socket_, endpoints, [this](const boost::system::error_code & ec, const boost::asio::ip::tcp::endpoint&)
			{
				on_connect(ec);
			});
		}

	private:
		void on_connect(const boost::system::error_code& ec)
		{
			if (ec)
			{
				std::cerr << "Connect failed: " << ec.message() << "\n";
				return;
			}

			const auto remote = socket_.remote_endpoint();
			std::cout << "Connected to " << remote.address() << ":" << remote.port() << "\n";

			boost::asio::async_write(socket_, boost::asio::buffer(request_), [this](const boost::system::error_code & err, std::size_t)
			{
				on_write(err);
			});
		}

		void on_write(const boost::system::error_code& ec)
		{
			if (ec)
			{
				std::cerr << "Write failed: " << ec.message() << "\n";
				return;
			}

			std::cout << "--- Sent announce request ---\n" << request_ << "\n";
			socket_.async_read_some(boost::asio::buffer(read_buf_), [this](const boost::system::error_code & err, std::size_t n)
			{
				on_read(err, n);
			});
		}

		void on_read(const boost::system::error_code& ec, std::size_t n)
		{
			if (ec == boost::asio::error::eof) // tracker closed the connection: done
			{
				finish();
				return;
			}

			if (ec)
			{
				std::cerr << "Read failed: " << ec.message() << "\n";
				return;
			}

			response_.append(read_buf_, n);

			const auto head_end = response_.find("\r\n\r\n");

			if (head_end != std::string::npos)
			{
				const auto content_length = parse_content_length(response_.substr(0, head_end));
				const std::size_t body_start = head_end + 4;

				if (content_length && response_.size() >= body_start + *content_length)
				{
					finish(); // got the full body: done
					return;
				}
			}

			socket_.async_read_some(boost::asio::buffer(read_buf_), [this](const boost::system::error_code & ec2, std::size_t n2)
			{
				on_read(ec2, n2);
			});
		}

		void finish()
		{
			std::cout << "\n--- Tracker reply (" << response_.size() << " bytes) ---\n";
			std::cout << response_;

			boost::system::error_code ignored; // best-effort close so nothing lingers if io_context is reused later
			socket_.close(ignored);
		}

		boost::asio::ip::tcp::socket socket_;
		std::string request_;
		std::string response_;
		char read_buf_[4096];
};
}

int main()
{
	const std::string host = "tracker.opentrackr.org";
	const std::string port = "80"; // 6969 is closed from here; the tracker serves HTTP on 80

	boost::asio::io_context io;

	// Step 1: resolve the tracker's address (synchronous is fine for an experiment)
	boost::asio::ip::tcp::resolver resolver(io);

	boost::system::error_code ec;
	auto endpoints = resolver.resolve(host, port, ec);

	if (ec)
	{
		std::cerr << "Resolve failed: " << ec.message() << "\n";
		return 1;
	}

	std::cout << "Resolved " << host << ":" << port << " -> ";

	for (const auto& ep : endpoints)
	{
		std::cout << ep.endpoint().address() << ", ";
	}

	std::cout << "\n";

	// Steps 2-4: try each resolved endpoint in order (IPv6/IPv4 fallback), send the announce request, read until done.
	TrackerSession session(io);
	session.start(endpoints, build_request(host));

	io.run();
}
