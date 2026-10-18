# Publication validation — October 18, 2026

- Release x64 build of the complete solution: PASS.
- Publication test run: 134 logic/UI/native execution checks PASS, including the added default-disabled upstream updater check.
- Separate-process settings persistence: 6 checks PASS; isolated test profile reset afterward.
- Original enhanced implementation previously verified live on DOOM Eternal 1.1: native fullscreen readings, landing impulse, independent colors/visibility, and both low-health tint layers.
- This publication adds no changes to game hooks or speed calculation; upstream update checking is disabled by default.
- Update ZIP must be extracted over the official DESRU 1.6.17 folder to supply original fonts, macro, and meath00k components.
- No PDBs, logs, user.config, continuation context, credentials, or game binaries are included.
- Full manual regression of every original DESRU feature is not claimed.
