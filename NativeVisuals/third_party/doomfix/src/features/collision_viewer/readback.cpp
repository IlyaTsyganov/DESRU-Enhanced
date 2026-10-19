// Based on doom-eternal-collision-viewer by IlyaTsyganov (commit 43ac57d), used with the author's permission.

#include "features/collision_viewer/readback.h"

#include <cstdio>
#include <unordered_set>

namespace doomfix::cv {

std::string ReadbackStats::Format() const {
  char text[512];
  std::snprintf(text, sizeof(text),
                "width=%u height=%u green=%u cyan=%u orange=%u uniqueGreens=%u leftColored=%u rightColored=%u "
                "lit=%u consoleTop=%u consoleLeak=%u belowConsole=%u center=%u,%u,%u,%u "
                "edge=%u,%u,%u,%u",
                width, height, green, cyan, orange, uniqueGreens, leftColored, rightColored, lit, consoleTop,
                consoleLeak, belowConsole, center[0], center[1], center[2], center[3], edge[0], edge[1],
                edge[2], edge[3]);
  return text;
}

ReadbackStats AnalyzeReadback(const uint8_t* pixels, uint32_t width, uint32_t height, bool bgra,
                              uint32_t consoleTop) {
  ReadbackStats s;
  s.width = width;
  s.height = height;
  s.consoleTop = consoleTop;
  if (!pixels || !width || !height) return s;
  auto rgba = [&](size_t i, uint8_t out[4]) {
    const uint8_t* p = pixels + i * 4;
    out[0] = bgra ? p[2] : p[0];
    out[1] = p[1];
    out[2] = bgra ? p[0] : p[2];
    out[3] = p[3];
  };
  std::unordered_set<uint32_t> greens;  // разные оттенки зелёного (RGB)
  const size_t count = size_t(width) * height;
  for (size_t i = 0; i < count; ++i) {
    uint8_t c[4];
    rgba(i, c);
    const float r = c[0];
    const float g = c[1];
    const float b = c[2];
    const bool isGreen = g > 30 && g > r * 1.3f && g > b * 1.3f;
    const bool isCyan = g > 100 && g > r * 1.8f && b > r * 1.8f;
    const bool isOrange = r > 100 && r > g * 1.5f && g > b * 2.0f;
    if (isGreen) {
      ++s.green;
      greens.insert(uint32_t(c[0]) | (uint32_t(c[1]) << 8) | (uint32_t(c[2]) << 16));
    }
    if (isCyan) ++s.cyan;
    if (isOrange) ++s.orange;
    const size_t row = i / width;
    const size_t col = i % width;
    if (c[0] > 10 || c[1] > 10 || c[2] > 10) {
      ++s.lit;
      if (row < consoleTop) {
        ++s.consoleLeak;
      } else {
        ++s.belowConsole;
      }
    }
    if (isGreen || isCyan || isOrange) {
      if (col < width / 2) {
        ++s.leftColored;
      } else {
        ++s.rightColored;
      }
    }
  }
  s.uniqueGreens = static_cast<uint32_t>(greens.size());
  rgba(size_t(height / 2) * width + width / 2, s.center);
  if (width > 2) rgba(size_t(height / 2) * width + 2, s.edge);
  return s;
}

}  // namespace doomfix::cv
