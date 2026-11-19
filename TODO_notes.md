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

🕒<prompt>
Can you fix the security problems mentioned in security_problems_TrackerClient.md? Make sure that check.sh all compile_run.sh still pass. 
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
