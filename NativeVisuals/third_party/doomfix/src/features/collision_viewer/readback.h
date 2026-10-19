// Based on doom-eternal-collision-viewer by IlyaTsyganov (commit 43ac57d), used with the author's permission.
//
// Разбор кадра, прочитанного с GPU в тестовом режиме (DOOMFIX_CV_TEST_READBACK): сколько пикселей
// каких цветов и где. Итог пишется в лог одной строкой; её проверяет tests/cv_smoke_test.cpp.

#pragma once

#include <cstdint>
#include <string>

namespace doomfix::cv {

struct ReadbackStats {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t green = 0;          // зелёные пиксели (квадрат теста окклюзии, рамка тестового рисунка)
  uint32_t cyan = 0;           // голубые (линия теста окклюзии, триггеры)
  uint32_t orange = 0;         // оранжевые (невидимые стены)
  uint32_t uniqueGreens = 0;   // сколько разных оттенков зелёного (>1 — смешение или полосы)
  uint32_t leftColored = 0;    // зелёные, голубые и оранжевые в левой половине экрана
  uint32_t rightColored = 0;   // ... в правой
  uint32_t lit = 0;            // все не чёрные пиксели
  uint32_t consoleTop = 0;     // первая строка под «консолью»
  uint32_t consoleLeak = 0;    // не чёрные пиксели выше consoleTop
  uint32_t belowConsole = 0;   // не чёрные пиксели ниже
  uint8_t center[4] = {};      // RGBA пикселя в центре
  uint8_t edge[4] = {};        // RGBA пикселя (2, height/2) — левая рамка тестового рисунка

  // "width=.. height=.. green=.. ..." — для лога.
  std::string Format() const;
};

// pixels — width * height пикселей по 4 байта; bgra — порядок каналов B, G, R, A (иначе R, G, B, A).
ReadbackStats AnalyzeReadback(const uint8_t* pixels, uint32_t width, uint32_t height, bool bgra,
                              uint32_t consoleTop);

}  // namespace doomfix::cv
