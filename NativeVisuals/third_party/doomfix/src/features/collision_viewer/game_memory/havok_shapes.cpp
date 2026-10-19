// Based on doom-eternal-collision-viewer by IlyaTsyganov (commit 43ac57d), used with the author's permission.

#include "features/collision_viewer/game_memory/havok_shapes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "features/collision_viewer/game_memory/exe_profile.h"

namespace doomfix::cv {
namespace {

using namespace exe11;

uint16_t U16(const unsigned char* data, size_t at) {
  uint16_t n;
  std::memcpy(&n, data + at, 2);
  return n;
}

uint32_t EdgeKey(unsigned a, unsigned b) { return (std::min(a, b) << 16) | std::max(a, b); }

}  // namespace

bool ReadInstanceTransform(const unsigned char* row, Transform& local) {
  float columns[16];
  float scale[3];
  std::memcpy(columns, row, 64);
  std::memcpy(scale, row + kInstanceScale, 12);
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 4; ++c) {
      const float v = c < 3 ? columns[c * 4 + r] * scale[c] : columns[12 + r];
      if (!std::isfinite(v) || std::fabs(v) > 1000000.0f) return false;
      local.m[r * 4 + c] = v;
    }
  }
  return true;
}

bool ShapeDecoder::Decode(uintptr_t address, const Transform& transform, Geometry& geometry, unsigned depth) {
  if (depth > 12 || geometry.triangles.size() > kMaxTriangleVertices) return false;
  unsigned char header[kShapeReadSize] = {};
  if (!memory_.Read(address, header, sizeof(header))) return Fail("shape header unreadable");
  if (header[kShapeType] == kShapeCompound) {
    uintptr_t rowsAddress = 0;
    uint32_t count = 0;
    std::memcpy(&rowsAddress, header + kCompoundInstances, 8);
    std::memcpy(&count, header + kCompoundCount, 4);
    if (count > 4096) return Fail("compound count");
    std::vector<unsigned char> rows(size_t(count) * kInstanceSize);
    if (count && !memory_.Read(rowsAddress, rows.data(), rows.size())) return Fail("compound instances");
    for (uint32_t i = 0; i < count; ++i) {
      const unsigned char* row = rows.data() + size_t(i) * kInstanceSize;
      if (row[kInstanceDisabled]) continue;
      uintptr_t child = 0;
      std::memcpy(&child, row + kInstanceShape, 8);
      Transform local;
      if (!ReadInstanceTransform(row, local)) return Fail("compound transform");
      if (!Decode(child, transform.Compose(local), geometry, depth + 1)) return false;
    }
    return true;
  }
  if (contents_ && !contents_->AllowsShape(address)) return true;
  if (header[kShapeType] == kShapeCompressedMesh) return Compressed(address, transform, geometry);
  // Box (тип 6) — тоже выпуклый многогранник с теми же массивами.
  if (!(U16(header, kShapeFlags) & kShapeFlagConvex)) {
    error_ = "shape type " + std::to_string(header[kShapeType]);
    return false;
  }
  return Convex(address, header, transform, geometry);
}

bool ShapeDecoder::Convex(uintptr_t address, const unsigned char* header, const Transform& transform,
                          Geometry& geometry) {
  const uint16_t vertices = U16(header, kConvexVertices);
  const uint16_t faces = U16(header, kConvexFaces);
  const uint16_t indices = U16(header, kConvexIndices);
  if (vertices < 4 || vertices > 256 || faces < 4 || faces > 512 || indices > 8192) return Fail("convex counts");
  std::vector<std::array<float, 4>> points(vertices);
  std::vector<std::array<unsigned char, 4>> polygons(faces);
  std::vector<unsigned char> corners(indices);
  if (!memory_.Read(address + kConvexVertices + U16(header, kConvexVertices + 2), points.data(),
                    points.size() * 16) ||
      !memory_.Read(address + kConvexFaces + U16(header, kConvexFaces + 2), polygons.data(), polygons.size() * 4) ||
      !memory_.Read(address + kConvexIndices + U16(header, kConvexIndices + 2), corners.data(), corners.size())) {
    return Fail("convex arrays");
  }
  for (const auto& point : points) {
    for (int k = 0; k < 3; ++k) {
      if (!std::isfinite(point[k]) || std::fabs(point[k]) > 100000.0f) return Fail("convex vertex");
    }
  }
  auto point = [&](unsigned index) {
    return transform.Apply({points[index][0], points[index][1], points[index][2]});
  };
  std::unordered_set<uint32_t> edges;
  for (const auto& face : polygons) {
    const unsigned first = face[0] | (unsigned(face[1]) << 8);
    const unsigned count = face[2];
    if (count < 3 || first + count > corners.size()) return Fail("convex face");
    for (unsigned i = 0; i < count; ++i) {
      if (corners[first + i] >= vertices) return Fail("convex face vertex");
    }
    for (unsigned i = 1; i + 1 < count; ++i) {
      geometry.triangles.push_back(point(corners[first]));
      geometry.triangles.push_back(point(corners[first + i]));
      geometry.triangles.push_back(point(corners[first + i + 1]));
    }
    for (unsigned i = 0; i < count; ++i) {
      const unsigned a = corners[first + i];
      const unsigned b = corners[first + (i + 1) % count];
      if (edges.insert(EdgeKey(a, b)).second) {
        geometry.lines.push_back(point(a));
        geometry.lines.push_back(point(b));
      }
    }
  }
  return true;
}

bool ShapeDecoder::Compressed(uintptr_t address, const Transform& transform, Geometry& geometry) {
  uintptr_t data = 0;
  if (!memory_.Get(address + kMeshData, data)) return Fail("compressed data pointer");
  const uintptr_t tree = data + kMeshTree;
  float domain[8] = {};
  std::vector<std::array<unsigned char, kSectionSize>> sections;
  std::vector<std::array<unsigned char, 4>> primitives;
  std::vector<uint16_t> sharedIndices;
  std::vector<uint32_t> packed;
  std::vector<uint64_t> shared;
  std::vector<std::array<unsigned char, 4>> dataRuns;
  if (!memory_.Read(tree + kTreeDomain, domain, 32) ||
      !ReadHavokArray<std::array<unsigned char, kSectionSize>>(memory_, tree + kTreeSections, sections, 4096) ||
      !ReadHavokArray<std::array<unsigned char, 4>>(memory_, tree + kTreePrimitives, primitives) ||
      !ReadHavokArray<uint16_t>(memory_, tree + kTreeSharedIndices, sharedIndices) ||
      !ReadHavokArray<uint32_t>(memory_, tree + kTreePackedVertices, packed) ||
      !ReadHavokArray<uint64_t>(memory_, tree + kTreeSharedVertices, shared)) {
    return Fail("compressed arrays");
  }
  for (float v : domain) {
    if (!std::isfinite(v)) return Fail("compressed domain nonfinite");
  }
  // Old/unknown metadata falls back to the original geometry. The normal world
  // path uses per-primitive shape tags, rather than an aggregate mesh flag.
  const bool haveTags = contents_ &&
      ReadHavokArray<std::array<unsigned char, 4>>(memory_, tree + kTreeDataRuns, dataRuns);
  // Общие вершины: 21+21+22 бита в домене дерева.
  std::vector<Vec3> sharedPoints;
  sharedPoints.reserve(shared.size());
  for (uint64_t v : shared) {
    const uint32_t q[3] = {uint32_t(v & 0x1fffff), uint32_t((v >> 21) & 0x1fffff), uint32_t((v >> 42) & 0x3fffff)};
    float p[3];
    for (int k = 0; k < 3; ++k) {
      p[k] = domain[k] + q[k] * (domain[k + 4] - domain[k]) / float(k == 2 ? 0x3fffff : 0x1fffff);
    }
    sharedPoints.push_back({p[0], p[1], p[2]});
  }
  struct Edge {
    unsigned a, b;
    unsigned count = 0;
    Vec3 normal{};
    bool crease = false;
  };
  std::vector<Vec3> points;
  std::unordered_map<uint32_t, Edge> edges;
  for (const auto& section : sections) {
    float codec[6];
    uint32_t first = 0;
    uint32_t sharedDesc = 0;
    uint32_t primitiveDesc = 0;
    uint32_t runDesc = 0;
    std::memcpy(codec, section.data() + kSectionCodec, 24);
    std::memcpy(&first, section.data() + kSectionFirstPacked, 4);
    std::memcpy(&sharedDesc, section.data() + kSectionShared, 4);
    std::memcpy(&primitiveDesc, section.data() + kSectionPrimitives, 4);
    std::memcpy(&runDesc, section.data() + kSectionDataRuns, 4);
    const unsigned packedCount = section[kSectionPackedCount];
    const unsigned sharedCount = section[kSectionSharedCount];
    if (size_t(first) + packedCount > packed.size() ||
        size_t(sharedDesc >> 8) + sharedCount > sharedIndices.size() ||
        size_t(primitiveDesc >> 8) + (primitiveDesc & 255) > primitives.size()) {
      return Fail("compressed section range");
    }
    if (packedCount) {
      for (float v : codec) {
        if (!std::isfinite(v)) return Fail("compressed codec nonfinite");
      }
    }
    // Вершины секции: упакованные (11+11+10 бит в кодеке секции), затем общие.
    points.clear();
    for (unsigned i = 0; i < packedCount; ++i) {
      const uint32_t v = packed[first + i];
      points.push_back({codec[0] + (v & 2047) * codec[3], codec[1] + ((v >> 11) & 2047) * codec[4],
                        codec[2] + ((v >> 22) & 1023) * codec[5]});
    }
    for (unsigned i = 0; i < sharedCount; ++i) {
      const unsigned index = sharedIndices[(sharedDesc >> 8) + i];
      if (index >= sharedPoints.size()) return Fail("compressed shared vertex index");
      points.push_back(sharedPoints[index]);
    }
    // Ребро — в контур, если оно край (у него не два треугольника) или излом (нормали треугольников не
    // параллельны). Диагональ плоского четырёхугольника — ни то ни другое.
    edges.clear();
    auto triangle = [&](unsigned a, unsigned b, unsigned c) {
      if (a == b || b == c || a == c) return;
      for (unsigned i : {a, b, c}) geometry.triangles.push_back(transform.Apply(points[i]));
      const Vec3 u{points[b].x - points[a].x, points[b].y - points[a].y, points[b].z - points[a].z};
      const Vec3 v{points[c].x - points[a].x, points[c].y - points[a].y, points[c].z - points[a].z};
      const Vec3 normal{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
      for (auto pair : {std::pair<unsigned, unsigned>{a, b}, {b, c}, {c, a}}) {
        auto [it, inserted] = edges.emplace(EdgeKey(pair.first, pair.second), Edge{pair.first, pair.second});
        Edge& edge = it->second;
        if (inserted) {
          edge.normal = normal;
        } else {
          const double aa = double(edge.normal.x) * edge.normal.x + double(edge.normal.y) * edge.normal.y +
                            double(edge.normal.z) * edge.normal.z;
          const double bb =
              double(normal.x) * normal.x + double(normal.y) * normal.y + double(normal.z) * normal.z;
          const double dot =
              double(edge.normal.x) * normal.x + double(edge.normal.y) * normal.y + double(edge.normal.z) * normal.z;
          edge.crease = edge.crease || aa < 1e-18 || bb < 1e-18 || dot * dot < .99999 * .99999 * aa * bb;
        }
        ++edge.count;
      }
    };
    std::array<uint16_t, 256> tags;
    tags.fill(0xffff);
    if (haveTags && size_t(runDesc >> 8) + (runDesc & 255) <= dataRuns.size()) {
      for (unsigned i = 0; i < (runDesc & 255); ++i) {
        const auto& run = dataRuns[(runDesc >> 8) + i];
        if (unsigned(run[2]) + run[3] > (primitiveDesc & 255)) continue;
        for (unsigned j = run[2]; j < unsigned(run[2]) + run[3]; ++j)
          tags[j] = U16(run.data(), 0);
      }
    }
    for (unsigned i = 0; i < (primitiveDesc & 255); ++i) {
      const auto& p = primitives[(primitiveDesc >> 8) + i];
      if (p == std::array<unsigned char, 4>{0xde, 0xad, 0xde, 0xad}) continue;
      if (contents_ && !contents_->AllowsTag(address, tags[i])) continue;
      for (unsigned v : p) {
        if (v >= points.size()) return Fail("compressed primitive vertex index");
      }
      triangle(p[0], p[1], p[2]);
      triangle(p[0], p[2], p[3]);
    }
    for (const auto& item : edges) {
      if (item.second.count != 2 || item.second.crease) {
        geometry.lines.push_back(transform.Apply(points[item.second.a]));
        geometry.lines.push_back(transform.Apply(points[item.second.b]));
      }
    }
    if (geometry.triangles.size() > kMaxTriangleVertices) return Fail("compressed vertex budget");
  }
  return true;
}

}  // namespace doomfix::cv
