// Based on doom-eternal-collision-viewer by IlyaTsyganov (commit 43ac57d), used with the author's permission.

#include "features/collision_viewer/game_memory/fake_game.h"

#include <windows.h>

#include <cstdint>

#include "features/collision_viewer/game_memory/exe_profile.h"

namespace doomfix::cv {
namespace {

using namespace exe11;

constexpr uintptr_t kPage = 0x1000;

// Где в поддельном образе лежат vtable триггера, код его GetType() и описание типа.
constexpr uintptr_t kTriggerVtableRva = 0x3000000;   // < kMaxVtableRva
constexpr uintptr_t kTriggerGetterRva = 0x2400000;   // < kMaxCodeRva
constexpr uintptr_t kTriggerTypeInfoRva = 0x2400100;
constexpr uintptr_t kTriggerNameRva = 0x2400200;

}  // namespace

FakeGame::FakeGame() {
  image_ = static_cast<unsigned char*>(VirtualAlloc(nullptr, kImageSize, MEM_RESERVE, PAGE_READWRITE));
  heap_ = static_cast<unsigned char*>(VirtualAlloc(nullptr, kHeapSize, MEM_RESERVE, PAGE_READWRITE));
  if (Valid()) Build();
}

FakeGame::~FakeGame() {
  if (image_) VirtualFree(image_, 0, MEM_RELEASE);
  if (heap_) VirtualFree(heap_, 0, MEM_RELEASE);
}

void FakeGame::Commit(uintptr_t address, size_t size) {
  const uintptr_t first = address & ~(kPage - 1);
  const uintptr_t last = (address + size + kPage - 1) & ~(kPage - 1);
  VirtualAlloc(reinterpret_cast<void*>(first), last - first, MEM_COMMIT, PAGE_READWRITE);
}

void FakeGame::Write(uintptr_t address, const void* data, size_t size) {
  Commit(address, size);
  std::memcpy(reinterpret_cast<void*>(address), data, size);
}

uintptr_t FakeGame::Allocate(size_t size) {
  heapUsed_ = (heapUsed_ + 15) & ~size_t(15);
  if (heapUsed_ + size > kHeapSize) return 0;
  const uintptr_t address = reinterpret_cast<uintptr_t>(heap_) + heapUsed_;
  heapUsed_ += size;
  if (size <= 0x10000) Commit(address, size);  // большие объекты — по страницам, по мере записи
  return address;
}

Camera FakeGame::DefaultCamera() {
  Camera c;
  c.matrix[0] = 1.0f;   // x
  c.matrix[5] = 1.0f;   // y
  c.matrix[10] = 1.0f;  // z: обычная глубина 1 - 0.1 / расстояние (шейдер переводит из reverse-Z)
  c.matrix[11] = -0.1f;
  c.matrix[14] = 1.0f;  // w — расстояние вдоль взгляда
  return c;
}

void FakeGame::SetCamera(const Camera& camera) {
  Write(objects.view + kViewPosition, camera.position, sizeof(camera.position));
  Write(objects.view + kViewMatrix, camera.matrix, sizeof(camera.matrix));
}

void FakeGame::SetConsole(float fraction, bool forced) {
  Put<float>(Base() + kConsoleRva + kConsoleFraction, fraction);
  Put<uint8_t>(Base() + kConsoleRva + kConsoleForced, forced ? 1 : 0);
}

void FakeGame::SetMapName(const std::string& name) {
  objects.mapName = Allocate(name.size() + 1);
  Write(objects.mapName, name.c_str(), name.size() + 1);
  Put<uintptr_t>(objects.map + kMapName, objects.mapName);
  Put<uint32_t>(objects.map + kMapNameLength, static_cast<uint32_t>(name.size()));
}

uintptr_t FakeGame::MakeBox(float hx, float hy, float hz) {
  const uintptr_t shape = Allocate(0x200);
  Put<uint16_t>(shape + kShapeFlags, kShapeFlagConvex);
  Put<uint8_t>(shape + kShapeType, 1);
  // Массивы: число и смещение от поля числа.
  constexpr uintptr_t kVertices = 0x70, kFaces = 0xf0, kCorners = 0x108;
  Put<uint16_t>(shape + kConvexVertices, 8);
  Put<uint16_t>(shape + kConvexVertices + 2, static_cast<uint16_t>(kVertices - kConvexVertices));
  Put<uint16_t>(shape + kConvexFaces, 6);
  Put<uint16_t>(shape + kConvexFaces + 2, static_cast<uint16_t>(kFaces - kConvexFaces));
  Put<uint16_t>(shape + kConvexIndices, 24);
  Put<uint16_t>(shape + kConvexIndices + 2, static_cast<uint16_t>(kCorners - kConvexIndices));
  for (unsigned i = 0; i < 8; ++i) {
    const float point[4] = {i & 1 ? hx : -hx, i & 2 ? hy : -hy, i & 4 ? hz : -hz, 0.0f};
    Write(shape + kVertices + 16 * i, point, 16);
  }
  const uint8_t corners[24] = {0, 1, 3, 2, 4, 6, 7, 5, 0, 4, 5, 1, 2, 3, 7, 6, 0, 2, 6, 4, 1, 5, 7, 3};
  Write(shape + kCorners, corners, sizeof(corners));
  for (unsigned i = 0; i < 6; ++i) {
    Put<uint16_t>(shape + kFaces + i * 4, static_cast<uint16_t>(i * 4));
    Put<uint8_t>(shape + kFaces + i * 4 + 2, 4);
  }
  return shape;
}

void FakeGame::Build() {
  Objects& o = objects;
  const uintptr_t base = Base();

  // idGameSystemLocal -> карта -> игрок (HUD с одним закрытым меню).
  o.system = Allocate(0x60);
  Put<uintptr_t>(base + kGameSystemRva, o.system);
  Put<uint32_t>(o.system + kGameSystemState, kStateInLevel);
  o.map = Allocate(0x61cc0);
  Put<uintptr_t>(o.system + kGameSystemMap, o.map);
  SetMapName("game/sp/test_a/test_a");
  o.player = Allocate(0x36400);
  Put<uintptr_t>(o.map + kMapPlayer, o.player);
  Put<uintptr_t>(o.player, base + kPlayerVtableRva);
  Put<uint8_t>(o.player + kEntityRemoved, 0);
  o.menu = Allocate(kMenuReadSize);
  Put<int>(o.menu + kMenuScreen, -1);
  Put<int>(o.menu + kMenuNextScreen, -1);
  o.menus = Allocate(8);
  Put<uintptr_t>(o.menus, o.menu);
  Put<uintptr_t>(o.player + kPlayerHud + kHudMenus, o.menus);
  Put<uint32_t>(o.player + kPlayerHud + kHudMenuCount, 1);
  Put<uint8_t>(o.player + kPlayerHud + kHudPaused, 0);

  // Консоль закрыта.
  Put<uintptr_t>(base + kConsoleRva, base + kConsoleVtableRva);
  SetConsole(0.0f, false);

  // Рендерер с одним видом.
  o.renderer = Allocate(kRendererViewCount + 8);
  Put<uintptr_t>(base + kRendererRva, o.renderer);
  o.view = Allocate(kViewMatrix + 64);
  const uintptr_t views = Allocate(8);
  Put<uintptr_t>(views, o.view);
  Put<uintptr_t>(o.renderer + kRendererViews, views);
  Put<int>(o.renderer + kRendererViewCount, 1);
  SetCamera(DefaultCamera());

  // Мир Havok: физика игрока -> idHavokPhysics -> обёртка -> составная форма из двух экземпляров.
  o.floorShape = MakeBox(1.5f, 0.2f, 0.5f);
  o.meshShape = Allocate(kShapeReadSize);
  Put<uint8_t>(o.meshShape + kShapeType, kShapeCompressedMesh);
  {
    // Сжатая сетка: одна секция, четырёхугольник из 3 упакованных вершин и 1 общей. Шаг кодека 1 см.
    const uintptr_t data = Allocate(0x100);
    Put<uintptr_t>(o.meshShape + kMeshData, data);
    const uintptr_t tree = data + kMeshTree;
    const float domain[8] = {-2, -2, 0, 0, 2, 2, 8, 0};
    Write(tree + kTreeDomain, domain, sizeof(domain));
    auto array = [&](uintptr_t field, uintptr_t items, uint32_t count) {
      Put<uintptr_t>(tree + field, items);
      Put<uint32_t>(tree + field + 8, count);
    };
    const uintptr_t section = Allocate(kSectionSize);
    const float codec[6] = {-1.5f, 0.0f, 0.0f, 0.01f, 0.01f, 0.01f};
    Write(section + kSectionCodec, codec, sizeof(codec));
    Put<uint32_t>(section + kSectionFirstPacked, 0);
    Put<uint32_t>(section + kSectionShared, 0);       // общие индексы с 0
    Put<uint32_t>(section + kSectionPrimitives, 1);   // примитивы с 0, один
    Put<uint8_t>(section + kSectionPackedCount, 3);
    Put<uint8_t>(section + kSectionSharedCount, 1);
    array(kTreeSections, section, 1);
    const uintptr_t packed = Allocate(12);
    const uint32_t points[3] = {0u, 300u, 300u | (100u << 22)};  // (0,0,0), (3,0,0), (3,0,1) м от кодека
    Write(packed, points, sizeof(points));
    array(kTreePackedVertices, packed, 3);
    // Общая вершина (-1.5, 0, 1): 21+21+22 бита в домене.
    const auto q = [](float v, float lo, float hi, uint64_t max) {
      return static_cast<uint64_t>((v - lo) / (hi - lo) * float(max) + 0.5f);
    };
    const uint64_t shared = q(-1.5f, -2, 2, 0x1fffff) | (q(0.0f, -2, 2, 0x1fffff) << 21) |
                            (q(1.0f, 0, 8, 0x3fffff) << 42);
    const uintptr_t sharedVertices = Allocate(8);
    Put<uint64_t>(sharedVertices, shared);
    array(kTreeSharedVertices, sharedVertices, 1);
    const uintptr_t sharedIndices = Allocate(2);
    Put<uint16_t>(sharedIndices, 0);
    array(kTreeSharedIndices, sharedIndices, 1);
    const uintptr_t primitives = Allocate(4);
    const uint8_t quad[4] = {0, 1, 2, 3};
    Write(primitives, quad, sizeof(quad));
    array(kTreePrimitives, primitives, 1);
  }
  o.world = Allocate(kShapeReadSize);
  Put<uint8_t>(o.world + kShapeType, kShapeCompound);
  const uintptr_t instances = Allocate(2 * kInstanceSize);
  auto instance = [&](unsigned index, uintptr_t shape, float x, float y, float z) {
    const uintptr_t row = instances + index * kInstanceSize;
    float columns[16] = {};
    columns[0] = columns[5] = columns[10] = columns[15] = 1.0f;
    columns[12] = x;
    columns[13] = y;
    columns[14] = z;
    Write(row, columns, sizeof(columns));
    const float scale[3] = {1, 1, 1};
    Write(row + kInstanceScale, scale, sizeof(scale));
    Put<uintptr_t>(row + kInstanceShape, shape);
  };
  instance(0, o.floorShape, 0.0f, -1.0f, 3.5f);
  instance(1, o.meshShape, 0.0f, 1.0f, 3.0f);
  Put<uintptr_t>(o.world + kCompoundInstances, instances);
  Put<uint32_t>(o.world + kCompoundCount, 2);
  o.worldWrapper = Allocate(8);
  Put<uintptr_t>(o.worldWrapper, o.world);
  o.havok = Allocate(kHavokWorldShape + 8);
  Put<uintptr_t>(o.havok + kHavokWorldShape, o.worldWrapper);
  o.playerPhysics = Allocate(0x220);
  Put<uintptr_t>(o.playerPhysics + kPhysicsHavok, o.havok);
  Put<uintptr_t>(o.player + kEntityPhysics, o.playerPhysics);

  // Сущности: стена playerclip и триггер.
  auto entity = [&](uintptr_t vtable, uint32_t contents, uintptr_t shape, float x, float y, float z,
                    uintptr_t& entityOut, uintptr_t& physicsOut, uintptr_t& wrapperOut) {
    entityOut = Allocate(kEntityReadSize);
    physicsOut = Allocate(0x220);
    wrapperOut = Allocate(8);
    Put<uintptr_t>(entityOut, vtable);
    Put<uintptr_t>(entityOut + kEntityPhysics, physicsOut);
    Put<uint32_t>(physicsOut + kPhysicsType, kPhysicsStatic);
    Put<uint32_t>(physicsOut + kPhysicsContents, contents);
    Put<uint32_t>(physicsOut + kPhysicsBody, 1);
    const float axis[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const float origin[3] = {x, y, z};
    Write(physicsOut + kPhysicsAxis, axis, sizeof(axis));
    Write(physicsOut + kPhysicsOrigin, origin, sizeof(origin));
    Put<uintptr_t>(wrapperOut, shape);
    Put<uintptr_t>(physicsOut + kPhysicsShape, wrapperOut);
  };
  o.wallShape = MakeBox(1.5f, 0.3f, 0.05f);
  entity(base + kDynamicEntityVtableRva, kContentsPlayerClip, o.wallShape, 0.0f, 0.0f, 3.55f, o.wall, o.wallPhysics,
         o.wallWrapper);
  // RTTI триггера: vtable[+16] -> GetType() = lea rax, [rip+disp32]; ret -> описание -> имя.
  o.triggerVtable = base + kTriggerVtableRva;
  Put<uintptr_t>(o.triggerVtable + kVtableTypeGetter, base + kTriggerGetterRva);
  const uint8_t getter[8] = {0x48, 0x8d, 0x05, 0, 0, 0, 0, 0xc3};
  Write(base + kTriggerGetterRva, getter, sizeof(getter));
  Put<int32_t>(base + kTriggerGetterRva + 3, static_cast<int32_t>(kTriggerTypeInfoRva - (kTriggerGetterRva + 7)));
  Put<uintptr_t>(base + kTriggerTypeInfoRva, base + kTriggerNameRva);
  const char name[] = "idTriggerHurt";
  Write(base + kTriggerNameRva, name, sizeof(name));
  o.triggerShape = MakeBox(1.5f, 0.15f, 0.15f);
  entity(o.triggerVtable, kContentsTrigger, o.triggerShape, 0.0f, 0.55f, 3.15f, o.trigger, o.triggerPhysics,
         o.triggerWrapper);

  o.entities = Allocate(2 * sizeof(EntityEntry));
  const EntityEntry entries[2] = {{1, 0, o.wall, 0, 0}, {2, 0, o.trigger, 0, 0}};
  Write(o.entities, entries, sizeof(entries));
  Put<uintptr_t>(o.map + kMapEntities, o.entities);
  Put<uint32_t>(o.map + kMapEntityCount, 2);
}

}  // namespace doomfix::cv
