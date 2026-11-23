#include "HttpTransport.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <random>
#include <string>
#include <system_error>
#include <thread>

// Minimal single-shot local HTTP server so the end-to-end tests need no external network (shared with test_TrackerClient).
#include "local_test_server.hpp"

class test_HttpTransport
{
	public:
		test_HttpTransport()
		{
			test_parse_http_response();
			test_parse_http_response_invalid();
			test_content_length_of_head();
			test_header_lookup();
			test_url_scheme_validation();
			test_request_component_validation();
			test_get_local_server();
			test_get_no_content_length_split_writes();
			test_url_ipv6_and_port();
			test_get_connection_refused();
			test_max_response_size();
			test_async_get_local_server();
			test_async_get_timeout();
			test_async_get_errors();
			test_parse_random_inputs();
		}

		static void test_parse_http_response()
		{
			/*
			Test inputs:
			    HTTP/1.1 200 OK + headers + exact Content-Length body
			    Body longer than declared Content-Length -> clamped to Content-Length
			    Status line without reason phrase (HTTP/1.1 404)
			    Duplicate header names -> last one wins; keys stored lowercase
			    Malformed header lines (no colon) are skipped
			*/
			{
				std::string response = "HTTP/1.1 200 OK\r\n"
				                       "Content-Type: text/plain\r\n"
				                       "X-Custom-Header: hello world\r\n"
				                       "Content-Length: 5\r\n"
				                       "\r\n"
				                       "hello";

				HttpResponse resp = parse_http_response(response);

				assert(resp.status_code == 200);
				assert(resp.reason_phrase == "OK");
				assert(resp.body == "hello");
				assert(resp.headers.size() == 3);
			}
			{
				std::string response = "HTTP/1.1 200 OK\r\n"
				                       "Content-Length: 5\r\n"
				                       "\r\n"
				                       "helloEXTRA";

				HttpResponse resp = parse_http_response(response);

				assert(resp.status_code == 200);
				assert(resp.body.size() == 5);
				assert(resp.body == "hello");
			}
			{
				std::string response = "HTTP/1.1 404\r\n"
				                       "Content-Length: 0\r\n"
				                       "\r\n";

				HttpResponse resp = parse_http_response(response);

				assert(resp.status_code == 404);
				assert(resp.reason_phrase.empty());
				assert(resp.body.empty());
			}
			{
				std::string response = "HTTP/1.1 200 OK\r\n"
				                       "X-Dup: first\r\n"
				                       "X-Dup: second\r\n"
				                       "Content-Length: 3\r\n"
				                       "\r\n"
				                       "abc";

				HttpResponse resp = parse_http_response(response);

				assert(resp.headers.size() == 2);
				assert(resp.header("x-dup") != nullptr);
				assert(*resp.header("X-DUP") == "second");
			}
			{
				std::string response = "HTTP/1.1 200 OK\r\n"
				                       "Good: value\r\n"
				                       "not a header line\r\n"
				                       "Content-Length: 3\r\n"
				                       "\r\n"
				                       "abc";

				HttpResponse resp = parse_http_response(response);

				assert(resp.headers.size() == 2); // good + content-length; the colon-less line was skipped
				assert(*resp.header("good") == "value");
				assert(*resp.header("content-length") == "3");
				assert(resp.body == "abc");
			}
		}

		static void test_parse_http_response_invalid()
		{
			/*
			    (empty string)              no header terminator at all
			    no terminator at all        missing \r\n\r\n
			    200 OK ...                  status line does not start with HTTP/
			    HTTP/1.1                    status line without a code
			    HTTP/1.1 abc def            non-numeric status code
			    Transfer-Encoding: chunked  unsupported framing -> runtime_error
			*/
			std::vector<std::string> inputs_strings
			{
				"",
				"no terminator at all",
				"200 OK\r\nX-Y: z\r\n\r\nbody",
				"HTTP/1.1\r\nX-Y: z\r\n\r\nb",
				"HTTP/1.1 abc def\r\n\r\nb",
				"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\nhello",
			};

			for (uint64_t i = 0; i < inputs_strings.size(); ++i)
			{
				bool exceptionThrown = false;

				try
				{
					//https://stackoverflow.com/questions/15668309/c-assertion-function-to-check-that-exception-is-thrown
					HttpResponse resp = parse_http_response(inputs_strings[i]);
					std::cout << "#c644a3: " << inputs_strings[i] << std::endl;
				}
				catch(...)
				{
					exceptionThrown = true;
				}

				assert(exceptionThrown);
			}
		}

		static void test_content_length_of_head()
		{
			/*
			    Content-Length: 42        -> 42
			    cOnTeNt-lEnGtH:   7       -> 7 (case-insensitive, whitespace tolerated)
			    Server: test              -> nullopt (no Content-Length header)
			    Content-Length: abc       -> nullopt (non-numeric value)
			*/
			{
				auto cl = content_length_of_head("Content-Length: 42");
				assert(cl.has_value());
				assert(*cl == 42);
			}
			{
				auto cl = content_length_of_head("cOnTeNt-lEnGtH:   7 ");
				assert(cl.has_value());
				assert(*cl == 7);
			}
			{
				auto cl = content_length_of_head("Server: test\r\nX-Y: z");
				assert(!cl.has_value());
			}
			{
				auto cl = content_length_of_head("Content-Length: abc");
				assert(!cl.has_value());
			}
			{
				auto cl = content_length_of_head("X-Content-Length: 10");
				assert(!cl.has_value()); // must match the header name at line start, not as a substring
			}
			{
				auto cl = content_length_of_head("Content-Length: 42abc");
				assert(!cl.has_value()); // trailing garbage -> reject
			}
		}

		static void test_header_lookup()
		{
			HttpResponse resp;
			resp.headers["content-type"] = "text/plain"; // parser stores keys lowercase

			// Case-insensitive lookup against the lowercase key.
			assert(resp.header("CONTENT-TYPE") != nullptr);
			assert(*resp.header("content-type") == "text/plain");

			// Absent header -> nullptr.
			assert(resp.header("X-Missing") == nullptr);
		}

		static void test_url_scheme_validation()
		{
			/*
			    https://example.com/      only http is supported
			    ftp://example.com/        unknown scheme
			    example.com/path          no scheme at all
			    (empty string)            no scheme at all
			*/
			HttpTransport http;

			std::vector<std::string> inputs_strings
			{
				"https://example.com/",
				"ftp://example.com/",
				"example.com/path",
				"",
			};

			for (uint64_t i = 0; i < inputs_strings.size(); ++i)
			{
				bool exceptionThrown = false;

				try
				{
					//https://stackoverflow.com/questions/15668309/c-assertion-function-to-check-that-exception-is-thrown
					HttpResponse resp = http.get(inputs_strings[i]);
					std::cout << "#cea6a9: " << inputs_strings[i] << std::endl;
				}
				catch(...)
				{
					exceptionThrown = true;
				}

				assert(exceptionThrown);
			}
		}

		static void test_request_component_validation()
		{
			/*
			H1: host/port/path come from the .torrent (attacker-controlled) and are spliced into the wire
			request. Any control character, DEL, space or non-ASCII byte in the path, any unsafe host, or a
			non-numeric port must be rejected with std::invalid_argument before any network I/O:
			    "/announce\r\nX-Injected: evil"   CRLF header injection
			    "/a\r\n\r\nGET /b HTTP/1.1"       request smuggling (header terminator)
			    "" host                           empty host would resolve to localhost
			    "bad\thost"                       control character in host
			    port "sqlservr"                   IANA service name must not steer the destination port
			    port "999999"                     more than 5 digits is not a valid port
			*/
			HttpTransport http;

			std::vector<std::string> bad_paths
			{
				"/announce\r\nX-Injected: evil",
				"/a\r\n\r\nGET /b HTTP/1.1",
				"/path with space",
				"/\x01" "control", // split literals: a hex escape would otherwise swallow the following 'c'
				"/del" "\x7f" "byte", // split literals: a hex escape would otherwise swallow the following 'b'
			};

			for (uint64_t i = 0; i < bad_paths.size(); ++i)
			{
				bool exceptionThrown = false;

				try
				{
					HttpResponse resp = http.get("127.0.0.1", "80", bad_paths[i]);
					std::cout << "#a3f7c2: path unexpectedly accepted" << std::endl;
				}
				catch (const std::invalid_argument&)
				{
					exceptionThrown = true;
				}

				assert(exceptionThrown);
			}

			bool exceptionThrown = false;

			try // empty host must be rejected, not silently resolved to localhost
			{
				HttpResponse resp = http.get("", "80", "/");
				std::cout << "#e1b94d: empty host unexpectedly accepted" << std::endl;
			}
			catch (const std::invalid_argument&)
			{
				exceptionThrown = true;
			}

			assert(exceptionThrown);

			exceptionThrown = false;

			try // control character in the host must be rejected
			{
				HttpResponse resp = http.get("bad\thost", "80", "/");
				std::cout << "#7d2c58: unsafe host unexpectedly accepted" << std::endl;
			}
			catch (const std::invalid_argument&)
			{
				exceptionThrown = true;
			}

			assert(exceptionThrown);

			std::vector<std::string> bad_ports{"sqlservr", "999999", "", "80x"};

			for (uint64_t i = 0; i < bad_ports.size(); ++i)
			{
				bool portRejected = false;

				try // IANA service names and malformed ports must be rejected before any network I/O
				{
					HttpResponse resp = http.get("127.0.0.1", bad_ports[i], "/");
					std::cout << "#c84e1f: port unexpectedly accepted" << std::endl;
				}
				catch (const std::invalid_argument&)
				{
					portRejected = true;
				}

				assert(portRejected);
			}

			// The URL overload must reject the same inputs (port parsed from the URL).
			std::vector<std::string> bad_urls
			{
				"http://127.0.0.1:sqlservr/announce",
				"http:///announce", // empty host
			};

			for (uint64_t i = 0; i < bad_urls.size(); ++i)
			{
				bool urlRejected = false;

				try
				{
					HttpResponse resp = http.get(bad_urls[i]);
					std::cout << "#59a3b7: URL unexpectedly accepted" << std::endl;
				}
				catch (const std::invalid_argument&)
				{
					urlRejected = true;
				}

				assert(urlRejected);
			}
		}

		static void test_get_local_server()
		{
			/*
			End-to-end against a local single-shot server (no external network):
			    200 OK + headers + exact Content-Length body
			    No Content-Length: read until the server closes the connection
			    404 Not Found with reason phrase and body
			    URL overload "http://127.0.0.1:<port>/path?query=1"
			*/
			{
				local_test_server server("HTTP/1.1 200 OK\r\n"
				                         "Content-Type: text/plain\r\n"
				                         "X-Server: test\r\n"
				                         "Content-Length: 5\r\n"
				                         "\r\n"
				                         "hello");

				HttpTransport http;
				HttpResponse resp = http.get("127.0.0.1", server.port(), "/announce?info_hash=abc");

				assert(resp.status_code == 200);
				assert(resp.reason_phrase == "OK");
				assert(resp.body == "hello");
				assert(resp.header("Content-Type") != nullptr);
				assert(*resp.header("content-type") == "text/plain");
				assert(resp.header("X-Server") != nullptr);
				assert(*resp.header("x-server") == "test");
			}
			{
				local_test_server server("HTTP/1.0 200 OK\r\n"
				                         "Server: test\r\n"
				                         "\r\n"
				                         "hello world");

				HttpTransport http;
				HttpResponse resp = http.get("127.0.0.1", server.port(), "/");

				assert(resp.status_code == 200);
				assert(resp.body == "hello world");
			}
			{
				local_test_server server("HTTP/1.1 404 Not Found\r\n"
				                         "Content-Length: 9\r\n"
				                         "\r\n"
				                         "not found");

				HttpTransport http;
				HttpResponse resp = http.get("127.0.0.1", server.port(), "/missing");

				assert(resp.status_code == 404);
				assert(resp.reason_phrase == "Not Found");
				assert(resp.body == "not found");
			}
			{
				local_test_server server("HTTP/1.1 200 OK\r\n"
				                         "Content-Length: 2\r\n"
				                         "\r\n"
				                         "ok");

				HttpTransport http;
				const std::string url = "http://127.0.0.1:" + server.port() + "/path?query=1";
				HttpResponse resp = http.get(url);

				assert(resp.status_code == 200);
				assert(resp.body == "ok");
			}
		}

		static void test_get_no_content_length_split_writes()
		{
			/*
			Server without Content-Length sends headers and body in two separate write() calls
			with a 10 ms delay between them: the client must keep reading until EOF (not stop
			after the first read) and return the full body.
			*/
			local_test_server server("HTTP/1.1 200 OK\r\n"
			                         "Server: test\r\n"
			                         "\r\n",
			                         "hello world");

			HttpTransport http;
			HttpResponse resp = http.get("127.0.0.1", server.port(), "/split");

			assert(resp.status_code == 200);
			assert(resp.body == "hello world");
		}

		static void test_url_ipv6_and_port()
		{
			/*
			URL overload with a bracketed IPv6 literal and a non-default port:
			    http://[::1]:<port>/announce  -> host "::1", port <port>, path "/announce"
			The Host header must keep the brackets and include the port (RFC 7230).
			*/
			local_test_server server("HTTP/1.1 200 OK\r\n"
			                         "Content-Length: 2\r\n"
			                         "\r\n"
			                         "ok",
			                         "",
			                         "::1");

			HttpTransport http;
			const std::string url = "http://[::1]:" + server.port() + "/announce";
			HttpResponse resp = http.get(url);

			assert(resp.status_code == 200);
			assert(resp.body == "ok");
			assert(server.last_request().find("Host: [::1]:" + server.port()) != std::string::npos);
		}

		static void test_get_connection_refused()
		{
			/*
			Bind a port to get a free one, release it immediately (inner scope), then expect the
			connect to fail with an exception (no server listening). The acceptor must be closed
			before connecting: while it is open the kernel completes the handshake and the read hangs.
			*/
			std::string port;

			{
				boost::asio::io_context io;
				boost::system::error_code ec;
				boost::asio::ip::tcp::acceptor acceptor(io);

				acceptor.open(boost::asio::ip::tcp::v4(), ec);
				assert(!ec);

				acceptor.set_option(boost::asio::socket_base::reuse_address(true));
				acceptor.listen(1, ec);
				assert(!ec);

				port = std::to_string(acceptor.local_endpoint(ec).port());
				assert(!ec);
			} // acceptor destroyed here: the port is now free -> connect must be refused

			HttpTransport http;

			bool exceptionThrown = false;

			try
			{
				//https://stackoverflow.com/questions/15668309/c-assertion-function-to-check-that-exception-is-thrown
				HttpResponse resp = http.get("127.0.0.1", port, "/");
				std::cout << "#d994f9: connect to " << port << " unexpectedly succeeded" << std::endl;
			}
			catch(const std::system_error& e)
			{
				exceptionThrown = true;
				std::cout << "#6fa1ca: expected failure: " << e.what() << std::endl;
			}

			assert(exceptionThrown);
		}

		static void test_max_response_size()
		{
			/*
			Config.max_response_size = 16, server sends a ~250 byte response ->
			the read loop must throw before the response is complete.
			*/
			HttpTransport::Config config;
			config.max_response_size = 16;
			HttpTransport http(config);

			const std::string big_body(200, 'x');
			local_test_server server("HTTP/1.1 200 OK\r\n"
			                         "Content-Length: " + std::to_string(big_body.size()) + "\r\n"
			                         "\r\n" + big_body);

			bool exceptionThrown = false;

			try
			{
				//https://stackoverflow.com/questions/15668309/c-assertion-function-to-check-that-exception-is-thrown
				HttpResponse resp = http.get("127.0.0.1", server.port(), "/big");
				std::cout << "#fd4b6c: response of " << resp.body.size() << " bytes was not rejected" << std::endl;
			}
			catch(const std::runtime_error& e)
			{
				exceptionThrown = true;
				std::cout << "#5679b1: expected failure: " << e.what() << std::endl;
			}

			assert(exceptionThrown);
		}

		static void test_parse_random_inputs()
		{
			/*
			Idea:
			If we have an encoder too we can encode random data
			and see if we break anything.

			For now just check that it doesn't segfault or similar:
			parse_http_response must either return a valid HttpResponse or throw, never crash.
			*/
			static volatile size_t sink = 0;
			const int64_t N_test = 100;
			std::mt19937 rng(42);
			std::uniform_int_distribution<int64_t> dist_length(0, 500);
			std::uniform_int_distribution<uint8_t> dist_bytes(0x0, 0xff);

			for (int64_t i = 0; i < N_test; ++i)
			{
				std::string input(dist_length(rng), '\0');

				for (size_t j = 0; j < input.size(); j++)
				{
					input[j] = static_cast<char>(dist_bytes(rng));
				}

				try
				{
					HttpResponse resp = parse_http_response(input);
					sink = sink + sizeof(resp) + resp.body.size();
				}
				catch(...)
				{
					;
				}
			}
		}

		static void test_async_get_local_server()
		{
			/*
			async_get against a local single-shot server (no external network):
			    200 OK + Content-Length body -> on_complete fires exactly once with no exception and the parsed response.
			*/
			local_test_server server("HTTP/1.1 200 OK\r\n"
			                         "Content-Type: text/plain\r\n"
			                         "Content-Length: 5\r\n"
			                         "\r\nhello");

			HttpTransport http;
			boost::asio::io_context io;

			std::exception_ptr ex;
			HttpResponse resp;
			int calls = 0;

			http.async_get(io, "127.0.0.1", server.port(), "/announce?info_hash=abc", [&ex, &resp, &calls](std::exception_ptr e, HttpResponse r)
			{
				ex = std::move(e);
				resp = std::move(r);
				calls++;
			});

			io.run(); // drives the async operations to completion (no-op if already done)

			assert(calls == 1);
			assert(ex == nullptr);
			assert(resp.status_code == 200);
			assert(resp.body == "hello");
		}

		static void test_async_get_timeout()
		{
			/*
			The server accepts the connection but sends nothing: async_get must time out (read_timeout) and deliver a
			std::system_error with boost::asio::error::timed_out via the exception_ptr.
			*/
			boost::asio::io_context accept_io;
			boost::system::error_code ec;
			boost::asio::ip::tcp::acceptor acceptor(accept_io);

			acceptor.open(boost::asio::ip::tcp::v4(), ec);
			assert(!ec);

			acceptor.set_option(boost::asio::socket_base::reuse_address(true));
			acceptor.listen(1, ec);
			assert(!ec);

			const std::string port = std::to_string(acceptor.local_endpoint(ec).port());
			assert(!ec);

			std::thread holder([&acceptor, &accept_io]()
			{
				boost::asio::ip::tcp::socket peer(accept_io);
				boost::system::error_code accept_ec;
				acceptor.accept(peer, accept_ec); // hold the connection open without sending anything

				if (!accept_ec)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(500));
				}
			});

			HttpTransport::Config config;
			config.read_timeout = std::chrono::milliseconds(50); // short: the timeout must fire well before the holder gives up
			HttpTransport http(config);

			boost::asio::io_context io;
			std::exception_ptr ex;
			int calls = 0;

			http.async_get(io, "127.0.0.1", port, "/", [&ex, &calls](std::exception_ptr e, HttpResponse)
			{
				ex = std::move(e);
				calls++;
			});

			io.run(); // must return once the timeout delivers the result
			holder.join();

			assert(calls == 1);
			assert(ex != nullptr);

			bool timedOut = false;

			try
			{
				std::rethrow_exception(ex);
			}
			catch (const std::system_error& e)
			{
				timedOut = (e.code() == boost::asio::error::make_error_code(boost::asio::error::timed_out));
				std::cout << "#3ebb53: expected timeout: " << e.what() << std::endl;
			}

			assert(timedOut);
		}

		static void test_async_get_errors()
		{
			/*
			Error propagation through the callback (no throw):
			    - invalid URL scheme -> on_complete fires synchronously with std::invalid_argument
			    - connection refused  -> on_complete fires after io.run() with std::system_error
			*/
			HttpTransport http;

			// (a) invalid URL: delivered to the callback before any I/O is started
			boost::asio::io_context io_invalid;
			std::exception_ptr ex_url;
			int calls = 0;

			http.async_get(io_invalid, "https://example.com/", [&ex_url, &calls](std::exception_ptr e, HttpResponse)
			{
				ex_url = std::move(e);
				calls++;
			});

			io_invalid.run(); // no-op: the callback already fired synchronously

			assert(calls == 1);
			assert(ex_url != nullptr);

			bool invalidArgument = false;

			try
			{
				std::rethrow_exception(ex_url);
			}
			catch (const std::invalid_argument& e)
			{
				invalidArgument = true;
				std::cout << "#3b4e26: expected failure: " << e.what() << std::endl;
			}

			assert(invalidArgument);

			// (b) connection refused: bind a port to get a free one, release it, then expect the connect to fail
			std::string port;

			{
				boost::asio::io_context io;
				boost::system::error_code ec;
				boost::asio::ip::tcp::acceptor acceptor(io);

				acceptor.open(boost::asio::ip::tcp::v4(), ec);
				assert(!ec);

				acceptor.set_option(boost::asio::socket_base::reuse_address(true));
				acceptor.listen(1, ec);
				assert(!ec);

				port = std::to_string(acceptor.local_endpoint(ec).port());
				assert(!ec);
			} // acceptor destroyed here: the port is now free -> connect must be refused

			boost::asio::io_context io_refused;
			std::exception_ptr ex_refused;
			calls = 0;

			http.async_get(io_refused, "127.0.0.1", port, "/", [&ex_refused, &calls](std::exception_ptr e, HttpResponse)
			{
				ex_refused = std::move(e);
				calls++;
			});

			io_refused.run();

			assert(calls == 1);
			assert(ex_refused != nullptr);

			bool systemError = false;

			try
			{
				std::rethrow_exception(ex_refused);
			}
			catch (const std::system_error& e)
			{
				systemError = true;
				std::cout << "#0dfc28: expected failure: " << e.what() << std::endl;
			}

			assert(systemError);
		}


};



#ifndef FUZZER
int main()
{
	std::cout << "test_HttpTransport.cpp" << std::endl;
	test_HttpTransport test;
	return 0;
}
#else
// fuzz_target.cc
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size)
{
	try
	{
		std::string input(reinterpret_cast<const char*>(Data), Size);
		HttpResponse resp = parse_http_response(input);
		volatile auto sink = resp.body.size();
	}
	catch(...)
	{

	}

	return 0;
}
#endif
