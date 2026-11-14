#include "HttpTransport.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>
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
			test_get_local_server();
			test_get_no_content_length_split_writes();
			test_url_ipv6_and_port();
			test_get_connection_refused();
			test_max_response_size();
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
					std::cout << "#e29e5d: " << inputs_strings[i] << std::endl;
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
					std::cout << "#46188c: " << inputs_strings[i] << std::endl;
				}
				catch(...)
				{
					exceptionThrown = true;
				}

				assert(exceptionThrown);
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
				std::cout << "#e29e5d: connect to " << port << " unexpectedly succeeded" << std::endl;
			}
			catch(const std::system_error& e)
			{
				exceptionThrown = true;
				std::cout << "#0a9e8b: expected failure: " << e.what() << std::endl;
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
				std::cout << "#46188c: response of " << resp.body.size() << " bytes was not rejected" << std::endl;
			}
			catch(const std::runtime_error& e)
			{
				exceptionThrown = true;
				std::cout << "#0a9e8b: expected failure: " << e.what() << std::endl;
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
