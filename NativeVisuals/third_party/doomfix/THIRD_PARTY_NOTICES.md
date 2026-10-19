# Third-party notices

## DOOM Eternal collision viewer — IlyaTsyganov

The `collision-viewer` feature is based on
[doom-eternal-collision-viewer](https://github.com/IlyaTsyganov/doom-eternal-collision-viewer) by **IlyaTsyganov**,
commit `43ac57d`. Its code is used in this project **with the author's permission**.

Ported or derived from it (each file says so in its first line):

- `src/features/collision_viewer/` (the feature, its shaders and the embedded SPIR-V in `shaders_spirv.h`);
- `tests/cv_depth_test.cpp`, `tests/cv_render_test.cpp`, `tests/cv_scene_test.cpp`, `tests/cv_smoke_test.cpp`;
- `scripts/embed_shaders.py`.

The [MIT license](LICENSE) of this repository covers doomfix's own code. It does not grant rights to the code
above: that code is here by the author's permission. To use it outside this project, ask the author.

## Build-time dependencies (not included in this repository)

- [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) v1.4.341, The Khronos Group: downloaded by CMake
  when no Vulkan SDK is found. Apache-2.0 / MIT, as stated in that repository.
- [Inno Setup](https://jrsoftware.org/isinfo.php) by Jordan Russell and Martijn Laan: builds the installer. Its
  license is in the Inno Setup installation.

DOOM Eternal is © id Software / Bethesda. This repository contains no game files; the collision viewer reads the
running game's memory and changes nothing in the game.
