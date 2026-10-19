#include "features/collision_viewer/scene/solid_visibility.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <unordered_map>

namespace doomfix::cv {
namespace {

constexpr double kEpsilon = 1e-6;
struct Point {
  double x, y, z;
  Point operator+(Point b) const { return {x + b.x, y + b.y, z + b.z}; }
  Point operator-(Point b) const { return {x - b.x, y - b.y, z - b.z}; }
  Point operator*(double v) const { return {x * v, y * v, z * v}; }
};
Point P(Vec3 v) { return {v.x, v.y, v.z}; }
Vec3 V(Point v) { return {float(v.x), float(v.y), float(v.z)}; }
double Dot(Point a, Point b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Point Cross(Point a, Point b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Point World(const Mesh& m, Vec3 v) {
  const auto* a = m.transform.m;
  return {
      a[0] * double(v.x) + a[1] * double(v.y) + a[2] * double(v.z) + a[3],
      a[4] * double(v.x) + a[5] * double(v.y) + a[6] * double(v.z) + a[7],
      a[8] * double(v.x) + a[9] * double(v.y) + a[10] * double(v.z) + a[11]};
}
struct Plane {
  Point normal;
  double offset;
  double Distance(Point p) const { return Dot(normal, p) - offset; }
};
struct Polygon {
  std::vector<Point> points;
  Point normal;
  // Outgoing edge provenance: original mesh edge, or one cutting plane in
  // one original triangle. It survives further splitting without fuzzy welding.
  std::vector<uint64_t> edges;
};
Plane Face(const Polygon& p) {
  Point n = Cross(p.points[1] - p.points[0], p.points[2] - p.points[0]);
  const double length = std::sqrt(Dot(n, n));
  if (length < 1e-14) throw std::runtime_error("degenerate solid face");
  n = n * (1 / length);
  return {n, Dot(n, p.points[0])};
}
// Sutherland-Hodgman split; on-plane vertices belong to both fragments, with
// shared intersections.
void Split(const Polygon& p, const Plane& plane, Polygon& front, Polygon& back,
           uint64_t cutEdge = 0) {
  front.normal = back.normal = p.normal;
  for (size_t i = 0; i < p.points.size(); ++i) {
    const Point a = p.points[i], b = p.points[(i + 1) % p.points.size()];
    const double da = plane.Distance(a), db = plane.Distance(b);
    const bool crosses =
        (da > kEpsilon && db < -kEpsilon) || (da < -kEpsilon && db > kEpsilon);
    auto append = [&](Polygon& out, Point v, bool followsSource) {
      out.points.push_back(v);
      if (!p.edges.empty())
        out.edges.push_back(followsSource ? p.edges[i] : cutEdge);
    };
    if (da >= -kEpsilon) append(front, a, db >= -kEpsilon || crosses);
    if (da <= kEpsilon) append(back, a, db <= kEpsilon || crosses);
    if (crosses) {
      const Point cut = a + (b - a) * (da / (da - db));
      append(front, cut, db > 0);
      append(back, cut, db < 0);
    }
  }
}

struct EdgeTopology {
  struct Edge {
    uint32_t uses = 0;
    bool contour = false;
  };
  std::vector<Edge> edges;
  std::vector<std::array<uint32_t, 3>> triangles;
};
EdgeTopology MakeEdgeTopology(const Geometry& geometry,
                              const std::function<bool()>& cancelled) {
  EdgeTopology result;
  std::map<std::array<float, 3>, uint32_t> vertices;
  std::unordered_map<uint64_t, uint32_t> edges;
  auto vertex = [&](Vec3 v) {
    return vertices
        .emplace(std::array<float, 3>{v.x, v.y, v.z},
                 static_cast<uint32_t>(vertices.size()))
        .first->second;
  };
  auto key = [](uint32_t a, uint32_t b) {
    return (uint64_t(std::min(a, b)) << 32) | std::max(a, b);
  };
  for (size_t i = 0; i + 2 < geometry.triangles.size(); i += 3) {
    if ((i & 1023) == 0 && cancelled()) throw std::runtime_error("cancelled");
    uint32_t ids[] = {vertex(geometry.triangles[i]),
                      vertex(geometry.triangles[i + 1]),
                      vertex(geometry.triangles[i + 2])};
    std::array<uint32_t, 3> triangle;
    for (int k = 0; k < 3; ++k) {
      auto found = edges.emplace(key(ids[k], ids[(k + 1) % 3]),
                                 static_cast<uint32_t>(result.edges.size()));
      if (found.second) result.edges.push_back({});
      triangle[k] = found.first->second;
      ++result.edges[triangle[k]].uses;
    }
    result.triangles.push_back(triangle);
  }
  for (size_t i = 0; i + 1 < geometry.lines.size(); i += 2) {
    if ((i & 1023) == 0 && cancelled()) throw std::runtime_error("cancelled");
    auto found = edges.find(
        key(vertex(geometry.lines[i]), vertex(geometry.lines[i + 1])));
    if (found != edges.end()) result.edges[found->second].contour = true;
  }
  return result;
}

// Oppositely oriented intervals cancel even if one side was split further by
// another BSP plane. Only the boundary of the retained surface survives.
class CutContours {
 public:
  explicit CutContours(const EdgeTopology& topology) : topology_(topology) {}
  void Add(const Polygon& polygon) {
    for (size_t i = 0; i < polygon.points.size(); ++i) {
      const uint64_t id = polygon.edges[i];
      if (id < topology_.edges.size()) {
        const auto& original = topology_.edges[size_t(id)];
        if (original.contour || original.uses != 2) continue;
      }
      const Point a = polygon.points[i],
                  b = polygon.points[(i + 1) % polygon.points.size()];
      const Point delta = b - a;
      const double length = std::sqrt(Dot(delta, delta));
      if (length <= kEpsilon) continue;
      auto found = groups_.find(id);
      if (found == groups_.end()) {
        Group group;
        group.origin = a;
        group.direction = delta * (1 / length);
        found = groups_.emplace(id, std::move(group)).first;
      }
      Group& group = found->second;
      const double start = Dot(a - group.origin, group.direction);
      const double end = Dot(b - group.origin, group.direction);
      const int sign = end > start ? 1 : -1;
      group.events.push_back({std::min(start, end), sign});
      group.events.push_back({std::max(start, end), -sign});
    }
  }
  template <class Emit>
  bool EmitSegments(const Emit& emit, const std::function<bool()>& cancelled) {
    for (auto& entry : groups_) {
      if (cancelled()) return false;
      Group& group = entry.second;
      auto& events = group.events;
      std::sort(events.begin(), events.end());
      int winding = 0;
      double begin = 0;
      for (size_t i = 0; i < events.size();) {
        const double at = events[i].first;
        int change = 0;
        do {
          change += events[i++].second;
        } while (i < events.size() && events[i].first - at <= kEpsilon);
        const int next = winding + change;
        if (winding == 0 && next != 0) begin = at;
        if (winding != 0 && next == 0 && at - begin > kEpsilon)
          emit(group.origin + group.direction * begin,
               group.origin + group.direction * at);
        winding = next;
      }
    }
    return true;
  }

 private:
  struct Group {
    Point origin, direction;
    std::vector<std::pair<double, int>> events;
  };
  const EdgeTopology& topology_;
  std::map<uint64_t, Group> groups_;
};
int Side(const Polygon& p, const Plane& plane) {
  int result = 0;
  for (Point v : p.points) {
    const double d = plane.Distance(v);
    result |= d > kEpsilon ? 1 : d < -kEpsilon ? 2 : 0;
  }
  return result;
}
struct Node {
  Plane plane;
  int front = -1, back = -1;
};
struct Solid {
  std::vector<Node> nodes;
  double orientation = 1;
  bool convex = false;
};

int BuildTree(std::vector<Polygon> polygons, Solid& solid, unsigned depth,
              const std::function<bool()>& cancelled) {
  if (cancelled()) throw std::runtime_error("cancelled");
  if (polygons.empty()) return -1;
  if (depth > 256) throw std::runtime_error("solid BSP depth");
  size_t selected = 0;
  double best = 1e100;
  const size_t samples = std::min<size_t>(16, polygons.size());
  for (size_t sample = 0; sample < samples; ++sample) {
    const size_t index = sample * polygons.size() / samples;
    const Plane plane = Face(polygons[index]);
    int front = 0, back = 0, split = 0;
    for (const auto& p : polygons) {
      const int s = Side(p, plane);
      front += s == 1;
      back += s == 2;
      split += s == 3;
    }
    const double score = split * 8.0 + std::abs(front - back);
    if (score < best) {
      best = score;
      selected = index;
    }
    if (best == 0) break;
  }
  const Plane plane = Face(polygons[selected]);
  const int index = static_cast<int>(solid.nodes.size());
  solid.nodes.push_back({plane});
  std::vector<Polygon> front, back;
  for (auto& p : polygons) {
    switch (Side(p, plane)) {
      case 1:
        front.push_back(std::move(p));
        break;
      case 2:
        back.push_back(std::move(p));
        break;
      case 3: {
        Polygon f, b;
        Split(p, plane, f, b);
        if (f.points.size() >= 3) front.push_back(std::move(f));
        if (b.points.size() >= 3) back.push_back(std::move(b));
        break;
      }
    }
  }
  solid.nodes[index].front =
      BuildTree(std::move(front), solid, depth + 1, cancelled);
  solid.nodes[index].back =
      BuildTree(std::move(back), solid, depth + 1, cancelled);
  return index;
}

std::shared_ptr<const Solid> MakeSolid(const Geometry& geometry,
                                       const std::function<bool()>& cancelled) {
  if (geometry.triangles.size() < 12 || geometry.triangles.size() % 3)
    return {};
  std::map<std::array<float, 3>, uint32_t> vertices;
  struct Edge {
    uint32_t count = 0;
    int direction = 0;
  };
  std::unordered_map<uint64_t, Edge> edges;
  std::vector<Polygon> faces;
  const Point origin = P(geometry.triangles[0]);
  double volume = 0;
  for (size_t i = 0; i < geometry.triangles.size(); i += 3) {
    if ((i & 1023) == 0 && cancelled()) throw std::runtime_error("cancelled");
    uint32_t ids[3];
    Polygon p;
    for (int k = 0; k < 3; ++k) {
      const Vec3 v = geometry.triangles[i + k];
      if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
        return {};
      const std::array<float, 3> key = {v.x, v.y, v.z};
      ids[k] = vertices.emplace(key, static_cast<uint32_t>(vertices.size()))
                   .first->second;
      p.points.push_back(P(v));
    }
    if (ids[0] == ids[1] || ids[1] == ids[2] || ids[0] == ids[2]) continue;
    p.normal = Face(p).normal;
    volume += Dot(p.points[0] - origin,
                  Cross(p.points[1] - origin, p.points[2] - origin));
    for (int k = 0; k < 3; ++k) {
      const uint32_t a = ids[k], b = ids[(k + 1) % 3];
      Edge& e = edges[(uint64_t(std::min(a, b)) << 32) | std::max(a, b)];
      ++e.count;
      e.direction += a < b ? 1 : -1;
    }
    faces.push_back(std::move(p));
  }
  if (std::fabs(volume) < 1e-12) return {};
  for (const auto& [key, edge] : edges)
    if (edge.count != 2 || edge.direction != 0) return {};
  auto result = std::make_shared<Solid>();
  result->orientation = volume > 0 ? 1 : -1;
  if (volume < 0)
    for (auto& p : faces) {
      std::reverse(p.points.begin(), p.points.end());
      p.normal = p.normal * -1;
    }
  // A convex solid needs only its distinct half-spaces; avoids a long BSP chain
  // for spheres.
  std::vector<Plane> planes;
  bool convex = true;
  for (const auto& p : faces) {
    if (cancelled()) throw std::runtime_error("cancelled");
    const Plane plane = Face(p);
    for (const auto& [v, id] : vertices)
      if (plane.Distance({v[0], v[1], v[2]}) > kEpsilon) {
        convex = false;
        break;
      }
    if (!convex) break;
    bool duplicate = false;
    for (const Plane& old : planes)
      if (Dot(old.normal, plane.normal) > 1 - 1e-10 &&
          std::fabs(old.offset - plane.offset) < kEpsilon) {
        duplicate = true;
        break;
      }
    if (!duplicate) planes.push_back(plane);
  }
  result->convex = convex;
  if (convex) {
    for (const auto& plane : planes) result->nodes.push_back({plane});
  } else
    BuildTree(std::move(faces), *result, 0, cancelled);
  return result;
}

struct WorldSolid {
  std::shared_ptr<const Solid> local;
  std::vector<Node> nodes;
  double orientation = 1;
};
WorldSolid PlaceSolid(const Mesh& mesh, std::shared_ptr<const Solid> solid) {
  WorldSolid result;
  result.local = std::move(solid);
  if (!result.local) return result;
  const auto* m = mesh.transform.m;
  const Point x = {m[0], m[4], m[8]}, y = {m[1], m[5], m[9]},
              z = {m[2], m[6], m[10]};
  const double determinant = Dot(x, Cross(y, z));
  if (std::fabs(determinant) < 1e-15) {
    result.local.reset();
    return result;
  }
  const Point ix = Cross(y, z) * (1 / determinant),
              iy = Cross(z, x) * (1 / determinant),
              iz = Cross(x, y) * (1 / determinant);
  result.orientation = result.local->orientation * (determinant > 0 ? 1 : -1);
  const Point translation = {m[3], m[7], m[11]};
  result.nodes = result.local->nodes;
  for (auto& node : result.nodes) {
    const auto& p = node.plane;
    Point n = ix * p.normal.x + iy * p.normal.y + iz * p.normal.z;
    const double length = std::sqrt(Dot(n, n));
    node.plane = {n * (1 / length), (p.offset + Dot(n, translation)) / length};
  }
  return result;
}

void ClipTree(Polygon polygon, const WorldSolid& solid, int index,
              bool ownBoundary, bool closedTarget, bool& removed,
              std::vector<Polygon>& outside, uint64_t cutBase) {
  const Node& node = solid.nodes[index];
  const int side = Side(polygon, node.plane);
  if (side == 0) {
    const bool front =
        !closedTarget ||
        (ownBoundary && Dot(polygon.normal, node.plane.normal) > 0);
    if (front) {
      if (node.front >= 0)
        ClipTree(std::move(polygon), solid, node.front, ownBoundary,
                 closedTarget, removed, outside, cutBase);
      else
        outside.push_back(std::move(polygon));
    } else {
      if (node.back >= 0)
        ClipTree(std::move(polygon), solid, node.back, ownBoundary,
                 closedTarget, removed, outside, cutBase);
      else
        removed = true;
    }
  } else if (side == 1) {
    if (node.front >= 0)
      ClipTree(std::move(polygon), solid, node.front, ownBoundary, closedTarget,
               removed, outside, cutBase);
    else
      outside.push_back(std::move(polygon));
  } else if (side == 2) {
    if (node.back >= 0)
      ClipTree(std::move(polygon), solid, node.back, ownBoundary, closedTarget,
               removed, outside, cutBase);
    else
      removed = true;
  } else {
    Polygon front, back;
    Split(polygon, node.plane, front, back, cutBase + uint64_t(index));
    if (front.points.size() >= 3) {
      if (node.front >= 0)
        ClipTree(std::move(front), solid, node.front, ownBoundary, closedTarget,
                 removed, outside, cutBase);
      else
        outside.push_back(std::move(front));
    }
    if (back.points.size() >= 3) {
      if (node.back >= 0)
        ClipTree(std::move(back), solid, node.back, ownBoundary, closedTarget,
                 removed, outside, cutBase);
      else
        removed = true;
    }
  }
}

std::vector<Polygon> Clip(const Polygon& polygon, const WorldSolid& solid,
                          bool ownBoundary, bool closedTarget, bool& removed,
                          uint64_t cutBase) {
  std::vector<Polygon> outside;
  if (!solid.local->convex) {
    ClipTree(polygon, solid, 0, ownBoundary, closedTarget, removed, outside,
             cutBase);
    return outside;
  }
  Polygon remaining = polygon;
  for (size_t planeIndex = 0; planeIndex < solid.nodes.size(); ++planeIndex) {
    const Node& node = solid.nodes[planeIndex];
    const int side = Side(remaining, node.plane);
    if (side == 1) {
      outside.push_back(std::move(remaining));
      return outside;
    }
    if (side == 0) {
      if (!closedTarget ||
          (ownBoundary && Dot(remaining.normal, node.plane.normal) > 0)) {
        outside.push_back(std::move(remaining));
        return outside;
      }
    } else if (side == 3) {
      Polygon f, b;
      Split(remaining, node.plane, f, b, cutBase + planeIndex);
      if (f.points.size() >= 3) outside.push_back(std::move(f));
      remaining = std::move(b);
      if (remaining.points.size() < 3) return outside;
    }
  }
  removed = true;
  return outside;
}

bool Overlap(const float* a, const float* b) {
  for (int k = 0; k < 3; ++k)
    if (a[k] > b[k + 3] + kEpsilon || b[k] > a[k + 3] + kEpsilon) return false;
  return true;
}
void Bounds(const Polygon& p, float* bounds) {
  ResetBounds(bounds);
  for (Point v : p.points) ExpandBounds(bounds, V(v));
}

// Segments use the same volume classification. A boundary segment is retained
// only by its owner.
void ClipSegment(Point a, Point b, const WorldSolid& solid, int index,
                 bool ownBoundary,
                 std::vector<std::pair<Point, Point>>& outside, bool& removed) {
  if (solid.local->convex) {
    double enter = 0, exit = 1;
    for (const Node& node : solid.nodes) {
      const double da = node.plane.Distance(a), db = node.plane.Distance(b);
      if (std::fabs(da) <= kEpsilon && std::fabs(db) <= kEpsilon &&
          ownBoundary) {
        outside.push_back({a, b});
        return;
      }
      if (da > kEpsilon && db > kEpsilon) {
        outside.push_back({a, b});
        return;
      }
      if (da > kEpsilon) enter = std::max(enter, da / (da - db));
      if (db > kEpsilon) exit = std::min(exit, da / (da - db));
    }
    if (enter >= exit - kEpsilon) {
      outside.push_back({a, b});
      return;
    }
    if (enter > kEpsilon) outside.push_back({a, a + (b - a) * enter});
    if (exit < 1 - kEpsilon) outside.push_back({a + (b - a) * exit, b});
    removed = true;
    return;
  }
  const Node& node = solid.nodes[index];
  const double da = node.plane.Distance(a), db = node.plane.Distance(b);
  auto front = [&](Point x, Point y) {
    if (node.front >= 0)
      ClipSegment(x, y, solid, node.front, ownBoundary, outside, removed);
    else
      outside.push_back({x, y});
  };
  auto back = [&](Point x, Point y) {
    if (node.back >= 0)
      ClipSegment(x, y, solid, node.back, ownBoundary, outside, removed);
    else
      removed = true;
  };
  if (std::fabs(da) <= kEpsilon && std::fabs(db) <= kEpsilon) {
    if (ownBoundary)
      front(a, b);
    else
      back(a, b);
  } else if (da >= -kEpsilon && db >= -kEpsilon)
    front(a, b);
  else if (da <= kEpsilon && db <= kEpsilon)
    back(a, b);
  else {
    const Point cut = a + (b - a) * (da / (da - db));
    if (da > 0) {
      front(a, cut);
      back(cut, b);
    } else {
      back(a, cut);
      front(cut, b);
    }
  }
}

}  // namespace

bool OptimizeCollisionSurfaces(
    const std::vector<Mesh>& source, const std::vector<SpatialNode>& nodes,
    const std::vector<uint32_t>& order, std::vector<Mesh>& output,
    SolidVisibilityStats& stats, const std::function<bool()>& cancelled,
    const std::function<void(uint32_t, uint32_t)>& progress) {
  const auto started = std::chrono::steady_clock::now();
  stats = {};
  output.clear();
  std::unordered_map<const Geometry*, std::shared_ptr<const Solid>> cache;
  std::vector<WorldSolid> solids(source.size());
  std::unordered_map<const Geometry*, EdgeTopology> edgeCache;
  for (size_t i = 0; i < source.size(); ++i) {
    if (cancelled()) return false;
    if (progress && (i & 511) == 0)
      progress(static_cast<uint32_t>(i),
               static_cast<uint32_t>(source.size() * 2));
    const Mesh& mesh = source[i];
    if (mesh.kind != MeshKind::kCollision) continue;
    stats.originalTriangles += mesh.geometry->triangles.size() / 3;
    stats.originalEdges += mesh.geometry->lines.size() / 2;
    auto it = cache.find(mesh.geometry.get());
    if (it == cache.end()) {
      std::shared_ptr<const Solid> solid;
      try {
        solid = MakeSolid(*mesh.geometry, cancelled);
      } catch (const std::runtime_error&) {
      }
      if (solid)
        ++stats.closedShapes;
      else
        ++stats.unclassifiedShapes;
      it = cache.emplace(mesh.geometry.get(), std::move(solid)).first;
    }
    solids[i] = PlaceSolid(mesh, it->second);
  }
  output.reserve(source.size());
  for (uint32_t index = 0; index < source.size(); ++index) {
    if (cancelled()) return false;
    if (progress && (index & 511) == 0)
      progress(index + static_cast<uint32_t>(source.size()),
               static_cast<uint32_t>(source.size() * 2));
    const Mesh& mesh = source[index];
    if (mesh.kind != MeshKind::kCollision) {
      output.push_back(mesh);
      continue;
    }
    std::vector<uint32_t> neighbours;
    std::function<void(uint32_t)> visit = [&](uint32_t n) {
      const auto& node = nodes[n];
      if (!Overlap(mesh.bounds, node.bounds)) return;
      if (node.left != ~0u) {
        visit(node.left);
        visit(node.right);
        return;
      }
      for (uint32_t k = node.begin; k < node.begin + node.count; ++k) {
        const uint32_t j = order[k];
        if (j != index && solids[j].local &&
            Overlap(mesh.bounds, source[j].bounds))
          neighbours.push_back(j);
      }
    };
    if (!nodes.empty()) visit(0);
    std::sort(neighbours.begin(), neighbours.end(),
              [&](uint32_t a, uint32_t b) {
                auto volume = [&](uint32_t j) {
                  const float* v = source[j].bounds;
                  return double(v[3] - v[0]) * (v[4] - v[1]) * (v[5] - v[2]);
                };
                const double av = volume(a), bv = volume(b);
                return av == bv ? a < b : av > bv;
              });
    bool contained = false;
    for (uint32_t j : neighbours)
      if (solids[j].local->convex) {
        bool all = true;
        for (unsigned corner = 0; corner < 8 && all; ++corner) {
          const Point p = {mesh.bounds[corner & 1 ? 3 : 0],
                           mesh.bounds[corner & 2 ? 4 : 1],
                           mesh.bounds[corner & 4 ? 5 : 2]};
          for (const Node& n : solids[j].nodes)
            if (n.plane.Distance(p) >= -kEpsilon) {
              all = false;
              break;
            }
        }
        if (all) {
          contained = true;
          break;
        }
      }
    if (contained) {
      ++stats.removed;
      continue;
    }
    if (neighbours.empty()) {
      output.push_back(mesh);
      stats.triangles += mesh.geometry->triangles.size() / 3;
      stats.edges += mesh.geometry->lines.size() / 2;
      continue;
    }
    auto result = std::make_shared<Geometry>();
    result->kind = mesh.kind;
    bool changed = false;
    auto topology = edgeCache.find(mesh.geometry.get());
    if (topology == edgeCache.end()) {
      try {
        topology = edgeCache
                       .emplace(mesh.geometry.get(),
                                MakeEdgeTopology(*mesh.geometry, cancelled))
                       .first;
      } catch (const std::runtime_error&) {
        return false;
      }
    }
    CutContours contours(topology->second);
    uint64_t nextCut = topology->second.edges.size();
    for (size_t k = 0; k < mesh.geometry->triangles.size(); k += 3) {
      if ((k & 1023) == 0 && cancelled()) return false;
      Polygon triangle;
      const auto& originalEdges = topology->second.triangles[k / 3];
      triangle.edges.assign(originalEdges.begin(), originalEdges.end());
      for (int v = 0; v < 3; ++v)
        triangle.points.push_back(World(mesh, mesh.geometry->triangles[k + v]));
      triangle.normal = Cross(triangle.points[1] - triangle.points[0],
                              triangle.points[2] - triangle.points[0]) *
                        (solids[index].local ? solids[index].orientation : 1);
      std::vector<Polygon> fragments = {triangle};
      for (uint32_t j : neighbours) {
        if (fragments.empty()) break;
        const uint64_t cutBase = nextCut;
        nextCut += solids[j].nodes.size();
        std::vector<Polygon> next;
        for (const Polygon& p : fragments) {
          float bounds[6];
          Bounds(p, bounds);
          if (!Overlap(bounds, source[j].bounds)) {
            next.push_back(p);
            continue;
          }
          bool removed = false;
          auto clipped = Clip(p, solids[j], index < j,
                              bool(solids[index].local), removed, cutBase);
          if (!removed)
            next.push_back(p);
          else {
            changed = true;
            for (auto& f : clipped) next.push_back(std::move(f));
          }
        }
        fragments = std::move(next);
      }
      for (const Polygon& p : fragments) contours.Add(p);
      for (const Polygon& p : fragments)
        for (size_t v = 1; v + 1 < p.points.size(); ++v) {
          if (Dot(Cross(p.points[v] - p.points[0],
                        p.points[v + 1] - p.points[0]),
                  Cross(p.points[v] - p.points[0],
                        p.points[v + 1] - p.points[0])) < 1e-22)
            continue;
          result->triangles.insert(
              result->triangles.end(),
              {V(p.points[0]), V(p.points[v]), V(p.points[v + 1])});
        }
    }
    for (size_t k = 0; k + 1 < mesh.geometry->lines.size(); k += 2) {
      if ((k & 1023) == 0 && cancelled()) return false;
      std::vector<std::pair<Point, Point>> segments = {
          {World(mesh, mesh.geometry->lines[k]),
           World(mesh, mesh.geometry->lines[k + 1])}};
      for (uint32_t j : neighbours) {
        if (segments.empty()) break;
        std::vector<std::pair<Point, Point>> next;
        for (const auto& segment : segments) {
          float bounds[6];
          ResetBounds(bounds);
          ExpandBounds(bounds, V(segment.first));
          ExpandBounds(bounds, V(segment.second));
          if (!Overlap(bounds, source[j].bounds)) {
            next.push_back(segment);
            continue;
          }
          bool removed = false;
          std::vector<std::pair<Point, Point>> clipped;
          ClipSegment(segment.first, segment.second, solids[j], 0, index < j,
                      clipped, removed);
          if (!removed)
            next.push_back(segment);
          else {
            changed = true;
            next.insert(next.end(), clipped.begin(), clipped.end());
          }
        }
        segments = std::move(next);
      }
      for (const auto& segment : segments)
        result->lines.insert(result->lines.end(),
                             {V(segment.first), V(segment.second)});
    }
    if (changed &&
        !contours.EmitSegments(
            [&](Point a, Point b) {
              std::vector<std::pair<Point, Point>> segments = {{a, b}};
              // Keep one stable owner at a shared boundary, and remove any part
              // buried by a third solid. Open targets have no complementary
              // solid owner.
              for (uint32_t j : neighbours) {
                if (segments.empty()) break;
                std::vector<std::pair<Point, Point>> next;
                for (const auto& segment : segments) {
                  bool removed = false;
                  ClipSegment(segment.first, segment.second, solids[j], 0,
                              index < j || !solids[index].local, next, removed);
                }
                segments = std::move(next);
              }
              for (const auto& segment : segments) {
                if (Dot(segment.second - segment.first,
                        segment.second - segment.first) <= kEpsilon * kEpsilon)
                  continue;
                result->lines.insert(result->lines.end(),
                                     {V(segment.first), V(segment.second)});
                ++stats.intersectionEdges;
              }
            },
            cancelled))
      return false;
    Mesh next = mesh;
    if (changed) {
      next.geometry = std::move(result);
      next.transform = Transform{};
    }
    if (next.geometry->triangles.empty() && next.geometry->lines.empty()) {
      ++stats.removed;
      continue;
    }
    if (changed) ++stats.trimmed;
    stats.triangles += next.geometry->triangles.size() / 3;
    stats.edges += next.geometry->lines.size() / 2;
    output.push_back(std::move(next));
  }
  stats.milliseconds = std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - started)
                           .count();
  return true;
}

}  // namespace doomfix::cv
