// Based on doom-eternal-collision-viewer by IlyaTsyganov (commit 43ac57d), used with the author's permission.

#include <algorithm>
#include <cfloat>
#include <numeric>

#include "features/collision_viewer/scene/scene_types.h"

namespace doomfix::cv {
namespace {

constexpr uint32_t kLeafSize = 12;

uint32_t BuildNode(const std::vector<Mesh>& meshes, std::vector<SpatialNode>& nodes, std::vector<uint32_t>& order,
                   uint32_t begin, uint32_t end) {
  SpatialNode node;
  node.begin = begin;
  node.count = end - begin;
  ResetBounds(node.bounds);
  for (uint32_t i = begin; i < end; ++i) {
    const float* box = meshes[order[i]].bounds;
    for (int axis = 0; axis < 3; ++axis) {
      node.bounds[axis] = std::min(node.bounds[axis], box[axis]);
      node.bounds[axis + 3] = std::max(node.bounds[axis + 3], box[axis + 3]);
    }
  }
  const auto index = static_cast<uint32_t>(nodes.size());
  nodes.push_back(node);
  if (node.count > kLeafSize) {
    int axis = 0;
    for (int k = 1; k < 3; ++k) {
      if (node.bounds[k + 3] - node.bounds[k] > node.bounds[axis + 3] - node.bounds[axis]) axis = k;
    }
    const uint32_t middle = begin + node.count / 2;
    std::nth_element(order.begin() + begin, order.begin() + middle, order.begin() + end,
                     [axis, &meshes](uint32_t a, uint32_t b) {
                       const float* x = meshes[a].bounds;
                       const float* y = meshes[b].bounds;
                       return x[axis] + x[axis + 3] < y[axis] + y[axis + 3];
                     });
    const uint32_t left = BuildNode(meshes, nodes, order, begin, middle);
    const uint32_t right = BuildNode(meshes, nodes, order, middle, end);
    nodes[index].left = left;
    nodes[index].right = right;
  }
  return index;
}

}  // namespace

void ResetBounds(float bounds[6]) {
  for (int k = 0; k < 3; ++k) {
    bounds[k] = FLT_MAX;
    bounds[k + 3] = -FLT_MAX;
  }
}

void ExpandBounds(float bounds[6], Vec3 p) {
  const float xyz[3] = {p.x, p.y, p.z};
  for (int k = 0; k < 3; ++k) {
    bounds[k] = std::min(bounds[k], xyz[k]);
    bounds[k + 3] = std::max(bounds[k + 3], xyz[k]);
  }
}

void BuildBvh(const std::vector<Mesh>& meshes, std::vector<SpatialNode>& nodes, std::vector<uint32_t>& order) {
  nodes.clear();
  order.resize(meshes.size());
  std::iota(order.begin(), order.end(), 0u);
  if (meshes.empty()) return;
  nodes.reserve(meshes.size() / 4 + 1);
  BuildNode(meshes, nodes, order, 0, static_cast<uint32_t>(meshes.size()));
}

}  // namespace doomfix::cv
