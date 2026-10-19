#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "common/log.h"

namespace doomfix {

// Настройки читаются один раз из переменных окружения процесса.
//
//   DOOMFIX_ENABLE=1                 включает слой (его же проверяет loader по манифесту)
//   DOOMFIX_LOG_LEVEL=debug|info|warn|error            (по умолчанию info)
//   DOOMFIX_DATA_DIR=<путь>          корень данных      (по умолчанию %LOCALAPPDATA%\doomfix)
//   DOOMFIX_LOG_DIR=<путь>           каталог логов      (по умолчанию <DATA_DIR>\logs)
//   DOOMFIX_HIDE_OVERALLOCATION=1    скрыть VK_AMD_memory_overallocation_behavior (по умолчанию нет;
//                                    нужно только для захватов RenderDoc, см. docs/TECHNICAL.md)
//   DOOMFIX_HIDE_DEVICE_EXTENSIONS=A,B   дополнительно скрыть эти расширения устройства
//   DOOMFIX_INDIRECT_FIX=off|on|strict|aggressive   исправление барьеров перед indirect-командами
//                                                   (по умолчанию on)
//   DOOMFIX_TRACE_EVENTS=1           отладочная фича event-trace: подписывается на все события ядра и
//                                    пишет в лог счётчики вызовов (по умолчанию выключена)
//   DOOMFIX_COLLISION_VIEWER=0       выключить фичу collision-viewer целиком (по умолчанию включена)
//   DOOMFIX_CV_TEST=smoke|pattern|scene   тестовые режимы collision-viewer (см. features/collision_viewer/
//   DOOMFIX_CV_OCCLUSION_TEST=<случай>   test_scene.h); для проверок на GPU и в игре, не для игроков
//   DOOMFIX_CV_TEST_CONSOLE_FRACTION=<0..1>, DOOMFIX_CV_TEST_READBACK=0|1
enum class IndirectFixMode { Off, On, Strict, Aggressive };

struct Config {
  bool enabled = false;
  bool hideOverallocation = false;
  IndirectFixMode indirectFixMode = IndirectFixMode::On;
  std::string indirectFixRaw;  // исходное значение переменной, для лога
  bool traceEvents = false;
  bool collisionViewer = true;
  // Тестовые режимы collision-viewer: исходные значения переменных, разбирает сама фича.
  std::string cvTest;
  std::string cvOcclusionTest;
  std::string cvTestConsoleFraction;
  std::string cvTestReadback;
  log::Level logLevel = log::Level::Info;
  std::filesystem::path dataDir;
  std::filesystem::path logDir;
  std::vector<std::string> extraHiddenDeviceExtensions;
};

const Config& GetConfig();

}  // namespace doomfix
