# 🏗️ Under construction 🏗️
Intention:
- Torrent client with a terminal interface, think of htop but for Torrents
- Practically usable
- Lightweight
- Cool personal project
- Yes I like emojis
- No I'm not an AI

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
