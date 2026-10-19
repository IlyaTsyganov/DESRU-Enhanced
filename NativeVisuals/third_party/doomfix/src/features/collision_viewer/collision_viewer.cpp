// Based on doom-eternal-collision-viewer by IlyaTsyganov (commit 43ac57d), used with the author's permission.
//
// Фича collision-viewer: связывает события ядра с трекером глубины (depth_tracker.h) и рендерером
// оверлея (overlay_renderer.h). Включена по умолчанию; DOOMFIX_COLLISION_VIEWER=0 — фича не создаётся
// вовсе (features.cpp), и маска интересов остаётся как без неё.
//
// Надёжность: любое исключение внутри фичи выключает только viewer (запись в лог, дальше события
// игнорируются, кроме освобождения своих ресурсов). Барьеры фича только читает, поэтому
// indirect-barrier-fix от неё не зависит.

#include <windows.h>

#include <atomic>
#include <exception>
#include <map>
#include <memory>
#include <mutex>

#include "common/config.h"
#include "common/log.h"
#include "features/collision_viewer/depth_tracker.h"
#include "features/collision_viewer/game_memory/fake_game.h"
#include "features/collision_viewer/game_memory/version_check.h"
#include "features/collision_viewer/game_memory_source.h"
#include "features/collision_viewer/overlay_renderer.h"
#include "features/collision_viewer/sources.h"
#include "features/collision_viewer/test_scene.h"
#include "features/feature.h"
#include "layer/device_create_edit.h"
#include "layer/dispatch.h"
#include "layer/present_edit.h"
#include "runtime.h"

namespace doomfix {
namespace {

using cv::DepthTracker;
using cv::OverlayRenderer;

// Всё, что фича хранит на устройстве (DeviceData::featureStates[Index()]).
struct DeviceState final : FeatureState {
  DeviceState(DeviceData& device, cv::ICameraSource& camera, cv::ISceneSource& scene,
              const cv::TestOptions& test)
      : tracker(desru::Controlled()),renderer(device, tracker, camera, scene, test) {}
  DepthTracker tracker;
  OverlayRenderer renderer;
};

class CollisionViewer final : public Feature {
 public:
  explicit CollisionViewer(const Config& config)
      : test_(cv::ParseTestOptions(config.cvTest, config.cvOcclusionTest, config.cvTestConsoleFraction,
                                   config.cvTestReadback)) {
    DF_LOG_INFO("  collision-viewer: ON (DOOMFIX_COLLISION_VIEWER=0 turns it off), test mode: %s%s%s%s",
                cv::TestModeName(test_.mode),
                test_.occlusion != cv::OcclusionCase::kNone ? ", occlusion " : "",
                test_.occlusion != cv::OcclusionCase::kNone ? cv::OcclusionCaseName(test_.occlusion) : "",
                test_.readback ? ", frame readback" : "");
    if (!test_.problems.empty()) DF_LOG_WARN("  collision-viewer: %s", test_.problems.c_str());
    if (test_.mode == cv::TestMode::kScene) {
      // Настоящий источник над поддельной памятью игры.
      fakeGame_ = new cv::FakeGame();
      if (fakeGame_->Valid()) {
        fakeGame_->SetConsole(test_.consoleFraction, false);
        UseGameSource(fakeGame_->Base());
        DF_LOG_INFO("  collision-viewer: test scene: fake game memory at %p",
                    reinterpret_cast<void*>(fakeGame_->Base()));
      } else {
        DF_LOG_WARN("  collision-viewer: test scene: cannot reserve the fake game memory");
      }
    } else if (test_.Active()) {
      testSource_ = std::make_unique<cv::TestSource>(test_);
      camera_ = testSource_.get();
      scene_ = testSource_.get();
    } else {
      // Версия exe проверяется до любых чтений памяти игры. Не 1.1 — viewer ничего не рисует.
      const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
      const cv::ExeVersion version = cv::CheckExeVersion(base);
      if (version.known || desru::Controlled()) {
        DF_LOG_INFO("  collision-viewer: %s (%s); reading the game's memory",
                    desru::Controlled()?"DESRU adaptive profile":"DOOM Eternal 1.1 recognised",version.description.c_str());
        UseGameSource(base);
      } else {
        DF_LOG_INFO("  collision-viewer: not the DOOM Eternal 1.1 exe (%s): the viewer draws nothing, the "
                    "lighting fix works as usual",
                    version.description.c_str());
      }
    }
  }

  // Поток сцены не остановлен (игра вышла без vkDestroyInstance): при выходе процесса он уже убит, и
  // освобождать его данные (сотни МБ, возможно захваченные мьютексы) нельзя — они остаются в памяти.
  ~CollisionViewer() override {
    if (gameSource_ && !gameSource_->Stopped()) return;
    delete gameSource_;
    delete fakeGame_;
  }

  void OnDestroyInstance(InstanceData&) override {
    Cleanup("vkDestroyInstance", [&] { StopScene(); });
  }

  const char* Name() const override { return "collision-viewer"; }
  EventMask Subscriptions() const override {
    return event::kSwapchain | event::kPresent | event::kImages | event::kImageViews |
           event::kRenderPasses | event::kFramebuffers | event::kCommandBuffers | event::kViewport |
           event::kRenderPassCmds | event::kExecuteCommands | event::kBarriers | event::kWaitEvents |
           event::kSubmit;
  }

  // --- Устройство -------------------------------------------------------------------------------

  // Своя графическая очередь: игра может показывать кадр с очереди без графики (DOOM Eternal 1.1 — с
  // compute-очереди), а рисовать на чужой очереди игры с другого потока нельзя (внешняя синхронизация).
  void OnCreateDevice(VkPhysicalDevice physicalDevice, DeviceCreateEdit& edit) override {
    Guard("vkCreateDevice", [&] {
      const auto& families = edit.QueueFamilies();
      for (uint32_t family = 0; family < families.size(); ++family) {
        if (!(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        uint32_t index = 0;
        if (edit.AddQueue(family, &index)) {
          std::lock_guard lock(pendingMutex_);
          pendingQueues_[physicalDevice] = {family, index};
          DF_LOG_INFO("collision-viewer: requested its own graphics queue: family %u, index %u", family, index);
        } else {
          DF_LOG_WARN("collision-viewer: no free graphics queue in family %u; the overlay can be drawn only "
                      "when the game presents on a graphics queue",
                      family);
        }
        return;
      }
    });
  }

  void OnDeviceCreated(DeviceData& device) override {
    // Запрос очереди сделан для этого vkCreateDevice: забрать его, даже если viewer уже выключен.
    std::pair<uint32_t, uint32_t> queue = {VK_QUEUE_FAMILY_IGNORED, 0};
    {
      std::lock_guard lock(pendingMutex_);
      auto it = pendingQueues_.find(device.physicalDevice);
      if (it != pendingQueues_.end()) {
        queue = it->second;
        pendingQueues_.erase(it);
      }
    }
    Guard("device setup", [&] {
      if (Index() >= device.featureStates.size()) return;
      auto state = std::make_unique<DeviceState>(device, *camera_, *scene_, test_);
      if(device.sharedGraphicsQueue) state->renderer.SetOverlayQueue(device.sharedGraphicsQueue,device.sharedGraphicsFamily);
      if (queue.first != VK_QUEUE_FAMILY_IGNORED && device.setDeviceLoaderData) {
        VkQueue handle = VK_NULL_HANDLE;
        device.vk.GetDeviceQueue(device.handle, queue.first, queue.second, &handle);
        // Очередь получена мимо loader'а: ей нужна его таблица, как своим командным буферам.
        if (handle && device.setDeviceLoaderData(device.handle, handle) == VK_SUCCESS) {
          state->renderer.SetOverlayQueue(handle, queue.first);
        }
      }
      device.featureStates[Index()] = std::move(state);
    });
  }

  void OnDestroyDevice(DeviceData& device) override {
    Cleanup("device shutdown", [&] {
      if (DeviceState* s = State(device)) s->renderer.Shutdown();
      StopScene();
    });
  }

  // --- Swapchain и present ------------------------------------------------------------------------

  void OnCreateSwapchain(DeviceData& device, VkSwapchainCreateInfoKHR& info) override {
    Guard("vkCreateSwapchainKHR", [&] {
      if (DeviceState* s = State(device)) s->renderer.PrepareSwapchain(info);
    });
  }

  void OnSwapchainCreated(DeviceData& device, VkSwapchainKHR swapchain,
                          const VkSwapchainCreateInfoKHR& info) override {
    if(desru::Controlled()) InterlockedIncrement(&DESRUVisualControl.swapchains);
    Guard("vkCreateSwapchainKHR", [&] {
      if (DeviceState* s = State(device)) s->renderer.OnSwapchainCreated(swapchain, info);
    });
  }

  void OnDestroySwapchain(DeviceData& device, VkSwapchainKHR swapchain) override {
    Cleanup("vkDestroySwapchainKHR", [&] {
      if (DeviceState* s = State(device)) s->renderer.OnSwapchainDestroyed(swapchain);
    });
  }

  void OnQueuePresent(DeviceData& device, const QueueData& queue, PresentEdit& edit) override {
    DeviceState* s = State(device);
    if (!s) return;
    if(desru::Controlled()&&!desru::Viewer()) {
      if(wasVisible_) { StopScene(); wasVisible_=false; }
      s->renderer.EndPresent(); return;
    }
    wasVisible_=true;
    Guard("vkQueuePresentKHR", [&] { s->renderer.Present(queue, edit); });
    Cleanup("vkQueuePresentKHR", [&] { s->renderer.EndPresent(); });
  }

  // --- Ресурсы -------------------------------------------------------------------------------------

  void OnImageCreated(DeviceData& device, VkImage image, const VkImageCreateInfo& info) override {
    Guard("vkCreateImage", [&] {
      if (DeviceState* s = State(device)) s->tracker.OnImageCreated(image, info);
    });
  }
  void OnDestroyImage(DeviceData& device, VkImage image) override {
    Cleanup("vkDestroyImage", [&] {
      DeviceState* s = State(device);
      if (!s) return;
      if (const VkImageView view = s->tracker.OnImageDestroyed(image)) {
        s->renderer.OnDepthImageDestroyed(image, view);
      }
    });
  }
  void OnImageViewCreated(DeviceData& device, VkImageView view, const VkImageViewCreateInfo& info) override {
    Guard("vkCreateImageView", [&] {
      if (DeviceState* s = State(device)) s->tracker.OnImageViewCreated(view, info);
    });
  }
  void OnDestroyImageView(DeviceData& device, VkImageView view) override {
    Cleanup("vkDestroyImageView", [&] {
      if (DeviceState* s = State(device)) s->tracker.OnImageViewDestroyed(view);
    });
  }
  void OnRenderPassCreated(DeviceData& device, VkRenderPass pass, const VkRenderPassCreateInfo& info) override {
    Guard("vkCreateRenderPass", [&] {
      if (DeviceState* s = State(device)) s->tracker.OnRenderPassCreated(pass, info);
    });
  }
  void OnRenderPass2Created(DeviceData& device, VkRenderPass pass,
                            const VkRenderPassCreateInfo2& info) override {
    Guard("vkCreateRenderPass2", [&] {
      if (DeviceState* s = State(device)) s->tracker.OnRenderPass2Created(pass, info);
    });
  }
  void OnDestroyRenderPass(DeviceData& device, VkRenderPass pass) override {
    Cleanup("vkDestroyRenderPass", [&] {
      if (DeviceState* s = State(device)) s->tracker.OnRenderPassDestroyed(pass);
    });
  }
  void OnFramebufferCreated(DeviceData& device, VkFramebuffer framebuffer,
                            const VkFramebufferCreateInfo& info) override {
    Guard("vkCreateFramebuffer", [&] {
      if (DeviceState* s = State(device)) s->tracker.OnFramebufferCreated(framebuffer, info);
    });
  }
  void OnDestroyFramebuffer(DeviceData& device, VkFramebuffer framebuffer) override {
    Cleanup("vkDestroyFramebuffer", [&] {
      if (DeviceState* s = State(device)) s->tracker.OnFramebufferDestroyed(framebuffer);
    });
  }

  // --- Командные буферы и пулы ---------------------------------------------------------------------

  void OnAllocateCommandBuffers(DeviceData& device, const VkCommandBufferAllocateInfo& info,
                                const VkCommandBuffer* buffers) override {
    Guard("vkAllocateCommandBuffers", [&] {
      if (DeviceState* s = State(device)) s->tracker.OnAllocated(info.commandPool, info.commandBufferCount, buffers);
    });
  }
  void OnBeginCommandBuffer(DeviceData& device, VkCommandBuffer cb, const VkCommandBufferBeginInfo&) override {
    Guard("vkBeginCommandBuffer", [&] {
      if (DeviceState* s = State(device)) s->tracker.OnBegin(cb);
    });
  }
  void OnResetCommandBuffer(DeviceData& device, VkCommandBuffer cb) override {
    Cleanup("vkResetCommandBuffer", [&] {
      if (DeviceState* s = State(device)) s->tracker.OnReset(cb);
    });
  }
  void OnFreeCommandBuffers(DeviceData& device, VkCommandPool pool, uint32_t count,
                            const VkCommandBuffer* buffers) override {
    Cleanup("vkFreeCommandBuffers", [&] {
      if (DeviceState* s = State(device)) s->tracker.OnFreed(pool, count, buffers);
    });
  }
  void OnResetCommandPool(DeviceData& device, VkCommandPool pool) override {
    Cleanup("vkResetCommandPool", [&] {
      if (DeviceState* s = State(device)) s->tracker.OnPoolReset(pool);
    });
  }
  void OnDestroyCommandPool(DeviceData& device, VkCommandPool pool) override {
    Cleanup("vkDestroyCommandPool", [&] {
      if (DeviceState* s = State(device)) s->tracker.OnPoolDestroyed(pool);
    });
  }

  // --- Запись команд (потоки игры; только лента своего командного буфера) -------------------------

  void OnCmdSetViewport(DeviceData& device, VkCommandBuffer cb, uint32_t first, uint32_t count,
                        const VkViewport* viewports) override {
    Guard("vkCmdSetViewport", [&] {
      if (DeviceState* s = State(device)) s->tracker.RecordViewport(cb, first, count, viewports);
    });
  }
  void OnCmdBeginRenderPass(DeviceData& device, VkCommandBuffer cb, const char*,
                            const VkRenderPassBeginInfo& info, VkSubpassContents) override {
    Guard("vkCmdBeginRenderPass", [&] {
      if (DeviceState* s = State(device)) s->tracker.RecordBeginRenderPass(cb, info);
    });
  }
  void OnCmdEndRenderPass(DeviceData& device, VkCommandBuffer cb, const char*) override {
    Guard("vkCmdEndRenderPass", [&] {
      if (DeviceState* s = State(device)) s->tracker.RecordEndRenderPass(cb);
    });
  }
  void OnCmdBeginRendering(DeviceData& device, VkCommandBuffer cb, const char*,
                           const VkRenderingInfo& info) override {
    Guard("vkCmdBeginRendering", [&] {
      if (DeviceState* s = State(device)) s->tracker.RecordBeginRendering(cb, info);
    });
  }
  void OnCmdEndRendering(DeviceData& device, VkCommandBuffer cb, const char*) override {
    Guard("vkCmdEndRendering", [&] {
      if (DeviceState* s = State(device)) s->tracker.RecordEndRendering(cb);
    });
  }
  void OnCmdExecuteCommands(DeviceData& device, VkCommandBuffer cb, uint32_t count,
                            const VkCommandBuffer* secondaries) override {
    Guard("vkCmdExecuteCommands", [&] {
      if (DeviceState* s = State(device)) s->tracker.RecordExecuteCommands(cb, count, secondaries);
    });
  }
  // Барьеры фича только читает (после indirect-barrier-fix): смена layout картинок глубины.
  void OnCmdPipelineBarrier(DeviceData& device, VkCommandBuffer cb, VkDependencyFlags,
                            BarrierSet& barriers) override {
    Guard("vkCmdPipelineBarrier", [&] {
      if (DeviceState* s = State(device)) {
        s->tracker.RecordImageBarriers(cb, barriers.imageMemoryBarrierCount, barriers.pImageMemoryBarriers);
      }
    });
  }
  void OnCmdWaitEvents(DeviceData& device, VkCommandBuffer cb, uint32_t, const VkEvent*,
                       BarrierSet& barriers) override {
    Guard("vkCmdWaitEvents", [&] {
      if (DeviceState* s = State(device)) {
        s->tracker.RecordImageBarriers(cb, barriers.imageMemoryBarrierCount, barriers.pImageMemoryBarriers);
      }
    });
  }
  void OnCmdPipelineBarrier2(DeviceData& device, VkCommandBuffer cb, const char*,
                             const VkDependencyInfo*& info) override {
    Guard("vkCmdPipelineBarrier2", [&] {
      DeviceState* s = State(device);
      if (s && info) s->tracker.RecordImageBarriers2(cb, info->imageMemoryBarrierCount, info->pImageMemoryBarriers);
    });
  }
  void OnCmdWaitEvents2(DeviceData& device, VkCommandBuffer cb, const char*, uint32_t eventCount,
                        const VkEvent*, const VkDependencyInfo*& infos) override {
    Guard("vkCmdWaitEvents2", [&] {
      DeviceState* s = State(device);
      if (!s || !infos) return;
      for (uint32_t i = 0; i < eventCount; ++i) {
        s->tracker.RecordImageBarriers2(cb, infos[i].imageMemoryBarrierCount, infos[i].pImageMemoryBarriers);
      }
    });
  }

  // --- Отправка ----------------------------------------------------------------------------------------

  void OnQueueSubmitted(DeviceData& device, const QueueData& queue, uint32_t submitCount,
                        const VkSubmitInfo* submits, VkResult result) override {
    if (result != VK_SUCCESS) return;
    Guard("vkQueueSubmit", [&] {
      if (DeviceState* s = State(device)) s->tracker.OnSubmitted(queue.family, submitCount, submits);
    });
  }
  void OnQueueSubmitted2(DeviceData& device, const QueueData& queue, const char*, uint32_t submitCount,
                         const VkSubmitInfo2* submits, VkResult result) override {
    if (result != VK_SUCCESS) return;
    Guard("vkQueueSubmit2", [&] {
      if (DeviceState* s = State(device)) s->tracker.OnSubmitted2(queue.family, submitCount, submits);
    });
  }

 private:
  bool wasVisible_=false;
  void UseGameSource(uintptr_t base) {
    gameSource_ = new cv::GameMemorySource(base);
    camera_ = gameSource_;
    scene_ = gameSource_;
  }

  // Фоновый поток сцены останавливается здесь, а не в деструкторе (там ждать потоки нельзя).
  void StopScene() {
    if (gameSource_) gameSource_->Stop();
  }

  DeviceState* State(DeviceData& device) const {
    if (Index() >= device.featureStates.size()) return nullptr;
    return static_cast<DeviceState*>(device.featureStates[Index()].get());
  }

  // Работа, которая выполняется только пока viewer включён. Исключение выключает viewer.
  template <typename Fn>
  void Guard(const char* where, Fn&& fn) noexcept {
    if (disabled_.load(std::memory_order_relaxed)) return;
    Run(where, fn);
  }

  // Освобождение и учёт уничтожения: выполняется и после выключения viewer, чтобы не оставлять
  // своих объектов и устаревших записей.
  template <typename Fn>
  void Cleanup(const char* where, Fn&& fn) noexcept {
    Run(where, fn);
  }

  template <typename Fn>
  void Run(const char* where, Fn& fn) noexcept {
    try {
      fn();
    } catch (const std::exception& e) {
      Disable(where, e.what());
    } catch (...) {
      Disable(where, "unknown exception");
    }
  }

  void Disable(const char* where, const char* what) noexcept {
    if(desru::Controlled()) { DESRUVisualControl.error=4;InterlockedAnd(&DESRUVisualControl.active,~desru::kViewer); }
    if (disabled_.exchange(true)) return;
    DF_LOG_ERROR("collision-viewer: %s: %s. The viewer is turned off for this session; the game and "
                 "the lighting fix go on as usual.",
                 where, what);
    // Фоновый поток сцены больше не нужен (исключение уже вышло из всех блокировок источника).
    try {
      StopScene();
    } catch (...) {
    }
  }

  cv::TestOptions test_;
  cv::EmptySource empty_;
  std::unique_ptr<cv::TestSource> testSource_;
  // Владеет фича; см. деструктор. fakeGame_ — только в режиме DOOMFIX_CV_TEST=scene.
  cv::GameMemorySource* gameSource_ = nullptr;
  cv::FakeGame* fakeGame_ = nullptr;
  cv::ICameraSource* camera_ = &empty_;
  cv::ISceneSource* scene_ = &empty_;
  std::atomic<bool> disabled_{false};
  // Очередь, запрошенная в OnCreateDevice, до OnDeviceCreated (физическое устройство -> семейство, индекс).
  std::mutex pendingMutex_;
  std::map<VkPhysicalDevice, std::pair<uint32_t, uint32_t>> pendingQueues_;
};

}  // namespace

std::unique_ptr<Feature> CreateCollisionViewerFeature(const Config& config) {
  return std::make_unique<CollisionViewer>(config);
}

}  // namespace doomfix
