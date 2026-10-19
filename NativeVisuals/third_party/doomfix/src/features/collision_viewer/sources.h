// Источники данных оверлея: камера игры и геометрия сцены.
//
// Рендерер (overlay_renderer.h) в каждом present спрашивает камеру, затем сцену. Сейчас есть две
// реализации: пустая (EmptySource — в игре ничего не рисуется) и тестовая (TestSource, test_scene.h —
// рисунки для проверок на GPU и тестовый рисунок в игре). Источник из памяти игры (камера, сущности,
// коллизии) добавит следующий шаг переноса viewer.
//
// Методы вызываются в vkQueuePresentKHR, на потоке present игры, под блокировкой рендерера (по одному
// вызову за раз). Они не должны ждать GPU и долго работать: это время кадра игры.

#pragma once

#include "features/collision_viewer/overlay_types.h"

namespace doomfix::cv {

class ICameraSource {
 public:
  virtual ~ICameraSource() = default;
  // Текущая камера игры. false — камеры нет (меню, загрузка): оверлей в этом кадре не рисуется.
  virtual bool ReadCamera(Camera& camera) = 0;
};

class ISceneSource {
 public:
  virtual ~ISceneSource() = default;
  // Что рисовать в этом кадре (scene уже очищена). false — ничего.
  virtual bool BuildScene(const FrameInfo& frame, const Camera& camera, OverlayScene& scene) = 0;
};

// Нет ни камеры, ни сцены: оверлей ничего не рисует и не создаёт своих ресурсов на GPU.
class EmptySource final : public ICameraSource, public ISceneSource {
 public:
  bool ReadCamera(Camera&) override { return false; }
  bool BuildScene(const FrameInfo&, const Camera&, OverlayScene&) override { return false; }
};

}  // namespace doomfix::cv
