# Security Review — `phase2/HttpTransport.hpp`

**Threat model:** BitTorrent inputs are untrusted by design. The attacker controls:
- the `.torrent` file, including every announce URL (host, port, path/query) that reaches [`get(url)`](phase2/HttpTransport.hpp:468) / [`async_get`](phase2/HttpTransport.hpp:756),
- every byte of the tracker's HTTP response (status line, headers, body),
- DNS answers for attacker-chosen hostnames.

Memory-safety review found **no out-of-bounds reads/writes** (all index arithmetic is bounds-checked; `stoul`/`stoull` overflows are caught). The risks are protocol injection, integer overflow in framing logic, and several DoS vectors.

---

## High severity

### H1. CRLF / control-character injection into the request → HTTP header injection & request smuggling
- **Where:** [`make_request()`](phase2/HttpTransport.hpp:524) — path spliced raw at [line 541](phase2/HttpTransport.hpp:541); inline copy in [`get()`](phase2/HttpTransport.hpp:346). No validation anywhere; the only check is `front() == '/'` ([line 526](phase2/HttpTransport.hpp:526)).
- **Problem:** `path_and_query` comes from the torrent's announce URL and is inserted verbatim into the request line. A path containing CR/LF injects arbitrary headers or a second request, e.g. announce URL `http://tracker.example/announce\r\nX-Injected: evil` produces:
  ```
  GET /announce
  X-Injected: evil HTTP/1.1
  Host: tracker.example
  ...
  ```
  A path containing `\r\n\r\n` can terminate the header section and smuggle a full second request (delimiter-based smuggling). RFC 3986 §2.2 forbids unescaped control characters in URLs; they must be rejected or percent-encoded.
- **Attack:** Malicious `.torrent` steers what the tracker (or any intermediate proxy) sees — header injection (`X-Forwarded-For`, etc.), request splitting, filter bypass. Note: CRLF via *host/port* is mostly fail-closed because `getaddrinfo` rejects such names, but the **path** path is fully exploitable and is exactly what `TrackerClient::announce()` uses.
- **Fix:** In `make_request()` (and before calling it), reject or percent-encode any byte `< 0x20`, `0x7F`, or space in `path_and_query`; validate `host` against a hostname/IPv6-literal grammar and `port` as 1–5 digits.

---

## Medium severity

### M1. Integer overflow in the Content-Length completion check → silently truncated body
- **Where:** [`get()`](phase2/HttpTransport.hpp:455) — `response.size() >= body_start + *content_length`; identical at [line 706](phase2/HttpTransport.hpp:706) (async). CL parsing at [lines 109–116](phase2/HttpTransport.hpp:109).
- **Problem:** `std::stoull` accepts values up to `ULLONG_MAX`, so a tracker can declare `Content-Length: 18446744073709551615`. `body_start + *content_length` then wraps around `size_t`, the comparison becomes true immediately, and both read loops **break with an incomplete body**. [`parse_http_response()`](phase2/HttpTransport.hpp:236) only clamps when the body is *larger* than CL, so a 200 response with a truncated/empty body is returned as if complete.
- **Attack:** Malicious tracker delivers a valid-looking head plus a few bytes and claims a huge CL; downstream code (bencode parser, UI) operates on silently truncated data — inconsistent state, or mis-parsing when combined with other fields.
- **Fix:** Use checked arithmetic: `if (*content_length <= response.size() - body_start)` after ensuring `response.size() >= body_start`, and/or reject any CL `> max_response_size` up front (a tracker reply can never legitimately exceed the cap).

### M2. Slowloris-style unbounded total read time
- **Where:** sync loop [lines 425–460](phase2/HttpTransport.hpp:425); async [`async_start_read()`](phase2/HttpTransport.hpp:665) with timer re-arm at [line 713](phase2/HttpTransport.hpp:713).
- **Problem:** Timeouts are *per-read* (`SO_RCVTIMEO` / `read_timeout`), and the only total bound is `max_response_size`. A malicious tracker that sends one byte just under every timeout keeps each read "in progress" indefinitely — up to 8 MiB of bytes can be trickled over **days or weeks**, pinning a worker thread (sync) or an `AsyncSession` with its socket/timer (async) the entire time.
- **Attack:** Resource-exhaustion DoS: many torrents × slow-drip trackers exhaust threads/sessions/fds without ever tripping a timeout.
- **Fix:** Add an overall deadline per request (e.g., total wall-clock budget for resolve+write+read), and/or a minimum throughput check (bytes-per-time window) that aborts stalled transfers.

### M3. `connect()` is not actually bounded by the socket timeouts (comment at line 381 is wrong)
- **Where:** [lines 380–398](phase2/HttpTransport.hpp:380), blocking [`socket.connect()`](phase2/HttpTransport.hpp:388).
- **Problem:** `SO_RCVTIMEO`/`SO_SNDTIMEO` apply to recv/send syscalls, **not** to a blocking `connect()`. A SYN to a blackholed IP hangs for the kernel's TCP SYN-retry timeout (typically ~75 s on Linux) per endpoint. The loop tries every resolved endpoint sequentially ([lines 370–406](phase2/HttpTransport.hpp:370)), and an attacker-controlled hostname can return many A/AAAA records — worst case is *N × ~75 s of blocking* for a single `get()` call (tens of minutes).
- **Attack:** Torrent pointing at a domain with many blackholed IPs stalls the client's thread for a long, unbounded-by-config time.
- **Fix:** Use non-blocking connect + timer (as the async path already does correctly via [`async_arm_timer()`](phase2/HttpTransport.hpp:566)), or `socket.connect` with an explicit deadline; fix the misleading comment.

### M4. SSRF / local-network exposure via attacker-chosen host, empty host, and service-name ports
- **Where:** [`resolver.resolve(host, port)`](phase2/HttpTransport.hpp:358); [`parse_http_url()`](phase2/HttpTransport.hpp:476) — no empty-host check ([lines 485–491](phase2/HttpTransport.hpp:485)), non-numeric port passed through at [line 512](phase2/HttpTransport.hpp:512) and [lines 515–519](phase2/HttpTransport.hpp:515).
- **Problem:**
  - Hostnames come from the `.torrent`; resolved IPs are never filtered, so a tracker URL can steer connections to `127.0.0.1`, RFC1918 services, or cloud metadata (`169.254.169.254`). Inherent to BEP-3, but should be an explicit documented decision.
  - **Empty host** (`http:///announce`): `getaddrinfo` with an empty nodename resolves to a *local* address — the client silently talks to localhost:80. Reject empty hosts explicitly.
  - **Non-numeric ports are passed to `getaddrinfo` as IANA service names**: `http://host:sqlservr/...` connects to port 1433, `:mysql` → 3306, etc. An attacker can pick any registered service name to steer the destination port beyond what a "port" field suggests.
- **Fix:** Reject empty hosts; validate ports as digits in 1–65535 (or an explicit allow-list of service names); document the private/loopback SSRF trade-off.

### M5. User callback throwing inside an Asio handler → `std::terminate`
- **Where:** [`async_deliver()`](phase2/HttpTransport.hpp:549) — direct `session->on_complete(...)` call at [line 563](phase2/HttpTransport.hpp:563).
- **Problem:** `on_complete` runs inside Boost.Asio completion handlers (timer, resolve, connect, read). If the user-supplied callback throws (`bad_alloc`, a bug in downstream handling of a malicious response), the exception escapes the Asio dispatch path → `std::terminate`. A crafted tracker response that triggers an edge case downstream can crash the whole process.
- **Fix:** Wrap the `on_complete` invocation in `try { ... } catch (...) { /* log, swallow */ }`, or document/enforce a nothrow requirement on callbacks.

---

## Low severity / hardening notes

### L1. O(n²) rescanning of the growing response buffer (CPU DoS)
[Line 446](phase2/HttpTransport.hpp:446) and [line 697](phase2/HttpTransport.hpp:697): `response.find("\r\n\r\n")` re-scans the *entire* accumulated buffer on every 8 KiB chunk — total cost ≈ N²/16K byte operations (seconds of CPU for an 8 MiB response), and once the head is found, [line 450](phase2/HttpTransport.hpp:450) additionally does `substr` + full-head lowercasing per iteration. With unbounded tracker fan-out this becomes a parallel CPU-exhaustion vector. Track a single search offset (and cache the parsed CL) instead of rescanning from zero.

### L2. Memory amplification
[`max_response_size`](phase2/HttpTransport.hpp:252) defaults to 8 MiB *per response*, and [`parse_http_response()`](phase2/HttpTransport.hpp:156) copies the body into a second string (≈2× transient). Combined with an uncapped number of announce URLs, this is a reliable multi-hundred-MB allocation from small network input. Lower the default for tracker traffic (real announce replies are < 100 KB) and/or cap concurrent in-flight responses.

### L3. Timeouts silently absent on non-unix platforms
The `SO_RCVTIMEO`/`SO_SNDTIMEO` setup is wrapped in [`#ifdef __unix__`](phase2/HttpTransport.hpp:380). On Windows the synchronous path has **no** read/write timeout at all — a silent peer blocks indefinitely (the async path remains bounded by its timer). Use portable Asio deadlines for both paths.

### L4. Log/UI injection via attacker-controlled strings in exceptions
Status line, reason phrase, host and port are embedded verbatim into exception messages ([lines 170–194](phase2/HttpTransport.hpp:170), [line 362](phase2/HttpTransport.hpp:362), [line 410](phase2/HttpTransport.hpp:410), [line 598](phase2/HttpTransport.hpp:598)). These originate from the network (or the torrent) and may contain CRLF or ANSI escapes → log forging / terminal escape injection when written unescaped. Sanitize at the logging boundary; treat all remote text as data.

### L5. Trailing bytes after Content-Length are silently discarded
[Lines 236–239](phase2/HttpTransport.hpp:236): a malicious server can send a complete response followed by a second concatenated response (response splitting); the tail is dropped without warning. Fail-safe for this client today, but log or assert on unexpected trailing data when reusing `HttpResponse` in stricter contexts. Related: with multiple `Content-Length` headers, the *first* wins and a garbage first value degrades to read-until-EOF ([lines 107–121](phase2/HttpTransport.hpp:107)) — acceptable, but worth documenting.

### L6. Status code cast can wrap negative
[Line 185](phase2/HttpTransport.hpp:185): `static_cast<int>(std::stoul(...))` accepts values up to `ULONG_MAX`; codes > `INT_MAX` wrap to negative. Callers check `== 200` so this is not exploitable today, but validate the range 100–599 and reject otherwise for defense in depth.

### L7. Async session thread-safety is undocumented
[`AsyncSession`](phase2/HttpTransport.hpp:279) state (`response`, `phase`, `completed`, `timer_generation`) has no synchronization; it is only safe if the caller runs its `io_context` on a **single** thread. Document this requirement explicitly in the `async_get` contract to prevent multi-threaded `run()` data races.

---

## Positive observations (done right)
- Hard cap on total response bytes in both sync and async paths — no unbounded memory growth from a single peer.
- Chunked transfer-encoding is rejected rather than mis-parsed ([line 230](phase2/HttpTransport.hpp:230)).
- `on_complete` at-most-once guard plus timer-generation invalidation prevent stale-handler double delivery and use-after-free (all handlers capture `shared_ptr<AsyncSession>`).
- Malformed responses fail closed: missing terminator, bad status line, and non-numeric Content-Length all throw rather than mis-frame.
- `stoul`/`stoull` overflow exceptions are caught; trailing-garbage CL values (`123abc`) are rejected ([line 111](phase2/HttpTransport.hpp:111)).
