#pragma once

#include <functional>

#include "features/collision_viewer/scene/scene_types.h"

namespace doomfix::cv {

struct SolidVisibilityStats {
  uint32_t closedShapes = 0, unclassifiedShapes = 0, removed = 0, trimmed = 0;
  uint64_t originalTriangles = 0, triangles = 0, originalEdges = 0, edges = 0;
  uint64_t intersectionEdges = 0;
  double milliseconds = 0;
};

// Exterior patches only: no Boolean cut caps. Original object bounds/range
// eligibility are retained. Exposed cut boundaries are added to the contour
// lines, without internal triangulation/BSP edges. Open surfaces may be
// trimmed, but never act as solid occluders.
bool OptimizeCollisionSurfaces(
    const std::vector<Mesh>& source, const std::vector<SpatialNode>& nodes,
    const std::vector<uint32_t>& order, std::vector<Mesh>& output,
    SolidVisibilityStats& stats, const std::function<bool()>& cancelled,
    const std::function<void(uint32_t, uint32_t)>& progress = {});

}  // namespace doomfix::cv
