# Publication validation — October 19, 2026

- Release x64 build of the complete solution: PASS.
- Publication test run: 209 logic/UI/native execution checks PASS, including the added default-disabled upstream updater check.
- Separate-process settings persistence: 7 checks PASS; isolated test profile reset afterward.
- Native numbers, console commands and both low-health layers verified live on 1.1 and Steam 6.66 Rev 3.2; executable resolution also passed on 1.0 (no live 1.0 claim).
- Current desru_ values promoted to shared source/config/help defaults: both meters enabled, X 0/100, Y 480, scale 3.5, white, low-health tint 100.
- Includes live combined cheats/meath00k controls, baseline-preserving cvar restoration, cvar/player-command indicators below modifiers, AMD driver-label removal, and native speedometer startup retry.
- Native startup ordering and live controls were user-confirmed on 1.1. Upstream update checking remains disabled by default.
- Update ZIP must be extracted over the official DESRU 1.6.17 folder to supply original fonts, macro, and meath00k components.
- No PDBs, logs, user.config, continuation context, credentials, or game binaries are included.
- Full manual regression of every original DESRU feature is not claimed.
