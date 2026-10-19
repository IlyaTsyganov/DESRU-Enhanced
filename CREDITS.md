# Credits and third-party notices

## Original utility

- **bowsr** — [DESRU](https://github.com/bowsr/DESRU), the original Windows utility and its existing features. This repository preserves its Git history and original GPL-3.0 LICENSE. Base: `f78f602e80c4cd1ceb7c6ac306cb4531f111f3ab`, DESRU 1.6.17.
- **bowsr** — [desru-macro](https://github.com/bowsr/desru-macro), freescroll integration. Its included ISC notice credits **Adrian Bibby Walther**, Copyright 2017, 2019. The standalone release includes the unchanged original macro and its ISC notice.
- **Christopher Serr, Sergey Papushin and LiveSplit contributors** — [LiveSplit](https://github.com/LiveSplit/LiveSplit), memory-access code used by the original DESRU, as acknowledged in upstream HOWITWORKS.md. The MIT copyright notice is retained in `THIRD-PARTY-LICENSES/LiveSplit-LICENSE.txt`.
- **brongo and contributors** — [meath00k](https://github.com/brongo/m3337ho0o0ok), originally bundled by DESRU. Its type definitions, rendering, and command-system sources were consulted as references while researching native HUD integration. This is not a claim that the new adapter is a complete meath00k implementation. The standalone release includes the unchanged DLL from the original DESRU distribution. No explicit license was found at the referenced repository root; this fork does not grant a new license to that component.

## Enhancements

**IlyaTsyganov** maintains this unofficial fork. Added work covers horizontal speed/landing tracking, native HUD rendering, console commands, independent positions and colors, two-layer low-health opacity control, live cheats/meath00k switching, firewall corrections, visual-module integration, collision visibility/contours, UI, tests, and documentation. Implementation was developed with OpenAI Codex assistance. Original contributors retain credit and copyright for their work. REA was considered but was not used in the final implementation.

**billymooreq and contributors** — [DoomEternal-v1.1--RTXfix-DrlaCollisionViewer](https://github.com/billymooreq/DoomEternal-v1.1--RTXfix-DrlaCollisionViewer), pinned at `e7adfa980a8a35af5a6f401b8eb6a47f02759d6b`, supplies the adapted Vulkan lighting fix and collision-viewer implementation. The viewer derives from **IlyaTsyganov's** collision viewer, used with its author's permission. The upstream MIT license covers its own code, not a blanket relicensing of the viewer. See [native module notices](NativeVisuals/third_party/doomfix/THIRD_PARTY_NOTICES.md) and retained LICENSE.

**Khronos Group and contributors** — vendored Vulkan-Headers, with Apache-2.0/MIT notices retained under `NativeVisuals/third_party/Vulkan-Headers` and in the visual module's binary license folder.

## Application dependencies

| Component | Author / project | Version | License |
| --- | --- | --- | --- |
| Markdig | Alexandre Mutel and contributors | 0.30.4 | BSD-2-Clause |
| Newtonsoft.Json | James Newton-King and contributors | 13.0.2 | MIT |
| RawInput.Sharp | mfakane | 0.0.4 | Zlib |
| Serilog | Serilog contributors | 3.1.1 | Apache-2.0 |
| Serilog.Sinks.Debug | Serilog contributors | 2.0.0 | Apache-2.0 |
| Serilog.Sinks.File | Serilog contributors | 5.0.0 | Apache-2.0 |
| WindowsInput | Mediated Communications, Tony Valenti and contributing authors | 6.4.1 | MIT |
| Microsoft.Web.WebView2 | Microsoft Corporation | 1.0.1462.37 | Microsoft package license |

Dependency license texts and relevant notices are included in `THIRD-PARTY-LICENSES`. NuGet package metadata remains the authority for package-specific terms. The standalone archive also includes the Microsoft .NET/Windows Desktop runtime 6.0.36 and its license/notices. Required fonts and the original bundled macro/meath00k are copied unchanged from DESRU 1.6.17; they retain their original ownership and terms and are not relicensed under this fork's GPL. No game executable or extracted level data is included.

## Game ownership

DOOM Eternal and its assets belong to their respective owners, including id Software/Bethesda. This is an unofficial external utility, not an official game product. Upstream speedrun instructions do not constitute approval of this modified utility for any leaderboard category.
