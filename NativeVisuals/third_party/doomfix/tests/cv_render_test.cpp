// Based on doom-eternal-collision-viewer by IlyaTsyganov (commit 43ac57d), used with the author's permission.
//
// Модульный тест расчётов рендерера collision-viewer без GPU: push-константы и формулы шейдеров
// (повторены здесь на C++) для всех тестовых сцен, отсечение консоли (из viewer_behavior_tests.cpp
// исходного viewer), разбор переменных DOOMFIX_CV_*, тестовый рисунок, разбор прочитанного кадра.
//
//   ctest --test-dir build-release            или   build-release\bin\doomfix_cv_render_test.exe

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>
#include "runtime.h"

#include "features/collision_viewer/overlay_math.h"
#include "features/collision_viewer/readback.h"
#include "features/collision_viewer/test_scene.h"

namespace {

using namespace doomfix::cv;

int g_failures = 0;

void Check(bool ok, const char* what) {
  std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_failures;
}

// --- Шейдеры на C++ (collision.vert / collision.frag) ------------------------------------------------

struct VertexOut {
  float clip[4];
  float viewDistance;
};

float Dot4(const float* row, const float p[4]) {
  return row[0] * p[0] + row[1] * p[1] + row[2] * p[2] + row[3] * p[3];
}

VertexOut RunVertexShader(const PushConstants& pc, const float position[3]) {
  const float p[4] = {position[0], position[1], position[2], 1.0f};
  const float x = Dot4(pc.matrix, p);
  const float y = Dot4(pc.matrix + 4, p);
  const float w = Dot4(pc.matrix + 12, p);
  const float z = w - 0.05f;
  return {{x, -y, z, w}, w};
}

// Uniform-depth scalar cases; the 2x2 footprint and whole objects are tested by GPU readbacks.
// true — фрагмент рисуется, false — discard. sampled — значение из буфера глубины игры.
bool RunFragmentShader(const PushConstants& pc, float sampled, float viewDistance) {
  if (pc.options[3] > 0.5f) {
    float ndc = (sampled - pc.depth[0]) / (pc.depth[1] - pc.depth[0]);
    if (pc.depth[2] == 1.0f) ndc = ndc * 2.0f - 1.0f;
    if (pc.depth[2] == 2.0f) ndc = 1.0f - ndc;
    const float distanceToSurface = pc.options[2] / (ndc - pc.options[1]);
    const float tolerance = pc.depth[3] + std::max(distanceToSurface, 0.0f) * 0.001f;
    if (distanceToSurface > 0.0f && viewDistance > distanceToSurface + tolerance) return false;
  }
  return true;
}

// Виден ли первый треугольник сцены (квадрат теста окклюзии) при значении глубины sampled.
bool QuadVisible(const OverlayScene& scene, const DepthSampleInfo& depth, float sampled) {
  const PushConstants pc = BuildPushConstants(scene.views[0], &depth, 128, 128);
  const VertexOut v = RunVertexShader(pc, scene.vertices[0].position);
  return RunFragmentShader(pc, sampled, v.viewDistance);
}

OverlayScene BuildTestScene(TestOptions options, bool depthFound = true, uint64_t frame = 0) {
  TestSource source(options);
  Camera camera;
  OverlayScene scene;
  FrameInfo info;
  info.width = info.height = 128;
  info.frame = frame;
  info.depthFound = depthFound;
  if (!source.ReadCamera(camera) || !source.BuildScene(info, camera, scene)) scene.Clear();
  return scene;
}

TestOptions Occlusion(OcclusionCase c) {
  TestOptions o;
  o.mode = TestMode::kSmoke;
  o.occlusion = c;
  return o;
}

void OcclusionMath() {
  std::printf("occlusion test scenes through the shader math\n");
  DepthSampleInfo normal;
  normal.width = normal.height = 128;
  normal.viewport = {0, 0, 128, 128, 0, 1};
  DepthSampleInfo reversed = normal;
  reversed.viewport.minDepth = 1;
  reversed.viewport.maxDepth = 0;
  // Значения глубины, которые пишет tests/cv_smoke_test.cpp: очистка 1 (reverse-Z — 0), стена 0.95
  // (reverse-Z и перевёрнутый viewport — 0.05). Квадрат за стеной (2) или перед ней (0.5).
  const OverlayScene hidden = BuildTestScene(Occlusion(OcclusionCase::kHidden));
  const OverlayScene front = BuildTestScene(Occlusion(OcclusionCase::kFront));
  const OverlayScene reverse = BuildTestScene(Occlusion(OcclusionCase::kReverse));
  const OverlayScene inverted = BuildTestScene(Occlusion(OcclusionCase::kInverted));
  const OverlayScene invertedFront = BuildTestScene(Occlusion(OcclusionCase::kInvertedFront));
  Check(!hidden.draws.empty() && hidden.views[0].occlusion, "occlusion scene has an occluding view");
  Check(!QuadVisible(hidden, normal, 0.95f), "hidden: the quad behind the wall is discarded");
  Check(QuadVisible(hidden, normal, 1.0f), "partial: the quad is visible where there is no wall");
  Check(QuadVisible(front, normal, 0.95f), "front: the quad in front of the wall is drawn");
  Check(!QuadVisible(reverse, reversed, 0.05f), "reverse viewport: the wall still hides the quad");
  Check(!QuadVisible(inverted, normal, 0.05f), "reverse-Z: the wall hides the quad");
  Check(QuadVisible(inverted, normal, 0.0f), "reverse-Z: cleared depth (0) hides nothing");
  Check(QuadVisible(invertedFront, normal, 0.05f), "reverse-Z: the quad in front of the wall is drawn");
  const OverlayScene smoke = BuildTestScene([] {
    TestOptions o;
    o.mode = TestMode::kSmoke;
    return o;
  }());
  const PushConstants pc = BuildPushConstants(smoke.views[0], &normal, 128, 128);
  Check(pc.options[3] == 0.0f && RunFragmentShader(pc, 0.0f, 1.0f), "smoke scene draws without occlusion");
  Check(BuildTestScene(Occlusion(OcclusionCase::kHidden), false).draws.empty(),
        "smoke scenes wait for a depth buffer");

  // Координаты экрана: квадрат 0.6 от центра при любом z.
  const VertexOut corner = RunVertexShader(BuildPushConstants(hidden.views[0], &normal, 128, 128),
                                           hidden.vertices[2].position);
  Check(std::fabs(corner.clip[0] / corner.clip[3] - 0.6f) < 1e-5f &&
            std::fabs(corner.clip[1] / corner.clip[3] + 0.6f) < 1e-5f && corner.clip[2] >= 0 &&
            corner.clip[2] <= corner.clip[3],
        "occlusion quad lands at +-0.6 inside the depth range");
}

void PatternMath() {
  std::printf("in-game test pattern\n");
  TestOptions options;
  options.mode = TestMode::kPattern;
  const OverlayScene scene = BuildTestScene(options, true, 60);
  Check(scene.views.size() == 4 && !scene.views[0].occlusion, "four views, the first without occlusion");
  bool inside = true;
  for (const Vertex& v : scene.vertices) {
    inside = inside && std::fabs(v.position[0]) <= 1.0f && std::fabs(v.position[1]) <= 1.0f;
  }
  Check(inside, "pattern stays on the screen");
  bool rangesOk = !scene.draws.empty();
  bool bandsSeen[4] = {};
  for (const OverlayDraw& d : scene.draws) {
    rangesOk = rangesOk && d.vertexCount && d.firstVertex + d.vertexCount <= scene.vertices.size() &&
               d.view < scene.views.size();
    if (d.view < 4) bandsSeen[d.view] = true;
  }
  Check(rangesOk && bandsSeen[0] && bandsSeen[1] && bandsSeen[2] && bandsSeen[3],
        "draws reference valid vertices and all four bands");
  Check(scene.vertices[0].color[1] > 0.9f && scene.vertices[0].color[0] < 0.2f,
        "frame is green when the depth buffer is found");
  const OverlayScene noDepth = BuildTestScene(options, false);
  Check(noDepth.vertices[0].color[0] > 0.9f && noDepth.vertices[0].color[1] < 0.2f,
        "frame is red without a depth buffer");

  DepthSampleInfo depth;
  depth.width = depth.height = 128;
  depth.viewport = {0, 0, 128, 128, 0, 1};
  bool thresholdsOk = true;
  for (uint32_t band = 1; band < 4; ++band) {
    const float threshold = TestSource::kPatternThresholds[band - 1];
    const PushConstants pc = BuildPushConstants(scene.views[band], &depth, 128, 128);
    const float position[3] = {0.0f, 0.0f, 0.0f};
    const VertexOut v = RunVertexShader(pc, position);
    thresholdsOk = thresholdsOk && std::fabs(v.viewDistance - 1.0f) < 1e-6f && v.clip[2] >= 0 &&
                   v.clip[2] <= v.clip[3] && RunFragmentShader(pc, 0.0f, v.viewDistance) &&
                   RunFragmentShader(pc, threshold * 0.9f, v.viewDistance) &&
                   !RunFragmentShader(pc, threshold * 1.1f, v.viewDistance) &&
                   !RunFragmentShader(pc, 1.0f, v.viewDistance);
  }
  Check(thresholdsOk, "band b hides cells where the raw reverse-Z depth exceeds its threshold");
  const PushConstants always = BuildPushConstants(scene.views[0], &depth, 128, 128);
  Check(RunFragmentShader(always, 1.0f, 1.0f), "band 0 is never hidden");
  const OverlayScene later = BuildTestScene(options, true, 120);
  Check(later.vertices.size() == scene.vertices.size() &&
            later.vertices[24].position[0] != scene.vertices[24].position[0],
        "the moving bar moves between frames");
}

void PushConstantMath() {
  std::printf("push constants\n");
  OverlayView view;
  view.camera.matrix[0] = view.camera.matrix[5] = view.camera.matrix[10] = view.camera.matrix[14] = 1;
  view.camera.matrix[11] = -0.05f;
  view.occlusion = true;
  DepthSampleInfo depth;
  depth.width = 256;
  depth.height = 128;
  depth.viewport = {16, 128, 128, -64, 1, 0};
  const PushConstants pc = BuildPushConstants(view, &depth, 512, 256);
  Check(pc.options[1] == 1.0f && std::fabs(pc.options[2] + 0.05f) < 1e-7f && pc.options[3] == 1.0f,
        "depth coefficients from the camera rows");
  Check(pc.depth[0] == 1.0f && pc.depth[1] == 0.0f, "depth range from the game's viewport");
  Check(std::fabs(pc.uv[0] - 128.0f / 256 / 512) < 1e-9f && std::fabs(pc.uv[1] - 64.0f / 128 / 256) < 1e-9f &&
            std::fabs(pc.uv[2] - 16.0f / 256) < 1e-9f && std::fabs(pc.uv[3] - 64.0f / 128) < 1e-9f,
        "screen pixel to depth texture coordinates");
  const PushConstants none = BuildPushConstants(view, nullptr, 512, 256);
  Check(none.options[3] == 0.0f && none.uv[0] == 0.0f, "no depth buffer: occlusion off");
}

void ConsoleClip() {
  std::printf("console clipping (from viewer_behavior_tests)\n");
  Check(ConsoleClipTop(0.5f, 100) == 52, "half console plus its border");
  Check(ConsoleClipTop(0.0f, 100) == 0 && ConsoleClipTop(1.0f, 100) == 100 && ConsoleClipTop(-1.0f, 100) == 0 &&
            ConsoleClipTop(0.1f, 100) == 12,
        "console clipping bounds");
  Check(ConsoleClipTop(std::numeric_limits<float>::quiet_NaN(), 100) == 100, "invalid fraction hides all");
  Check(ConsoleClipTop(0.99f, 100) == 100, "never below the screen");
}

void Options() {
  std::printf("DOOMFIX_CV_* variables\n");
  TestOptions o = ParseTestOptions("", "", "", "");
  Check(!o.Active() && !o.readback && o.problems.empty(), "no variables: no test mode");
  o = ParseTestOptions("smoke", "", "0.5", "");
  Check(o.mode == TestMode::kSmoke && o.readback && o.consoleFraction == 0.5f, "smoke reads frames back");
  o = ParseTestOptions("", "inverted-partial", "", "");
  Check(o.mode == TestMode::kSmoke && o.occlusion == OcclusionCase::kInvertedPartial,
        "an occlusion case implies smoke");
  o = ParseTestOptions("pattern", "", "", "");
  Check(o.mode == TestMode::kPattern && !o.readback, "pattern draws without readback");
  o = ParseTestOptions("pattern", "", "", "1");
  Check(o.readback, "DOOMFIX_CV_TEST_READBACK=1 for the pattern");
  o = ParseTestOptions("smok", "sideways", "2", "");
  Check(!o.Active() && o.occlusion == OcclusionCase::kNone && o.consoleFraction == 0.0f &&
            !o.problems.empty(),
        "unknown values are reported and ignored");
  o = ParseTestOptions("pattern", "hidden", "", "");
  Check(o.mode == TestMode::kPattern && o.occlusion == OcclusionCase::kNone && !o.problems.empty(),
        "occlusion cases only with smoke");
}

void Readback() {
  std::printf("frame readback analysis\n");
  constexpr uint32_t w = 8;
  constexpr uint32_t h = 4;
  std::vector<uint8_t> rgba(w * h * 4, 0);
  auto put = [&](uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b) {
    uint8_t* p = &rgba[(y * w + x) * 4];
    p[0] = r;
    p[1] = g;
    p[2] = b;
    p[3] = 255;
  };
  put(6, 2, 64, 242, 31);   // зелёный справа
  put(7, 3, 25, 217, 255);  // голубой справа
  put(1, 0, 200, 200, 200); // белый вверху
  ReadbackStats s = AnalyzeReadback(rgba.data(), w, h, false, 1);
  Check(s.green == 1 && s.cyan == 1 && s.uniqueGreens == 1 && s.leftColored == 0 && s.rightColored == 2,
        "green and cyan pixels, by half of the screen");
  Check(s.lit == 3 && s.consoleLeak == 1 && s.belowConsole == 2, "console leak and pixels below it");
  std::vector<uint8_t> bgra = rgba;
  for (size_t i = 0; i < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
  const ReadbackStats b = AnalyzeReadback(bgra.data(), w, h, true, 1);
  Check(b.green == s.green && b.cyan == s.cyan && b.center[0] == s.center[0],
        "BGRA swapchains give the same result");
}

}  // namespace

int main() {
  const float actual[16]={0.577345f,0.00249116f,0,-16.4831f,-0.0000367651f,0.00852060f,1.026365f,-5.52663f,-0.00431469f,0.99996376f,-0.00830157f,1600.9608f,-0.00431466f,0.99995625f,-0.00830151f,1601.0089f};
  float position[3];
  Check(desru::DecodeCamera(actual,position)&&std::fabs(position[0]-35.4569f)<0.01f&&std::fabs(position[1]+1600.771f)<0.01f&&std::fabs(position[2]-18.6751f)<0.01f,"camera position is recovered from an actual renderer projection snapshot");
  float bad[16]={};bad[12]=1;
  Check(!desru::DecodeCamera(bad,position),"a unit forward vector without a valid camera transform is rejected");
  OcclusionMath();
  PatternMath();
  PushConstantMath();
  ConsoleClip();
  Options();
  Readback();
  if (g_failures) {
    std::printf("\n%d check(s) FAILED\n", g_failures);
    return 1;
  }
  std::printf("\nPASSED\n");
  return 0;
}
