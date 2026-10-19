// Based on doom-eternal-collision-viewer by IlyaTsyganov (commit 43ac57d), used with the author's permission.
//
// Состояние игры, которое нужно в каждом кадре (поток present): камера, «идёт ли игра» (не меню, не
// пауза, не диалог, не загрузка), высота консоли, какой уровень загружен. Все функции принимают base
// — начало образа exe (в игре GetModuleHandleW(nullptr), в тестах — поддельная память) — и читают
// только через MemoryReader. Раскладки — exe_profile.h.

#pragma once

#include <cstdint>
#include <string>

#include "features/collision_viewer/game_memory/memory_reader.h"
#include "features/collision_viewer/overlay_types.h"

namespace doomfix::cv {

// Камера вида 0 рендерера. false — рендерера/вида нет или числа не похожи на камеру (загрузка).
bool ReadGameCamera(const MemoryReader& memory, uintptr_t base, Camera& camera);

// true — идёт игра: уровень загружен, игрок есть и не удалён, нет паузы, ни одно меню HUD не открыто
// (экран, переход, всплывающее окно, диалог). Нечитаемое состояние считается меню/загрузкой.
bool GameplayVisible(const MemoryReader& memory, uintptr_t base);

// Доля высоты экрана, занятая консолью (0 — закрыта, 1 — на весь экран). 0, если консоль не найдена.
float ReadConsoleFraction(const MemoryReader& memory, uintptr_t base);

// Какой уровень загружен: карта, корневая форма мира Havok и имя карты. Смена любого из трёх — новый
// уровень (имя ловит переход, даже если память карты переиспользована).
struct LevelIdentity {
  uintptr_t map = 0;
  uintptr_t world = 0;
  std::string name;

  bool operator==(const LevelIdentity& other) const {
    return map == other.map && world == other.world && name == other.name;
  }
  bool operator!=(const LevelIdentity& other) const { return !(*this == other); }
  explicit operator bool() const { return map && world && !name.empty(); }
};

// Пустой результат — уровня нет (меню, загрузка) или память нечитаема.
LevelIdentity ReadLevelIdentity(const MemoryReader& memory, uintptr_t base);

}  // namespace doomfix::cv
