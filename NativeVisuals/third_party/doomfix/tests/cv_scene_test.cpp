// Based on doom-eternal-collision-viewer by IlyaTsyganov (commit 43ac57d), used with the author's permission.
//
// Модульный тест источника «память игры» collision-viewer без игры и без GPU (перенос
// tests/viewer_behavior_tests.cpp исходного viewer): поддельная память DOOM Eternal 1.1 (FakeGame) и
// настоящий код чтения. Проверяются: режимы и группы (F5–F8), непрозрачность заливки, дальности F9,
// F10, опрос клавиш по фронту и только на переднем плане, меню/пауза/диалоги, консоль, камера, стены и
// триггеры (включение, удаление, повторное появление, движение), сжатая сетка без диагоналей, обнаружение
// карты, мир Havok и BVH, отмена фоновой работы, быстрые смены карт, запрет старой геометрии, остановка
// и повторный запуск потока, деструктор без ожидания потока.
//
//   ctest --test-dir build-release            или   build-release\bin\doomfix_cv_scene_test.exe

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "features/collision_viewer/game_memory/exe_profile.h"
#include "features/collision_viewer/game_memory/fake_game.h"
#include "features/collision_viewer/game_memory/game_state.h"
#include "features/collision_viewer/game_memory/havok_shapes.h"
#include "features/collision_viewer/game_memory/live_entities.h"
#include "features/collision_viewer/game_memory/version_check.h"
#include "features/collision_viewer/game_memory_source.h"
#include "features/collision_viewer/scene/collect_vertices.h"
#include "features/collision_viewer/scene/display_settings.h"
#include "features/collision_viewer/scene/runtime_scene.h"

namespace {

using namespace doomfix::cv;
namespace exe = doomfix::cv::exe11;

int g_failures = 0;

void Check(bool ok, const char* what) {
  std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_failures;
}

// Поддельные клавиши для KeyPoller.
bool g_keys[256] = {};
bool g_foreground = true;
bool FakeKeyDown(int key) { return key >= 0 && key < 256 && g_keys[key]; }
bool FakeForeground() { return g_foreground; }

// Ждёт условие до timeout.
template <typename Fn>
bool WaitFor(Fn&& fn, int milliseconds = 3000) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
  while (std::chrono::steady_clock::now() < deadline) {
    if (fn()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return fn();
}

Camera ScreenCamera() {
  Camera c;
  c.matrix[0] = c.matrix[5] = c.matrix[10] = c.matrix[15] = 1.0f;
  return c;
}

// --- Отбор и вершины (как viewer_behavior_tests: синтетическая сцена из трёх групп) -------------------

void TestCollect() {
  std::printf("collect vertices\n");
  std::vector<Mesh> world;
  std::vector<Mesh> entities;
  for (uint32_t kind = 0; kind < 3; ++kind) {
    auto g = std::make_shared<Geometry>();
    g->kind = static_cast<MeshKind>(kind);
    g->triangles = {{-.2f, -.2f, .2f}, {.2f, -.2f, .2f}, {0, .2f, .2f}};
    g->lines = {{-.2f, -.2f, .2f}, {.2f, -.2f, .2f}};
    Mesh m;
    m.kind = g->kind;
    m.geometry = g;
    const float box[6] = {-.2f, -.2f, .2f, .2f, .2f, .2f};
    std::memcpy(m.bounds, box, sizeof(box));
    (kind == 2 ? entities : world).push_back(m);
  }
  std::vector<SpatialNode> nodes;
  std::vector<uint32_t> order;
  BuildBvh(world, nodes, order);
  SceneInput input{&world, &nodes, &order, &entities};
  const Camera camera = ScreenCamera();
  DisplaySettings settings;
  Check(settings.overlay && settings.triggers && settings.collision && settings.walls && settings.fill &&
            settings.radius == 40.0f,
        "start: all groups, contours with fill, 40 m");
  std::vector<Candidate> visible;
  OverlayScene scene;
  auto run = [&](const DisplaySettings& s) {
    scene.Clear();
    CollectVertices(camera, s, input, visible, scene);
  };
  auto triangleVertices = [&] {
    return !scene.draws.empty() && scene.draws[0].primitive == Primitive::kTriangles ? scene.draws[0].vertexCount
                                                                                       : 0u;
  };
  settings.fill = false;
  run(settings);
  Check(triangleVertices() == 0 && scene.vertices.size() == 6, "F5 contours: 3 groups, lines only");
  settings.fill = true;
  run(settings);
  Check(triangleVertices() == 9 && scene.vertices.size() == 15 && scene.draws.size() == 2,
        "F5 contours with fill: 9 fill vertices + 6 line vertices");
  bool alpha = true;
  for (unsigned i = 0; i < 9; ++i) alpha = alpha && scene.vertices[i].color[3] == kFillAlpha;
  Check(alpha && kFillAlpha == 0.09f, "fill opacity 9% for every group");
  Check(scene.vertices[0].color[2] == 1.0f && scene.vertices[0].color[0] < 0.2f, "triggers first, cyan");
  Check(scene.vertices[3].color[0] == 1.0f && scene.vertices[3].color[1] == 0.45f, "walls second, orange");
  Check(scene.vertices[6].color[1] == 0.95f, "collision last, green");
  Check(scene.views.size() == 1 && scene.views[0].occlusion && scene.views[0].depthMode == 2 &&
            scene.views[0].farDistance == 40.0f && scene.views[0].depthBias == 0.035f,
        "view: occlusion by the game's depth, reverse-Z, range 40 m");
  for (bool DisplaySettings::*toggle : {&DisplaySettings::triggers, &DisplaySettings::collision,
                                        &DisplaySettings::walls}) {
    DisplaySettings s = settings;
    s.*toggle = false;
    run(s);
    Check(triangleVertices() == 6 && scene.vertices.size() == 10, "F6/F7/F8: one group off, the others stay");
  }
  DisplaySettings none = settings;
  none.triggers = none.collision = none.walls = false;
  run(none);
  Check(scene.vertices.empty() && scene.draws.empty() && scene.views.empty(), "all groups off: nothing to draw");

  // Дальность и пирамида видимости.
  Camera distant = camera;
  distant.position[2] = -100.0f;
  scene.Clear();
  CollectVertices(distant, settings, input, visible, scene);
  Check(scene.vertices.empty(), "beyond the range: culled");
  DisplaySettings wide = settings;
  wide.radius = 200.0f;
  scene.Clear();
  CollectVertices(distant, wide, input, visible, scene);
  Check(!scene.vertices.empty(), "F9 range 200 m: visible again");
  Camera behind = FakeGame::DefaultCamera();
  behind.matrix[14] = -1.0f;  // смотрит вдоль -z, всё за спиной
  scene.Clear();
  CollectVertices(behind, settings, input, visible, scene);
  Check(scene.vertices.empty(), "behind the camera: culled by the view frustum");
  SceneInput loading{};
  scene.Clear();
  CollectVertices(camera, settings, loading, visible, scene);
  Check(scene.vertices.empty(), "no scene yet (loading): nothing, no stale fallback");

  // Dense selection: no fixed closest-vertex cap; keep every face and contour.
  std::vector<Mesh> many;
  auto big = std::make_shared<Geometry>();
  big->kind = MeshKind::kCollision;
  big->triangles.assign(30000, Vec3{0, 0, 0.5f});
  big->lines.assign(30000, Vec3{0, 0, 0.5f});
  for (int i = 0; i < 10; ++i) {
    Mesh m;
    m.kind = MeshKind::kCollision;
    m.geometry = big;
    m.transform.m[3] = i * .001f;
    const float box[6] = {-.1f, -.1f, .4f + i * .01f, .1f, .1f, .6f};
    std::memcpy(m.bounds, box, sizeof(box));
    many.push_back(m);
  }
  std::vector<SpatialNode> manyNodes;
  std::vector<uint32_t> manyOrder;
  BuildBvh(many, manyNodes, manyOrder);
  SceneInput heavy{&many, &manyNodes, &manyOrder, &entities};
  CollectStats stats;
  scene.Clear();
  CollectVertices(camera, settings, heavy, visible, scene, &stats);
  Check(triangleVertices() == 300003 && scene.vertices.size() == 600005 &&
            stats.drawn[1] == 10 && stats.drawn[2] == 1,
        "dense frame: all 600005 vertices retained, including complete fill and contours");
  Check(stats.examined == 10 && manyNodes.size() == 1, "BVH leaf of 10 instances examined");
  bool exact = true;
  for (uint32_t i = 3; i < 300003; ++i) {
    const Vertex& v = scene.vertices[i];
    exact &= v.position[0] == ((i - 3) / 30000) * .001f && v.position[1] == 0 && v.position[2] == .5f &&
             v.color[0] == .25f && v.color[1] == .95f && v.color[2] == .12f && v.color[3] == kFillAlpha;
  }
  for (uint32_t i = 300005; i < 600005; ++i) {
    const Vertex& v = scene.vertices[i];
    exact &= v.position[0] == ((i - 300005) / 30000) * .001f && v.position[1] == 0 && v.position[2] == .5f &&
             v.color[0] == .4f && v.color[1] == 1 && v.color[2] == .2f && v.color[3] == .65f;
  }
  Check(exact, "parallel dense emission preserves every transformed position, color and alpha across slices");
  scene.Clear();
  scene.vertices.resize(1);
  scene.vertices[0].position[0] = 123;
  CollectVertices(camera, settings, heavy, visible, scene);
  Check(scene.vertices.size() == 600006 && scene.vertices[0].position[0] == 123 &&
            scene.draws[0].firstVertex == 1 && scene.draws[1].firstVertex == 300004,
        "parallel emission appends after existing geometry without overwriting it or shifting draw ranges");
}

void TestWholeObjects() {
  std::vector<Mesh> world;
  for (uint32_t kind = 0; kind < 3; ++kind) {
    auto g = std::make_shared<Geometry>();
    g->kind = static_cast<MeshKind>(kind);
    g->triangles = {{-.2f, -.2f, 1}, {.2f, -.2f, 1}, {0, .2f, 1},
                    {-.2f, -.2f, 100}, {.2f, -.2f, 100}, {0, .2f, 100}};
    g->lines = {{-.2f, -.2f, 1}, {.2f, -.2f, 1}, {0, .2f, 1}, {0, .2f, 100}};
    Mesh m;
    m.kind = g->kind;
    m.geometry = g;
    ResetBounds(m.bounds);
    for (Vec3 p : g->triangles) ExpandBounds(m.bounds, p);
    for (Vec3 p : g->lines) ExpandBounds(m.bounds, p);
    world.push_back(m);
  }
  std::vector<SpatialNode> nodes;
  std::vector<uint32_t> order;
  BuildBvh(world, nodes, order);
  SceneInput input{&world, &nodes, &order, nullptr};
  OverlayScene scene;
  std::vector<Candidate> visible;
  DisplaySettings settings;
  settings.radius = .99f;
  CollectVertices(ScreenCamera(), settings, input, visible, scene);
  Check(scene.vertices.empty(), "all categories: a wholly outside object is omitted");
  settings.radius = 1.0f;
  scene.Clear();
  CollectVertices(ScreenCamera(), settings, input, visible, scene);
  Check(scene.vertices.size() == 30 && visible.size() == 3,
        "touching the radius admits complete triggers, collision and invisible walls");
  size_t distant = 0;
  for (const Vertex& v : scene.vertices) if (v.position[2] == 100) ++distant;
  Check(distant == 12, "all far faces and edges of admitted objects are emitted unchanged");
  settings.radius = 200;
  scene.Clear();
  CollectVertices(ScreenCamera(), settings, input, visible, scene);
  Check(scene.vertices.size() == 30, "a larger radius does not change parts of an already admitted object");
  Camera inside = ScreenCamera();
  inside.position[2] = 50;
  settings.radius = 0;
  scene.Clear();
  CollectVertices(inside, settings, input, visible, scene);
  Check(scene.vertices.size() == 30, "camera inside an object's bounds retains that whole object at radius zero");

  // A separate object outside the radius appears when that radius grows.
  auto distantGeometry = std::make_shared<Geometry>();
  distantGeometry->triangles = {{0, 0, 10}, {.1f, 0, 10}, {0, .1f, 10}};
  Mesh extra;
  extra.kind = MeshKind::kCollision;
  extra.geometry = distantGeometry;
  ResetBounds(extra.bounds);
  for (Vec3 p : distantGeometry->triangles) ExpandBounds(extra.bounds, p);
  world.push_back(extra);
  // Duplicate bounds also exercise deterministic ordering of equal-distance objects.
  world.push_back(world[1]);
  BuildBvh(world, nodes, order);
  settings.radius = 5;
  scene.Clear();
  CollectVertices(ScreenCamera(), settings, input, visible, scene);
  Check(scene.vertices.size() == 40 && visible.size() == 4, "small radius excludes the separate distant object");
  settings.radius = 15;
  scene.Clear();
  CollectVertices(ScreenCamera(), settings, input, visible, scene);
  Check(scene.vertices.size() == 43 && visible.size() == 5, "growing the radius includes the separate distant object");
  std::vector<const Mesh*> first;
  for (auto c : visible) first.push_back(c.mesh);
  std::reverse(order.begin(), order.end());
  scene.Clear();
  CollectVertices(ScreenCamera(), settings, input, visible, scene);
  bool stable = first.size() == visible.size();
  for (size_t i = 0; i < first.size() && stable; ++i) stable &= first[i] == visible[i].mesh;
  Check(stable, "equal-distance objects keep a stable order despite changing BVH traversal");
}

void TestKeys() {
  std::printf("keys F5-F10\n");
  Check(NextRadius(20) == 40 && NextRadius(40) == 100 && NextRadius(100) == 200 && NextRadius(200) == 20,
        "F9 cycle 40 -> 100 -> 200 -> 20 -> 40");
  DisplaySettings s;
  ApplyKey(s, DisplayKey::kF5);
  ApplyKey(s, DisplayKey::kF6);
  ApplyKey(s, DisplayKey::kF7);
  ApplyKey(s, DisplayKey::kF8);
  ApplyKey(s, DisplayKey::kF9);
  ApplyKey(s, DisplayKey::kF10);
  Check(!s.fill && !s.triggers && !s.collision && !s.walls && s.radius == 100.0f && !s.overlay,
        "each key changes its own setting");
  Check(KeyPoller::VirtualKey(DisplayKey::kF5) == VK_F5 && KeyPoller::VirtualKey(DisplayKey::kF10) == VK_F10,
        "virtual key codes F5..F10");
  KeyPoller poller(FakeKeyDown, FakeForeground);
  int presses = 0;
  auto count = [&](DisplayKey) { ++presses; };
  g_keys[VK_F9] = true;
  poller.Poll(count);
  poller.Poll(count);
  Check(presses == 1, "a held key counts once (press edge)");
  g_keys[VK_F9] = false;
  poller.Poll(count);
  g_keys[VK_F9] = true;
  poller.Poll(count);
  Check(presses == 2, "released and pressed again: counts again");
  g_keys[VK_F9] = false;
  poller.Poll(count);
  g_foreground = false;
  g_keys[VK_F6] = true;
  poller.Poll(count);
  Check(presses == 2, "game window not in the foreground: ignored");
  g_foreground = true;
  poller.Poll(count);
  Check(presses == 2, "key already down when the window comes back: no press");
  g_keys[VK_F6] = false;
}

// --- Поддельная память игры -------------------------------------------------------------------------------

void TestGameState(FakeGame& game) {
  std::printf("game state (fake 1.1 memory)\n");
  const MemoryReader memory;
  const uintptr_t b = game.Base();
  const auto& o = game.objects;
  Check(!CheckExeVersion(b).known, "fake memory is not taken for the 1.1 exe");
  Check(CheckExeVersion(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))).known == false,
        "the test exe is not the 1.1 exe");

  Camera camera;
  Check(ReadGameCamera(memory, b, camera) && camera.matrix[14] == 1.0f, "camera of view 0");
  game.Put<float>(o.view + exe::kViewMatrix + 14 * 4, 2.0f);
  Check(!ReadGameCamera(memory, b, camera), "camera with a non-unit forward row rejected");
  game.Put<float>(o.view + exe::kViewMatrix + 14 * 4, NAN);
  Check(!ReadGameCamera(memory, b, camera), "camera with NaN rejected");
  game.SetCamera(FakeGame::DefaultCamera());
  game.Put<int>(o.renderer + exe::kRendererViewCount, 0);
  Check(!ReadGameCamera(memory, b, camera), "no views: no camera");
  game.Put<int>(o.renderer + exe::kRendererViewCount, 1);

  Check(ReadConsoleFraction(memory, b) == 0.0f, "console closed");
  game.SetConsole(0.5f, false);
  Check(ReadConsoleFraction(memory, b) == 0.5f, "console fraction 0.5");
  game.SetConsole(0.5f, true);
  Check(ReadConsoleFraction(memory, b) == 1.0f, "forced full console");
  game.SetConsole(NAN, false);
  Check(ReadConsoleFraction(memory, b) == 1.0f, "bad fraction: treated as full");
  game.SetConsole(0.0f, false);
  game.Put<uintptr_t>(b + exe::kConsoleRva, 0);
  Check(ReadConsoleFraction(memory, b) == 0.0f, "console not found: 0");
  game.Put<uintptr_t>(b + exe::kConsoleRva, b + exe::kConsoleVtableRva);

  Check(GameplayVisible(memory, b), "gameplay visible");
  const uintptr_t hud = o.player + exe::kPlayerHud;
  game.Put<uint8_t>(hud + exe::kHudPaused, 1);
  Check(!GameplayVisible(memory, b), "pause hidden");
  game.Put<uint8_t>(hud + exe::kHudPaused, 0);
  game.Put<int>(o.menu + exe::kMenuScreen, 2);
  Check(!GameplayVisible(memory, b), "HUD menu hidden");
  game.Put<uint8_t>(o.menu + exe::kMenuHidden, 1);
  Check(GameplayVisible(memory, b), "explicitly hidden menu ignored");
  game.Put<uint8_t>(o.menu + exe::kMenuHidden, 0);
  game.Put<int>(o.menu + exe::kMenuScreen, -1);
  game.Put<int>(o.menu + exe::kMenuNextScreen, 2);
  Check(!GameplayVisible(memory, b), "menu transition hidden");
  game.Put<int>(o.menu + exe::kMenuNextScreen, -1);
  game.Put<int>(o.menu + exe::kMenuPopups, 1);
  Check(!GameplayVisible(memory, b), "popup hidden");
  game.Put<int>(o.menu + exe::kMenuPopups, 0);
  game.Put<uint8_t>(o.menu + exe::kMenuBlocking, 1);
  Check(!GameplayVisible(memory, b), "dialog hidden");
  game.Put<uint8_t>(o.menu + exe::kMenuBlocking, 0);
  game.Put<uint32_t>(o.system + exe::kGameSystemState, 0);
  Check(!GameplayVisible(memory, b), "main menu/loading hidden");
  game.Put<uint32_t>(o.system + exe::kGameSystemState, exe::kStateInLevel);
  game.Put<uint8_t>(o.player + exe::kEntityRemoved, 1);
  Check(!GameplayVisible(memory, b), "removed player hidden");
  game.Put<uint8_t>(o.player + exe::kEntityRemoved, 0);
  game.Put<uintptr_t>(o.player, 0x1234);
  Check(!GameplayVisible(memory, b), "not a player (wrong vtable) hidden");
  game.Put<uintptr_t>(o.player, b + exe::kPlayerVtableRva);
  game.Put<uintptr_t>(o.map + exe::kMapPlayer, 0);
  Check(!GameplayVisible(memory, b), "no level player hidden");
  game.Put<uintptr_t>(o.map + exe::kMapPlayer, o.player);
  Check(GameplayVisible(memory, b), "gameplay visible again");
}

void TestShapesAndEntities(FakeGame& game) {
  std::printf("Havok shapes and live entities\n");
  const uintptr_t b = game.Base();
  const auto& o = game.objects;
  ShapeDecoder decoder;
  Geometry mesh;
  Check(decoder.Decode(o.meshShape, Transform{}, mesh) && mesh.triangles.size() == 6 && mesh.lines.size() == 8,
        "compressed mesh: 2 triangles, 4 contour edges (coplanar diagonal removed)");
  Geometry box;
  Check(decoder.Decode(o.floorShape, Transform{}, box) && box.triangles.size() == 36 && box.lines.size() == 24,
        "convex box: 12 triangles, 12 edges (no face diagonals)");
  const uintptr_t odd = game.Allocate(exe::kShapeReadSize);
  game.Put<uint8_t>(odd + exe::kShapeType, 5);
  Geometry none;
  Check(!decoder.Decode(odd, Transform{}, none) && decoder.Error() == "shape type 5", "unsupported shape type");

  EntityScanner scanner;
  scanner.Refresh(b, true);
  Check(scanner.meshes.size() == 2 && scanner.walls == 1 && scanner.triggers == 1 && scanner.unsupported == 0,
        "one wall and one trigger found");
  const Mesh* wall = nullptr;
  for (const Mesh& m : scanner.meshes) {
    if (m.kind == MeshKind::kWall) wall = &m;
  }
  Check(wall && wall->geometry->triangles.size() == 36 && wall->geometry->lines.size() == 24,
        "exact live convex topology");
  Check(wall && std::fabs(wall->bounds[2] - 3.5f) < 1e-4f && std::fabs(wall->bounds[5] - 3.6f) < 1e-4f,
        "live world transform");
  scanner.Refresh(b, false);
  Check(scanner.meshes.size() == 1 && scanner.triggers == 0, "without triggers: walls only");
  game.Put<uint32_t>(o.wallPhysics + exe::kPhysicsContents, exe::kContentsNoCollision | exe::kContentsPlayerClip);
  scanner.Refresh(b, true);
  Check(scanner.walls == 0, "wall with NOCOLLISION disappears");
  game.Put<uint32_t>(o.wallPhysics + exe::kPhysicsContents, exe::kContentsPlayerClip | exe::kContentsSolid);
  scanner.Refresh(b, true);
  Check(scanner.walls == 0, "solid geometry is not an invisible wall");
  game.Put<uint32_t>(o.wallPhysics + exe::kPhysicsContents, exe::kContentsPlayerClip);
  game.Put<uint8_t>(o.wall + exe::kEntityRemoved, 1);
  scanner.Refresh(b, true);
  Check(scanner.walls == 0, "deleted entity disappears");
  game.Put<uint8_t>(o.wall + exe::kEntityRemoved, 0);
  game.Put<uint8_t>(o.triggerPhysics + exe::kPhysicsDisabled, 1);
  scanner.Refresh(b, true);
  Check(scanner.triggers == 0 && scanner.walls == 1, "disabled trigger disappears");
  game.Put<uint8_t>(o.triggerPhysics + exe::kPhysicsDisabled, 0);
  game.Put<uint32_t>(o.map + exe::kMapEntityCount, 0);
  scanner.Refresh(b, true);
  Check(scanner.meshes.empty(), "despawned slots disappear");
  game.Put<uint32_t>(o.map + exe::kMapEntityCount, 2);
  game.Put<float>(o.wallPhysics + exe::kPhysicsOrigin, 12.0f);
  scanner.Refresh(b, true);
  wall = nullptr;
  for (const Mesh& m : scanner.meshes) {
    if (m.kind == MeshKind::kWall) wall = &m;
  }
  Check(scanner.meshes.size() == 2 && wall && std::fabs(wall->bounds[0] - 10.5f) < 1e-4f,
        "respawn and movement refreshed");
  game.Put<float>(o.wallPhysics + exe::kPhysicsOrigin, 0.0f);
  game.Put<uint32_t>(o.wallPhysics + exe::kPhysicsType, exe::kPhysicsRigidBody);
  scanner.Refresh(b, true);
  Check(scanner.walls == 0, "rigid wall outside the Havok world hidden");
  game.Put<uint8_t>(o.wallPhysics + exe::kPhysicsInWorld, 1);
  scanner.Refresh(b, true);
  Check(scanner.walls == 1, "rigid wall added to the world");
  game.Put<uint32_t>(o.wallPhysics + exe::kPhysicsType, exe::kPhysicsStatic);
  game.Put<uint8_t>(o.wallPhysics + exe::kPhysicsInWorld, 0);
}

void TestLevelAndWorld(FakeGame& game) {
  std::printf("map discovery, world and background thread\n");
  const MemoryReader memory;
  const uintptr_t b = game.Base();
  const auto& o = game.objects;
  const LevelIdentity levelA = ReadLevelIdentity(memory, b);
  Check(levelA && levelA.name == "game/sp/test_a/test_a" && levelA.world == o.world, "current map identity");
  game.Put<uint32_t>(o.system + exe::kGameSystemState, 0);
  Check(!ReadLevelIdentity(memory, b), "no level while loading");
  game.Put<uint32_t>(o.system + exe::kGameSystemState, exe::kStateInLevel);

  RuntimeSceneBuilder builder;
  auto world = builder.Build(levelA);
  Check(world && world->meshes.size() == 2 && world->triangles == 14 && world->unique == 2 &&
            world->unsupported == 0 && !world->nodes.empty(),
        "runtime world decoding (box + compressed mesh) and BVH");
  Check(world && std::fabs(world->meshes[1].bounds[1] - 1.0f) < 1e-4f &&
            std::fabs(world->meshes[1].bounds[5] - 4.0f) < 1e-3f,
        "compound instance transform applied");
  builder.cancelled = [] { return true; };
  Check(!builder.Build(levelA), "obsolete build cancelled");

  RuntimeSceneManager manager;
  Check(!manager.Running(), "thread starts lazily");
  manager.Request(levelA, b);
  std::shared_ptr<const RuntimeScene> ready;
  std::shared_ptr<const RuntimeEntities> live;
  WaitFor([&] {
    manager.Snapshot(levelA, ready, live);
    return ready && live;
  });
  Check(ready && ready->level == levelA && live && live->walls == 1 && live->triggers == 1,
        "background publication for the current map");
  Check(WaitFor([&] {manager.Snapshot(levelA,ready,live);return ready&&ready->optimized;}) &&
            ready->optimization.originalTriangles==14 && live && live->walls==1,
        "exterior optimization publishes a completed snapshot while the entity worker keeps running");
  LevelIdentity levelB = levelA;
  levelB.name = "game/sp/test_b/test_b";
  manager.Snapshot(levelB, ready, live);
  Check(!ready && !live, "never returns another map's snapshot");
  manager.Request(levelB, b);
  manager.Snapshot(levelA, ready, live);
  Check(!ready && !live, "old map removed immediately on transition");
  manager.Request(levelA, b);
  WaitFor([&] {
    manager.Snapshot(levelA, ready, live);
    return bool(ready);
  });
  Check(ready && ready->level == levelA, "rapid level switch resolves to the latest request");
  manager.Stop();
  Check(!manager.Running(), "Stop joins the thread");
  manager.Snapshot(levelA, ready, live);
  Check(!ready && !live, "Stop drops the snapshots");
  manager.Request(levelA, b);
  WaitFor([&] {
    manager.Snapshot(levelA, ready, live);
    return bool(ready);
  });
  Check(manager.Running() && ready, "Request after Stop starts the thread again");
  manager.Stop();
  ready.reset();
  live.reset();

  // Деструктор работающего менеджера не ждёт поток (выход игры без vkDestroyInstance).
  const auto start = std::chrono::steady_clock::now();
  {
    auto running = std::make_unique<RuntimeSceneManager>();
    running->Request(levelA, b);
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  Check(elapsed < std::chrono::milliseconds(500), "destructor of a running manager returns at once");

  game.SetMapName("game/sp/test_b/test_b");
  Check(ReadLevelIdentity(memory, b) != levelA, "map name detects a transition even if pointers are reused");
  game.SetMapName("game/sp/test_a/test_a");
}

void TestShapeContents() {
  FakeGame game;
  const auto& o = game.objects;
  const uintptr_t wrapper = game.Allocate(0x58);
  const uintptr_t table = game.Allocate(4 * exe::kShapeDataSize);
  game.Put<uintptr_t>(wrapper + exe::kShapeDataList, table);
  game.Put<uint32_t>(wrapper + exe::kShapeDataCount, 4);
  const uint32_t contents[] = {exe::kContentsSolid, 0x1c2, exe::kContentsPlayerClip,
                               exe::kContentsSolid | exe::kContentsNoCollision};
  for (unsigned i = 0; i < 4; ++i) {
    game.Put<uintptr_t>(table + i * exe::kShapeDataSize + exe::kShapeDataRoot, wrapper);
    game.Put<uint32_t>(table + i * exe::kShapeDataSize + exe::kShapeDataContents, contents[i]);
  }
  game.Put<uintptr_t>(o.floorShape + exe::kShapeUserData, table);
  game.Put<uintptr_t>(o.meshShape + exe::kShapeUserData, table + exe::kShapeDataSize);
  ShapeContentsFilter filter;
  Check(filter.AllowsShape(o.floorShape) && !filter.AllowsShape(o.meshShape),
        "player-solid shape retained, opaque/projectile-only shape excluded");
  Check(filter.AllowsTag(o.floorShape, 2) && !filter.AllowsTag(o.floorShape, 3) &&
            filter.AllowsTag(o.floorShape, 0xffff) && filter.AllowsTag(o.floorShape, 500),
        "playerclip retained, no-collision excluded, unknown tags preserved");
  RuntimeSceneBuilder builder;
  auto scene = builder.Build(ReadLevelIdentity({}, game.Base()));
  Check(scene && scene->meshes.size() == 1 && scene->nonBlocking == 1,
        "nonblocking world leaves are removed before surface optimization");
  // Split the test mesh into two triangle primitives with independent tags.
  game.Put<uintptr_t>(o.meshShape + exe::kShapeUserData, table);
  const uintptr_t tree = game.Get<uintptr_t>(o.meshShape + exe::kMeshData) + exe::kMeshTree;
  const uintptr_t section = game.Get<uintptr_t>(tree + exe::kTreeSections);
  const uintptr_t primitives = game.Allocate(8);
  const unsigned char indices[] = {0, 1, 2, 2, 0, 2, 3, 3};
  game.Write(primitives, indices, sizeof(indices));
  game.Put<uintptr_t>(tree + exe::kTreePrimitives, primitives);
  game.Put<uint32_t>(tree + exe::kTreePrimitives + 8, 2);
  game.Put<uint32_t>(section + exe::kSectionPrimitives, 2);
  const uintptr_t runs = game.Allocate(8);
  const unsigned char tags[] = {0, 0, 0, 1, 1, 0, 1, 1};
  game.Write(runs, tags, sizeof(tags));
  game.Put<uintptr_t>(tree + exe::kTreeDataRuns, runs);
  game.Put<uint32_t>(tree + exe::kTreeDataRuns + 8, 2);
  game.Put<uint32_t>(section + exe::kSectionDataRuns, 2);
  Geometry original, filtered;
  ShapeDecoder plain;
  ShapeDecoder decoder({}, &filter);
  Check(plain.Decode(o.meshShape, {}, original) && original.triangles.size() == 6 &&
            decoder.Decode(o.meshShape, {}, filtered) && filtered.triangles.size() == 3 &&
            filtered.lines.size() == 6,
        "mixed compressed mesh filters individual triangles and rebuilds their outlines");
  const uintptr_t unknown = game.Allocate(112);
  game.Put<uintptr_t>(unknown + exe::kShapeUserData, table + 1);
  Check(filter.AllowsShape(unknown), "arbitrary or misaligned userData is not a content flag");
  game.Put<uintptr_t>(table + exe::kShapeDataSize + exe::kShapeDataRoot, 0);
  filter.Clear();
  Check(filter.AllowsShape(o.meshShape), "inconsistent metadata table stays visible conservatively");
}

void TestSource(FakeGame& game) {
  std::printf("game memory source (end to end)\n");
  const auto& o = game.objects;
  g_keys[VK_F9] = g_keys[VK_F10] = false;
  GameMemorySource source(game.Base(), MemoryReader{}, KeyPoller(FakeKeyDown, FakeForeground));
  Camera camera;
  Check(source.ReadCamera(camera), "camera in gameplay");
  FrameInfo frame;
  frame.width = 128;
  frame.height = 128;
  OverlayScene scene;
  frame.depthFound = false;
  Check(!source.BuildScene(frame, camera, scene), "no depth buffer: nothing drawn");
  frame.depthFound = true;
  auto build = [&] {
    scene.Clear();
    return source.BuildScene(frame, camera, scene);
  };
  // Мир строится в фоне (сначала сущности, потом коллизии). Ящик — 36 вершин заливки и 24 контура (их
  // три: плита, стена, триггер), сетка — 6 и 8.
  constexpr size_t kFull = 3 * (36 + 24) + 6 + 8;
  WaitFor([&] { return build() && scene.vertices.size() == kFull; });
  Check(scene.vertices.size() == kFull && scene.draws.size() == 2 && scene.views.size() == 1,
        "fake level: three boxes and the mesh, fill and contours");
  Check(scene.consoleFraction == 0.0f, "console closed: no clipping");
  game.SetConsole(0.5f, false);
  build();
  Check(scene.consoleFraction == 0.5f && !scene.draws.empty(), "console 50%: passed to the renderer");
  game.SetConsole(1.0f, false);
  Check(!build(), "console over the whole screen: nothing drawn");
  game.SetConsole(0.0f, false);

  game.Put<uint8_t>(o.player + exe::kPlayerHud + exe::kHudPaused, 1);
  Check(!source.ReadCamera(camera), "pause: overlay hidden");
  game.Put<uint8_t>(o.player + exe::kPlayerHud + exe::kHudPaused, 0);
  Check(source.ReadCamera(camera), "unpaused: shown again");

  g_keys[VK_F10] = true;
  Check(!source.ReadCamera(camera), "F10: overlay off");
  g_keys[VK_F10] = false;
  source.ReadCamera(camera);
  g_keys[VK_F10] = true;
  Check(source.ReadCamera(camera), "F10 again: overlay on");
  g_keys[VK_F10] = false;
  g_keys[VK_F9] = true;
  source.ReadCamera(camera);
  g_keys[VK_F9] = false;
  Check(source.Settings().radius == 100.0f, "F9: range 100 m");
  build();
  Check(!scene.views.empty() && scene.views[0].farDistance == 100.0f, "range reaches the view");

  // Смена уровня: мир уровня B — только плита. Старая геометрия (сетка уровня A) не должна появиться ни
  // в одном кадре; новая появляется после сборки в фоне.
  const uintptr_t worldB = game.Allocate(exe::kShapeReadSize);
  const uintptr_t instanceB = game.Allocate(exe::kInstanceSize);
  game.Write(instanceB, reinterpret_cast<const void*>(game.Get<uintptr_t>(o.world + exe::kCompoundInstances)),
             exe::kInstanceSize);
  game.Put<uint8_t>(worldB + exe::kShapeType, exe::kShapeCompound);
  game.Put<uintptr_t>(worldB + exe::kCompoundInstances, instanceB);
  game.Put<uint32_t>(worldB + exe::kCompoundCount, 1);
  game.SetMapName("game/sp/test_b/test_b");
  game.Put<uintptr_t>(o.worldWrapper, worldB);
  bool sawOld = false;
  const bool levelB = WaitFor([&] {
    build();
    sawOld = sawOld || scene.vertices.size() == kFull;
    return scene.vertices.size() == 3 * (36 + 24);
  });
  Check(levelB && !sawOld, "level change: old geometry never drawn, the new scene comes");
  Check(source.Settings().radius == 100.0f, "settings survive the level change");
  game.Put<uintptr_t>(o.worldWrapper, o.world);
  game.SetMapName("game/sp/test_a/test_a");

  source.Stop();
  Check(source.Stopped(), "Stop stops the background thread");
  Check(WaitFor([&] { return build() && !scene.vertices.empty(); }), "after Stop the next frame restarts it");
  source.Stop();
}

}  // namespace

int main() {
  TestKeys();
  TestCollect();
  TestWholeObjects();
  FakeGame game;
  if (!game.Valid()) {
    std::printf("FAIL: cannot reserve the fake game memory\n");
    return 1;
  }
  TestGameState(game);
  TestShapesAndEntities(game);
  TestLevelAndWorld(game);
  TestShapeContents();
  TestSource(game);
  if (g_failures) {
    std::printf("\n%d check(s) FAILED\n", g_failures);
    return 1;
  }
  std::printf("\nPASSED\n");
  return 0;
}
