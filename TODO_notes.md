# TODO Notes

### 🕒 Checking for security problems

Checking everything in phase 2 is too much for a single prompt. Qwen can't handle that, so we break it down into steps.

✅<prompt>
Can you critically review everything in phase2/TrackerClient.hpp for me? Assume that inputs can be malicious, since this is a BitTorrent program and there is a high risk of malicious input files or peers. Write the potential security problems in markdown to security_problems_TrackerClient.md.
</prompt>

✅<prompt>
Can you critically review everything in phase2/HttpTransport.hpp for me? Assume that inputs can be malicious, since this is a BitTorrent program and there is a high risk of malicious input files or peers. Write the potential security problems in markdown to security_problems_HttpTransport.md.
</prompt>

✅<prompt>
Can you critically review everything in phase1/BencodeParser.hpp for me? Assume that inputs can be malicious, since this is a BitTorrent program and there is a high risk of malicious input files or peers. Write the potential security problems in markdown to security_problems_BencodeParser.md.
</prompt>

❌<prompt>
Can you fix the security problems mentioned in security_problems_TrackerClient.md? Make sure that check.sh all compile_run.sh still pass. 
</prompt>

✅<<prompt>
Can you fix the following security issue layed out in security_problems_TrackerClient.md:

H1. Memory amplification: no cap on bencode element count or peer list size
</prompt>

✅<<prompt>
Lees `security_problems_TrackerClient.md` en `TrackerClient.hpp` / `TrackerClient.cpp`.
Los de onderstaande 5 security-issues **strikt één voor één** op. Doe NOOIT twee fixes tegelijk in één stap.

Werk voor elk punt onderstaande vaste cyclus af voordat je aan het volgende punt begint:
1. Lees de beschrijving van het specifieke issue in `security_problems_TrackerClient.md`.
2. Pas alleen de relevante code in `TrackerClient` (en eventuele unit tests) aan.
3. Draai de build / unit tests om te verifiëren dat alles compileert en slaagt.
4. Vink het punt af in je overzicht en ga pas daarna door naar het volgende punt.

Lijst van op te lossen issues:
- [ ] 1. "Unbounded tracker fan-out from attacker-controlled URL list"
- [ ] 2. "H3. SSRF / local-network exposure via attacker-chosen peers and tracker hosts"
- [ ] 3. "No sanity bounds on `interval`"
- [ ] 4. "`async_announce` user callback throwing → `std::terminate`"
- [ ] 5. "Attacker-controlled strings embedded in exceptions and logs (log/UI injection)"

Na elke stap, draai check.sh en los de problemen op die dit script geeft.

Begin nu uitsluitend met punt 1.
</prompt>

✅<prompt>
Controleer of de onderstaande prompt duidelijk is, stop daarna. De prompt:
```
Lees `security_problems_HttpTransport.md` en `HttpTransport.hpp`.
Los de onderstaande security-issues **strikt één voor één** op. Doe NOOIT twee fixes tegelijk in één stap.

Werk voor elk punt onderstaande vaste cyclus af voordat je aan het volgende punt begint:
1. Lees de beschrijving van het specifieke issue in `security_problems_HttpTransport.md`.
2. Pas alleen de relevante code in `HttpTransport` (en eventuele unit tests) aan.
3. Draai de build / unit tests via check.sh om te verifiëren dat alles compileert en slaagt.
4. Maak een git commit van de aangepaste bestanden, benoem de issue in de commit tekst. GEEN PUSH, ALLEEN COMMIT.
5. Vink het punt af in je overzicht en ga pas daarna door naar het volgende punt.

Lijst van op te lossen issues:
- [ ] "H1. CRLF / control-character injection into the request → HTTP header injection & request smuggling"
- [ ] "M1. Integer overflow in the Content-Length completion check → silently truncated body"
- [ ] "M2. Slowloris-style unbounded total read time"
- [ ] "M3. `connect()` is not actually bounded by the socket timeouts (comment at line 381 is wrong)"
- [ ] "M4. SSRF / local-network exposure via attacker-chosen host, empty host, and service-name ports"
- [ ] "M5. User callback throwing inside an Asio handler → `std::terminate`"
- [ ] "L1. O(n²) rescanning of the growing response buffer (CPU DoS)"
- [ ] "L2. Memory amplification"
- [ ] "L3. Timeouts silently absent on non-unix platforms"
- [ ] "L4. Log/UI injection via attacker-controlled strings in exceptions"
- [ ] "L5. Trailing bytes after Content-Length are silently discarded"
- [ ] "L6. Status code cast can wrap negative"
- [ ] "L7. Async session thread-safety is undocumented"

Begin nu uitsluitend met punt 1.
```
</prompt>

🕒<prompt>
Lees `security_problems_HttpTransport.md`, `phase2/HttpTransport.hpp` en `phase2/test_HttpTransport.cpp`.
Los de onderstaande security-issues **strikt één voor één** op. Doe NOOIT twee fixes tegelijk in één stap. Controleer na het lezen de git history of er al issues uit te lijst zijn opgelost.

Werk voor elk punt onderstaande vaste cyclus af voordat je aan het volgende punt begint:
1. Lees de beschrijving van het specifieke issue in `security_problems_HttpTransport.md`.
2. Pas alleen de relevante code in `HttpTransport` (en eventuele unit tests) aan.
3. Draai check.sh om te verifiëren dat alles compileert, slaagt en geformat is, fix alle problemen die check.sh noemt. Je hoeft niet zelf unieke hex codes te genereren voor error messages, check.sh geeft suggesties.
4. Maak een git commit van alle in het repo aangepaste bestanden, benoem de issue in de commit tekst. GEEN PUSH, ALLEEN COMMIT.
5. Vink het punt af in je overzicht en ga pas daarna door naar het volgende punt.

Lijst van op te lossen issues:
- [ ] "H1. CRLF / control-character injection into the request → HTTP header injection & request smuggling"
- [ ] "M1. Integer overflow in the Content-Length completion check → silently truncated body"
- [ ] "M2. Slowloris-style unbounded total read time"
- [ ] "M3. `connect()` is not actually bounded by the socket timeouts (comment at line 381 is wrong)"
- [ ] "M4. SSRF / local-network exposure via attacker-chosen host, empty host, and service-name ports"
- [ ] "M5. User callback throwing inside an Asio handler → `std::terminate`"
- [ ] "L1. O(n²) rescanning of the growing response buffer (CPU DoS)"
- [ ] "L2. Memory amplification"
- [ ] "L3. Timeouts silently absent on non-unix platforms"
- [ ] "L4. Log/UI injection via attacker-controlled strings in exceptions"
- [ ] "L5. Trailing bytes after Content-Length are silently discarded"
- [ ] "L6. Status code cast can wrap negative"
- [ ] "L7. Async session thread-safety is undocumented"
</prompt>

🕒<prompt>
Update the tests (files starting with test_) and experiments (files starting with exp_) such that there is only terminal output on failure. Work on ONLY one test or experiment at a time. After modifying one test or experiment, verify that everything is correct by running check.sh. After all test and experiments are correct and updated, make a git commit compactly describing this task,  DO NOT GIT PUSH.
Tests to check for output and update:
- [ ] ./phase1/test_BencodeParser.cpp
- [ ] ./phase2/test_TrackerClient.cpp
- [ ] ./phase2/test_HttpTransport.cpp
Experiments to check for output and update:
- [ ] ./phase1/exp_read_torrent.cpp
- [ ] ./phase1/exp_sha_hash.cpp
- [ ] ./phase2/exp_announce.cpp
- [ ] ./phase2/exp_boost_asio.cpp
</prompt>

🕒<prompt>
Can you critically review everything in phase2 for me? Assume that inputs can be malicious, since this is a BitTorrent program and there is a high risk of malicious input files or peers. Write the potential security problems in markdown to security_problems_phase2.md.
</prompt>

🕒<prompt>
Can you critically review everything in phase1 for me? Assume that inputs can be malicious, since this is a BitTorrent program and there is a high risk of malicious input files or peers. Write the potential security problems in markdown to security_problems_phase1.md.
</prompt>

🕒<prompt>
Can you fix the security problems mentioned in security_problems_phase2.md? Make sure that check.sh and all compile_run.sh still pass. 
</prompt>

🕒<prompt>
Can you fix the security problems mentioned in security_problems_phase1.md? Make sure that check.sh and all compile_run.sh still pass. 
</prompt>

🕒<prompt>
Can you critically review everything in this project for me? Assume that inputs can be malicious, since this is a BitTorrent program and there is a high risk of malicious input files or peers. Write the potential security problems in markdown to security_problems_phase1.md.
</prompt>

🕒<prompt>
Can you check that all security problems from security_problems_*.md are fixed?
</prompt>

🕒<prompt>
Can you check that all unit tests are up to date? Make sure that check.sh and all compile_run.sh still pass.
</prompt>

### 🕒 Move to CMake

🕒<prompt>
</prompt>

### 🕒 Move to Boost test

🕒<prompt>
</prompt>
