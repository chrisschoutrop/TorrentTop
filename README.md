# 🏗️ Under construction 🏗️
Intention:

- Torrent client with a terminal interface, think of htop but for Torrents
- Practically usable
- Lightweight
- Cool personal project, learn some new stuff; parsing, networking, complex file I/O, handling potentially malicious interactions, practical coding with AI for larger projects.

Ideas:

- Header only, just use ccache and avoid duplication over header/implementation. Or avoid the header/implementation split by using the fancy new modules from C++20.
- Everything formatted with astyle
- Tests for as much as possible (test_*)
- Develop small experiments (exp_*) first and then combine with main code.

# 📝 The plan 📝
## ✅ Phase 1 (Data and parsing)
- ✅ Bencode parsing
- ✅ Compute SHA-1 info hash for the Arch Linux .torrent file

## ✅ Phase 2 (Networking 1)
- ✅ HTTP interface for making GET requests to trackers
- ✅ Implement tracker protocol
- ✅ Peer list parser

## Intermezzo
- Check for security problems
- Move tests to Boost test
- Move from .sh files to CMake

## Phase 3 (Networking 2)
- TCP connections with single peer
- BitTorrent handshake protocol
- Bitfield Exchange
- Request/response loop, request blocks receive data
- Piece assembly

## Phase 4 (Everything else core related)
- File I/O
- Stategy for picking which pieces to download (Start with random, or first available, or least available)
- Whatever we cannot forsee, but is required to make the main functionality work

## Phase 5 (Interface)
- TUI Layout design
- Render loop
