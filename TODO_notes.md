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

🕒<<prompt>
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

🕒<prompt>
Can you fix the security problems mentioned in security_problems_HttpTransport.md? Make sure that check.sh and all compile_run.sh still pass.
</prompt>

🕒<prompt>
Can you fix the security problems mentioned in security_problems_BencodeParser.md? Make sure that check.sh and all compile_run.sh still pass.
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
