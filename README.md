# DESRU Enhanced

An unofficial enhancement of [bowsr's DOOM Eternal Speedrun Utility](https://github.com/bowsr/DESRU), based on upstream commit `f78f602e80c4cd1ceb7c6ac306cb4531f111f3ab` (DESRU 1.6.17).

Maintained by [IlyaTsyganov](https://github.com/IlyaTsyganov). Original DESRU is by **bowsr**; this project does not claim authorship of the original utility and is not an official release or speedrun rules endorsement.

## Added features

- Native game HUD speedometers, including exclusive fullscreen: current horizontal speed and the most recent landing impulse.
- Numbers only, two decimal places, independent physical-pixel positions and RGB colors.
- Master visibility plus an independent landing-speed switch, shared text size, persistent settings.
- Low-health red-tint opacity control, affecting both identified effect layers.
- Verified native integration for **DOOM Eternal 1.1**; other builds are not claimed supported by these commands.

## Install

1. Install the x64 [.NET 6 Desktop Runtime](https://dotnet.microsoft.com/en-us/download/dotnet/6.0/runtime).
2. Extract the original [DESRU 1.6.17 distribution](https://github.com/bowsr/DESRU/releases/tag/1.6.17) into a folder. Keep its `fonts`, `macro`, and `meath00k` folders.
3. Close DESRU. Extract this fork's **DESRU-Enhanced-v1.0.0-update-win-x64.zip** from [Releases](https://github.com/IlyaTsyganov/DESRU-Enhanced/releases) over that folder, replacing the supplied files.
4. Start `DESRU.exe` from that folder. Preserve your existing user settings. Start the utility before launching the game when possible.

This is an **update archive**, not a standalone package. Original game fonts, the meath00k DLL, and the freescroll executable are obtained from their original distribution rather than republished here. Automatic upstream update checks are disabled by default in this fork; install future fork releases manually. Do not run two DESRU copies simultaneously.

## Game console commands

```text
desru_speedometer_show 1
desru_speedometer_showlanding 1
desru_speedometer_x_pos 400 current
desru_speedometer_y_pos 600 current
desru_speedometer_x_pos 800 landing
desru_speedometer_y_pos 600 landing
desru_speedometer_size 2
desru_speedometer_color 255 255 255 current
desru_speedometer_color 255 128 0 landing
desru_lowhptintopacity 100
```

`show 0` hides the meters; `showlanding 0` keeps only current speed. X/Y use physical pixels from the game's top-left corner. Omit `current`/`landing` to apply coordinates or RGB to both. Colors are three integers from 0 to 255; size is 0.1–10. Tint is 0–100: 0 removes the low-health redness, 100 restores its original opacity. Settings are saved while DESRU runs. The old name `desru_show_speedometer` was replaced by `desru_speedometer_show`.

Landing speed retains the observed peak at contact and for the following 8 ms. The reader requests approximately 1 ms sampling; Windows scheduling can still miss a short impulse. No landing yet is shown as `--`.

## Documentation and build

- [Usage and version support](docs/SPEEDOMETERS.md)
- [Detailed implementation report](docs/IMPLEMENTATION-REPORT-EN.md)
- [Original DESRU README](docs/UPSTREAM-README.md)
- [Credits and third-party notices](CREDITS.md)

Build on Windows with Visual Studio MSBuild and the .NET 6 SDK. Full MSBuild is required by the existing firewall COM reference.

```powershell
MSBuild.exe 'DOOMEternal Speedrun Utility.sln' /restore /t:Build /p:Configuration=Release /p:Platform=x64 /p:SkipBundleBuildEvents=true
MSBuild.exe tests/DESRU.Tests/DESRU.Tests.csproj /restore /t:Build /p:Configuration=Release /p:Platform=x64 /p:SkipBundleBuildEvents=true
& tests/DESRU.Tests/bin/x64/Release/net6.0-windows/DESRU.Tests.exe
```

`SkipBundleBuildEvents=true` avoids upstream packaging/cleanup targets. Place built files over an original DESRU distribution to supply its required resources. Latest implementation validation: 133 logic/UI/native execution checks and six persistence checks; manual validation on game version 1.1. Full regression coverage of every original function is not claimed.

## Credits and license

**bowsr** — original DESRU and freescroll integration; **brongo and meath00k contributors** — meath00k and reference rendering/command-system sources; **Christopher Serr, Sergey Papushin and LiveSplit contributors** — memory-access code used by upstream DESRU. See [CREDITS.md](CREDITS.md) for additional authors, dependencies, and license notices.

DESRU modifications remain under the original [GNU GPL v3 license](LICENSE). Third-party components retain their own licenses. DOOM Eternal is a product of id Software/Bethesda; this project is not affiliated with or endorsed by them.
