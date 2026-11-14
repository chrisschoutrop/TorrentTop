#ifndef LOCAL_TEST_SERVER_HPP
#define LOCAL_TEST_SERVER_HPP

/*
Shared single-shot local HTTP server for the phase 2 unit tests, so the end-to-end
tests need no external network.

The constructor binds a listening socket (optionally to a specific address), starts
serving exactly one connection in a background thread and captures the raw request
for assertions. The response is written verbatim; an optional delayed_part is sent
as a second write after a short delay so clients must keep reading until EOF.

Header-only, C++23, boost::asio (same dependency as HttpTransport).
*/

#include <boost/asio.hpp>

#include <cassert>
#include <chrono>
#include <string>
#include <thread>

class local_test_server
{
	public:
		explicit local_test_server(const std::string& response, const std::string& delayed_part = "", const std::string& bind_address = "127.0.0.1") : m_response(response), m_delayed_part(delayed_part), m_io(), m_acceptor(m_io)
		{
			boost::system::error_code ec;

			const auto addr = boost::asio::ip::make_address(bind_address, ec);
			assert(!ec);

			m_acceptor.open(addr.is_v6() ? boost::asio::ip::tcp::v6() : boost::asio::ip::tcp::v4(), ec);
			assert(!ec);

			m_acceptor.set_option(boost::asio::socket_base::reuse_address(true));
			m_acceptor.bind(boost::asio::ip::tcp::endpoint(addr, 0), ec);
			assert(!ec);

			m_acceptor.listen(1, ec);
			assert(!ec);

			const auto ep = m_acceptor.local_endpoint(ec);
			assert(!ec);
			m_port = std::to_string(ep.port());

			m_thread = std::thread([this]()
			{
				serve();
			});
		}

		~local_test_server()
		{
			boost::system::error_code ec;
			m_acceptor.close(ec); // unblock a pending accept() so join() cannot hang on test failure

			if (m_thread.joinable())
			{
				m_thread.join();
			}
		}

		const std::string& port() const
		{
			return m_port;
		}

		const std::string& last_request() const
		{
			return m_request;
		}

	private:
		void serve()
		{
			boost::system::error_code ec;
			boost::asio::ip::tcp::socket socket(m_io);

			m_acceptor.accept(socket, ec);

			if (ec)
			{
				return; // acceptor closed in the destructor, or no client: nothing to serve
			}

			std::string request;
			char buf[4096];

			while (request.find("\r\n\r\n") == std::string::npos)
			{
				const auto n = socket.read_some(boost::asio::buffer(buf), ec);

				if (ec || n == 0)
				{
					break;
				}

				request.append(buf, n);
			}

			m_request = request; // capture the raw request for assertions (Host header etc.)

			boost::system::error_code send_ec;
			boost::asio::write(socket, boost::asio::buffer(m_response), send_ec);

			if (!m_delayed_part.empty())
			{
				// Second write after a short delay: the client must keep reading until EOF.
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				boost::system::error_code delayed_ec;
				boost::asio::write(socket, boost::asio::buffer(m_delayed_part), delayed_ec);
			}

			socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
			socket.close(ec);
		}

		std::string m_response;
		std::string m_delayed_part;
		boost::asio::io_context m_io;
		boost::asio::ip::tcp::acceptor m_acceptor;
		std::string m_port;
		std::string m_request;
		std::thread m_thread;
};

#endif // LOCAL_TEST_SERVER_HPP
