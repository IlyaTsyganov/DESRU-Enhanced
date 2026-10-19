// Based on doom-eternal-collision-viewer by IlyaTsyganov (commit 43ac57d), used with the author's permission.
//
// Что рисует оверлей collision-viewer за один кадр: вершины, вызовы отрисовки и «виды» (проекция и
// правило окклюзии по глубине игры). Источники (sources.h) заполняют OverlayScene, рендерер
// (overlay_renderer.h) переводит её в команды Vulkan. Без Vulkan: используется и в CPU-тестах.

#pragma once

#include <cstdint>
#include <vector>

namespace doomfix::cv {

// Камера игры: матрица clip = matrix * (x, y, z, 1) по строкам и позиция в мире.
struct Camera {
  float matrix[16] = {};
  float position[3] = {};
};

// Вершина оверлея (раскладка совпадает с шейдером collision.vert).
struct Vertex {
  float position[3];
  float color[4];
};
static_assert(sizeof(Vertex) == 28, "Vertex layout is shared with the shaders");

enum class Primitive : uint32_t { kTriangles, kLines };

// Как читать значение из буфера глубины игры перед сравнением.
enum class DepthRange : uint32_t {
  kViewport,  // нормировать по minDepth/maxDepth viewport'а, с которым игра писала глубину
  kRaw,       // брать значение как есть (0..1)
};

// Проекция и окклюзия для группы вызовов отрисовки.
struct OverlayView {
  Camera camera;               // по матрице строятся позиции на экране и расстояние до точки
  float farDistance = 40.0f;   // радиус отбора целых объектов; их дальние части не обрезаются
  bool occlusion = false;      // прятать то, что за поверхностью из буфера глубины игры
  DepthRange range = DepthRange::kViewport;
  // 0 — глубина 0..1 как есть, 1 — OpenGL (-1..1), 2 — обратная (reverse-Z: 1 у камеры).
  uint32_t depthMode = 2;
  float depthBias = 0.035f;    // допуск сравнения, в единицах расстояния
};

struct OverlayDraw {
  Primitive primitive = Primitive::kLines;
  uint32_t firstVertex = 0;
  uint32_t vertexCount = 0;
  uint32_t view = 0;  // индекс в OverlayScene::views
};

struct OverlayScene {
  std::vector<Vertex> vertices;
  std::vector<OverlayDraw> draws;
  std::vector<OverlayView> views;
  // Доля высоты экрана сверху, где рисовать нельзя (консоль игры): 0 — нет консоли, 1 — весь экран.
  float consoleFraction = 0.0f;

  void Clear() {
    vertices.clear();
    draws.clear();
    views.clear();
    consoleFraction = 0.0f;
  }
};

// Что известно рендереру о кадре, когда он спрашивает источник сцены.
struct FrameInfo {
  uint32_t width = 0;   // размер картинки swapchain
  uint32_t height = 0;
  uint64_t frame = 0;   // номер кадра оверлея (для анимации тестового рисунка)
  bool depthFound = false;  // найден ли свежий буфер глубины игры
};

}  // namespace doomfix::cv
