// Based on doom-eternal-collision-viewer by IlyaTsyganov (commit 43ac57d), used with the author's permission.
//
// Геометрия сцены collision-viewer: формы (треугольники для заливки и рёбра для контуров), их
// экземпляры в мире и дерево ограничивающих объёмов (BVH). Без Vulkan и без чтения памяти.

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace doomfix::cv {

struct Vec3 {
  float x, y, z;
};

// Группы геометрии: у каждой свой цвет и своя клавиша (F6/F7/F8).
enum class MeshKind : uint32_t {
  kTrigger = 0,    // триггеры (голубые)
  kCollision = 1,  // коллизии уровня (зелёные)
  kWall = 2,       // невидимые стены playerclip (оранжевые)
};
constexpr uint32_t kMeshKinds = 3;

// Аффинное преобразование 3x4 (по строкам): p' = M * (p, 1).
struct Transform {
  float m[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};

  Vec3 Apply(Vec3 p) const {
    return {m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3], m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
            m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
  }
  // this * other.
  Transform Compose(const Transform& other) const {
    Transform result;
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 4; ++c) {
        float v = c == 3 ? m[r * 4 + 3] : 0.0f;
        for (int k = 0; k < 3; ++k) v += m[r * 4 + k] * other.m[k * 4 + c];
        result.m[r * 4 + c] = v;
      }
    }
    return result;
  }
};

// Форма в своих координатах: треугольники (по 3 точки) и рёбра контура (по 2 точки). Рёбра —
// только настоящие края: диагонали между треугольниками одной плоскости в контур не попадают.
struct Geometry {
  MeshKind kind = MeshKind::kCollision;
  std::vector<Vec3> triangles;
  std::vector<Vec3> lines;
};

// Экземпляр формы в мире: преобразование и ограничивающий объём в мировых координатах.
struct Mesh {
  MeshKind kind = MeshKind::kCollision;
  float bounds[6] = {};  // min xyz, max xyz
  std::shared_ptr<const Geometry> geometry;
  Transform transform;

  Vec3 World(Vec3 p) const { return transform.Apply(p); }
};

// Узел BVH: объём, диапазон в порядке экземпляров (лист) или два потомка.
struct SpatialNode {
  float bounds[6] = {};
  uint32_t begin = 0;
  uint32_t count = 0;
  uint32_t left = ~0u;
  uint32_t right = ~0u;
};

// Пустой объём (min = +inf, max = -inf) и его расширение точкой.
void ResetBounds(float bounds[6]);
void ExpandBounds(float bounds[6], Vec3 p);

// BVH над meshes: order — перестановка индексов экземпляров, nodes[0] — корень. Листья — не больше 12
// экземпляров; деление по середине самой длинной оси (как в исходном viewer).
void BuildBvh(const std::vector<Mesh>& meshes, std::vector<SpatialNode>& nodes, std::vector<uint32_t>& order);

}  // namespace doomfix::cv
