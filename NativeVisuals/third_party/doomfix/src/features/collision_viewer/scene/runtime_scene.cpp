// Based on doom-eternal-collision-viewer by IlyaTsyganov (commit 43ac57d), used with the author's permission.

#include "features/collision_viewer/scene/runtime_scene.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>

#include "features/collision_viewer/game_memory/exe_profile.h"
#include "features/collision_viewer/game_memory/havok_shapes.h"
#include "features/collision_viewer/game_memory/live_entities.h"
#include "common/log.h"

namespace doomfix::cv {

using namespace exe11;

// --- RuntimeSceneBuilder ----------------------------------------------------------------------------------

bool RuntimeSceneBuilder::Walk(uintptr_t address, const Transform& transform, RuntimeScene& scene, unsigned depth) {
  if (cancelled()) return false;
  if (depth > 64 || scene.meshes.size() > 2000000) {
    failed_ = true;
    return false;
  }
  unsigned char header[kShapeReadSize] = {};
  if (!memory_.Read(address, header, sizeof(header))) {
    failed_ = true;
    return false;
  }
  if (header[kShapeType] == kShapeCompound) {
    uintptr_t data = 0;
    uint32_t count = 0;
    std::memcpy(&data, header + kCompoundInstances, 8);
    std::memcpy(&count, header + kCompoundCount, 4);
    if (count > 1000000) {
      failed_ = true;
      return false;
    }
    // Пачками по 256 экземпляров: большой мир можно прервать посередине.
    constexpr uint32_t kBatch = 256;
    std::vector<unsigned char> rows(size_t(kBatch) * kInstanceSize);
    for (uint32_t begin = 0; begin < count; begin += kBatch) {
      const uint32_t batch = std::min(kBatch, count - begin);
      if (cancelled()) return false;
      if (!memory_.Read(data + size_t(begin) * kInstanceSize, rows.data(), size_t(batch) * kInstanceSize)) {
        failed_ = true;
        return false;
      }
      for (uint32_t i = 0; i < batch; ++i) {
        const unsigned char* row = rows.data() + size_t(i) * kInstanceSize;
        if (row[kInstanceDisabled]) continue;
        uintptr_t child = 0;
        std::memcpy(&child, row + kInstanceShape, 8);
        Transform local;
        if (!ReadInstanceTransform(row, local)) {
          failed_ = true;
          return false;
        }
        if (!Walk(child, transform.Compose(local), scene, depth + 1)) return false;
      }
    }
    return true;
  }
  if (!contents_.AllowsShape(address)) {
    ++scene.nonBlocking;
    return true;
  }
  auto found = cache_.find(address);
  if (found == cache_.end()) {
    ShapeDecoder decoder(memory_, &contents_);
    auto geometry = std::make_shared<Geometry>();
    geometry->kind = MeshKind::kCollision;
    const bool decoded = decoder.Decode(address, Transform{}, *geometry);
    if (decoded && geometry->triangles.empty()) {
      ++errors_["empty shape"];
      return true;
    }
    if (!decoded) {
      ++scene.unsupported;
      ++errors_[decoder.Error().empty() ? std::string("unreadable shape") : decoder.Error()];
      return true;
    }
    Cached item;
    ResetBounds(item.bounds);
    for (Vec3 p : geometry->triangles) ExpandBounds(item.bounds, p);
    for (Vec3 p : geometry->lines) ExpandBounds(item.bounds, p);
    item.geometry = std::move(geometry);
    found = cache_.emplace(address, std::move(item)).first;
  }
  {
    Mesh mesh;
    mesh.kind = MeshKind::kCollision;
    mesh.geometry = found->second.geometry;
    mesh.transform = transform;
    ResetBounds(mesh.bounds);
    const float* b = found->second.bounds;
    for (unsigned corner = 0; corner < 8; ++corner) {
      ExpandBounds(mesh.bounds, mesh.World({b[corner & 1 ? 3 : 0], b[corner & 2 ? 4 : 1], b[corner & 4 ? 5 : 2]}));
    }
    scene.triangles += mesh.geometry->triangles.size() / 3;
    scene.meshes.push_back(std::move(mesh));
  }
  return true;
}

std::shared_ptr<RuntimeScene> RuntimeSceneBuilder::Build(const LevelIdentity& level) {
  const auto start = std::chrono::steady_clock::now();
  auto scene = std::make_shared<RuntimeScene>();
  scene->level = level;
  cache_.clear();
  contents_.Clear();
  errors_.clear();
  failed_ = false;
  if (!Walk(level.world, Transform{}, *scene, 0) || failed_ || cancelled()) return {};
  scene->unique = static_cast<uint32_t>(cache_.size());
  cache_.clear();
  BuildBvh(scene->meshes, scene->nodes, scene->order);
  if (cancelled()) return {};
  for (const auto& [reason, count] : errors_) {
    if (!scene->problems.empty()) scene->problems += "; ";
    scene->problems += reason + " x" + std::to_string(count);
  }
  scene->milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  return scene;
}

// --- RuntimeSceneManager ----------------------------------------------------------------------------------

struct RuntimeSceneManager::Shared {
  MemoryReader memory;
  std::mutex mutex;
  std::condition_variable wake;
  std::atomic<uint64_t> generation{0};
  std::atomic<bool> stopping{false};
  // Под mutex.
  LevelIdentity requested;
  uintptr_t base = 0;
  std::shared_ptr<const RuntimeScene> world;
  std::shared_ptr<const RuntimeEntities> entities;
  std::vector<std::shared_ptr<const void>> retired;  // освобождаются в фоновом потоке
};

RuntimeSceneManager::RuntimeSceneManager(MemoryReader memory) : shared_(new Shared) { shared_->memory = memory; }

RuntimeSceneManager::~RuntimeSceneManager() {
  if (worker_.joinable()) {
    // Не остановлен: выход процесса без vkDestroyInstance (поток уже убит) или ошибка порядка вызовов.
    // Ждать и брать мьютекс нельзя; данные остаются в памяти намеренно.
    shared_->stopping = true;
    shared_->wake.notify_all();
    worker_.detach();
    return;
  }
  delete shared_;
}

void RuntimeSceneManager::Request(const LevelIdentity& level, uintptr_t base) {
  std::lock_guard lock(shared_->mutex);
  if (shared_->requested == level && shared_->base == base && worker_.joinable()) return;
  shared_->requested = level;
  shared_->base = base;
  // Старые снимки освободит фоновый поток.
  if (shared_->world) shared_->retired.push_back(std::move(shared_->world));
  if (shared_->entities) shared_->retired.push_back(std::move(shared_->entities));
  shared_->world.reset();
  shared_->entities.reset();
  ++shared_->generation;
  if (!worker_.joinable()) {
    shared_->stopping = false;
    worker_ = std::thread(Run, shared_);
  }
  shared_->wake.notify_one();
}

void RuntimeSceneManager::Retire(std::shared_ptr<const RuntimeScene> old) {
  if (!old) return;
  std::lock_guard lock(shared_->mutex);
  shared_->retired.push_back(std::move(old));
  shared_->wake.notify_one();
}

void RuntimeSceneManager::Snapshot(const LevelIdentity& level, std::shared_ptr<const RuntimeScene>& world,
                                   std::shared_ptr<const RuntimeEntities>& entities) {
  std::lock_guard lock(shared_->mutex);
  world = shared_->world && shared_->world->level == level ? shared_->world : nullptr;
  entities = shared_->entities && shared_->entities->level == level ? shared_->entities : nullptr;
}

void RuntimeSceneManager::Stop() {
  shared_->stopping = true;
  ++shared_->generation;
  shared_->wake.notify_all();
  if (worker_.joinable()) worker_.join();
  std::lock_guard lock(shared_->mutex);
  shared_->world.reset();
  shared_->entities.reset();
  shared_->retired.clear();
  shared_->requested = {};
  shared_->base = 0;
}

void RuntimeSceneManager::Run(Shared* s) {
  uint64_t version = ~0ull;
  LevelIdentity level;
  uintptr_t base = 0;
  bool built = false;
  ULONGLONG retryAt = 0;
  EntityScanner scanner(s->memory);
  std::thread optimizer;
  while (!s->stopping.load()) {
    std::vector<std::shared_ptr<const void>> garbage;
    {
      std::unique_lock lock(s->mutex);
      s->wake.wait_for(lock, std::chrono::milliseconds(100),
                       [&] { return s->stopping.load() || s->generation.load() != version; });
      if (s->stopping.load()) break;
      // Retired snapshots must be released here only after readers have let go of them.
      for (auto it = s->retired.begin(); it != s->retired.end();) {
        if (it->use_count() == 1) {
          garbage.push_back(std::move(*it));
          it = s->retired.erase(it);
        } else
          ++it;
      }
      if (s->generation.load() != version) {
        version = s->generation.load();
        level = s->requested;
        base = s->base;
        built = false;
        retryAt = 0;
      }
    }
    garbage.clear();  // большие старые сцены освобождаются здесь, не на потоке present
    if (!built && optimizer.joinable()) optimizer.join();
    if (!level) continue;
    try {
      if (ReadLevelIdentity(s->memory, base) != level) continue;
      scanner.Refresh(base, true);
      auto next = std::make_shared<RuntimeEntities>();
      next->level = level;
      next->meshes.swap(scanner.meshes);
      next->triggers = scanner.triggers;
      next->walls = scanner.walls;
      next->unsupported = scanner.unsupported;
      next->milliseconds = scanner.milliseconds;
      if (ReadLevelIdentity(s->memory, base) != level) continue;
      std::shared_ptr<const RuntimeEntities> previous;
      {
        std::lock_guard lock(s->mutex);
        if (s->generation.load() != version) continue;
        previous = std::move(s->entities);
        s->entities = std::move(next);
      }
      previous.reset();
      if (!built && GetTickCount64() >= retryAt) {
        RuntimeSceneBuilder builder(s->memory);
        builder.cancelled = [&] { return s->stopping.load() || s->generation.load() != version; };
        std::shared_ptr<const RuntimeScene> world = builder.Build(level);
        if (world && ReadLevelIdentity(s->memory, base) == level) {
          std::lock_guard lock(s->mutex);
          if (s->generation.load() == version) {
            s->world = world;
            built = true;
            optimizer = std::thread(Optimize, s, std::move(world), version);
          }
        }
        retryAt = GetTickCount64() + 2000;
      }
    } catch (...) {
      // Нехватка памяти и т.п.: попробовать позже, игра идёт дальше.
      retryAt = GetTickCount64() + 2000;
    }
  }
  if (optimizer.joinable()) optimizer.join();
}

void RuntimeSceneManager::Optimize(Shared* s, std::shared_ptr<const RuntimeScene> source,
                                   uint64_t generation) {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  auto cancelled = [&] { return s->stopping.load() || s->generation.load() != generation; };
  try {
    DF_LOG_INFO("collision-viewer: preparing exterior collision surfaces in the background (%zu objects)",
                source->meshes.size());
    auto result = std::make_shared<RuntimeScene>();
    result->level = source->level;
    result->unique = source->unique;
    result->unsupported = source->unsupported;
    result->nonBlocking = source->nonBlocking;
    result->problems = source->problems;
    result->milliseconds = source->milliseconds;
    auto lastProgress = std::chrono::steady_clock::now();
    auto progress = [&](uint32_t done, uint32_t total) {
      const auto now = std::chrono::steady_clock::now();
      if (now - lastProgress >= std::chrono::seconds(10)) {
        lastProgress = now;
        DF_LOG_INFO("collision-viewer: exterior surfaces progress %u/%u", done, total);
      }
    };
    if (!OptimizeCollisionSurfaces(source->meshes, source->nodes, source->order, result->meshes,
                                   result->optimization, cancelled, progress))
      return;
    BuildBvh(result->meshes, result->nodes, result->order);
    if (cancelled()) return;
    result->optimized = true;
    result->triangles = result->optimization.triangles;
    const auto& stats = result->optimization;
    DF_LOG_INFO(
        "collision-viewer: exterior surfaces ready: %u closed shapes, %u open/unclassified, "
        "%u objects removed, %u trimmed; triangles %llu -> %llu, edges %llu -> %llu, %.1f ms",
        stats.closedShapes, stats.unclassifiedShapes, stats.removed, stats.trimmed,
        static_cast<unsigned long long>(stats.originalTriangles),
        static_cast<unsigned long long>(stats.triangles),
        static_cast<unsigned long long>(stats.originalEdges), static_cast<unsigned long long>(stats.edges),
        stats.milliseconds);
    DF_LOG_INFO("collision-viewer: retained %llu exposed intersection contour segments",
                static_cast<unsigned long long>(stats.intersectionEdges));
    std::lock_guard lock(s->mutex);
    if (cancelled()) return;
    if (s->world) s->retired.push_back(std::move(s->world));
    s->world = std::move(result);
  } catch (const std::exception& error) {
    DF_LOG_WARN("collision-viewer: exterior optimization kept the original scene: %s", error.what());
  } catch (...) {
    DF_LOG_WARN("collision-viewer: exterior optimization kept the original scene after an error");
  }
}

}  // namespace doomfix::cv
