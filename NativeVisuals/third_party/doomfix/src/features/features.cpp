#include "features/feature.h"

#include "common/config.h"

namespace doomfix {

std::unique_ptr<Feature> CreateHideDeviceExtensionsFeature(const Config& config);
std::unique_ptr<Feature> CreateIndirectBarrierFixFeature(const Config& config);
std::unique_ptr<Feature> CreateCollisionViewerFeature(const Config& config);
std::unique_ptr<Feature> CreateEventTraceFeature(const Config& config);

// Порядок в списке — порядок, в котором фичи получают события. Он важен для барьеров:
// indirect-barrier-fix получает их ПЕРВЫМ и правит; все, кто только наблюдает (collision-viewer,
// event-trace), стоят после него и видят уже исправленные барьеры.
std::vector<std::unique_ptr<Feature>> CreateFeatures(const Config& config) {
  std::vector<std::unique_ptr<Feature>> features;
  features.push_back(CreateHideDeviceExtensionsFeature(config));
  // Создаётся и при DOOMFIX_INDIRECT_FIX=off: тогда только считает барьеры (A/B-сравнение).
  features.push_back(CreateIndirectBarrierFixFeature(config));
  // Включена по умолчанию; DOOMFIX_COLLISION_VIEWER=0 — не создаётся, и её перехваты не отдаются.
  if (config.collisionViewer) features.push_back(CreateCollisionViewerFeature(config));
  // Отладочная: только счётчики событий, последней.
  if (config.traceEvents) features.push_back(CreateEventTraceFeature(config));
  return features;
}

}  // namespace doomfix
