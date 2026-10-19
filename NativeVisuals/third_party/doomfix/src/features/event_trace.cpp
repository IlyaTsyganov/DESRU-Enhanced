// Отладочная фича: подписывается на все события ядра и считает вызовы. Включается только
// переменной DOOMFIX_TRACE_EVENTS=1. Ничего не меняет; раз в 5 секунд (если что-то изменилось) и при
// vkDestroyInstance пишет в лог счётчики. Нужна, чтобы проверить диспетчеризацию событий
// (tests/event_test.cpp) и посмотреть, какие вызовы делает игра.

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <mutex>
#include <string>

#include "common/config.h"
#include "common/log.h"
#include "features/feature.h"
#include "layer/dispatch.h"
#include "layer/present_edit.h"

namespace doomfix {
namespace {

constexpr auto kReportInterval = std::chrono::seconds(5);

#define DOOMFIX_TRACE_EVENTS_LIST(X)                                                           \
  X(OnDeviceCreated) X(OnDestroyDevice) X(OnDestroyInstance) X(OnGetDeviceQueue)               \
  X(OnCreateSwapchain) X(OnSwapchainCreated) X(OnDestroySwapchain) X(OnQueuePresent)           \
  X(OnImageCreated) X(OnDestroyImage) X(OnImageViewCreated) X(OnDestroyImageView)              \
  X(OnRenderPassCreated) X(OnRenderPass2Created) X(OnDestroyRenderPass)                        \
  X(OnFramebufferCreated) X(OnDestroyFramebuffer)                                              \
  X(OnAllocateCommandBuffers) X(OnBeginCommandBuffer) X(OnResetCommandBuffer)                  \
  X(OnFreeCommandBuffers) X(OnResetCommandPool) X(OnDestroyCommandPool)                        \
  X(OnCmdSetViewport) X(OnCmdBeginRenderPass) X(OnCmdNextSubpass) X(OnCmdEndRenderPass)        \
  X(OnCmdBeginRendering) X(OnCmdEndRendering) X(OnCmdExecuteCommands)                         \
  X(OnCmdPipelineBarrier) X(OnCmdWaitEvents) X(OnCmdPipelineBarrier2) X(OnCmdWaitEvents2)      \
  X(OnQueueSubmitted) X(OnQueueSubmitted2)

enum Ev {
#define DOOMFIX_ENUM(name) k##name,
  DOOMFIX_TRACE_EVENTS_LIST(DOOMFIX_ENUM)
#undef DOOMFIX_ENUM
  kEventCount
};

const char* const kEventNames[kEventCount] = {
#define DOOMFIX_NAME(name) #name,
    DOOMFIX_TRACE_EVENTS_LIST(DOOMFIX_NAME)
#undef DOOMFIX_NAME
};

class EventTrace final : public Feature {
 public:
  EventTrace() : lastReport_(std::chrono::steady_clock::now()) {
    DF_LOG_INFO("  event-trace: ON (DOOMFIX_TRACE_EVENTS=1), counting all core events");
  }

  const char* Name() const override { return "event-trace"; }
  EventMask Subscriptions() const override { return event::kAll; }

  void OnDestroyInstance(InstanceData&) override {
    Count(kOnDestroyInstance);
    Report(true);
  }

  void OnDeviceCreated(DeviceData& device) override {
    Count(kOnDeviceCreated);
    DF_LOG_INFO("event-trace: device [%s] api %u.%u, loader data callback: %s, queue families: %zu, "
                "memory types: %u",
                device.deviceName.c_str(), VK_API_VERSION_MAJOR(device.apiVersion),
                VK_API_VERSION_MINOR(device.apiVersion), device.setDeviceLoaderData ? "yes" : "no",
                device.queueFamilies.size(), device.memoryProperties.memoryTypeCount);
  }
  void OnDestroyDevice(DeviceData&) override { Count(kOnDestroyDevice); }
  void OnGetDeviceQueue(DeviceData&, const QueueData& queue) override {
    Count(kOnGetDeviceQueue);
    DF_LOG_INFO("event-trace: queue %p family %u index %u", static_cast<void*>(queue.handle),
                queue.family, queue.index);
  }

  void OnCreateSwapchain(DeviceData&, VkSwapchainCreateInfoKHR&) override { Count(kOnCreateSwapchain); }
  void OnSwapchainCreated(DeviceData&, VkSwapchainKHR, const VkSwapchainCreateInfoKHR&) override {
    Count(kOnSwapchainCreated);
  }
  void OnDestroySwapchain(DeviceData&, VkSwapchainKHR) override { Count(kOnDestroySwapchain); }
  void OnQueuePresent(DeviceData&, const QueueData& queue, PresentEdit& edit) override {
    if (Count(kOnQueuePresent) == 1) {
      DF_LOG_INFO("event-trace: first present on queue family %u index %u, %zu wait semaphore(s)",
                  queue.family, queue.index, edit.WaitSemaphores().size());
    }
  }

  void OnImageCreated(DeviceData&, VkImage, const VkImageCreateInfo&) override { Count(kOnImageCreated); }
  void OnDestroyImage(DeviceData&, VkImage) override { Count(kOnDestroyImage); }
  void OnImageViewCreated(DeviceData&, VkImageView, const VkImageViewCreateInfo&) override {
    Count(kOnImageViewCreated);
  }
  void OnDestroyImageView(DeviceData&, VkImageView) override { Count(kOnDestroyImageView); }
  void OnRenderPassCreated(DeviceData&, VkRenderPass, const VkRenderPassCreateInfo&) override {
    Count(kOnRenderPassCreated);
  }
  void OnRenderPass2Created(DeviceData&, VkRenderPass, const VkRenderPassCreateInfo2&) override {
    Count(kOnRenderPass2Created);
  }
  void OnDestroyRenderPass(DeviceData&, VkRenderPass) override { Count(kOnDestroyRenderPass); }
  void OnFramebufferCreated(DeviceData&, VkFramebuffer, const VkFramebufferCreateInfo&) override {
    Count(kOnFramebufferCreated);
  }
  void OnDestroyFramebuffer(DeviceData&, VkFramebuffer) override { Count(kOnDestroyFramebuffer); }

  void OnAllocateCommandBuffers(DeviceData&, const VkCommandBufferAllocateInfo&,
                                const VkCommandBuffer*) override {
    Count(kOnAllocateCommandBuffers);
  }
  void OnBeginCommandBuffer(DeviceData&, VkCommandBuffer, const VkCommandBufferBeginInfo&) override {
    Count(kOnBeginCommandBuffer);
  }
  void OnResetCommandBuffer(DeviceData&, VkCommandBuffer) override { Count(kOnResetCommandBuffer); }
  void OnFreeCommandBuffers(DeviceData&, VkCommandPool, uint32_t, const VkCommandBuffer*) override {
    Count(kOnFreeCommandBuffers);
  }
  void OnResetCommandPool(DeviceData&, VkCommandPool) override { Count(kOnResetCommandPool); }
  void OnDestroyCommandPool(DeviceData&, VkCommandPool) override { Count(kOnDestroyCommandPool); }

  void OnCmdSetViewport(DeviceData&, VkCommandBuffer, uint32_t, uint32_t, const VkViewport*) override {
    Count(kOnCmdSetViewport);
  }
  void OnCmdBeginRenderPass(DeviceData&, VkCommandBuffer, const char*, const VkRenderPassBeginInfo&,
                            VkSubpassContents) override {
    Count(kOnCmdBeginRenderPass);
  }
  void OnCmdNextSubpass(DeviceData&, VkCommandBuffer, const char*, VkSubpassContents) override {
    Count(kOnCmdNextSubpass);
  }
  void OnCmdEndRenderPass(DeviceData&, VkCommandBuffer, const char*) override {
    Count(kOnCmdEndRenderPass);
  }
  void OnCmdBeginRendering(DeviceData&, VkCommandBuffer, const char*, const VkRenderingInfo&) override {
    Count(kOnCmdBeginRendering);
  }
  void OnCmdEndRendering(DeviceData&, VkCommandBuffer, const char*) override {
    Count(kOnCmdEndRendering);
  }
  void OnCmdExecuteCommands(DeviceData&, VkCommandBuffer, uint32_t, const VkCommandBuffer*) override {
    Count(kOnCmdExecuteCommands);
  }
  void OnCmdPipelineBarrier(DeviceData&, VkCommandBuffer, VkDependencyFlags, BarrierSet&) override {
    Count(kOnCmdPipelineBarrier);
  }
  void OnCmdWaitEvents(DeviceData&, VkCommandBuffer, uint32_t, const VkEvent*, BarrierSet&) override {
    Count(kOnCmdWaitEvents);
  }
  void OnCmdPipelineBarrier2(DeviceData&, VkCommandBuffer, const char*,
                             const VkDependencyInfo*&) override {
    Count(kOnCmdPipelineBarrier2);
  }
  void OnCmdWaitEvents2(DeviceData&, VkCommandBuffer, const char*, uint32_t, const VkEvent*,
                        const VkDependencyInfo*&) override {
    Count(kOnCmdWaitEvents2);
  }

  void OnQueueSubmitted(DeviceData&, const QueueData& queue, uint32_t, const VkSubmitInfo*,
                        VkResult result) override {
    if (Count(kOnQueueSubmitted) == 1) LogFirstSubmit("vkQueueSubmit", queue, result);
  }
  void OnQueueSubmitted2(DeviceData&, const QueueData& queue, const char* entryPoint, uint32_t,
                         const VkSubmitInfo2*, VkResult result) override {
    if (Count(kOnQueueSubmitted2) == 1) LogFirstSubmit(entryPoint, queue, result);
  }

 private:
  // Возвращает новое значение счётчика.
  uint64_t Count(Ev e) {
    const uint64_t n = counts_[e].fetch_add(1, std::memory_order_relaxed) + 1;
    MaybeReport();
    return n;
  }

  static void LogFirstSubmit(const char* entryPoint, const QueueData& queue, VkResult result) {
    DF_LOG_INFO("event-trace: first %s on queue family %u index %u -> %d", entryPoint, queue.family,
                queue.index, static_cast<int>(result));
  }

  void MaybeReport() {
    const auto now = std::chrono::steady_clock::now();
    auto last = lastReport_.load(std::memory_order_relaxed);
    if (now - last < kReportInterval) return;
    if (!lastReport_.compare_exchange_strong(last, now)) return;  // отчёт пишет один поток
    Report(false);
  }

  void Report(bool final) {
    std::lock_guard lock(reportMutex_);
    uint64_t total = 0;
    std::string list;
    for (int i = 0; i < kEventCount; ++i) {
      const uint64_t n = counts_[i].load(std::memory_order_relaxed);
      total += n;
      if (!n) continue;
      if (!list.empty()) list += ", ";
      list += std::string(kEventNames[i]) + "=" + std::to_string(n);
    }
    if (!final && total == reportedTotal_) return;
    reportedTotal_ = total;
    DF_LOG_INFO("event-trace%s: %s", final ? " final" : "", list.empty() ? "none" : list.c_str());
  }

  std::atomic<uint64_t> counts_[kEventCount] = {};
  std::atomic<std::chrono::steady_clock::time_point> lastReport_;
  std::mutex reportMutex_;
  uint64_t reportedTotal_ = 0;  // под reportMutex_
};

}  // namespace

std::unique_ptr<Feature> CreateEventTraceFeature(const Config&) {
  return std::make_unique<EventTrace>();
}

}  // namespace doomfix
