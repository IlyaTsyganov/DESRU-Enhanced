# Publication validation — October 18, 2026

- Release x64 build of the complete solution: PASS.
- Publication test run: 151 logic/UI/native execution checks PASS, including the added default-disabled upstream updater check.
- Separate-process settings persistence: 7 checks PASS; isolated test profile reset afterward.
- Native numbers, console commands and both low-health layers verified live on 1.1 and Steam 6.66 Rev 3.2; executable resolution also passed on 1.0 (no live 1.0 claim).
- Adds adaptive native layouts and no-argument command help with immutable application defaults; upstream update checking remains disabled by default.
- Update ZIP must be extracted over the official DESRU 1.6.17 folder to supply original fonts, macro, and meath00k components.
- No PDBs, logs, user.config, continuation context, credentials, or game binaries are included.
- Full manual regression of every original DESRU feature is not claimed.
