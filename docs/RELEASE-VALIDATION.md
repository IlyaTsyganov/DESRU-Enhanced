# Release validation — DESRU-Enhanced v1.1.1

- Managed Release x64 suite: 446 checks PASS, including executed x64 modbot guard fixtures and shifted Steam event IDs.
- Layout resolution passed on 1.0 Release, 1.1 and Steam 6.66 Rev 3.2.
- Manual 1.1 verification confirmed that modbot E/Q duplication disappeared; a second test was accepted. FPS remained 250.
- Native graphics DLL is unchanged from v1.1.0; previous graphics/geometry validation below still applies.
- Main application assembly identity stays 1.1.0.0 for settings continuity; file/product release version is 1.1.1.
- Standalone startup/runtime/native/Help checks and ZIP integrity are repeated for the release package.

## Previous native/visual release validation (v1.1.0)


Validation performed on Windows x64, October 19–20, 2026.

- Fresh public-source native build: PASS.
- Five CPU suites (`cv_depth`, `cv_render`, `cv_scene`, `cv_solid`, `cv_commands`): PASS. Includes range/whole-object rendering, visibility trimming, intersection contours and cheat-gated command tests.
- Public-source Release x64 managed build and **427 logic/UI checks**: PASS. Includes the fork's default-disabled upstream updater, Help content, Other-tab bounds, dark scrolling control, visual protocol and Options/Trainer layout.
- Approved On-Screen Display, Info Panel and Other help pages rendered and visually inspected: PASS.
- Main application and updater published as self-contained Windows x64 with .NET runtime 6.0.36. Native visual module is included explicitly.
- Assembled standalone: **17 startup/runtime/native/Help checks PASS**, including the real startup resource check, parsed configuration, both self-contained runtime manifests, required runtime files, x64 native exports and exact approved Help strings in the packaged assembly.

Earlier live verification covered native speedometers, command changes and both low-health tint layers on game 1.1 and Steam 6.66 Rev 3.2. Visual integration was exercised on 1.0 Release. The user confirmed working draw-distance changes and visible intersection lines with fill both enabled and disabled. Remaining surface shimmer was substantially improved and accepted; it is not claimed to be eliminated.

Earlier Vulkan GPU barrier/event/render readback tests passed during development. They were not all rerun as part of the final documentation-only packaging pass. A full manual regression of every original DESRU feature, game build and driver is not claimed. The viewer has no artificial version whitelist but still requires successful engine-layout resolution.

Final archive checks and SHA-256 are recorded with the GitHub release assets. Packaging excludes PDBs, logs, user.config, continuation context, private maintenance notes, credentials, game executables and extracted level geometry. See [standalone packaging](STANDALONE.md).
