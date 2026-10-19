# Firewall rule switching

The firewall button enables or disables outbound block rules for the configured DOOM Eternal installations, including the currently running executable. Existing matching rules are toggled directly, so duplicate rules and shared rule names cannot leave a block active or select another installation by mistake. Repeated setup reuses existing blocks instead of adding duplicates. Inbound and allow rules are excluded.

Status checks only read Windows Firewall; they never enable a disabled rule. The button and the in-game `F` indicator are refreshed from the enabled outbound block for the current game executable. Both disabling and re-enabling update the HUD flag immediately, and periodic checks keep it synchronized.

The separate firewall restart notice in DESRU is retained: enabling a block cannot undo balance data already downloaded during the current game session.

## Verification

Release x64 solution and test project builds pass, along with the existing 208 logic/UI checks (209 in the publication checkout). The explicit `DESRU.Tests.exe --test-firewall` diagnostic requires administrator rights and passed 13 checks using temporary rules for nonexistent test executables. It exercises the real button, duplicate rules, shared names across applications, ignored inbound/allow rules, repeated read/off/on cycles, immediate HUD flag restoration, idempotent setup and creation of a missing block. All temporary rules are removed afterward. It does not toggle the user's running game rules during the test.
