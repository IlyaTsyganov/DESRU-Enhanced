// Based on doom-eternal-collision-viewer by IlyaTsyganov (commit 43ac57d), used with the author's permission.

#include "features/collision_viewer/game_memory_source.h"

#include <algorithm>
#include <utility>
#include <cstring>

#include "common/log.h"
#include "runtime.h"

namespace doomfix::cv {
namespace {

constexpr auto kReportInterval = std::chrono::seconds(10);

}  // namespace

GameMemorySource::GameMemorySource(uintptr_t base, MemoryReader memory, KeyPoller keys)
    : base_(base), memory_(memory), keys_(keys), manager_(memory) {
  if(desru::Controlled()) DF_LOG_INFO("collision-viewer: DESRU console controls; start: %s",settings_.Describe().c_str());
  else DF_LOG_INFO("collision-viewer: keys F5 style, F6 triggers, F7 collision, F8 invisible walls, F9 range, F10 "
              "overlay; start: %s",
              settings_.Describe().c_str());
}

bool GameMemorySource::ReadCamera(Camera& camera) {
  std::lock_guard lock(mutex_);
  if(desru::Controlled()) {
    settings_.overlay=desru::Viewer();
    settings_.triggers=(DESRUVisualControl.requested&4)!=0;
    settings_.collision=(DESRUVisualControl.requested&8)!=0;
    settings_.walls=(DESRUVisualControl.requested&16)!=0;
    settings_.fill=(DESRUVisualControl.requested&desru::kFill)!=0;
    LONG bits=DESRUVisualControl.distanceBits;std::memcpy(&settings_.radius,&bits,4);
  } else keys_.Poll([&](DisplayKey key) { ApplyKey(settings_,key); });
  if (!settings_.overlay) return false;

  const bool hasCamera = ReadGameCamera(memory_, base_, camera);
  if (int(hasCamera) != cameraState_) {
    cameraState_ = int(hasCamera);
    DF_LOG_INFO("collision-viewer: %s", hasCamera ? "the game camera is readable"
                                                  : "waiting for a level camera (menu or loading)");
  }
  if (!hasCamera) return false;
  const bool playing = GameplayVisible(memory_, base_);
  if (int(playing) != gameplayState_) {
    gameplayState_ = int(playing);
    DF_LOG_INFO("collision-viewer: %s", playing ? "gameplay: overlay shown"
                                                : "menu, pause, dialog or loading: overlay hidden");
  }
  return playing;
}

bool GameMemorySource::UpdateLevel() {
  const LevelIdentity level = ReadLevelIdentity(memory_, base_);
  if (level != level_) {
    // Старая геометрия больше не рисуется; освобождает её фоновый поток.
    manager_.Retire(std::move(world_));
    world_.reset();
    entities_.reset();
    level_ = level;
    loggedWorld_ = false;
    if (level || manager_.Running()) manager_.Request(level, base_);  // пустой уровень прерывает работу
    DF_LOG_INFO("collision-viewer: level changed: %s%s", level ? level.name.c_str() : "none (menu or loading)",
                level ? "; old geometry removed, preparing the scene in the background" : "");
  } else if (level && !world_) {
    manager_.Request(level, base_);  // поток мог быть остановлен (Stop); иначе ничего не делает
  }
  if (!level) return false;
  manager_.Snapshot(level, world_, entities_);
  const auto now = Clock::now();
  if (world_ && !loggedWorld_) {
    loggedWorld_ = true;
    DF_LOG_INFO("collision-viewer: scene ready: %s, %zu collision instances, %u unique shapes, %llu triangles, "
                "unsupported %u%s%s%s, background build %.1f ms",
                level.name.c_str(), world_->meshes.size(), world_->unique,
                static_cast<unsigned long long>(world_->triangles), world_->unsupported,
                world_->problems.empty() ? "" : " (", world_->problems.c_str(), world_->problems.empty() ? "" : ")",
                world_->milliseconds);
    DF_LOG_INFO("collision-viewer: excluded %u world instances whose contents do not block the player",
                world_->nonBlocking);
  }
  if (entities_ && now - lastEntities_ >= kReportInterval) {
    lastEntities_ = now;
    DF_LOG_INFO("collision-viewer: entities: %u triggers, %u invisible walls, unsupported %u, background scan "
                "%.2f ms",
                entities_->triggers, entities_->walls, entities_->unsupported, entities_->milliseconds);
  }
  return world_ || entities_;
}

bool GameMemorySource::BuildScene(const FrameInfo& frame, const Camera& camera, OverlayScene& scene) {
  std::lock_guard lock(mutex_);
  // Без буфера глубины игры окклюзии нет — не рисовать (как в исходном viewer).
  if (!frame.depthFound) return false;
  if (!UpdateLevel()) return false;
  const float console = ReadConsoleFraction(memory_, base_);
  if (console >= 1.0f) return false;
  scene.consoleFraction = console;
  SceneInput input;
  if (world_) {
    input.world = &world_->meshes;
    input.nodes = &world_->nodes;
    input.order = &world_->order;
  }
  if (entities_) input.entities = &entities_->meshes;
  CollectStats stats;
  CollectVertices(camera, settings_, input, visible_, scene, &stats);
  ++frames_;
  prepareTotal_ += stats.milliseconds;
  prepareMax_ = std::max(prepareMax_, stats.milliseconds);
  LogStatistics(stats, camera);
  return !scene.draws.empty();
}

void GameMemorySource::LogStatistics(const CollectStats& s, const Camera& camera) {
  const auto now = Clock::now();
  if (now - lastStatistics_ < kReportInterval) return;
  lastStatistics_ = now;
  const size_t total = world_ ? world_->meshes.size() : 0;
  DF_LOG_INFO("collision-viewer: frame geometry: triggers %u (%u edges), walls %u (%u edges), collision %u (%u "
              "edges), filled %u (%u triangles), examined %u/%zu; prepare avg %.3f ms, max %.3f "
              "ms over %llu frames; range %.0f m, camera (%.1f, %.1f, %.1f)",
              s.drawn[0], s.segments[0], s.drawn[2], s.segments[2], s.drawn[1], s.segments[1], s.filled,
              s.filledTriangles, s.examined, total, frames_ ? prepareTotal_ / double(frames_) : 0.0,
              prepareMax_, static_cast<unsigned long long>(frames_), settings_.radius, camera.position[0],
              camera.position[1], camera.position[2]);
  frames_ = 0;
  prepareTotal_ = 0;
  prepareMax_ = 0;
}

void GameMemorySource::Stop() {
  std::lock_guard lock(mutex_);
  manager_.Stop();
  world_.reset();
  entities_.reset();
  level_ = {};
  loggedWorld_ = false;
}

}  // namespace doomfix::cv
