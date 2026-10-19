// Based on doom-eternal-collision-viewer by IlyaTsyganov (commit 43ac57d), used with the author's permission.
//
// Сущности уровня, которые меняются во время игры: триггеры (классы idTrigger*/idEncounterTrigger*,
// по RTTI движка) и невидимые стены playerclip (idDynamicEntity с contents PLAYERCLIP без SOLID и
// OPAQUE). Список сущностей карты обходится заново при каждом Refresh (фоновый поток, раз в 100 мс):
// выключенные, удалённые и убранные из мира пропадают, появившиеся и сдвинутые — обновляются.
// Раскладки — exe_profile.h.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "features/collision_viewer/game_memory/memory_reader.h"
#include "features/collision_viewer/scene/scene_types.h"

namespace doomfix::cv {

class EntityScanner {
 public:
  explicit EntityScanner(MemoryReader memory = {}) : memory_(memory) {}

  // Обходит сущности текущей карты (base — начало образа exe). includeTriggers — искать и триггеры.
  void Refresh(uintptr_t base, bool includeTriggers);

  // Результат последнего Refresh.
  std::vector<Mesh> meshes;
  uint32_t examined = 0;     // сущностей просмотрено
  uint32_t walls = 0;        // активных стен
  uint32_t triggers = 0;     // включённых триггеров
  uint32_t unsupported = 0;  // форм, которые не удалось разобрать
  double milliseconds = 0;

  // Стена: блокирует игрока (PLAYERCLIP), но не видна и не твёрдая для всего (нет SOLID и OPAQUE);
  // скрытые и выключенные динамические сущности переключают contents на NOCOLLISION.
  static bool BlocksPlayer(uint32_t contents, bool disabled);

 private:
  // Класс по vtable — триггер? Ответ кешируется по vtable.
  bool IsTriggerType(uintptr_t vtable, uintptr_t base);

  MemoryReader memory_;
  std::unordered_map<uintptr_t, bool> triggerTypes_;
};

}  // namespace doomfix::cv
