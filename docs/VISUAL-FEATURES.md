# Lighting Fix and Collision Viewer

**Lighting Fix** keeps its saved checkbox in **Options**, status in **Info Panel** and automatic application to a directly launched game. **Collision Viewer** is console-only: enable cheats in DESRU, then use `desru_volumes_show 1`. Its checkbox, Info Panel status and V modifier are removed. Each game session starts with the viewer hidden, regardless of the former saved checkbox. Disabling cheats hides the viewer and locks all six volume commands; re-enabling cheats does not automatically show it.

Enabling cheats registers console commands without starting Vulkan hooks or rendering. The first explicit viewer activation briefly rebuilds graphics resources and restores the video mode in the same game process. Later show/hide commands reuse those resources. An expired DESRU heartbeat disables both effects.

Options remains on the left, Trainer on the right, with their lower edges aligned. Lighting Fix sits above the Firewall Rule and Cheats buttons. The removed viewer row's space is reclaimed. Help uses the existing dark scrollbar and wheel navigation.

The complete on-screen modifier order is **MFRSLX**; **X** still identifies Lighting Fix. Collision Viewer has no modifier letter.

## Viewer console commands

All six commands require **cheats enabled in DESRU**. Categories, fill and distance can be configured while the viewer is hidden. Enter a command without an argument for its current value and help.

| Command | Value | Default |
| --- | --- | --- |
| `desru_volumes_show` | `0` / `1`: hide/show the entire viewer | `0` |
| `desru_volumes_fill` | `0`: outlines only; `1`: outlines with fill for all categories | `1` |
| `desru_triggers_show` | `0` / `1`: hide/show cyan triggers | `1` |
| `desru_collision_show` | `0` / `1`: hide/show green level collision | `1` |
| `desru_invisible_walls_show` | `0` / `1`: hide/show orange invisible walls | `1` |
| `desru_volumes_render_distance` | Finite distance in metres >= 0, e.g. `73.5` | `40` |

Categories, fill and distance persist in DESRU settings. Viewer visibility is session-only and starts off. The obsolete saved checkbox cannot start rendering. F-key controls are disabled.

Distance selects **whole objects** for all three categories. When an object's world bounds intersect the radius around the camera, all its original faces and contour edges are submitted, including portions farther away. Radius changes do not slice surfaces or rebuild the level. Normal screen/frustum clipping and occlusion by foreground surfaces still apply. Whole Havok leaf-shape instances are retained; compound world containers are not treated as one map-sized object.

The former fixed limit of 250,000 vertices (and half-budget fill) has been removed from both collection and GPU upload. Buffers grow to fit the admitted objects, so a dense near region no longer silently removes farther objects. Greater distances can increase rendering cost. Equal-distance objects keep a stable order.

Green level collision additionally removes surfaces and original contour segments buried inside other collision solids. Fully absorbed objects are omitted; partial intersections retain only the exterior patches, without artificial internal cut faces. This runs once in a separate low-priority background task after the initial level snapshot is published. Live trigger/wall refresh continues independently. The completed result replaces the snapshot atomically, preserves original per-object distance bounds and cancels when the level changes. Triggers and invisible walls keep their separate diagnostic meaning.

Exposed intersections retain a contour line along the cut boundary. These contours use the existing green line style and remain visible with `desru_volumes_fill 0`. Original edge/cut-plane provenance and oriented interval cancellation remove internal triangulation/BSP partition edges. Shared intersections have a stable owner, and parts buried by a third solid are clipped. Completely absorbed shapes leave no seam. New contours appear with the completed background surface snapshot; they do not alter fill triangles, shaders or depth handling.

Interior classification uses validated closed triangle topology and oriented face planes (including concave BSP volumes and cavities), with BVH overlap queries. Open/non-manifold or unresolved shapes never act as solid occluders. Their own surfaces may still be trimmed by a confirmed closed solid. Failed/cancelled work keeps the original visible scene.

The viewer follows the current map, refreshes live entities, hides in menus/pause/loading screens, clips around the game console and samples the game's depth buffer for occlusion. There is no executable-version whitelist. Engine reflection, RTTI and validated camera transforms resolve the running build; an unresolved profile is logged instead of reading unchecked addresses.

## Implementation and validation

The native Vulkan bridge lives in `NativeVisuals/`. DESRU loads an immutable copy into the game through its existing HUD-thread bridge and renews the feature lease. It adopts the existing device/queues and replaces the game's Vulkan dispatch slots. It does not install a global Vulkan layer or change persistent environment variables. During a DESRU binary update, previous callbacks remain loaded until game exit; their effects are disabled before the new bridge takes over.

The lighting fix adds missing indirect-command read dependencies. The viewer tracks depth/image/pass/submit events independently, including explicit depth write-to-read dependencies when attaching after cached render passes already exist. Projection snapshots embedded in the renderer support builds that do not expose the original viewer's view array.

Occlusion considers the depth texture's 2×2 sample footprint to reduce checkerboard holes on coplanar surfaces when the game's depth grid is jittered or scaled. Neighbour selection uses the surface's screen-space depth gradient as well as the existing percentage/bias, so grazing surfaces are not rejected solely for having a large depth change per texel. The gradient comes from the overlay surface, preserving foreground discontinuities instead of using a jump in the sampled depth itself. Depth selection prefers the full world depth range over a full-resolution restricted-range weapon pass.

Earlier builds were checked in a running **DOOM Eternal 1.0 Release** process with an **AMD Radeon RX 9070 XT**: real collision frames, triggers/walls, fractional distance and independent switching. The new six-command interface is covered by isolated native callback tests, including cheat gating, configuring a hidden viewer, invalid input and expired lease. Managed tests cover switch combinations, removed UI elements and Help text. These observations do not imply every game build or driver has been tested.

CPU tests cover depth freshness/ownership, late attachment, camera reconstruction, scene geometry and visibility. GPU tests cover the lighting barriers/events, viewport dispatch, reverse-Z occlusion and console clipping. The managed test suite covers modifier order/buffer length, UI bounds, native protocol and Help text.

Whole-object CPU tests cover all three categories at the radius boundary, fully outside objects, far faces/edges, a camera inside an object's bounds, radius growth and a dense 600,005-vertex frame. GPU readbacks verify an entire oblique object extending beyond the radius and forward far plane, plus visible geometry placed after 300,000 vertices to catch renderer-side truncation. Four alternating jitter frames are checked in normal depth, reverse-Z, 75% depth resolution and a foreground-wall case. Live counters on the tested 1.0 Release level now exceed the old cap: at 200 metres, 971,527 triangles and 1,148,503 collision edges were submitted. A further CPU optimization writes vertices into fixed disjoint ranges and uses four tasks for dense frames. The user confirmed that distance now changes. A further regression reproduced remaining fill/line holes at grazing angles; the adaptive footprint passes this case and its foreground-wall variant. The background optimization completed on the live map: 77,869 objects removed and 117,701 trimmed. Edge count fell, but triangulation increased, so this still needs performance work. The user reports remaining flicker. Windows recorded an AMD driver timeout/recovery at 22:10 on 19 October 2026; its cause is unresolved. New command/UI work does not claim to fix that incident, and no automatic viewer rendering is started during installation.

## Source and licenses

Based on [billymooreq/DoomEternal-v1.1--RTXfix-DrlaCollisionViewer](https://github.com/billymooreq/DoomEternal-v1.1--RTXfix-DrlaCollisionViewer), commit `e7adfa980a8a35af5a6f401b8eb6a47f02759d6b`. Its viewer derives from IlyaTsyganov's collision viewer. See `NativeVisuals/README.md` and the retained third-party notices for attribution and licensing.

Exterior-surface CPU tests cover complete nesting, duplicate ownership, partial overlaps, shared faces, open targets/shells, rotated bounds, reflected/nonuniform transforms, concave notches, real cavities, cancellation and background publication alongside entity updates.

Intersection-contour tests check analytic seam length, reversed object order, a third occluder, a cut exactly on a triangulation diagonal, mirrored/nonuniform transforms, open surfaces, concave BSP boundaries, overlapping cuts and zero-length contact, while retaining the existing fill-area checks.

World shapes now use validated `havokShapeData_t.contentFlags`: known projectile/query-only and no-collision shapes are excluded before interior trimming. Mixed compressed meshes use per-primitive tags. Unknown metadata stays visible conservatively. CPU tests cover both paths; live visual confirmation is pending.

After this build, the user reported that flicker was substantially reduced and accepted the current rendering on 19 October 2026. Residual flicker is not claimed to be eliminated; further rendering experiments are deferred unless requested.
