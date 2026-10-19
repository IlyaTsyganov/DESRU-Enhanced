// Based on doom-eternal-collision-viewer by IlyaTsyganov (commit 43ac57d), used with the author's permission.

#include "features/collision_viewer/overlay_math.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace doomfix::cv {

PushConstants BuildPushConstants(const OverlayView& view, const DepthSampleInfo* depth,
                                 uint32_t screenWidth, uint32_t screenHeight) {
  PushConstants push = {};
  const float* m = view.camera.matrix;
  std::memcpy(push.matrix, m, sizeof(push.matrix));
  push.options[0] = view.farDistance;
  // Глубина точки в clip-пространстве линейна по w: z = a*w + b. Шейдер восстанавливает по значению из
  // буфера глубины расстояние до поверхности w = b / (ndc - a); a и b берутся из 3-й и 4-й строк матрицы.
  float forwardSquared = 0.0f;
  float dot = 0.0f;
  for (int k = 0; k < 3; ++k) {
    forwardSquared += m[12 + k] * m[12 + k];
    dot += m[8 + k] * m[12 + k];
  }
  push.options[1] = forwardSquared > 0.0f ? dot / forwardSquared : 1.0f;
  push.options[2] = m[11] - push.options[1] * m[15];
  push.options[3] = view.occlusion && depth ? 1.0f : 0.0f;

  if (view.range == DepthRange::kRaw || !depth) {
    push.depth[0] = 0.0f;
    push.depth[1] = 1.0f;
  } else {
    push.depth[0] = depth->viewport.minDepth;
    push.depth[1] = depth->viewport.maxDepth;
  }
  push.depth[2] = static_cast<float>(view.depthMode);
  push.depth[3] = view.depthBias;

  if (depth && depth->width && depth->height && screenWidth && screenHeight) {
    const VkViewport& vp = depth->viewport;
    push.uv[0] = std::fabs(vp.width) / depth->width / screenWidth;
    push.uv[1] = std::fabs(vp.height) / depth->height / screenHeight;
    push.uv[2] = std::min(vp.x, vp.x + vp.width) / depth->width;
    push.uv[3] = std::min(vp.y, vp.y + vp.height) / depth->height;
  }
  return push;
}

uint32_t ConsoleClipTop(float fraction, uint32_t height) {
  if (!std::isfinite(fraction) || fraction >= 1.0f) return height;
  if (fraction <= 0.0f) return 0;
  // Вместе с нижней рамкой консоли; мир под ней остаётся видимым.
  return std::min(height, static_cast<uint32_t>(std::ceil(fraction * height)) + 2);
}

}  // namespace doomfix::cv
