# 🏗️ Under construction 🏗️
Intention:

- Torrent client with a terminal interface, think of htop but for Torrents
- Practically usable
- Lightweight
- Cool personal project, learn some new stuff; parsing, networking, complex file I/O
- Yes I like emojis
- No I'm not an AI

Ideas:

- Header only, just use ccache and avoid duplication over header/implementation. Or avoid the header/implementation split by using the fancy new modules from C++20.
- Everything formatted with astyle
- Tests for as much as possible
- Develop something complex that's not im Fortran with 40 years of legacy
- Develop small experiments first and then combine with main code, this way we figure out the implementation without dragging along large codebase
- Do something other than HPC to gain new insights and trickery

# 📝 The plan 📝
## Phase 1 (Data and parsing)
- Bencode parsing
- Load torrent files
- SHA-1 hash for verifying integrity of downloaded pieces

## Phase 2 (Networking 1)
- HTTP interface for making GET requests to trackers
- Implement tracker protocol
- Peer list parser

## Phase 3 (Networking 2)
- TCP connections with single peer
- BitTorrent handshake protocol
- Bitfield Exchange
- Request/response loop, request blocks receive data
- Piece assembly

## Phase 4 (Everything else core related)
- File I/O
- Stategy for picking which pieces to download
- Whatever we cannot forsee, but is required to make the main functionality work

## Phase 5 (Interface)
- TUI Layout design
- Render loop
