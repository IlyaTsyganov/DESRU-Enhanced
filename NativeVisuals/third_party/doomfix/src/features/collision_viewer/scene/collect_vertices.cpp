// Based on doom-eternal-collision-viewer by IlyaTsyganov (commit 43ac57d), used with the author's permission.

#include "features/collision_viewer/scene/collect_vertices.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>
#include <ppl.h>
#include "runtime.h"

namespace doomfix::cv {
namespace {

using Color = std::array<float, 4>;

const Color kFillColors[kMeshKinds] = {
    {0.15f, 0.8f, 1.0f, kFillAlpha},   // триггеры
    {0.25f, 0.95f, 0.12f, kFillAlpha}, // коллизии
    {1.0f, 0.45f, 0.05f, kFillAlpha},  // стены
};
const Color kLineColors[kMeshKinds] = {
    {0.1f, 0.85f, 1.0f, 0.9f},
    {0.4f, 1.0f, 0.2f, 0.65f},
    {1.0f, 0.45f, 0.05f, 0.9f},
};

uint32_t KindIndex(MeshKind kind) { return static_cast<uint32_t>(kind); }

// Порядок отрисовки: триггеры, стены, коллизии.
uint32_t Priority(MeshKind kind) {
  return kind == MeshKind::kCollision ? 2u : (kind == MeshKind::kWall ? 1u : 0u);
}

bool Shown(const DisplaySettings& settings, MeshKind kind) {
  switch (kind) {
    case MeshKind::kTrigger:
      return settings.triggers;
    case MeshKind::kCollision:
      return settings.collision;
    case MeshKind::kWall:
      return settings.walls;
  }
  return false;
}

struct EmitJob {
  const Mesh* mesh;
  const std::vector<Vec3>* points;
  const Color* color;
  uint32_t first;
};

void EmitVertices(const std::vector<EmitJob>& jobs, std::vector<Vertex>& vertices,
                  uint32_t begin, uint32_t end) {
  for (const EmitJob& job : jobs) {
    const uint32_t jobEnd = job.first + static_cast<uint32_t>(job.points->size());
    if (jobEnd <= begin) continue;
    if (job.first >= end) break;
    const uint32_t from = std::max(begin, job.first), to = std::min(end, jobEnd);
    const Vec3* source = job.points->data() + from - job.first;
    Vertex* target = vertices.data() + from;
    for (uint32_t i = 0; i < to - from; ++i) {
      const Vec3 p = job.mesh->World(source[i]);
      target[i].position[0] = p.x;
      target[i].position[1] = p.y;
      target[i].position[2] = p.z;
      std::memcpy(target[i].color, job.color->data(), sizeof(target[i].color));
    }
  }
}

}  // namespace

ViewFilter::ViewFilter(const Camera& camera) {
  // Плоскости из строк матрицы: w ± x >= 0, w ± y >= 0, w >= 0.
  for (int plane = 0; plane < 5; ++plane) {
    for (int k = 0; k < 4; ++k) {
      const float sign = plane == 4 ? 0.0f : ((plane & 1) ? -1.0f : 1.0f);
      planes_[plane][k] = camera.matrix[12 + k] + (plane == 4 ? 0.0f : sign * camera.matrix[(plane / 2) * 4 + k]);
    }
  }
  std::memcpy(position_, camera.position, sizeof(position_));
}

float ViewFilter::DistanceSquared(const float* bounds) const {
  float result = 0.0f;
  for (int axis = 0; axis < 3; ++axis) {
    const float d = std::max(bounds[axis] - position_[axis], std::max(0.0f, position_[axis] - bounds[axis + 3]));
    result += d * d;
  }
  return result;
}

bool ViewFilter::InView(const float* bounds) const {
  for (const auto& p : planes_) {
    float maximum = p[3];
    for (int axis = 0; axis < 3; ++axis) maximum += p[axis] * (p[axis] >= 0 ? bounds[axis + 3] : bounds[axis]);
    if (maximum < 0) return false;
  }
  return true;
}

uint32_t GatherVisible(const Camera& camera, const DisplaySettings& settings, const SceneInput& input,
                       std::vector<Candidate>& visible) {
  visible.clear();
  const ViewFilter filter(camera);
  const float radiusSquared = settings.radius * settings.radius;
  uint32_t examined = 0;
  auto consider = [&](const Mesh& mesh) {
    if (!Shown(settings, mesh.kind)) return;
    const float distance = filter.DistanceSquared(mesh.bounds);
    if (distance <= radiusSquared && filter.InView(mesh.bounds)) visible.push_back({&mesh, distance});
  };
  if (input.world && input.nodes && input.order && !input.nodes->empty()) {
    const auto& nodes = *input.nodes;
    const auto& order = *input.order;
    const auto& meshes = *input.world;
    // Явный стек вместо рекурсии: глубина BVH мала, но так дешевле.
    uint32_t stack[128];
    uint32_t top = 0;
    stack[top++] = 0;
    while (top) {
      const SpatialNode& node = nodes[stack[--top]];
      if (filter.DistanceSquared(node.bounds) > radiusSquared || !filter.InView(node.bounds)) continue;
      if (node.left != ~0u && top + 2 <= std::size(stack)) {
        stack[top++] = node.right;
        stack[top++] = node.left;
        continue;
      }
      for (uint32_t i = node.begin; i < node.begin + node.count; ++i) {
        ++examined;
        consider(meshes[order[i]]);
      }
    }
  }
  if (input.entities) {
    for (const Mesh& mesh : *input.entities) consider(mesh);
  }
  return examined;
}

void CollectVertices(const Camera& camera, const DisplaySettings& settings, const SceneInput& input,
                     std::vector<Candidate>& visible, OverlayScene& scene, CollectStats* stats) {
  const auto started = std::chrono::steady_clock::now();
  CollectStats local;
  CollectStats& s = stats ? *stats : local;
  s = {};
  s.examined = GatherVisible(camera, settings, input, visible);
  // Stable category/distance order. Every admitted object is emitted in full.
  std::sort(visible.begin(), visible.end(), [](const Candidate& a, const Candidate& b) {
    if (a.mesh->kind != b.mesh->kind) return Priority(a.mesh->kind) < Priority(b.mesh->kind);
    if (a.distance != b.distance) return a.distance < b.distance;
    return std::less<const Mesh*>{}(a.mesh, b.mesh);
  });
  uint64_t required = scene.vertices.size();
  for (const Candidate& candidate : visible) {
    if (settings.fill) required += candidate.mesh->geometry->triangles.size();
    required += candidate.mesh->geometry->lines.size();
  }
  if (required > std::numeric_limits<uint32_t>::max()) throw std::length_error("Overlay exceeds Vulkan vertex indexing");
  if (required > scene.vertices.capacity()) {
    const uint64_t grown = uint64_t(scene.vertices.capacity()) + scene.vertices.capacity() / 2;
    scene.vertices.reserve(static_cast<size_t>(std::min<uint64_t>(std::numeric_limits<uint32_t>::max(),
        std::max(required, grown))));
  }
  const auto first = static_cast<uint32_t>(scene.vertices.size());
  uint32_t offset = first;
  std::vector<EmitJob> jobs;
  jobs.reserve(visible.size() * 2);
  if (settings.fill) {
    for (const Candidate& candidate : visible) {
      const Mesh& mesh = *candidate.mesh;
      if (mesh.geometry->triangles.empty()) continue;
      const Color& color = kFillColors[KindIndex(mesh.kind)];
      jobs.push_back({&mesh, &mesh.geometry->triangles, &color, offset});
      offset += static_cast<uint32_t>(mesh.geometry->triangles.size());
      ++s.filled;
      s.filledTriangles += static_cast<uint32_t>(mesh.geometry->triangles.size() / 3);
    }
  }
  const uint32_t triangleEnd = offset;
  for (const Candidate& candidate : visible) {
    const Mesh& mesh = *candidate.mesh;
    const Color& color = kLineColors[KindIndex(mesh.kind)];
    if (!mesh.geometry->lines.empty()) {
      jobs.push_back({&mesh, &mesh.geometry->lines, &color, offset});
      offset += static_cast<uint32_t>(mesh.geometry->lines.size());
    }
    ++s.drawn[KindIndex(mesh.kind)];
    s.segments[KindIndex(mesh.kind)] += static_cast<uint32_t>(mesh.geometry->lines.size() / 2);
  }
  const uint32_t end = offset;
  scene.vertices.resize(static_cast<size_t>(required));
  // Fixed disjoint output slices: no per-vertex push_back, contention or changing geometry order.
  // Small frames stay serial; dense frames use at most four simultaneous emission tasks.
  if (end - first >= 300000) {
    const uint32_t b = first + (end - first) / 4;
    const uint32_t c = first + (end - first) / 2;
    const uint32_t d = first + static_cast<uint32_t>(uint64_t(end - first) * 3 / 4);
    concurrency::parallel_invoke(
        [&] { EmitVertices(jobs, scene.vertices, first, b); },
        [&] { EmitVertices(jobs, scene.vertices, b, c); },
        [&] { EmitVertices(jobs, scene.vertices, c, d); },
        [&] { EmitVertices(jobs, scene.vertices, d, end); });
  } else EmitVertices(jobs, scene.vertices, first, end);
  if (end > first) {
    OverlayView view;
    view.camera = camera;
    view.farDistance = settings.radius;
    view.occlusion = !desru::Controlled() || DESRUVisualDiagnosticSkipDepth == 0;
    view.range = DepthRange::kViewport;
    view.depthMode = 2;  // reverse-Z
    view.depthBias = 0.035f;
    const auto viewIndex = static_cast<uint32_t>(scene.views.size());
    scene.views.push_back(view);
    if (triangleEnd > first) scene.draws.push_back({Primitive::kTriangles, first, triangleEnd - first, viewIndex});
    if (end > triangleEnd) scene.draws.push_back({Primitive::kLines, triangleEnd, end - triangleEnd, viewIndex});
  }
  s.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
}

}  // namespace doomfix::cv
