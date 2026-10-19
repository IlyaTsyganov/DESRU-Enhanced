// Based on doom-eternal-collision-viewer by IlyaTsyganov (commit 43ac57d), used with the author's permission.

#include "features/collision_viewer/test_scene.h"

#include <array>
#include <cmath>
#include <cstdlib>

namespace doomfix::cv {
namespace {

using Color = std::array<float, 4>;

void Append(OverlayScene& scene, float x, float y, float z, const Color& color) {
  Vertex v;
  v.position[0] = x;
  v.position[1] = y;
  v.position[2] = z;
  for (int i = 0; i < 4; ++i) v.color[i] = color[i];
  scene.vertices.push_back(v);
}

// Прямоугольник из двух треугольников (координаты экрана -1..1, y вверх).
void AppendQuad(OverlayScene& scene, float x0, float y0, float x1, float y1, const Color& color) {
  Append(scene, x0, y0, 0, color);
  Append(scene, x1, y0, 0, color);
  Append(scene, x1, y1, 0, color);
  Append(scene, x0, y0, 0, color);
  Append(scene, x1, y1, 0, color);
  Append(scene, x0, y1, 0, color);
}

void AppendLine(OverlayScene& scene, float x0, float y0, float x1, float y1, const Color& color) {
  Append(scene, x0, y0, 0, color);
  Append(scene, x1, y1, 0, color);
}

// Добавляет вызов отрисовки вершин, добавленных начиная с first.
void AddDraw(OverlayScene& scene, Primitive primitive, uint32_t first, uint32_t view) {
  const auto end = static_cast<uint32_t>(scene.vertices.size());
  if (end > first) scene.draws.push_back({primitive, first, end - first, view});
}

// Камера «экран»: x и y вершины — сразу координаты экрана, расстояние (w) всегда 1. Порог threshold
// задаёт, с какой сырой глубины поверхность закрывает рисунок: шейдер восстанавливает расстояние до
// поверхности как -options.z / (ndc - options.y); при строке 2 = (0, 0, 0, 1 - threshold) это
// threshold / depth, и точка на расстоянии 1 прячется, когда depth > threshold.
Camera ScreenCamera(float threshold) {
  Camera c;
  c.matrix[0] = 1.0f;
  c.matrix[5] = 1.0f;
  c.matrix[11] = 1.0f - threshold;
  c.matrix[15] = 1.0f;
  return c;
}

}  // namespace

TestOptions ParseTestOptions(const std::string& test, const std::string& occlusion,
                             const std::string& consoleFraction, const std::string& readback) {
  TestOptions o;
  auto problem = [&](const std::string& text) {
    if (!o.problems.empty()) o.problems += "; ";
    o.problems += text;
  };
  if (test == "smoke") {
    o.mode = TestMode::kSmoke;
  } else if (test == "pattern") {
    o.mode = TestMode::kPattern;
  } else if (test == "scene") {
    o.mode = TestMode::kScene;
  } else if (!test.empty() && test != "0" && test != "off") {
    problem("unknown DOOMFIX_CV_TEST=" + test);
  }

  static const struct {
    const char* name;
    OcclusionCase value;
  } kCases[] = {
      {"hidden", OcclusionCase::kHidden},
      {"partial", OcclusionCase::kPartial},
      {"front", OcclusionCase::kFront},
      {"reverse", OcclusionCase::kReverse},
      {"inverted", OcclusionCase::kInverted},
      {"inverted-partial", OcclusionCase::kInvertedPartial},
      {"inverted-front", OcclusionCase::kInvertedFront},
      {"range-crossing", OcclusionCase::kRangeCrossing},
      {"dense-object", OcclusionCase::kDenseObject},
      {"coplanar-jitter", OcclusionCase::kCoplanarJitter},
      {"coplanar-jitter-reverse", OcclusionCase::kCoplanarJitterReverse},
      {"coplanar-jitter-grazing", OcclusionCase::kCoplanarJitterGrazing},
  };
  if (!occlusion.empty() && occlusion != "none") {
    for (const auto& c : kCases) {
      if (occlusion == c.name) o.occlusion = c.value;
    }
    if (o.occlusion == OcclusionCase::kNone) {
      problem("unknown DOOMFIX_CV_OCCLUSION_TEST=" + occlusion);
    } else if (o.mode == TestMode::kNone) {
      o.mode = TestMode::kSmoke;  // проверка окклюзии — разновидность smoke
    } else if (o.mode != TestMode::kSmoke) {
      problem("DOOMFIX_CV_OCCLUSION_TEST works only with DOOMFIX_CV_TEST=smoke");
      o.occlusion = OcclusionCase::kNone;
    }
  }

  if (!consoleFraction.empty()) {
    char* end = nullptr;
    const float value = std::strtof(consoleFraction.c_str(), &end);
    if (end && *end == '\0' && std::isfinite(value) && value >= 0.0f && value <= 1.0f) {
      o.consoleFraction = value;
    } else {
      problem("bad DOOMFIX_CV_TEST_CONSOLE_FRACTION=" + consoleFraction);
    }
  }

  o.readback = o.mode == TestMode::kSmoke || o.mode == TestMode::kScene;
  if (readback == "1") o.readback = o.mode != TestMode::kNone;
  if (readback == "0") o.readback = false;
  return o;
}

const char* TestModeName(TestMode mode) {
  switch (mode) {
    case TestMode::kNone: return "none";
    case TestMode::kSmoke: return "smoke";
    case TestMode::kPattern: return "pattern";
    case TestMode::kScene: return "scene";
  }
  return "?";
}

const char* OcclusionCaseName(OcclusionCase occlusion) {
  switch (occlusion) {
    case OcclusionCase::kNone: return "none";
    case OcclusionCase::kHidden: return "hidden";
    case OcclusionCase::kPartial: return "partial";
    case OcclusionCase::kFront: return "front";
    case OcclusionCase::kReverse: return "reverse";
    case OcclusionCase::kInverted: return "inverted";
    case OcclusionCase::kInvertedPartial: return "inverted-partial";
    case OcclusionCase::kInvertedFront: return "inverted-front";
    case OcclusionCase::kRangeCrossing: return "range-crossing";
    case OcclusionCase::kDenseObject: return "dense-object";
    case OcclusionCase::kCoplanarJitter: return "coplanar-jitter";
    case OcclusionCase::kCoplanarJitterReverse: return "coplanar-jitter-reverse";
    case OcclusionCase::kCoplanarJitterGrazing: return "coplanar-jitter-grazing";
  }
  return "?";
}

bool TestSource::ReadCamera(Camera& camera) {
  camera = Camera{};
  if (options_.occlusion != OcclusionCase::kNone) {
    // Перспектива: w = z, глубина z' = z - 0.05.
    camera.matrix[0] = camera.matrix[5] = camera.matrix[10] = camera.matrix[14] = 1.0f;
    camera.matrix[11] = -0.05f;
  } else {
    camera.matrix[0] = camera.matrix[5] = camera.matrix[10] = camera.matrix[15] = 1.0f;
  }
  return options_.Active();
}

bool TestSource::BuildScene(const FrameInfo& frame, const Camera& camera, OverlayScene& scene) {
  switch (options_.mode) {
    case TestMode::kNone:
      return false;
    case TestMode::kSmoke:
      // Как в исходном viewer: без найденного буфера глубины оверлей не рисуется — так смоук-тест
      // проверяет и трекер глубины.
      if (!frame.depthFound) return false;
      scene.views.push_back(OverlayView{});
      scene.views[0].camera = camera;
      if (options_.occlusion != OcclusionCase::kNone) {
        BuildOcclusion(scene);
      } else {
        BuildSmoke(scene);
      }
      scene.consoleFraction = options_.consoleFraction;
      return true;
    case TestMode::kPattern:
      BuildPattern(frame, scene);
      return true;
    case TestMode::kScene:
      return false;  // рисует GameMemorySource над FakeGame (collision_viewer.cpp)
  }
  return false;
}

void TestSource::BuildSmoke(OverlayScene& scene) const {
  scene.views[0].occlusion = false;
  Append(scene, -0.6f, -0.6f, 0, {1, 0, 0, 1});
  Append(scene, 0.6f, -0.6f, 0, {0, 1, 0, 1});
  Append(scene, 0.0f, 0.6f, 0, {0, 0, 1, 1});
  AddDraw(scene, Primitive::kTriangles, 0, 0);
  Append(scene, -0.8f, 0, 0, {1, 1, 1, 1});
  Append(scene, 0.8f, 0, 0, {1, 1, 1, 1});
  AddDraw(scene, Primitive::kLines, 3, 0);
}

void TestSource::BuildOcclusion(OverlayScene& scene) const {
  const OcclusionCase c = options_.occlusion;
  OverlayView& view = scene.views[0];
  view.occlusion = true;
  view.range = DepthRange::kViewport;
  const bool inverted = c == OcclusionCase::kInverted || c == OcclusionCase::kInvertedPartial ||
                        c == OcclusionCase::kInvertedFront || c == OcclusionCase::kCoplanarJitterReverse ||
                        c == OcclusionCase::kCoplanarJitterGrazing;
  view.depthMode = inverted ? 2 : 0;
  if (c == OcclusionCase::kRangeCrossing) view.farDistance = 2.1f;
  const bool jitter = c == OcclusionCase::kCoplanarJitter || c == OcclusionCase::kCoplanarJitterReverse ||
                      c == OcclusionCase::kCoplanarJitterGrazing;
  if (jitter) view.farDistance = 100.0f;
  // Стена тестовой программы — на расстоянии 1; квадрат за ней (2) или перед ней (0.5).
  const float z = (c == OcclusionCase::kFront || c == OcclusionCase::kInvertedFront) ? 0.5f : 2.0f;
  const Color green = {0.25f, 0.95f, 0.12f, 1};
  const Color cyan = {0.1f, 0.85f, 1, 1};
  Append(scene, -0.6f * z, -0.6f * z, z, green);
  Append(scene, 0.6f * z, -0.6f * z, z, green);
  Append(scene, 0.6f * z, 0.6f * z, z, green);
  Append(scene, -0.6f * z, -0.6f * z, z, green);
  Append(scene, 0.6f * z, 0.6f * z, z, green);
  Append(scene, -0.6f * z, 0.6f * z, z, green);
  AddDraw(scene, Primitive::kTriangles, 0, 0);
  Append(scene, -0.6f * z, 0.75f * z, z, cyan);
  Append(scene, 0.6f * z, 0.75f * z, z, cyan);
  AddDraw(scene, Primitive::kLines, 6, 0);
  if (c == OcclusionCase::kRangeCrossing) {
    // One admitted object extends beyond the radius both radially and along the view axis.
    for (Vertex& v : scene.vertices) {
      const float sx = v.position[0] / v.position[2];
      const float sy = v.position[1] / v.position[2];
      const float distance = 1.6f / (1.0f - sx);
      v.position[0] = sx * distance;
      v.position[1] = sy * distance;
      v.position[2] = distance;
    }
  }
  if (c == OcclusionCase::kDenseObject) {
    // Put the visible part beyond the old renderer cap, not just the CPU collector cap.
    constexpr uint32_t padding = 300000;
    Vertex degenerate = {{0, 0, 2}, {0, 0, 0, 0}};
    scene.vertices.insert(scene.vertices.begin(), padding, degenerate);
    for (OverlayDraw& draw : scene.draws) draw.firstVertex += padding;
    scene.draws.insert(scene.draws.begin(), {Primitive::kTriangles, 0, padding, 0});
  }
  if (jitter) {
    // Oblique plane z = 20 / (1 - 0.8 * screenX), rendered over a jittered depth lattice.
    for (Vertex& v : scene.vertices) {
      const float sx = v.position[0] / v.position[2];
      const float sy = v.position[1] / v.position[2];
      const float slope = c == OcclusionCase::kCoplanarJitterGrazing ? 1.5f : 0.8f;
      const float distance = 20.0f / (1.0f - slope * sx);
      v.position[0] = sx * distance;
      v.position[1] = sy * distance;
      v.position[2] = distance;
    }
  }
}

void TestSource::BuildPattern(const FrameInfo& frame, OverlayScene& scene) const {
  // Вид 0 — без окклюзии; виды 1..3 — с порогами kPatternThresholds.
  OverlayView always;
  always.camera = ScreenCamera(0.0f);
  always.farDistance = 40.0f;
  scene.views.push_back(always);
  for (float threshold : kPatternThresholds) {
    OverlayView v = always;
    v.camera = ScreenCamera(threshold);
    v.occlusion = true;
    v.range = DepthRange::kRaw;
    v.depthMode = 2;
    v.depthBias = 0.0f;
    scene.views.push_back(v);
  }

  const float px = frame.width ? 2.0f / frame.width : 0.01f;   // один пиксель в координатах экрана
  const float py = frame.height ? 2.0f / frame.height : 0.01f;
  const float bx = 12 * px;
  const float by = 12 * py;

  // Рамка по краю экрана: зелёная — буфер глубины найден, красная — нет.
  uint32_t first = static_cast<uint32_t>(scene.vertices.size());
  const Color frameColor = frame.depthFound ? Color{0.1f, 1.0f, 0.2f, 0.9f} : Color{1.0f, 0.1f, 0.1f, 0.9f};
  AppendQuad(scene, -1, 1 - by, 1, 1, frameColor);
  AppendQuad(scene, -1, -1, 1, -1 + by, frameColor);
  AppendQuad(scene, -1, -1 + by, -1 + bx, 1 - by, frameColor);
  AppendQuad(scene, 1 - bx, -1 + by, 1, 1 - by, frameColor);
  // Бегущая полоска: видно, что оверлей обновляется каждый кадр.
  const float t = static_cast<float>(frame.frame % 240) / 240.0f;
  const float barX = -0.9f + 1.7f * t;
  AppendQuad(scene, barX, 0.72f, barX + 0.1f, 0.82f, {0.0f, 1.0f, 1.0f, 0.9f});
  AddDraw(scene, Primitive::kTriangles, first, 0);

  // Шахматка в четырёх полосах по 3 x 8 клеток; полоса b рисуется с видом b.
  const Color magenta = {1.0f, 0.0f, 1.0f, 0.45f};
  const Color yellow = {1.0f, 0.9f, 0.0f, 0.45f};
  const float left = -0.9f;
  const float bottom = -0.6f;
  const float cellW = 0.15f;
  const float cellH = 0.15f;
  for (uint32_t band = 0; band < 4; ++band) {
    first = static_cast<uint32_t>(scene.vertices.size());
    for (int col = 0; col < 3; ++col) {
      for (int row = 0; row < 8; ++row) {
        const float x0 = left + (band * 3 + col) * cellW;
        const float y0 = bottom + row * cellH;
        AppendQuad(scene, x0, y0, x0 + cellW, y0 + cellH, ((band * 3 + col + row) & 1) ? yellow : magenta);
      }
    }
    AddDraw(scene, Primitive::kTriangles, first, band);
  }

  // Линии: диагонали экрана, контур шахматки и границы полос.
  first = static_cast<uint32_t>(scene.vertices.size());
  const Color white = {1, 1, 1, 0.8f};
  AppendLine(scene, -1, -1, 1, 1, white);
  AppendLine(scene, -1, 1, 1, -1, white);
  const float right = left + 12 * cellW;
  const float top = bottom + 8 * cellH;
  for (int band = 0; band <= 4; ++band) {
    const float x = left + band * 3 * cellW;
    AppendLine(scene, x, bottom, x, top, white);
  }
  AppendLine(scene, left, bottom, right, bottom, white);
  AppendLine(scene, left, top, right, top, white);
  AddDraw(scene, Primitive::kLines, first, 0);
}

}  // namespace doomfix::cv
