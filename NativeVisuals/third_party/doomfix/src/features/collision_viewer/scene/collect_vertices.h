// Based on doom-eternal-collision-viewer by IlyaTsyganov (commit 43ac57d), used with the author's permission.
//
// Отбор видимой геометрии и вершины кадра (поток present):
//   - коллизии уровня — обход BVH с отсечением по дальности и пирамиде видимости камеры; триггеры и
//     стены — перебором (их мало);
//   - целые объекты: достаточно попадания их объёма в радиус; дальние части не обрезаются;
//   - все отобранные грани/рёбра, без фиксированного лимита ближайших вершин;
//   - цвета: триггеры голубые, коллизии зелёные, стены оранжевые; заливка — 9 % непрозрачности.
// Глубина (окклюзия по буферу игры) — в шейдере, см. overlay_renderer.h.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "features/collision_viewer/overlay_types.h"
#include "features/collision_viewer/scene/display_settings.h"
#include "features/collision_viewer/scene/scene_types.h"

namespace doomfix::cv {

// Непрозрачность заливки всех групп.
constexpr float kFillAlpha = 0.09f;

// Отсечение по дальности и пирамиде видимости камеры (4 боковые плоскости и «перед камерой»).
class ViewFilter {
 public:
  explicit ViewFilter(const Camera& camera);
  // Квадрат расстояния от камеры до ближайшей точки объёма.
  float DistanceSquared(const float* bounds) const;
  // Объём хотя бы частично в пирамиде видимости.
  bool InView(const float* bounds) const;

 private:
  float planes_[5][4];
  float position_[3];
};

// Геометрия, из которой собирается кадр: коллизии уровня с BVH и живые сущности (любое может
// отсутствовать).
struct SceneInput {
  const std::vector<Mesh>* world = nullptr;
  const std::vector<SpatialNode>* nodes = nullptr;
  const std::vector<uint32_t>* order = nullptr;
  const std::vector<Mesh>* entities = nullptr;
};

struct CollectStats {
  uint32_t drawn[kMeshKinds] = {};     // экземпляров с контуром, по группам
  uint32_t segments[kMeshKinds] = {};  // рёбер контура, по группам
  uint32_t filled = 0;                 // экземпляров с заливкой
  uint32_t filledTriangles = 0;
  uint32_t examined = 0;               // экземпляров коллизий, проверенных в листьях BVH
  double milliseconds = 0;
};

struct Candidate {
  const Mesh* mesh;
  float distance;  // квадрат расстояния
};

// Видимые экземпляры групп, включённых в settings, в пределах settings.radius. Возвращает examined.
uint32_t GatherVisible(const Camera& camera, const DisplaySettings& settings, const SceneInput& input,
                       std::vector<Candidate>& visible);

// Вершины и вызовы отрисовки (заливка — треугольники, затем контуры — линии) в scene; вид сцены
// (окклюзия по глубине игры, reverse-Z, дальность settings.radius) добавляется, если есть что рисовать.
// visible — рабочий буфер (переиспользуется между кадрами).
void CollectVertices(const Camera& camera, const DisplaySettings& settings, const SceneInput& input,
                     std::vector<Candidate>& visible, OverlayScene& scene, CollectStats* stats = nullptr);

}  // namespace doomfix::cv
