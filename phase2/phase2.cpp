#include <iostream>

int main()
{
	std::cout << "phase2.cpp" << std::endl;
}
/*
AI suggested way forward (Qwen ❤️):

┌─────────────────────────────────────────────┐
│  TrackerClient (your domain logic)          │
│  - builds the announce query string         │
│  - knows what "event=started" means         │
│  - decodes the bencoded response → Peers    │
│  - handles tracker-level errors             │
└──────────────────┬──────────────────────────┘
                   │ calls
┌──────────────────▼──────────────────────────┐
│  HttpTransport (generic, reusable)          │
│  - DNS resolve → connect → send → recv      │
│  - parses HTTP status line + headers        │
│  - returns body bytes                       │
│  - no knowledge of BitTorrent at all        │
└─────────────────────────────────────────────┘

src/
  BencodeParser.hpp          (existing)
  HttpTransport.hpp          (new – generic HTTP)
  TrackerClient.hpp          (new – BitTorrent tracker protocol)
  TrackerClient.cpp
  HttpTransport.cpp
tests/
  test_BencodeParser.cpp     (existing)
  test_HttpTransport.cpp     (mock server or local socketpair)
  test_TrackerClient.cpp     (parse a recorded response)
exp/
  exp_read_torrent.cpp       (existing)
  exp_sha_hash.cpp           (existing)
  exp_announce.cpp           (new – hit a real test tracker)

Phase	What to build	Why
1	Synchronous HttpTransport (single GET, read full response)	Get DNS + connect + read working
2	TrackerClient::announce returning parsed peers	End-to-end: one tracker, one request
3	Handle the two peers encodings (raw 6/18-byte list vs. dict list)	Some trackers use one, some the other
4	async version (same API, async_connect / async_read + steady_timer for timeout)	Needed when you have 10 trackers to hit concurrently
5	Multi-tracker: io_context with worker threads, one async request per tracker, collect responses	Real client behaviour
6	Retry logic, tracker fallback, compact vs non-compact peers	Robustness

The exp_announce.cpp would be your first integration test: hit a public test tracker (e.g. http://opentracker.i2p.rocks:6969/announce) with a dummy info hash and print whatever comes back. That validates DNS, HTTP, and bencode decoding in one shot before you build the async machinery.

Start with the synchronous exp_announce.cpp, get a real peer list back from a test tracker, and then you have a working end-to-end path to iterate on.
*/