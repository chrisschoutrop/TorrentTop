# Security Review — `phase1/BencodeParser.hpp`

**Threat model:** BitTorrent inputs are untrusted by design. The attacker controls:
- the `.torrent` file (every byte of it),
- every bencoded payload delivered by peers or trackers,
- the size and shape of nested structures (`l…e`, `d…e`) within those payloads.

Memory-safety review found **no out-of-bounds reads/writes, no integer overflows, and no infinite loops** (all index arithmetic is bounds-checked; declared lengths are validated before allocation; every parse loop consumes ≥ 1 byte or throws). The remaining risks are a memory-amplification DoS vector, a canonicalization gap in integer parsing, and one lifetime footgun.

---

## Medium severity

### M1. Memory amplification: no cap on node count / total parsed size
- **Where:** [`parse_list()`](phase1/BencodeParser.hpp:271) loop at [lines 281–284](phase1/BencodeParser.hpp:281); [`parse_dict()`](phase1/BencodeParser.hpp:289) loop at [lines 299–316](phase1/BencodeParser.hpp:299); per-string allocation at [line 261](phase1/BencodeParser.hpp:261).
- **Problem:** There is no limit on the number of parsed values. Every input byte can spawn a `BencodeValue` node — a `std::variant` holding a `vector<byte>`, nested `vector<BencodeValue>` and `std::map<Bytes, BencodeValue>` (≈ 60–100 bytes each) plus per-node heap overhead. The worst-case encoding is a list of empty strings:
  ```
  l0:0:0:...0:e     →  ~2 input bytes per node
  ```
  giving roughly **30–50× heap amplification** over the input size, with one allocation per node (allocator pressure on top). A 10 MB payload can force several hundred MB of live memory; an uncapped file read scales linearly to GBs.
- **Attack:** Malicious `.torrent` or tracker/peer response sized just under any upstream limit exhausts client memory → OOM kill / swap thrash (DoS). This is the classic bencode DoS shape and is only mitigated today by whatever size cap the *caller* happens to enforce.
- **Fix:** Enforce a hard input-size budget before parsing (caller), and add parser-side limits: max total node count and/or max total bytes materialized, throwing on exceed. Both are cheap counters in `parse_value()`.

### M2. Non-canonical integers accepted (`+` sign)
- **Where:** [`parse_raw_integer()`](phase1/BencodeParser.hpp:322) — [`std::from_chars`](phase1/BencodeParser.hpp:347) accepts an optional `'+'`; the spec checks at [lines 355–371](phase1/BencodeParser.hpp:355) only cover `--`, `-0` and leading zeros.
- **Problem:** `i+5e` and `i+0e` parse successfully, but the bencode decimal form is `[−]?digits` with no plus sign. Two distinct byte sequences now denote the same value — a canonicalization ambiguity that reference implementations (libtorrent et al.) reject.
- **Attack/impact:** Low direct impact since the numeric value is identical; it matters wherever raw encodings are compared, cached, re-encoded, or fed into identity/hashing logic — implementations disagreeing on validity can cause interop mismatches and bypass of "already validated" assumptions downstream.
- **Fix:** Reject `start_ptr[0] == '+'` explicitly alongside the existing `-`/zero checks.

### M3. `raw` span outlives its input buffer → dangling pointer (UB)
- **Where:** [`BencodeValue::raw`](phase1/BencodeParser.hpp:33), assigned at [line 229](phase1/BencodeParser.hpp:229).
- **Problem:** `raw` is a non-owning view into the caller's buffer. Nothing documents or enforces that the input must outlive every `BencodeValue`; if the buffer is freed, moved, or reallocated before `raw` is read, all reads through it are out-of-bounds (silent corruption or crash). This is a latent use-after-free waiting on call-site discipline.
- **Fix:** Document the lifetime contract on both `Parser` and `BencodeValue`, or drop `raw` in favor of owned data / an explicit "view" type that makes the aliasing visible at the API level.

---

## Low severity / hardening notes

### L1. `peek(offset)` integer addition can overflow (API footgun)
[Line 388](phase1/BencodeParser.hpp:388): `int64_t location = offset + m_pos;` — a hostile or buggy caller passing an extreme `offset` overflows signed arithmetic (UB). All internal call sites use the default `0`, so this is not exploitable today, but the public-ish API should clamp or use checked math.

### L2. `get_*()` accessors throw on type mismatch
[Lines 81–96](phase1/BencodeParser.hpp:81): `std::get<T>` throws `std::bad_variant_access` if the variant holds a different alternative. Safe (no UB), but an uncaught throw in a hot parse/dispatch path is a crash vector for malformed-but-valid structures; prefer the null-returning `as_*()` forms at trust boundaries.

### L3. `operator<<` recurses over the entire structure
[Lines 106–142](phase1/BencodeParser.hpp:106): printing an attacker-sized list/dict emits one line per node — O(n) log output (and CPU) from small input, a minor log-flooding/DoS lever if parsed values are ever logged.

### L4. Inconsistent exception taxonomy for "malformed input"
The same class of error throws `std::invalid_argument` ([line 226](phase1/BencodeParser.hpp:226)), `std::runtime_error` ([line 351](phase1/BencodeParser.hpp:351)) and `std::out_of_range` ([line 392](phase1/BencodeParser.hpp:392)). Callers must catch all three (or `std::exception`) to be safe; a single `BencodeError : std::runtime_error` type would make fail-closed handling uniform.

---

## Positive observations (done right)
- **Recursion depth capped at 128** with an RAII [`DepthGuard`](phase1/BencodeParser.hpp:155) — no stack-overflow DoS from deeply nested `l…l…e` / `d…d…e`, and the counter unwinds correctly on exception paths.
- **All buffer accesses are bounds-checked** ([`peek()`](phase1/BencodeParser.hpp:386), [`consume()`](phase1/BencodeParser.hpp:397)); the invariant `m_pos ≤ m_input.size()` is maintained everywhere — no OOB reads/writes found.
- **Declared string lengths are validated before allocation**: negative rejected ([line 250](phase1/BencodeParser.hpp:250)), and length checked against remaining input ([line 256](phase1/BencodeParser.hpp:256)) *before* `Bytes(n)` — a `99999999999999999999:x` payload throws instead of allocating.
- **Integer parsing cannot overflow**: `std::from_chars` reports `result_out_of_range`, which is rejected ([line 349](phase1/BencodeParser.hpp:349)); trailing garbage (`i12ab3e`) fails the `ptr != end_ptr` check — no silent truncation.
- **Canonical integer checks**: empty, leading zeros and `-0` all rejected ([lines 355–371](phase1/BencodeParser.hpp:355)).
- **Dict keys enforced strictly sorted and unique** using unsigned byte-wise comparison via `std::map<std::vector<std::byte>, …>` — spec-correct, no signedness/locale bugs, and a tree map (not hash map) avoids hash-flooding DoS.
- **Guaranteed termination**: every list/dict loop iteration consumes ≥ 1 input byte or throws; empty input fails cleanly via `peek()` rather than crashing.
- **Trailing garbage after the top-level value is rejected** ([lines 192–195](phase1/BencodeParser.hpp:192)) — strict single-value parse, no smuggling of extra values.
