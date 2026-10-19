// Based on doom-eternal-collision-viewer by IlyaTsyganov (commit 43ac57d), used with the author's permission.

#include "features/collision_viewer/game_memory/game_state.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "features/collision_viewer/game_memory/exe_profile.h"
#include "runtime.h"

namespace doomfix::cv {

using namespace exe11;

bool ReadGameCamera(const MemoryReader& memory, uintptr_t base, Camera& camera) {
  if(desru::Controlled()) return desru::ReadLiveCamera(camera.matrix,camera.position);
  uintptr_t renderer = 0;
  uintptr_t views = 0;
  uintptr_t view = 0;
  int count = 0;
  if(desru::Controlled()&&kResolvedRenderer) {
    renderer=kResolvedRenderer;
    if(!desru::ResolveCamera(renderer)) return false;
    view=kResolvedView;
  }
  if (!view&&((!renderer&&!memory.Get(base + kRendererRva, renderer)) || !renderer || !memory.Get(renderer + kRendererViewCount, count) ||
      count < 1 || count > kMaxViews || !memory.Get(renderer + kRendererViews, views) || !views ||
      !memory.Get(views, view) || !view)) {
    return false;
  }
  if (!memory.Read(view + kViewPosition, camera.position, sizeof(camera.position)) ||
      !memory.Read(view + kViewMatrix, camera.matrix, sizeof(camera.matrix))) {
    return false;
  }
  for (float value : camera.matrix) {
    if (!std::isfinite(value)) return false;
  }
  for (float value : camera.position) {
    if (!std::isfinite(value)) return false;
  }
  // Строка 3 — направление взгляда (единичное), и камера в своей позиции — на расстоянии 0 от себя.
  const float* m = camera.matrix;
  const float cameraW = m[12] * camera.position[0] + m[13] * camera.position[1] + m[14] * camera.position[2] + m[15];
  const float forwardLength = m[12] * m[12] + m[13] * m[13] + m[14] * m[14];
  return std::fabs(cameraW) < 0.5f && forwardLength > 0.9f && forwardLength < 1.1f;
}

bool GameplayVisible(const MemoryReader& memory, uintptr_t base) {
  uintptr_t system = 0;
  uintptr_t map = 0;
  uintptr_t player = 0;
  uintptr_t vtable = 0;
  uint32_t state = 0;
  if(desru::Controlled()) {
    system=kResolvedGameSystem;
    if(!system||!memory.Get(system+kGameSystemMap,map)||!map) return false;
    desru::ResolveMap(map);
  }
  if ((!system&&!memory.Get(base + kGameSystemRva, system)) || !memory.Get(system + kGameSystemState, state) ||
      state != kStateInLevel || !memory.Get(system + kGameSystemMap, map) || !memory.Get(map + kMapPlayer, player) ||
      !memory.Get(player, vtable) || vtable != base + kPlayerVtableRva) {
    return false;
  }
  const uintptr_t hud = player + kPlayerHud;
  unsigned char paused = 1;
  unsigned char removed = 1;
  if (!memory.Get(player + kEntityRemoved, removed) || removed || !memory.Get(hud + kHudPaused, paused) || paused) {
    return false;
  }
  uintptr_t menus = 0;
  uint32_t count = 0;
  if (!memory.Get(hud + kHudMenus, menus) || !memory.Get(hud + kHudMenuCount, count) || count > kMaxMenus) {
    return false;
  }
  for (uint32_t i = 0; i < count; ++i) {
    uintptr_t menu = 0;
    unsigned char data[kMenuReadSize] = {};
    if (!memory.Get(menus + size_t(i) * 8, menu) || !memory.Read(menu, data, sizeof(data))) return false;
    if (data[kMenuHidden]) continue;  // меню явно скрыто
    int screen = -1;
    int next = -1;
    int popups = 0;
    std::memcpy(&screen, data + kMenuScreen, 4);
    std::memcpy(&next, data + kMenuNextScreen, 4);
    std::memcpy(&popups, data + kMenuPopups, 4);
    if (screen >= 0 || next >= 0 || popups > 0 || data[kMenuBlocking]) return false;
  }
  return true;
}

float ReadConsoleFraction(const MemoryReader& memory, uintptr_t base) {
  // DrawSolidConsole умножает displayFrac на высоту кадра.
  const uintptr_t console = desru::Controlled()&&kResolvedConsole?kResolvedConsole:base + kConsoleRva;
  uintptr_t vtable = 0;
  float fraction = 0.0f;
  unsigned char forced = 0;
  if (!memory.Get(console, vtable) || vtable != base + kConsoleVtableRva ||
      !memory.Get(console + kConsoleFraction, fraction) || !memory.Get(console + kConsoleForced, forced)) {
    return 0.0f;
  }
  if (forced) return 1.0f;
  return std::isfinite(fraction) ? std::clamp(fraction, 0.0f, 1.0f) : 1.0f;
}

LevelIdentity ReadLevelIdentity(const MemoryReader& memory, uintptr_t base) {
  LevelIdentity result;
  uintptr_t system = 0;
  uintptr_t text = 0;
  uint32_t length = 0;
  uint32_t state = 0;
  if(desru::Controlled()) system=kResolvedGameSystem;
  if ((!system&&!memory.Get(base + kGameSystemRva, system)) || !memory.Get(system + kGameSystemState, state) ||
      state != kStateInLevel || !memory.Get(system + kGameSystemMap, result.map) ||
      !memory.Get(result.map + kMapName, text) || !memory.Get(result.map + kMapNameLength, length) || !length ||
      length > kMaxMapNameLength) {
    return {};
  }
  char name[kMaxMapNameLength + 1] = {};
  if (!memory.Read(text, name, length)) return {};
  for (uint32_t i = 0; i < length; ++i) {
    if (name[i] < 32 || name[i] >= 127) return {};
  }
  result.name.assign(name, length);
  uintptr_t player = 0;
  uintptr_t physics = 0;
  uintptr_t havok = 0;
  uintptr_t wrapper = 0;
  if (!memory.Get(result.map + kMapPlayer, player) || !memory.Get(player + kEntityPhysics, physics) ||
      !memory.Get(physics + kPhysicsHavok, havok) || !memory.Get(havok + kHavokWorldShape, wrapper) ||
      !memory.Get(wrapper, result.world) || !result.world) {
    return {};
  }
  return result;
}

}  // namespace doomfix::cv
