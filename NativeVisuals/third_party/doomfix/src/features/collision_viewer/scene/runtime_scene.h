// Based on doom-eternal-collision-viewer by IlyaTsyganov (commit 43ac57d), used with the author's permission.
//
// Сцена текущего уровня, собранная из памяти игры в фоновом потоке:
//   - RuntimeScene — коллизии уровня: обход мира Havok, разбор форм (одинаковые формы разбираются один
//     раз и делятся между экземплярами), BVH. Строится один раз на уровень (повтор через 2 с при ошибке);
//   - RuntimeEntities — триггеры и стены (EntityScanner), обновляются каждые 100 мс.
//
// RuntimeSceneManager владеет фоновым потоком. Поток present только обменивается готовыми снимками
// (shared_ptr под мьютексом менеджера): обход карты, разбор, BVH и освобождение больших старых сцен — в
// фоне. Смена уровня увеличивает счётчик поколений: устаревшая работа прерывается и не публикуется,
// снимок другого уровня никогда не отдаётся.
//
// Поток запускается лениво (первый Request) и останавливается Stop() (OnDestroyInstance/OnDestroyDevice).
// Деструктор не ждёт поток: игра выходит без vkDestroyInstance, и при выходе процесса поток уже убит
// системой, а его мьютекс мог остаться захваченным. Если поток не остановлен, деструктор только
// отпускает его (detach) и намеренно оставляет общие данные в памяти.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "features/collision_viewer/game_memory/game_state.h"
#include "features/collision_viewer/game_memory/memory_reader.h"
#include "features/collision_viewer/game_memory/shape_contents.h"
#include "features/collision_viewer/scene/scene_types.h"
#include "features/collision_viewer/scene/solid_visibility.h"

namespace doomfix::cv {

struct RuntimeScene {
  LevelIdentity level;
  std::vector<Mesh> meshes;
  std::vector<SpatialNode> nodes;
  std::vector<uint32_t> order;
  uint32_t unique = 0;       // разных форм
  uint32_t unsupported = 0;  // экземпляров, форму которых не удалось разобрать
  uint32_t nonBlocking = 0;  // known projectile/query-only world instances
  uint64_t triangles = 0;
  double milliseconds = 0;
  std::string problems;      // причины неразобранных форм: "причина x число; ..."
  bool optimized = false;
  SolidVisibilityStats optimization;
};

struct RuntimeEntities {
  LevelIdentity level;
  std::vector<Mesh> meshes;
  uint32_t triggers = 0;
  uint32_t walls = 0;
  uint32_t unsupported = 0;
  double milliseconds = 0;
};

// Обход мира Havok уровня в RuntimeScene.
class RuntimeSceneBuilder {
 public:
  explicit RuntimeSceneBuilder(MemoryReader memory = {}) : memory_(memory), contents_(memory) {}

  // Проверяется часто (между пачками экземпляров): true — прервать сборку.
  std::function<bool()> cancelled = [] { return false; };

  // nullptr — сборка прервана или мир нечитаем.
  std::shared_ptr<RuntimeScene> Build(const LevelIdentity& level);

 private:
  struct Cached {
    std::shared_ptr<const Geometry> geometry;
    float bounds[6];
  };
  bool Walk(uintptr_t address, const Transform& transform, RuntimeScene& scene, unsigned depth);

  MemoryReader memory_;
  ShapeContentsFilter contents_;
  std::unordered_map<uintptr_t, Cached> cache_;
  std::unordered_map<std::string, uint32_t> errors_;
  bool failed_ = false;
};

class RuntimeSceneManager {
 public:
  explicit RuntimeSceneManager(MemoryReader memory = {});
  ~RuntimeSceneManager();
  RuntimeSceneManager(const RuntimeSceneManager&) = delete;
  RuntimeSceneManager& operator=(const RuntimeSceneManager&) = delete;

  // Нужна сцена уровня level (base — начало образа exe). Новый уровень сбрасывает старые снимки и
  // прерывает устаревшую работу; запускает поток, если он ещё не запущен.
  void Request(const LevelIdentity& level, uintptr_t base);
  // Отдать старую сцену фоновому потоку на освобождение.
  void Retire(std::shared_ptr<const RuntimeScene> old);
  // Готовые снимки для level (или nullptr, если их нет или они другого уровня).
  void Snapshot(const LevelIdentity& level, std::shared_ptr<const RuntimeScene>& world,
                std::shared_ptr<const RuntimeEntities>& entities);
  // Остановить поток и освободить всё. Повторный Request запустит поток снова.
  void Stop();
  bool Running() const { return worker_.joinable(); }

 private:
  struct Shared;
  static void Run(Shared* shared);
  static void Optimize(Shared* shared, std::shared_ptr<const RuntimeScene> source, uint64_t generation);

  Shared* shared_;
  std::thread worker_;
};

}  // namespace doomfix::cv
