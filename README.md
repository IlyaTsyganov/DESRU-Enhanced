# DESRU-Enhanced

[English](README.md) · [Français](README.fr.md) · [Deutsch](README.de.md) · [Русский](README.ru.md)

An unofficial Windows utility for DOOM Eternal, based on [bowsr's DESRU 1.6.17](https://github.com/bowsr/DESRU). The original Git history and credits are preserved.

## Install

1. Open [Releases](https://github.com/IlyaTsyganov/DESRU-Enhanced/releases/latest) and download **DESRU-Enhanced-v1.1.1-standalone-win-x64.zip** from Assets. GitHub's “Source code” archives are for developers.
2. On Windows 10/11 x64, close other DESRU instances and extract the entire archive into a writable folder. Keep its subfolders together. Back up an existing installation/settings before replacing files.
3. Run **DESRU.exe**, accept the administrator prompt, and select the game directory when requested.
4. Keep DESRU running and launch DOOM Eternal normally. Either application may be started first; no separate game launcher is required.

The standalone archive includes the .NET 6.0.36 desktop runtime, application dependencies, fonts, freescroll macro, meath00k and the native visual module. It needs neither an existing DESRU installation nor a separate .NET installation. It contains no game executable or level files. Automatic updates from the original DESRU are disabled by default to prevent replacing this fork; install future fork releases manually.

## Added or changed compared with original DESRU

- Modbot input protection prevents the opening E/Q hold from switching weapon tabs while keeping the internal FPS cap. Fresh presses work after release; the compatibility warning appears only if protection cannot attach. See [details](docs/MODBOT-INPUT.md).
- Native HUD speedometers show current horizontal speed and the speed captured at the last landing, including the brief landing impulse. They work in exclusive fullscreen and support separate positions/colors, shared size and optional landing display.
- Low-health tint opacity can be changed for both identified tint layers without changing health or armor.
- Cheats and meath00k can be enabled/disabled during gameplay. Disabling cheats restores tracked cvars to their pre-session values and clears managed player cheats. The obsolete restart warning is removed when both are off.
- Changed protected cvars and active noclip, notarget and god modes appear beneath the existing indicators. Startup works in either order; the metrics driver-name hiding also handles AMD strings.
- Firewall toggling reads the actual rule state and correctly re-enables the rule and its F indicator.
- Lighting Fix has a saved Options checkbox, an Info Panel status and the X indicator. It changes live and also applies when the game is launched directly.
- Console-controlled Collision Viewer draws triggers in cyan, player-blocking level collision in green and spawnable/despawnable invisible walls in orange. It has no checkbox or V indicator. Its commands unlock with cheats.
- Collision range admits whole objects when any part of their bounds enters the radius. The old fixed geometry cap is removed. A background calculation removes buried/internal surfaces and retains exposed intersection contours, including with fill disabled. Known non-player collision shapes are filtered.
- Compact Options/Trainer layout, updated On-Screen Display and Info Panel help, and a new Other help page with all DESRU commands, mouse-wheel scrolling and a matching dark scrollbar.

Original macro, hotkeys, scaling, trainer and other DESRU features remain. Read the category rules before using modified tools in a speedrun; this fork is not a leaderboard approval.

## Console commands

Enter any command without arguments to see its current value and help.

| Collision Viewer command | Meaning |
| --- | --- |
| `desru_volumes_show 0/1` | Hide/show all volumes; starts at 0 each game session. |
| `desru_volumes_fill 0/1` | Outlines only / outlines with fill; default 1. |
| `desru_triggers_show 0/1` | Hide/show triggers. |
| `desru_collision_show 0/1` | Hide/show level collision. |
| `desru_invisible_walls_show 0/1` | Hide/show spawnable/despawnable invisible walls. |
| `desru_volumes_render_distance <metres>` | Rendering radius; default 40, for example 73.5; finite, nonnegative. |

Enable cheats, then enter `desru_volumes_show 1`. Disabling cheats hides the viewer and locks its commands. Fill, category visibility and distance are saved. Geometry optimization finishes in the background; intersection contours become available with the optimized scene.

| Speedometer / tint command | Meaning |
| --- | --- |
| `desru_speedometer_show 0/1` | Hide/show the main display. |
| `desru_speedometer_showlanding 0/1` | Hide/show landing speed; the main display must also be enabled. |
| `desru_speedometer_x_pos <pixels> [current\|landing]` | Position from the left edge. |
| `desru_speedometer_y_pos <pixels> [current\|landing]` | Position from the top edge. |
| `desru_speedometer_size <0.1–10>` | Text size for both readings. |
| `desru_speedometer_color <R> <G> <B> [current\|landing]` | RGB color, channels 0–255. |
| `desru_lowhptintopacity <0–100>` | Low-health tint opacity; default 100. |

These commands do not require cheats. Speed uses metres per second with two decimal places; `--` means no landing has been recorded. Positions use physical screen pixels. Omit the target in position/color commands to change both readings. For example, `desru_speedometer_x_pos 100 landing` and `desru_speedometer_color 255 255 255 current`. Preferences are saved while DESRU runs. Fresh defaults are white, both readings enabled, current at (0, 480), landing at (100, 480), size 3.5; existing preferences take priority.

## Compatibility and building

There is no artificial game-version whitelist for Collision Viewer; engine-layout resolution must still succeed. Native HUD/tint were exercised on 1.1 and Steam 6.66 Rev 3.2, and viewer/lighting integration on 1.0 Release. This does not certify every feature on every build/driver. Some surface shimmer can remain; long draw distances increase CPU, memory and GPU work.

Build on Windows with Visual Studio MSBuild, the .NET desktop SDK and Desktop development with C++ (CMake, Ninja, Windows SDK). Vulkan headers are included. Use full Visual Studio MSBuild because the firewall integration imports a COM type library. Set `SkipBundleBuildEvents=true` to bypass the original output-folder replacement events.

```powershell
MSBuild.exe 'DOOMEternal Speedrun Utility.sln' /restore /t:Build /p:Configuration=Release /p:Platform=x64 /p:SkipBundleBuildEvents=true
MSBuild.exe tests/DESRU.Tests/DESRU.Tests.csproj /restore /t:Build /p:Configuration=Release /p:Platform=x64 /p:SkipBundleBuildEvents=true /p:SkipNativeVisualBuild=true
& tests/DESRU.Tests/bin/x64/Release/net6.0-windows/DESRU.Tests.exe
ctest --test-dir artifacts/visuals-build --output-on-failure
```

Source builds need the original distribution's fonts/macro/meath00k resources. See [standalone packaging](docs/STANDALONE.md), [speedometers](docs/SPEEDOMETERS.md), [visual features](docs/VISUAL-FEATURES.md), [live cheats](docs/LIVE-CHEATS.md), [firewall](docs/FIREWALL.md), [native module](NativeVisuals/README.md) and the [original README](docs/UPSTREAM-README.md).

## Credits and license

Original DESRU: **bowsr**. Lighting/viewer integration adapts [billymooreq's project](https://github.com/billymooreq/DoomEternal-v1.1--RTXfix-DrlaCollisionViewer); the viewer includes code used with its author's permission, not a blanket MIT license. meath00k is by brongo and contributors. See [CREDITS](CREDITS.md) and retained notices. DESRU retains its GPL-3.0 license; third-party components retain their own terms. Unofficial; not endorsed by id Software/Bethesda.
