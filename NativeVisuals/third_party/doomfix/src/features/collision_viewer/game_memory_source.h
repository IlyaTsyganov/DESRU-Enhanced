// Based on doom-eternal-collision-viewer by IlyaTsyganov (commit 43ac57d), used with the author's permission.
//
// Источник «память игры»: камера и сцена DOOM Eternal 1.1 для оверлея (sources.h).
//
// ReadCamera (каждый present): опрос клавиш F5–F10 (display_settings.h); F10 выключен — false; камера
// вида 0 рендерера; меню, пауза, HUD-меню, диалоги, загрузка — false (оверлей не рисуется и ресурсов
// GPU не тратит).
// BuildScene: нужен найденный буфер глубины игры; уровень (имя карты + мир Havok) — смена уровня
// отдаёт старую сцену фоновому потоку и просит новую (runtime_scene.h); консоль -> consoleFraction
// (на весь экран — ничего); вершины — collect_vertices.h.
//
// Методы зовутся под мьютексом рендерера устройства; свой мьютекс источника — на случай двух устройств.
// Stop() останавливает фоновый поток (OnDestroyInstance/OnDestroyDevice); следующий кадр в уровне
// запустит его снова.

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "features/collision_viewer/game_memory/game_state.h"
#include "features/collision_viewer/game_memory/memory_reader.h"
#include "features/collision_viewer/scene/collect_vertices.h"
#include "features/collision_viewer/scene/display_settings.h"
#include "features/collision_viewer/scene/runtime_scene.h"
#include "features/collision_viewer/sources.h"

namespace doomfix::cv {

class GameMemorySource final : public ICameraSource, public ISceneSource {
 public:
  // base — начало образа exe 1.1 (версия уже проверена) или поддельной памяти (FakeGame).
  GameMemorySource(uintptr_t base, MemoryReader memory = {}, KeyPoller keys = {});

  bool ReadCamera(Camera& camera) override;
  bool BuildScene(const FrameInfo& frame, const Camera& camera, OverlayScene& scene) override;

  void Stop();
  // Фоновый поток не работает: источник можно уничтожать.
  bool Stopped() const { return !manager_.Running(); }

  DisplaySettings Settings() {
    std::lock_guard lock(mutex_);
    return settings_;
  }

 private:
  using Clock = std::chrono::steady_clock;
  // Уровень, запрос сцены, снимки. false — уровня нет или сцены ещё нет.
  bool UpdateLevel();
  void LogStatistics(const CollectStats& stats, const Camera& camera);

  const uintptr_t base_;
  const MemoryReader memory_;
  KeyPoller keys_;
  std::mutex mutex_;
  DisplaySettings settings_;
  RuntimeSceneManager manager_;
  LevelIdentity level_;
  std::shared_ptr<const RuntimeScene> world_;
  std::shared_ptr<const RuntimeEntities> entities_;
  std::vector<Candidate> visible_;

  // Логи: только смены состояния и сводка раз в 10 с.
  int cameraState_ = -1;    // 0 — нет камеры, 1 — есть
  int gameplayState_ = -1;  // 0 — меню/пауза/загрузка, 1 — игра
  bool loggedWorld_ = false;
  Clock::time_point lastStatistics_ = {};
  Clock::time_point lastEntities_ = {};
  uint64_t frames_ = 0;
  double prepareTotal_ = 0;
  double prepareMax_ = 0;
};

}  // namespace doomfix::cv
