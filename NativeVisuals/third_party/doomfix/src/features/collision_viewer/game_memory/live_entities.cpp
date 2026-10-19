// Based on doom-eternal-collision-viewer by IlyaTsyganov (commit 43ac57d), used with the author's permission.

#include "features/collision_viewer/game_memory/live_entities.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>

#include "features/collision_viewer/game_memory/exe_profile.h"
#include "features/collision_viewer/game_memory/havok_shapes.h"

namespace doomfix::cv {

using namespace exe11;

bool EntityScanner::BlocksPlayer(uint32_t contents, bool disabled) {
  return !disabled && (contents & kContentsPlayerClip) && !(contents & (kContentsSolid | kContentsOpaque)) &&
         !(contents & kContentsNoCollision);
}

bool EntityScanner::IsTriggerType(uintptr_t vtable, uintptr_t base) {
  auto found = triggerTypes_.find(vtable);
  if (found != triggerTypes_.end()) return found->second;
  uintptr_t getter = 0;
  uintptr_t name = 0;
  unsigned char code[8] = {};
  char text[96] = {};
  bool result = false;
  // GetType(): lea rax, [rip+disp32]; ret.
  if (vtable >= base && vtable < base + kMaxVtableRva && memory_.Get(vtable + kVtableTypeGetter, getter) &&
      getter >= base && getter < base + kMaxCodeRva && memory_.Read(getter, code, 8) && code[0] == 0x48 &&
      code[1] == 0x8d && code[2] == 0x05 && code[7] == 0xc3) {
    int32_t displacement = 0;
    std::memcpy(&displacement, code + 3, 4);
    const uintptr_t info = getter + 7 + static_cast<intptr_t>(displacement);
    if (memory_.Get(info, name) && memory_.Read(name, text, sizeof(text) - 1)) {
      result = std::strncmp(text, "idTrigger", 9) == 0 || std::strncmp(text, "idEncounterTrigger", 18) == 0;
    }
  }
  triggerTypes_.emplace(vtable, result);
  return result;
}

void EntityScanner::Refresh(uintptr_t base, bool includeTriggers) {
  const auto start = std::chrono::steady_clock::now();
  meshes.clear();
  examined = walls = triggers = unsupported = 0;
  uintptr_t system = 0;
  uintptr_t map = 0;
  uintptr_t list = 0;
  uint32_t count = 0;
  if (!memory_.Get(base + kGameSystemRva, system) || !memory_.Get(system + kGameSystemMap, map) ||
      !memory_.Get(map + kMapEntities, list) || !memory_.Get(map + kMapEntityCount, count) || count > kMaxEntities) {
    return;
  }
  std::vector<EntityEntry> entries(count);
  if (count && !memory_.Read(list, entries.data(), entries.size() * sizeof(EntityEntry))) return;
  ShapeDecoder decoder(memory_);
  for (const EntityEntry& entry : entries) {
    ++examined;
    uintptr_t vtable = 0;
    if (!memory_.Get(entry.entity, vtable)) continue;
    const bool trigger = includeTriggers && IsTriggerType(vtable, base);
    if (!trigger && vtable != base + kDynamicEntityVtableRva) continue;
    unsigned char entity[kEntityReadSize] = {};
    unsigned char physics[kPhysicsReadSize] = {};
    uintptr_t physicsAddress = 0;
    if (!memory_.Read(entry.entity, entity, sizeof(entity)) || entity[kEntityRemoved]) continue;
    std::memcpy(&physicsAddress, entity + kEntityPhysics, 8);
    if (!memory_.Read(physicsAddress, physics, sizeof(physics))) continue;
    uint32_t type = 0;
    uint32_t contents = 0;
    uint32_t body = 0;
    std::memcpy(&type, physics + kPhysicsType, 4);
    std::memcpy(&contents, physics + kPhysicsContents, 4);
    std::memcpy(&body, physics + kPhysicsBody, 4);
    const bool disabled = physics[kPhysicsDisabled] != 0;
    const bool enabledTrigger =
        trigger && (contents & kContentsTrigger) && !(contents & kContentsNoCollision) && !disabled;
    if ((type != kPhysicsStatic && type != kPhysicsRigidBody) || body == ~0u ||
        (!trigger && !BlocksPlayer(contents, disabled)) || (trigger && !enabledTrigger)) {
      continue;
    }
    if (type == kPhysicsRigidBody) {
      unsigned char inWorld = 0;
      if (!memory_.Get(physicsAddress + kPhysicsInWorld, inWorld) || !inWorld) continue;
    }
    if (trigger) {
      ++triggers;
    } else {
      ++walls;
    }
    uintptr_t wrapper = 0;
    uintptr_t root = 0;
    std::memcpy(&wrapper, physics + kPhysicsShape, 8);
    if (!memory_.Get(wrapper, root)) {
      ++unsupported;
      continue;
    }
    auto geometry = std::make_shared<Geometry>();
    geometry->kind = trigger ? MeshKind::kTrigger : MeshKind::kWall;
    if (!decoder.Decode(root, Transform{}, *geometry) || geometry->triangles.empty()) {
      ++unsupported;
      continue;
    }
    // Рваный снимок: сущность удалили или её слот переиспользовали, пока мы читали.
    uintptr_t stillPhysics = 0;
    uint32_t stillContents = 0;
    unsigned char removed = 1;
    if (!memory_.Get(entry.entity + kEntityPhysics, stillPhysics) || stillPhysics != physicsAddress ||
        !memory_.Get(physicsAddress + kPhysicsContents, stillContents) || stillContents != contents ||
        !memory_.Get(entry.entity + kEntityRemoved, removed) || removed) {
      continue;
    }
    Mesh mesh;
    mesh.kind = geometry->kind;
    float axis[9];
    float origin[3];
    std::memcpy(axis, physics + kPhysicsAxis, 36);
    std::memcpy(origin, physics + kPhysicsOrigin, 12);
    bool finite = true;
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 4; ++c) {
        const float v = c < 3 ? axis[c * 3 + r] : origin[r];
        finite = finite && std::isfinite(v);
        mesh.transform.m[r * 4 + c] = v;
      }
    }
    if (!finite) continue;
    ResetBounds(mesh.bounds);
    for (Vec3 point : geometry->triangles) ExpandBounds(mesh.bounds, mesh.World(point));
    mesh.geometry = std::move(geometry);
    meshes.push_back(std::move(mesh));
  }
  milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

}  // namespace doomfix::cv
