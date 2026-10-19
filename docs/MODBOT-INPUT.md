# Modbot input protection

Added in DESRU-Enhanced v1.1.1.

The original utility documents an E/Q duplication problem when using its internal FPS limiter. E/Q can both interact with a modbot and navigate the modbot's weapon tabs. In the reproduced case, the slightly held opening input was delivered as tab navigation after the menu opened. DESRU itself was not synthesizing another E/Q keypress.

The fix consumes only modbot-owned `SWF_EVENT_TAB_NEXT` / `SWF_EVENT_TAB_PREV` events during that opening hold. It observes release of the captured E/Q keys, drains one further UI update and then lets fresh navigation presses through. There is no fixed timeout in seconds. At 250 FPS, the release/drain delay is approximately one to two frames (4–8 ms). Other widget events and other tab lists pass through normally. Input queues, bindings, game-owned widget enable flags, cheat permissions and FPS settings are not changed.

Protection runs automatically while DESRU is attached and an internal cap below 1000 Hz is in use, including caps selected through FPS hotkeys. It does not require cheats. The old unconditional startup warning is removed. If protection cannot attach and the internal cap is active, DESRU retains a compatibility warning and the original external-limiter/rebinding fallback, respecting the user's “Don't Show This Warning Again” preference.

## Implementation

- `NativeGameLayout` resolves modbot/tab-list RTTI, virtual methods, fields, event enum values and the keyboard API import from the actual executable. It validates the show/hide base calls and the tab receiver's range check. No executable label selects hardcoded function addresses.
- `NativeModbotInputLayout` records that resolved layout. Legacy and current Steam versions have different virtual slots and event values; both are handled.
- `NativeModbotInputCode` emits four small x64 wrappers: modbot show, update, hide and tab-list event reception. The show wrapper captures held keys before the original show runs. The update wrapper observes release and drains one UI update. Hide clears the latch. Event filtering additionally checks the exact tab widget and its parent root.
- `NativeModbotInputGuard` installs aligned virtual-function pointers after initializing immutable executable code and separate writable state. The state marker is `DESRUMI1`; its pointer is at byte 152 of the HUD extension state. Windows API addresses are resolved in the game process, including forwarded clock exports. No assumption of identical local/remote module bases is used.
- A 3-second lease and explicit disable make the wrappers pass through if DESRU stops. Remote callback code remains allocated until game exit. Restarting DESRU reconnects only after verifying the marker, owned slots and exact callback bytes.
- `MainWindow.Overlays` owns attachment, lease renewal and cleanup. The fallback warning is deferred until a real attachment failure rather than displayed on every launch.

## Verification and scope

The x64 wrappers are executed against isolated local fixtures, not just inspected as byte strings. Tests cover a sustained opening hold, independent E/Q release, a fresh press, release-event draining, unrelated widgets/events, reparenting, closing/reopening, disable and lease expiry. Separate checks cover Steam's shifted event numbers. The managed publication suite passes 446 checks.

Layout resolution passed against installed 1.0 Release, 1.1 and Steam 6.66 Rev 3.2 executables. Live verification was on 1.1: the unwanted switch disappeared in manual testing. An initial three-minute run recorded 29 guarded openings and 26 consumed tab events; repeated manual testing accepted the fix. This does not claim live coverage of every build or controller/layout combination.

A separate experiment synchronized the integer and float caches of `com_adaptiveTickMaxHz`; it did not fix the bug, and the original value was restored. The final fix does not include that experimental cvar write. The native graphics module, collision rendering and lighting shaders are unchanged.

For patch updates, the application's assembly identity remains `1.1.0.0` so .NET continues using the existing settings directory. File/product version identifies v1.1.1. Extract the new archive over the same installation folder to retain its profile identity.
