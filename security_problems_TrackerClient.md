# Security Review — `phase2/TrackerClient.hpp`

**Threat model:** BitTorrent inputs are untrusted by design. The attacker controls:
- the `.torrent` file (announce URLs, tracker list),
- every byte of a tracker's HTTP response body (`failure reason`, `interval`, `peers`, `peers6`),
- peer-supplied data downstream of the parsed `Peer` list.

All findings below assume a malicious or compromised tracker / torrent author. Memory-safety review found **no out-of-bounds reads/writes** in this file (all index arithmetic is bounds-checked); the risks are DoS, SSRF/local-network exposure, and injection into logs/UI.

---

## High severity

### H1. Memory amplification: no cap on bencode element count or peer list size
- **Where:** [`parse_response()`](phase2/TrackerClient.hpp:222) — compact `peers` reserve at [line 280](phase2/TrackerClient.hpp:280), list-form `reserve(list.size())` at [line 297](phase2/TrackerClient.hpp:297); root cause shared with [`Parser`](phase1/BencodeParser.hpp:145) (no element-count limit).
- **Problem:** The only size bound is `HttpTransport::Config::max_response_size` (default 8 MiB on the wire). But each bencode element becomes a heap object (`std::variant` + `std::span`, ~72–96 B; dict entries add `std::map` nodes and per-key allocations). A response of ~2.7 M tiny integers (`li0ee…e`) or millions of small dict keys expands to **hundreds of MB – GBs** from a few MiB of network data (~30×+ amplification, worse for dicts). `resp.peers.reserve(raw.size() / 6)` can also pre-allocate ~1.4 M `Peer` objects (~45 MB) from one 8 MiB response.
- **Attack:** A single malicious tracker (or a torrent pointing at it) exhausts client memory with a small, cheap-to-send response. Repeated across many torrents/trackers this is a reliable DoS.
- **Fix:** Enforce semantic caps in `parse_response()`: max number of peers per field (e.g., 65 536), max list/dict element count, and/or a total-allocation budget. Consider lowering the default `max_response_size` for tracker responses (real announce replies are typically < 100 KB).

### H2. Unbounded tracker fan-out from attacker-controlled URL list
- **Where:** [`announce_all()`](phase2/TrackerClient.hpp:432) — worker count at [line 467](phase2/TrackerClient.hpp:467), per-URL loop at [lines 449–465](phase2/TrackerClient.hpp:449).
- **Problem:** `announce_urls` comes from the `.torrent` file. There is no cap on how many URLs are announced to, and no deduplication of duplicate URLs. A torrent listing 10 000 (duplicate) tracker entries spawns that many concurrent connections/threads and allocates a result slot per URL → thread/socket exhaustion, connection storm, and amplified memory use.
- **Attack:** Malicious `.torrent` causes resource exhaustion or a self-inflicted DDoS against the listed hosts.
- **Fix:** Cap the number of distinct trackers processed (e.g., 10–25), deduplicate URLs before dispatching, and cap `num_threads` by an absolute constant as well as by URL count.

### H3. SSRF / local-network exposure via attacker-chosen peers and tracker hosts
- **Where:** peer storage at [line 290](phase2/TrackerClient.hpp:290) (compact), [line 331](phase2/TrackerClient.hpp:331) (list-form `ip`), [line 368](phase2/TrackerClient.hpp:368) (`peers6`); tracker host from [`build_announce_url()`](phase2/TrackerClient.hpp:172).
- **Problem:**
  - Compact/list peers can be loopback, private, or link-local addresses (`127.0.0.1`, `10.x`, `192.168.x`, `::1`, `fe80::…`, IPv4-mapped `::ffff:10.0.0.1`). A malicious tracker can steer the client to probe internal services (ports, banners) and infer what is running on the LAN — a classic BitTorrent SSRF vector.
  - List-form `"ip"` values are **arbitrary byte strings with no validation** ([lines 308–315](phase2/TrackerClient.hpp:308)): they may be hostnames, empty strings, or multi-KB blobs. If downstream code performs DNS resolution on them, the attacker controls which names get resolved (DNS-rebinding / internal-IP steering); if they are logged or rendered in a UI, see M4.
  - Tracker URLs themselves can point at internal endpoints (e.g., cloud metadata `169.254.169.254`). This is inherent to BEP-3 but should be an explicit, documented decision rather than an accident.
- **Fix:** Validate list-form `ip` as a real IP literal (reject empty/non-IP), normalize IPv4-mapped forms for dedup, and either filter or explicitly allow private/loopback peers with documentation of the SSRF trade-off.

---

## Medium severity

### M1. No sanity bounds on `interval`
- **Where:** [lines 257–265](phase2/TrackerClient.hpp:257).
- **Problem:** Only a type check is performed. A tracker can return negative values or `9223372036854775807`. Callers that use the value directly for scheduling (e.g., `sleep(interval)`, timer arithmetic) get an infinite stall, immediate re-announce hot loop, or overflow in downstream `int` conversions.
- **Fix:** Clamp/validate to a sane range (BEP-3 intervals are seconds; e.g., reject `< 0` and cap at ~7 days), or document that callers must bound it.

### M2. Port validation is asymmetric / incomplete
- **Where:** compact peers [lines 282–291](phase2/TrackerClient.hpp:282) (no check possible, but port `0` accepted); list-form range check at [lines 326–329](phase2/TrackerClient.hpp:326).
- **Problem:** Port `0` is accepted from both forms and is invalid for connecting; reserved/well-known ports are not filtered. Not exploitable by itself, but malformed peers should be dropped rather than passed to the connection layer where they produce noisy failures or (in other codebases) surprising behavior.
- **Fix:** Reject port `0` uniformly in both forms.

### M3. Attacker-controlled strings embedded in exceptions and logs (log/UI injection)
- **Where:** [`announce()`](phase2/TrackerClient.hpp:376) error at [line 383](phase2/TrackerClient.hpp:383); `failure_reason` stored raw at [line 252](phase2/TrackerClient.hpp:252); list-form `ip` at [line 331](phase2/TrackerClient.hpp:331).
- **Problem:** `announce_url` (from the `.torrent`) and tracker-supplied strings (`failure reason`, `ip`) can contain CRLF, ANSI escapes, or other control bytes. When these exceptions/messages are written to logs, consoles, or UIs unescaped, they enable log forging / terminal escape injection.
- **Fix:** Sanitize/escape when logging; treat all tracker/torrent-derived text as data, never as formatting input.

### M4. `async_announce` user callback throwing → `std::terminate`
- **Where:** [lines 392–419](phase2/TrackerClient.hpp:392), specifically the direct `on_complete(...)` calls at [lines 400, 406, 412, 416](phase2/TrackerClient.hpp:400).
- **Problem:** The callback runs inside a Boost.Asio completion handler. If the user-supplied `on_complete` throws (e.g., allocates and gets `bad_alloc`, or has its own bug), the exception escapes the Asio dispatch path → `std::terminate`. A malicious response that triggers an edge case in downstream handling can therefore crash the whole process rather than degrading gracefully.
- **Fix:** Wrap `on_complete` invocations in `try/catch(...)` (or document the nothrow requirement and enforce it with a static wrapper).

### M5. Privacy: everything on cleartext HTTP, identity-revealing fields sent to attacker-chosen hosts
- **Where:** [`build_announce_url()`](phase2/TrackerClient.hpp:172) — `info_hash`, `peer_id` at [lines 174–175](phase2/TrackerClient.hpp:174); transport in [`HttpTransport`](phase2/HttpTransport.hpp:245).
- **Problem:** Inherent to BEP-3, but worth stating explicitly: the announce query reveals `info_hash` (identifies exactly which content you are downloading) and `peer_id` (client fingerprint + often embedded peer identity) to any host the torrent file names, over unencrypted TCP. A malicious tracker can correlate your activity with your IP.
- **Fix:** No code fix required; document as a known privacy property and avoid adding extra identifying fields beyond BEP-3 minimums.

---

## Low severity / hardening notes

### L1. Redundant (but harmless) trailing-bytes check
[Line 230](phase2/TrackerClient.hpp:230): `Parser::parse()` already rejects unconsumed input ([BencodeParser line 192](phase1/BencodeParser.hpp:192)), so the `value.raw.size() != input.size()` guard can never fire. Keep it as defense-in-depth, but note it is dead code today.

### L2. Deduplication misses equivalent addresses in different forms
[Lines 484–505](phase2/TrackerClient.hpp:484): `seen` compares raw strings, so `"10.0.0.1"`, `"::ffff:a00:1"` (IPv4-mapped), and list-form hostnames resolving to the same address are not deduplicated. Minor amplification of connection attempts; normalize addresses before inserting into `seen`.

### L3. No cap on `failure_reason` / `ip` string length
Both are bounded only by the 8 MiB response cap. A single multi-MiB `failure reason` or `ip` value is legal per current code and will be copied into strings, logs, and UIs. Cap lengths explicitly (e.g., truncate with a marker).

### L4. Self-connection not prevented
A tracker can return the client's own IP/port as a peer; the connection layer will then talk to itself. Harmless but noisy; optionally filter peers matching local addresses if known.

### L5. `percent_encode` / URL building are correct, one caveat
[`percent_encode()`](phase2/TrackerClient.hpp:142) is bounds-safe and RFC 3986-compliant; parameter injection from our side of the query string is not possible (all values numeric or percent-encoded). Caveat: if `announce_url` already contains a query, we append with `&` ([line 207](phase2/TrackerClient.hpp:207)) — an attacker URL could pre-set parameters that appear *before* ours; ensure downstream logic never trusts tracker-supplied query params (it doesn't today).

---

## Summary table

| # | Severity | Issue | Primary location |
|---|----------|-------|------------------|
| H1 | High | Memory amplification via unbounded element/peer counts | `parse_response()` + `Parser` |
| H2 | High | Unbounded/duplicated tracker fan-out from `.torrent` | `announce_all()` |
| H3 | High | SSRF / local-network exposure via peers & tracker hosts | peer parsing, URL building |
| M1 | Medium | Unsanitized `interval` (negative/huge) | [L257](phase2/TrackerClient.hpp:257) |
| M2 | Medium | Port 0 accepted; asymmetric port validation | [L282](phase2/TrackerClient.hpp:282), [L326](phase2/TrackerClient.hpp:326) |
| M3 | Medium | Log/UI injection via attacker strings in exceptions | [L383](phase2/TrackerClient.hpp:383), [L252](phase2/TrackerClient.hpp:252) |
| M4 | Medium | Throwing user callback → `std::terminate` in Asio handler | [L396–417](phase2/TrackerClient.hpp:396) |
| M5 | Medium | Cleartext disclosure of info_hash/peer_id (privacy) | `build_announce_url()` |
| L1–L5 | Low | Dead check, dedup gaps, unbounded strings, self-connect, URL param ordering | various |

**No memory-safety defects found in this file.** The dominant risks are resource-exhaustion DoS (H1, H2) and attacker-directed network exposure (H3); all are fixable with small validation/cap additions in `parse_response()` and `announce_all()`.
