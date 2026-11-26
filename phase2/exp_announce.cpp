// Phase 2 integration experiment (first step of the plan in phase2.cpp):
// hit a tracker through the header-only HttpTransport, silently on success.
// This validates DNS + connect + send + receive + response parsing end-to-end;
// only failures are printed to stderr.
//
// Default target is a public BitTorrent tracker; pass host/port/path to point it anywhere, e.g.:
//   ./exp_announce.out 127.0.0.1 8765 "/announce?info_hash=..."
//
// Build: g++ -O0 -g3 -std=c++23 -Wall -Wextra -Wpedantic exp_announce.cpp -o exp_announce.out

#include "HttpTransport.hpp"

#include <iostream>
#include <string>

namespace
{
// Info hash of the Arch Linux torrent from phase 1, so we announce a real torrent.
// Per BEP-3 it must be sent as the raw 20 bytes percent-encoded byte-by-byte (not hex).
constexpr char kInfoHash[] = "%9b%52%12%19%bc%48%55%01%40%a6%66%f7%4a%29%04%8e%d7%8e%93%3b";

std::string build_announce_path()
{
	return std::string("/announce?info_hash=") + kInfoHash +
	       "&peer_id=-TT0001-000000000001" +
	       "&port=6881" +
	       "&uploaded=0&downloaded=0&left=5368709120" +
	       "&compact=1&event=started";
}

}

int main(int argc, char** argv)
{
	std::string host = "tracker.opentrackr.org";
	std::string port = "80"; // 6969 is closed from here; the tracker serves HTTP on 80
	std::string path = build_announce_path();

	if (argc >= 4)
	{
		host = argv[1];
		port = argv[2];
		path = argv[3];
	}

	HttpTransport http;

	try
	{
		http.get(host, port, path); // exercise the full request/response cycle; silent on success
	}
	catch (const std::exception& e)
	{
		std::cerr << "Request failed: " << e.what() << "\n";
		return 1;
	}

	return 0;
}
