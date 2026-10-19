# DESRU native visual module

`build.ps1` builds the x64 C++17 module using Visual Studio Desktop development with C++, CMake and Ninja. Vulkan headers are vendored; no Vulkan SDK or launcher installation is required to run DESRU.

The C# project builds this module before compilation and packages it as `visuals/DESRU.Visuals.Runtime.dll`. For a managed-only rebuild after a successful native build, pass `/p:SkipNativeVisualBuild=true`. Use `/p:SkipBundleBuildEvents=true` when building in a workspace that contains a user-configured `output/` directory.

CPU checks:

```powershell
./NativeVisuals/build.ps1
ctest --test-dir artifacts/visuals-build --output-on-failure
```

The bridge uses the `DESRUV02` control protocol, version 2. Request bits are lighting=1, viewer=2, triggers=4, collision=8 and invisible walls=16, fill=32 and DESRU cheat permission=64. Viewer rendering requires both viewer and cheat bits. The native control struct also publishes actual state, counters and a float rendering distance at byte 80 (default 40 metres). Request, lease and command updates use atomic operations. DLL callbacks remain loaded until the game exits; an expired lease disables the effects.

`runtime.cpp` adopts the current Vulkan device and redirects game-owned dispatch slots; `profile.cpp` resolves engine objects and validates camera transforms; `commands.cpp` registers six cheat-gated viewer commands without starting Vulkan hooks; the runtime starts lazily for an explicit show request or Lighting Fix. The adapted upstream layer implements the Vulkan events, barrier fix and scene renderer.

Collision range filtering admits whole world-space bounds for triggers, collision leaf-shape instances and invisible walls. An intersection retains the object's full shared geometry; neither fragments nor a finite forward far plane slice it at the configured radius. The 250,000-vertex collector/renderer caps are removed. CPU and GPU buffers grow with demand and are reused. Vulkan's 32-bit draw indexing is checked rather than silently truncating draws. Dense frames use four disjoint CPU emission tasks with deterministic output; small frames stay serial. No additional runtime DLL is required.

The fragment shader uses a bounded 2×2 depth footprint adapted to the overlay surface gradient for coplanar stability with jitter/dynamic scaling, keeping foreground discontinuities occluding. Regenerate the embedded shaders after GLSL edits with `python third_party/doomfix/scripts/embed_shaders.py --glslang <compiler>`. The runtime build needs no shader compiler. `build.ps1` uses UTF-8 console output so localized MSVC diagnostics do not break Ninja's header dependency tracking.

Vendored sources:

- `third_party/doomfix`: [upstream](https://github.com/billymooreq/DoomEternal-v1.1--RTXfix-DrlaCollisionViewer), commit `e7adfa980a8a35af5a6f401b8eb6a47f02759d6b`. Retained [LICENSE](third_party/doomfix/LICENSE) and [THIRD_PARTY_NOTICES.md](third_party/doomfix/THIRD_PARTY_NOTICES.md). The upstream MIT license covers doomfix's own code; the viewer is included under the author's permission as described in those notices.
- `third_party/Vulkan-Headers`: Khronos Vulkan headers and their retained Apache-2.0/MIT notices.

No game binaries, runtime memory dumps or extracted level geometry are included in these sources.

`scene/solid_visibility.cpp` computes exterior collision patches against closed solids in a separate background optimization pass. Closed topology is checked before a shape can occlude another. Convex half-spaces and concave BSPs clip faces/contours; duplicate/coplanar ownership is stable and no interior cut caps are generated. Original distance bounds survive trimming. Open/ambiguous shapes remain non-occluding. The initial map is available immediately; a completed optimized snapshot replaces it, while map generation changes cancel old work. Retired snapshots are released by the worker only after readers finish.

Exposed intersection boundaries are emitted as contour lines after optimization, including in outlines-only mode. Polygon edges carry original-edge or cutting-plane provenance; signed interval cancellation removes tessellation and BSP partition edges even at split T-junctions. Existing contour edges are reused, shared seams use stable ownership and third-solid occlusion, and fill geometry/shaders stay unchanged. The optimizer logs the generated intersection segment count.

Viewer visibility is session-only and defaults off. `desru_volumes_show` controls all geometry; `desru_volumes_fill` controls fill independently of contours. Disabling cheats clears managed visibility and gates native drawing. Shape contents are validated through the engine shape-data table, including compressed per-primitive tags; unknown metadata is preserved.
