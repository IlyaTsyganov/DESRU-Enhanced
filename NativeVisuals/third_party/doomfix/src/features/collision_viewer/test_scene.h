// Based on doom-eternal-collision-viewer by IlyaTsyganov (commit 43ac57d), used with the author's permission.
//
// Тестовые режимы collision-viewer (не для игроков):
//
//   DOOMFIX_CV_TEST=smoke      GPU-проверка: треугольник и линия поверх кадра (без окклюзии). Рисуется,
//                              только если найден буфер глубины.
//   DOOMFIX_CV_OCCLUSION_TEST=hidden|partial|front|reverse|inverted|inverted-partial|inverted-front
//                              GPU-проверка окклюзии: зелёный квадрат и голубая линия за/перед стеной из
//                              буфера глубины тестовой программы (включает режим smoke).
//   DOOMFIX_CV_TEST_CONSOLE_FRACTION=<0..1>
//                              в режиме smoke: доля экрана сверху, занятая «консолью» (там не рисовать).
//   DOOMFIX_CV_TEST=pattern    тестовый рисунок поверх кадра игры: рамка (зелёная — буфер глубины
//                              найден, красная — нет), диагонали, бегущая полоска и шахматка в четырёх
//                              полосах. Левая полоса видна всегда, в остальных клетки прячутся за
//                              поверхностями ближе порога (сырое значение глубины reverse-Z > 0.5,
//                              0.05, 0.005): оружие и близкие стены «вырезают» рисунок. Без камеры игры.
//   DOOMFIX_CV_TEST=scene      настоящий источник «память игры» (game_memory_source.h) над поддельной
//                              памятью игры с маленьким уровнем (game_memory/fake_game.h): чтение
//                              памяти, фоновый поток, разбор Havok, BVH, отбор и цвета групп — как в
//                              игре. DOOMFIX_CV_TEST_CONSOLE_FRACTION задаёт консоль этой «игры».
//   DOOMFIX_CV_TEST_READBACK=1 прочитать кадр после отрисовки и записать в лог итог проверки пикселей
//                              (по умолчанию — в режимах smoke и scene).
//
// Имена соответствуют DCV_SMOKE_TEST, DCV_OCCLUSION_TEST и DCV_TEST_CONSOLE_FRACTION исходного viewer.

#pragma once

#include <string>

#include "features/collision_viewer/sources.h"

namespace doomfix::cv {

enum class TestMode { kNone, kSmoke, kPattern, kScene };

enum class OcclusionCase {
  kNone,
  kHidden,           // стена закрывает всё, квадрат за ней
  kPartial,          // стена закрывает левую половину
  kFront,            // квадрат перед стеной
  kReverse,          // как hidden, но глубина записана с перевёрнутым viewport (1..0)
  kInverted,         // обратная глубина (reverse-Z, очистка в 0), стена закрывает всё
  kInvertedPartial,  // reverse-Z, стена закрывает левую половину
  kInvertedFront,    // reverse-Z, квадрат перед стеной
  kRangeCrossing,
  kDenseObject,
  kCoplanarJitter,
  kCoplanarJitterReverse,
  kCoplanarJitterGrazing,
};

struct TestOptions {
  TestMode mode = TestMode::kNone;
  OcclusionCase occlusion = OcclusionCase::kNone;
  float consoleFraction = 0.0f;
  bool readback = false;
  std::string problems;  // нераспознанные значения переменных (для лога)

  bool Active() const { return mode != TestMode::kNone; }
};

// Разбор значений переменных DOOMFIX_CV_TEST, DOOMFIX_CV_OCCLUSION_TEST,
// DOOMFIX_CV_TEST_CONSOLE_FRACTION и DOOMFIX_CV_TEST_READBACK.
TestOptions ParseTestOptions(const std::string& test, const std::string& occlusion,
                             const std::string& consoleFraction, const std::string& readback);

const char* TestModeName(TestMode mode);
const char* OcclusionCaseName(OcclusionCase occlusion);

// Камера и сцена для тестовых режимов.
class TestSource final : public ICameraSource, public ISceneSource {
 public:
  explicit TestSource(const TestOptions& options) : options_(options) {}

  bool ReadCamera(Camera& camera) override;
  bool BuildScene(const FrameInfo& frame, const Camera& camera, OverlayScene& scene) override;

  // Пороги сырой глубины полос тестового рисунка (полоса 0 — без окклюзии).
  static constexpr float kPatternThresholds[3] = {0.5f, 0.05f, 0.005f};

 private:
  void BuildSmoke(OverlayScene& scene) const;
  void BuildOcclusion(OverlayScene& scene) const;
  void BuildPattern(const FrameInfo& frame, OverlayScene& scene) const;

  TestOptions options_;
};

}  // namespace doomfix::cv
