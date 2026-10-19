// Based on doom-eternal-collision-viewer by IlyaTsyganov (commit 43ac57d), used with the author's permission.
//
// Расчёты рендерера без вызовов Vulkan: push-константы шейдеров и отсечение консоли.

#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

#include "features/collision_viewer/overlay_types.h"

namespace doomfix::cv {

// Push-константы шейдеров collision.vert/.frag (блок Camera, 128 байт).
struct PushConstants {
  float matrix[16];  // row0..row3
  float options[4];  // x — дальность, y/z — коэффициенты глубины по матрице, w — окклюзия вкл.
  float depth[4];    // minDepth, maxDepth, режим глубины, допуск
  float uv[4];       // пиксель экрана -> координаты в буфере глубины: масштаб xy, сдвиг zw
  float debug[4];    // x > 0.5 — отладочный вывод глубины
};
static_assert(sizeof(PushConstants) == 128, "Must match the push constant block of the shaders");

// Буфер глубины, из которого шейдер читает окклюзию.
struct DepthSampleInfo {
  uint32_t width = 0;   // размер картинки глубины
  uint32_t height = 0;
  VkViewport viewport = {};  // viewport, с которым игра писала глубину
};

// Константы для вида view при экране screenWidth x screenHeight. depth == nullptr — буфера глубины
// нет (окклюзия тогда выключается).
PushConstants BuildPushConstants(const OverlayView& view, const DepthSampleInfo* depth,
                                 uint32_t screenWidth, uint32_t screenHeight);

// Первая строка экрана под консолью: выше неё оверлей не рисуется. fraction — доля высоты экрана,
// которую занимает консоль; height — если консоль закрывает всё (или значение неверное).
uint32_t ConsoleClipTop(float fraction, uint32_t height);

}  // namespace doomfix::cv
