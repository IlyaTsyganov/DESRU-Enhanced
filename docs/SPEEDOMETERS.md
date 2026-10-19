# Native horizontal speedometers

[Implementation report](IMPLEMENTATION-REPORT-EN.md), [defaults](DEFAULTS.md), and [live cheats/modifier indicators](LIVE-CHEATS.md).

DESRU now draws two numeric readings through the **game's own HUD text renderer**, including exclusive fullscreen. There are no additional checkboxes in DESRU. Start DESRU normally, open the game's console, and use:

```text
desru_speedometer_show 1
desru_speedometer_x_pos 0 current
desru_speedometer_y_pos 480 current
desru_speedometer_x_pos 100 landing
desru_speedometer_y_pos 480 landing
desru_speedometer_size 3.5
```

| Command | Values | Effect |
| --- | --- | --- |
| `desru_speedometer_show` | `0` or `1` | Hide or show both readings |
| `desru_speedometer_x_pos` | pixels, optionally followed by `current` or `landing` | X offset from the left edge for the selected meter, or both when omitted |
| `desru_speedometer_y_pos` | pixels, optionally followed by `current` or `landing` | Y offset from the top edge for the selected meter, or both when omitted |
| `desru_speedometer_size` | `0.1..10` | Shared text size multiplier; use a decimal point |

Each reading is drawn separately with its own X/Y coordinates. The initial layout places current speed to the left of last-landing speed, but either may be moved anywhere. Each text width is calculated from the game's actual glyph width (RVA `0x4F68560`); height comes from RVA `0x4F68564`. Positions are not automatically separated after initial migration: explicitly choosing identical coordinates can overlap the readings.

Visibility still accepts an optional final argument, **`current`** or **`landing`**, for independent control:

```text
desru_speedometer_show 0 landing
desru_speedometer_show 1 landing
desru_speedometer_show 0 current
```

Visibility and X/Y commands accept an optional target. Without a target they set the specified property for both meters; for example, `desru_speedometer_y_pos 600` aligns both on the same Y. Setting both X and Y without a target gives both readings identical positions, so use targets to separate them. X/Y are physical pixels, independent of Windows display scaling: `400,600` means 400 pixels right and 600 pixels down from the game's top-left corner. Native drawing converts these pixels to virtual HUD dimensions. Text size remains shared. Each reading is clamped at viewport edges using its own width; oversized text may be clipped. Invalid targets, negative coordinates, and values beyond Int32 range are ignored and logged. Earlier development builds used percentages; re-enter X/Y when switching from one of those builds.

The source and package defaults enable both readings: current at 0,480, landing at 100,480, size 3.5, white RGB, tint 100. Existing saved preferences take priority; see DEFAULTS.md. Commands persist through DESRU's existing mechanism: independent enable flags, RGB colors and X/Y coordinates, shared size control. `SpeedometerPositionsVersion` records a one-time upgrade: if old positions are identical, landing X is shifted right by `ceil(160 × size)` pixels. Already different positions are preserved. Later identical positions are allowed and persist. DESRU must be running to apply and save commands. Close any older DESRU instance before starting this build. DESRU and the game may now be started in either order; retry logic connects the native HUD when the game becomes available.

The left reading is **current horizontal speed**, `sqrt(vx² + vy²)`, in the same m/s units as the existing trainer. The right retains the **horizontal landing impulse**, including the brief acceleration when falling onto a surface with horizontal movement. It captures the first contact speed and the maximum of the next 8 ms, allowing contact and velocity to be published in adjacent physics steps. Later walking acceleration and deceleration do not change it. Both numbers have **two decimal places**, without captions or unit suffixes. `--` means no landing has been observed since attaching/loading. Readings are hidden while loading, outside gameplay, unfocused, or when velocity cannot be read.

## Visibility, RGB color, and low-health tint

```text
desru_speedometer_show 1
desru_speedometer_showlanding 0
desru_speedometer_color 255 255 255 current
desru_speedometer_color 255 128 0 landing
desru_lowhptintopacity 0
```

`desru_speedometer_show 0/1` is the overall visibility switch. Showing enables current speed and preserves the independent `desru_speedometer_showlanding 0/1` preference. Thus hiding and showing does not unexpectedly re-enable landing speed. Previous targeted visibility requests still work for compatibility.

`desru_speedometer_color R G B [current|landing]` accepts exactly three integer channels from 0 to 255. Omit the target to change both. White is `255 255 255`, red is `255 0 0`, and black is `0 0 0`. Invalid/incomplete requests leave both colors unchanged. Each color persists independently. The desktop fallback chooses a transparency key different from both text colors, so magenta/cyan text remains visible.

`desru_lowhptintopacity 0..100` controls the red low-health vignette: 0 removes it, 50 reduces its opacity, and 100 restores the original. Decimal percentages are accepted with a decimal point. Two distinct layers are controlled: the player low/very-low-health screen FX and the fullscreen background inside the low-health HUD element. In-game tests confirmed complete removal and restoration with the full HUD enabled. Game health, armor, warnings, and thresholds remain unchanged. Its heartbeat is independent of meter visibility, so hiding both meters does not reset the chosen tint opacity. The default is 100; settings persist in the normal DESRU profile.

The old spelling `desru_show_speedometer` has been replaced by `desru_speedometer_show`.

## Memory and rendering evidence

Ground contact is read directly, never inferred from `vz == 0`, so a jump apex is not a landing.

| Verified 1.1 item | Value |
| --- | --- |
| Executable MD5 | `70426fc580f2c064cb618317fd1a9d5e` (matches DESRU's version table) |
| PE SizeOfImage | `492113920` / `0x1D551000` |
| Existing velocity chain | `DOOMEternalx64vk.exe + 80143112 → 0x1510 → 0x598 → 0x1D0 → 0x3F40` |
| Embedded type table: `idHavokPhysics_Player` | RVA `0x3A60530` |
| `idHavokPhysics_Player.current` | `playerPState_t` at `0x3F28` |
| Embedded type table: `playerPState_t` | RVA `0x3A604A0` |
| `playerPState_t.velocity` | `idVec3` at `0x18`, hence physics offset `0x3F40` |
| `playerPState_t.groundPlane` | `bool` at `0xA0`, hence physics offset `0x3FC8` |
| Contact offset relative to velocity | `0x88` / `136` bytes |
| `idDebugHUDLocal` virtual table, Render slot | RVA `0x2608800` (slot 1) |
| Original HUD Render | RVA `0x44BE70` |
| `idRenderModelGui::DrawString` | RVA `0x5A5B90` |
| Virtual viewport width / height | RVAs `0x5A6090` / `0x5A6060` |
| Global command-system pointer | RVA `0x3BDF8A8`; AddCommand virtual slot 3 |

Type layouts, rendering and command signatures were checked against the primary sources in [meath00k](https://github.com/brongo/m3337ho0o0ok): `idtypeinfo.hpp`, `mh_guirender.cpp`, `rendermodelgui.cpp`, and `cmdsystem.cpp`. The live function signatures were verified before attaching. REA was not required.

The small Windows x64 adapter is generated by C# and redirects the aligned Render virtual-table slot. It calls the original HUD first, registers the seven commands once, and calls DrawString for each enabled reading at its own coordinates. The existing DESRU performance strings, trainer, FPS/resolution logic, hotkeys, macro, and firewall behavior remain on their original paths. There is no new DLL to install in the game directory and no new package dependency.

The adapter reserves **8 KiB per game process**: executable code is read/execute and shared state read/write. A render-frame heartbeat hides numbers if DESRU stops updating them. Clean close disables it immediately. Code and command callbacks remain allocated until game exit to avoid freeing executing code. Restarting reconnects to the same block. Earlier `DESRUS02` through `DESRUS05` adapters can upgrade to **`DESRUS07`**, which retains the independent 64-byte reading records, adds per-meter RGBA colors and two more console commands. The existing show-command string is renamed in its stable shared-memory slot; live command-list inspection verified that the game retains this pointer. Existing callbacks remain valid. Upgrade uses a fresh code allocation and the existing command mailboxes; old allocations remain until game exit. Foreign HUD hooks and mismatched signatures are left untouched.

Native drawing, console commands and the ground-contact offset are verified only for **1.1**. Other game versions retain all pre-existing DESRU functionality. A separate transparent desktop fallback can use previously saved settings and existing velocity support in windowed/borderless mode; it shows `N/A` for unverified contact layouts. Native addresses are never guessed for a different version.

An independent read-only worker polls velocity and the contact flag together with a requested 1 ms interval while either meter is enabled. UI rendering and the existing trainer remain on their original timer. Immutable snapshots cross the thread boundary; the worker never writes game data. A scoped Windows timer request is released when sampling pauses or stops, and the worker finishes before the game process handle is closed. The pointer and gameplay context are refreshed by the existing memory timer. Airborne intervals shorter than 8 ms are ignored, filtering the brief ground-flag resets observed inside physics updates. Failed reads, nonfinite values, loading/menu state, disabling both meters, and physics-object changes reset landing history. This is sampled telemetry, not a physics hook: scheduling stalls can still miss a very short impulse.

## Building and testing

Use Windows, .NET 6 desktop targeting/runtime support, and Visual Studio MSBuild with COM interop support. The existing firewall COM reference requires full Visual Studio MSBuild rather than `dotnet build`. From a Developer PowerShell:

```powershell
dotnet restore 'DOOMEternal Speedrun Utility.sln'
MSBuild.exe 'DOOMEternal Speedrun Utility.sln' /t:Build /p:Configuration=Release /p:Platform=x64 /p:SkipBundleBuildEvents=true
dotnet restore tests/DESRU.Tests/DESRU.Tests.csproj
MSBuild.exe tests/DESRU.Tests/DESRU.Tests.csproj /t:Build /p:Configuration=Release /p:Platform=x64 /p:SkipBundleBuildEvents=true
& tests/DESRU.Tests/bin/x64/Release/net6.0-windows/DESRU.Tests.exe
& tests/DESRU.Tests/bin/x64/Release/net6.0-windows/DESRU.Tests.exe --settings-write
& tests/DESRU.Tests/bin/x64/Release/net6.0-windows/DESRU.Tests.exe --settings-read
```

`SkipBundleBuildEvents=true` skips the existing packaging/deletion commands when the ignored `bundle` directory is absent. Default release packaging remains unchanged. The local runnable `output/` combines the compiled DESRU and Updater with required bundled utilities from the official 1.6.17 archive, whose SHA-256 was verified as `31556caae6e88d6b9bbd9d7079112f2b81a61c6bbe5e5cdd8e58e7b48344b080`.

The current suite has 133 checks covering impulse retention, transient flags, invalid samples, two decimals, independent visibility and coordinates, shared size commands, one-time migration, separate desktop rendering, the background sampler, and native x64 calling conventions. Native tests execute the generated code against controlled callbacks, including 1080p/4K conversion and independent edge clamping for strings of different widths. Six separate restart checks cover existing settings, different X/Y coordinates, migration version, tint opacity, overall visibility and independent RGB colors. The real startup file check is run against the assembled package.

A one-minute high-frequency read-only recording captured **37,167 samples**. One observed impact changed horizontal speed from **9.52 to 22.10** at contact, then to **18.75 after 16 ms**. This demonstrates why the previous UI-timer sampling could report an already-decayed speed. The raw local capture is `artifacts/landing-trace.csv`.

The previous five-minute fixed-pair/native-sampler test recorded **17,898 valid reads and zero failures**. The user confirmed fullscreen drawing, commands, two decimals and impulse retention. The new independent-coordinate revision passes the local native-code tests; it has not yet received another visual in-game confirmation. The delivered package starts current at (1172,740), landing at (1492,740), size=2; later changes persist in the normal DESRU user profile.

Initial read-only live check: **980 valid reads, 0 failed reads, 10 landings** over 30 seconds. A later desktop fallback check in borderless mode was confirmed by the user. The first native live test recorded **3916 valid reads, 0 failures, 12 landings, 46,450 HUD render calls**, and registration of all four commands; real console size changes were captured and applied. Detailed local results are in `artifacts/live-probe.json`, `artifacts/live-overlay.json`, and `artifacts/native-overlay.json`.

Live test modes require a loaded 1.1 game and are optional:

```powershell
# Read-only memory check, 30 seconds.
& tests/DESRU.Tests/bin/x64/Release/net6.0-windows/DESRU.Tests.exe --probe-game
# Native drawing and console integration, up to five minutes; disables drawing on completion.
& tests/DESRU.Tests/bin/x64/Release/net6.0-windows/DESRU.Tests.exe --native-overlay
```

Manual regression limits: existing FPS/macro/hotkey/firewall/resolution features are compiled, but their full behavior was not exhaustively retested. Level/checkpoint transitions and multiple-monitor/DPI changes still merit a longer play session.

## Querying commands without arguments

Enter any command name alone to print its current value, application default, and a short English description:

```text
"desru_speedometer_color" is:"current=255 0 0 landing=0 255 0" default:"255 255 255"
Set text color using three integer RGB channels (0 to 255). Applies to both speedometers unless [current|landing] is specified.
```

Equal per-reading values appear once; different ones are labeled current/landing. Defaults come from the installation's application config, not the saved user profile. The query does not change settings and works while speedometers are hidden. Size remains shared for both readings. Protocol DESRUS07 adds a bounded response buffer and game-thread VPrintf output; it upgrades S06 without replacing existing command callbacks.


## Adaptive version support

Native meters, console commands and both low-health layers are live-confirmed on 1.1 and Steam 6.66 Rev 3.2. Read-only resolution also passed on 1.0; live validation on 1.0 is pending. The older 1.1-only address table above is historical reference. NativeGameLayout resolves each installed executable and refuses unresolved or ambiguous layouts. Unknown versions are not guaranteed supported. See implementation report section 15. Current validation: 151 publication checks and seven restart checks.


## Startup and live control updates

The native HUD now waits for game console/font initialization and retries transient startup failures. Either startup order is supported. Cheats and Meath00k toggle together through one direct button. Their cvar changes restore without resetting initial preferences; active command flags appear in smaller text under DESRU modifiers. See LIVE-CHEATS.md.
