#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <tuple>

#include "features/collision_viewer/scene/solid_visibility.h"

using namespace doomfix::cv;
namespace {
int failures = 0;
void Check(bool ok, const char* message) {
  std::printf("%s %s\n", ok ? "ok" : "FAIL", message);
  if (!ok) ++failures;
}

std::shared_ptr<Geometry> Box(float x0, float y0, float z0, float x1, float y1,
                              float z1) {
  auto g = std::make_shared<Geometry>();
  Vec3 v[8];
  for (int i = 0; i < 8; ++i)
    v[i] = {i & 1 ? x1 : x0, i & 2 ? y1 : y0, i & 4 ? z1 : z0};
  const unsigned faces[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4},
                                {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
  std::set<std::pair<unsigned, unsigned>> edges;
  for (const auto& f : faces) {
    for (unsigned k : {0u, 1u})
      g->triangles.insert(g->triangles.end(),
                          {v[f[0]], v[f[k + 1]], v[f[k + 2]]});
    for (int k = 0; k < 4; ++k) {
      unsigned a = f[k], b = f[(k + 1) % 4];
      if (edges.insert(std::minmax(a, b)).second)
        g->lines.insert(g->lines.end(), {v[a], v[b]});
    }
  }
  return g;
}
Mesh Instance(std::shared_ptr<const Geometry> g, Transform transform = {}) {
  Mesh m;
  m.geometry = std::move(g);
  m.transform = transform;
  ResetBounds(m.bounds);
  for (Vec3 p : m.geometry->triangles) ExpandBounds(m.bounds, m.World(p));
  for (Vec3 p : m.geometry->lines) ExpandBounds(m.bounds, m.World(p));
  return m;
}
double Area(const Mesh& mesh) {
  double result = 0;
  const auto& v = mesh.geometry->triangles;
  for (size_t i = 0; i < v.size(); i += 3) {
    const Vec3 a = mesh.World(v[i]), b = mesh.World(v[i + 1]),
               c = mesh.World(v[i + 2]);
    const double ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z, vx = c.x - a.x,
                 vy = c.y - a.y, vz = c.z - a.z;
    result += .5 * std::sqrt(std::pow(uy * vz - uz * vy, 2) +
                             std::pow(uz * vx - ux * vz, 2) +
                             std::pow(ux * vy - uy * vx, 2));
  }
  return result;
}
std::vector<Mesh> Run(const std::vector<Mesh>& meshes,
                      SolidVisibilityStats& stats) {
  std::vector<SpatialNode> nodes;
  std::vector<uint32_t> order;
  BuildBvh(meshes, nodes, order);
  std::vector<Mesh> result;
  Check(OptimizeCollisionSurfaces(meshes, nodes, order, result, stats,
                                  [] { return false; }),
        "optimization completes");
  return result;
}
double Area(const std::vector<Mesh>& meshes) {
  double result = 0;
  for (const auto& m : meshes) result += Area(m);
  return result;
}

void NestedAndOverlap() {
  SolidVisibilityStats stats;
  auto outer = Box(-2, -2, -2, 2, 2, 2),
       inner = Box(-.5f, -.5f, -.5f, .5f, .5f, .5f);
  auto result = Run({Instance(outer), Instance(inner)}, stats);
  Check(result.size() == 1 && result[0].geometry == outer && stats.removed == 1,
        "fully nested volume disappears; outer geometry stays shared");
  result = Run({Instance(outer), Instance(outer)}, stats);
  Check(result.size() == 1 && std::fabs(Area(result) - 96) < 1e-6,
        "identical solids have one stable owner, not two removals");
  const auto a = Instance(Box(0, 0, 0, 2, 2, 2)),
             b = Instance(Box(1, 0, 0, 3, 2, 2));
  result = Run({a, b}, stats);
  Check(
      result.size() == 2 && stats.trimmed == 2 &&
          std::fabs(Area(result) - 32) < 1e-6,
      "partially overlapping boxes retain exactly the exterior union surface");
  bool noCaps = true;
  for (const auto& m : result) {
    for (size_t i = 0; i < m.geometry->triangles.size(); i += 3) {
      const auto& p = m.geometry->triangles;
      const float x = (p[i].x + p[i + 1].x + p[i + 2].x) / 3;
      const float y = (p[i].y + p[i + 1].y + p[i + 2].y) / 3,
                  z = (p[i].z + p[i + 1].z + p[i + 2].z) / 3;
      if ((std::fabs(x - 1) < 1e-6 || std::fabs(x - 2) < 1e-6) && y > 1e-6 &&
          y < 2 - 1e-6 && z > 1e-6 && z < 2 - 1e-6)
        noCaps = false;
    }
  }
  Check(noCaps,
        "no artificial faces are generated across the volume intersection");
  Check(std::memcmp(result[0].bounds, a.bounds, sizeof(a.bounds)) == 0 &&
            std::memcmp(result[1].bounds, b.bounds, sizeof(b.bounds)) == 0,
        "trimmed meshes retain the original whole-object distance bounds");
  result =
      Run({Instance(Box(0, 0, 0, 1, 1, 1)), Instance(Box(1, 0, 0, 2, 1, 1))},
          stats);
  Check(std::fabs(Area(result) - 10) < 1e-6,
        "touching boxes lose their internal shared face without losing "
        "exterior faces");
}

void OpenAndRotated() {
  SolidVisibilityStats stats;
  auto sheet = std::make_shared<Geometry>();
  sheet->triangles = {{-2, -2, .5f}, {2, -2, .5f}, {2, 2, .5f},
                      {-2, -2, .5f}, {2, 2, .5f},  {-2, 2, .5f}};
  sheet->lines = {{-2, 0, .5f}, {2, 0, .5f}};
  auto result =
      Run({Instance(sheet), Instance(Box(-1, -1, 0, 1, 1, 1))}, stats);
  Check(result.size() == 2 && std::fabs(Area(result[0]) - 12) < 1e-6,
        "an open target spanning a solid is cut even when all its vertices lie "
        "outside");
  const auto& lines = result[0].geometry->lines;
  Check(lines.size() >= 4 && lines[0].x == -2 && lines[1].x == -1 &&
            lines[2].x == 1 && lines[3].x == 2,
        "original contours retain the two unabsorbed segments before new cut "
        "contours");
  auto openBox = Box(-2, -2, -2, 2, 2, 2);
  openBox->triangles.resize(30);
  result =
      Run({Instance(openBox), Instance(Box(-.5f, -.5f, -.5f, .5f, .5f, .5f))},
          stats);
  Check(result.size() == 2 && stats.removed == 0,
        "an open shell never acts as a solid occluder");
  Transform rotate;
  const float c = std::sqrt(.5f);
  rotate.m[0] = rotate.m[5] = c;
  rotate.m[1] = -c;
  rotate.m[4] = c;
  result = Run({Instance(Box(-1, -1, -1, 1, 1, 1), rotate),
                Instance(Box(1.15f, 1.15f, -.2f, 1.3f, 1.3f, .2f)),
                Instance(Box(-.1f, -.1f, -.1f, .1f, .1f, .1f))},
               stats);
  Check(result.size() == 2 && stats.removed == 1,
        "rotated actual faces hide the inner box but preserve a box in an "
        "empty AABB corner");
  auto reversed = Box(-1, -1, -1, 1, 1, 1);
  for (size_t i = 0; i < reversed->triangles.size(); i += 3)
    std::swap(reversed->triangles[i], reversed->triangles[i + 2]);
  Transform mirror;
  mirror.m[0] = -2;
  mirror.m[5] = .5f;
  mirror.m[3] = 100;
  result = Run({Instance(reversed, mirror),
                Instance(Box(99.9f, -.1f, -.1f, 100.1f, .1f, .1f))},
               stats);
  Check(result.size() == 1 && stats.removed == 1,
        "reversed winding, reflection, nonuniform scale and translation retain "
        "correct solid interiors");
  result = Run({Instance(Box(-1, -1, -1, 1, 1, 1), rotate),
                Instance(Box(0, -3, -3, 3, 3, 3))},
               stats);
  Check(result.size() == 2 && std::fabs(Area(result[0]) - 12) < 1e-4 &&
            std::fabs(Area(result) - (156 - 4 * std::sqrt(2.0))) < 1e-4,
        "rotated partial overlap retains the analytic exterior area of both "
        "solids");
  result = Run(
      {Instance(Box(0, 0, 0, 3, 1, 1)), Instance(Box(-1, -1, -1, 1.5f, 2, 2)),
       Instance(Box(1.5f, -1, -1, 4, 2, 2))},
      stats);
  Check(result.size() == 2 && stats.removed == 1 &&
            std::fabs(Area(result) - 78) < 1e-5,
        "two neighbours jointly absorb an object that neither contains alone");
  result = Run({Instance(Box(-1, -1, -1, 1, 1, 1)),
                Instance(Box(.9999f, -.1f, -.1f, 1.0001f, .1f, .1f))},
               stats);
  bool protrusion = false;
  if (result.size() == 2)
    for (Vec3 p : result[1].geometry->triangles) protrusion |= p.x > 1.00005f;
  Check(protrusion,
        "a real thin protrusion is preserved instead of dropping its whole "
        "object");
}

template <class Predicate>
double LineLength(const std::vector<Mesh>& meshes, const Predicate& accept) {
  double length = 0;
  for (const auto& mesh : meshes)
    for (size_t i = 0; i + 1 < mesh.geometry->lines.size(); i += 2) {
      const Vec3 a = mesh.World(mesh.geometry->lines[i]);
      const Vec3 b = mesh.World(mesh.geometry->lines[i + 1]);
      if (accept(a, b))
        length += std::sqrt(std::pow(double(a.x) - b.x, 2) +
                            std::pow(double(a.y) - b.y, 2) +
                            std::pow(double(a.z) - b.z, 2));
    }
  return length;
}
bool Near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

void IntersectionContours() {
  SolidVisibilityStats stats;
  auto a = Instance(Box(-2, -2, -2, 2, 2, 2));
  auto b = Instance(Box(1, -1, -1, 3, 1, 1));
  auto seam = [](Vec3 p, Vec3 q) {
    return Near(p.x, 2) && Near(q.x, 2) &&
           std::max({std::fabs(p.y), std::fabs(p.z), std::fabs(q.y),
                     std::fabs(q.z)}) <= 1.00001f;
  };
  auto result = Run({a, b}, stats);
  Check(stats.intersectionEdges > 0 &&
            std::fabs(LineLength(result, seam) - 8) < 1e-5,
        "penetrating boxes keep the complete intersection rectangle exactly "
        "once");
  Check(std::fabs(Area(result) - 104) < 1e-5,
        "intersection lines do not change fill area or add cut caps");
  result = Run({b, a}, stats);
  Check(std::fabs(LineLength(result, seam) - 8) < 1e-5,
        "intersection ownership survives reversed object order without doubled "
        "lines");
  auto buried = Instance(Box(-3, -3, -3, 4, 3, 0));
  result = Run({a, b, buried}, stats);
  auto exposedSeam = [&](Vec3 p, Vec3 q) {
    return seam(p, q) &&
           ((Near(p.y, -1) && Near(q.y, -1)) ||
            (Near(p.y, 1) && Near(q.y, 1)) ||
            (Near(p.z, -1) && Near(q.z, -1)) || (Near(p.z, 1) && Near(q.z, 1)));
  };
  Check(std::fabs(LineLength(result, exposedSeam) - 4) < 1e-5,
        "a third solid removes the buried part of another pair's intersection "
        "line");

  Transform rotate;
  const float c = std::sqrt(.5f);
  rotate.m[0] = rotate.m[5] = c;
  rotate.m[1] = -c;
  rotate.m[4] = c;
  result = Run({a, Instance(Box(-10, 0, -10, 10, 10, 10), rotate)}, stats);
  auto diagonal = [](Vec3 p, Vec3 q) {
    return Near(p.z, 2) && Near(q.z, 2) && Near(p.x, p.y) && Near(q.x, q.y);
  };
  Check(std::fabs(LineLength(result, diagonal) - 4 * std::sqrt(2.0)) < 1e-4,
        "a cut coinciding with an original triangulation diagonal is not lost");

  Transform mirrored;
  mirrored.m[0] = -2;
  mirrored.m[5] = 3;
  mirrored.m[10] = .5f;
  mirrored.m[3] = 100;
  mirrored.m[7] = -150;
  mirrored.m[11] = 20;
  result = Run({Instance(a.geometry, mirrored), Instance(b.geometry, mirrored)},
               stats);
  Check(std::fabs(LineLength(result,
                             [](Vec3 p, Vec3 q) {
                               return Near(p.x, 96) && Near(q.x, 96) &&
                                      std::min(p.y, q.y) >= -153.00001f &&
                                      std::max(p.y, q.y) <= -146.99999f &&
                                      std::min(p.z, q.z) >= 19.49999f &&
                                      std::max(p.z, q.z) <= 20.50001f;
                             }) -
                  14) < 1e-4,
        "translated mirrored nonuniform instances retain correctly placed "
        "intersection lines");
  result = Run({a, Instance(Box(-.5f, -.5f, -.5f, .5f, .5f, .5f))}, stats);
  Check(stats.intersectionEdges == 0,
        "a fully absorbed object leaves no intersection contour");
  result = Run({a, a}, stats);
  Check(stats.intersectionEdges == 0,
        "duplicate solids do not invent intersection contours");
  result = Run({a, Instance(Box(2, 2, 2, 3, 3, 3))}, stats);
  Check(stats.intersectionEdges == 0,
        "point contact does not generate zero-length seams");

  auto sheet = std::make_shared<Geometry>();
  sheet->triangles = {{-3, -3, .5f}, {3, -3, .5f}, {3, 3, .5f},
                      {-3, -3, .5f}, {3, 3, .5f},  {-3, 3, .5f}};
  result = Run({Instance(sheet), Instance(Box(-2, -1, 0, 1, 1, 1)),
                Instance(Box(-1, -.5f, 0, 2, .5f, 1))},
               stats);
  std::vector<Mesh> first = {result[0]};
  Check(
      std::fabs(LineLength(first, [](Vec3, Vec3) { return true; }) - 12) < 1e-5,
      "split polygon partitions cancel and overlapping cuts leave only their "
      "outer contour");
}

void Concave() {
  // Closed L made from three unit cells; retain only the external faces of the
  // cell union.
  const std::set<std::tuple<int, int, int>> cells = {
      {0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
  auto l = std::make_shared<Geometry>();
  const int adjacent[6][3] = {{0, 0, -1}, {0, 0, 1},  {0, -1, 0},
                              {0, 1, 0},  {-1, 0, 0}, {1, 0, 0}};
  for (const auto& cell : cells) {
    const auto [x, y, z] = cell;
    auto box = Box(float(x), float(y), float(z), float(x + 1), float(y + 1),
                   float(z + 1));
    for (int face = 0; face < 6; ++face)
      if (!cells.count({x + adjacent[face][0], y + adjacent[face][1],
                        z + adjacent[face][2]}))
        l->triangles.insert(l->triangles.end(),
                            box->triangles.begin() + face * 6,
                            box->triangles.begin() + (face + 1) * 6);
  }
  SolidVisibilityStats stats;
  auto result =
      Run({Instance(l), Instance(Box(1.2f, 1.2f, .2f, 1.8f, 1.8f, .8f)),
           Instance(Box(.2f, .2f, .2f, .8f, .8f, .8f))},
          stats);
  Check(stats.closedShapes == 3 && stats.removed == 1 && result.size() == 2,
        "concave closed solid preserves its notch while removing an interior "
        "object");
  auto two =
      Run({Instance(l), Instance(Box(-.5f, .2f, .2f, .5f, .8f, .8f))}, stats);
  Check(two.size() == 2 && stats.trimmed > 0 &&
            Area(two[1]) < Area(Instance(Box(-.5f, .2f, .2f, .5f, .8f, .8f))),
        "concave BSP clips a partially intersecting volume");
  Check(std::fabs(LineLength(two,
                             [](Vec3 p, Vec3 q) {
                               return Near(p.x, 0) && Near(q.x, 0);
                             }) -
                  2.4) < 1e-4,
        "concave BSP yields the true intersection contour without "
        "partition-plane diagonals");
  std::vector<SpatialNode> nodes;
  std::vector<uint32_t> order;
  const std::vector<Mesh> source = {Instance(l)};
  BuildBvh(source, nodes, order);
  std::vector<Mesh> cancelled;
  Check(!OptimizeCollisionSurfaces(source, nodes, order, cancelled, stats,
                                   [] { return true; }),
        "obsolete optimization cancels without publishing a partial result");
  auto hollow = Box(-2, -2, -2, 2, 2, 2), cavity = Box(-1, -1, -1, 1, 1, 1);
  for (size_t i = 0; i < cavity->triangles.size(); i += 3)
    std::swap(cavity->triangles[i], cavity->triangles[i + 2]);
  hollow->triangles.insert(hollow->triangles.end(), cavity->triangles.begin(),
                           cavity->triangles.end());
  auto cavityResult =
      Run({Instance(hollow), Instance(Box(-.5f, -.5f, -.5f, .5f, .5f, .5f)),
           Instance(Box(1.2f, -.2f, -.2f, 1.8f, .2f, .2f))},
          stats);
  Check(cavityResult.size() == 2 && stats.removed == 1,
        "a real internal cavity remains empty while material in its wall "
        "occludes");
  auto ambiguous = Box(0, 0, 0, 1, 1, 1), inverted = Box(4, 0, 0, 6, 1, 1);
  for (size_t i = 0; i < inverted->triangles.size(); i += 3)
    std::swap(inverted->triangles[i], inverted->triangles[i + 2]);
  ambiguous->triangles.insert(ambiguous->triangles.end(),
                              inverted->triangles.begin(),
                              inverted->triangles.end());
  auto gap = Run({Instance(ambiguous), Instance(Box(2, .2f, .2f, 3, .8f, .8f))},
                 stats);
  Check(gap.size() == 2 && stats.removed == 0,
        "inconsistently oriented disconnected shells cannot hide the empty gap "
        "between them");
}
}  // namespace

int main() {
  NestedAndOverlap();
  OpenAndRotated();
  Concave();
  IntersectionContours();
  return failures ? 1 : 0;
}
