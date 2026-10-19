#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace doomfix {

struct Config;
struct DeviceData;
struct InstanceData;
struct QueueData;
class DeviceCreateEdit;
class PresentEdit;

// Фича — независимый модуль поведения слоя. Ядро (layer/layer.cpp) перехватывает вызовы
// Vulkan и раздаёт их фичам через виртуальные методы ниже; фича переопределяет только нужные.
//
// Чтобы добавить фичу:
//   1. Унаследоваться от Feature в новом файле src/features/<имя>.cpp.
//   2. Переопределить Subscriptions(): какие группы событий нужны (флаги event::k*).
//   3. Зарегистрировать фабрику в src/features/features.cpp (порядок в списке = порядок событий).
//
// Маска интересов. Перехватчики из необязательных групп ядро отдаёт из vkGet*ProcAddr только если
// группа нужна хотя бы одной включённой фиче; иначе приложение получает функцию следующего звена
// напрямую, и перехват ничего не стоит. Маска считается один раз при создании фич (первый вызов
// vkNegotiateLoaderLayerInterfaceVersion / vkGet*ProcAddr / vkCreateInstance) и дальше не меняется.
// События без группы (создание/уничтожение instance и устройства, vkGetDeviceQueue*) получают все фичи
// всегда: эти вызовы нужны самому ядру.
//
// Чтобы фичи могли реагировать на новый вызов Vulkan:
//   1. Добавить функцию в DOOMFIX_DEVICE_FUNCS_CORE или _OPTIONAL (layer/dispatch.h).
//   2. Написать перехватчик в layer/layer.cpp и внести его в kDeviceHooks с группой событий.
//   3. Добавить сюда виртуальный метод-событие (и флаг группы, если нужна новая) и вызывать его из
//      перехватчика.
//
// Когда вызываются события:
//   - создание объекта (On*Created) — после успешного вызова следующего звена, с теми параметрами,
//     которые ушли вниз;
//   - уничтожение (OnDestroy*, OnFree*, OnReset*) — до вызова следующего звена, пока хэндлы живы;
//   - запись команд (OnCmd*) — до передачи команды дальше; вызываются на потоке записи, часто и из
//     нескольких потоков сразу: без глобальных блокировок, логирования и лишних аллокаций;
//   - отправка (OnQueueSubmitted*) — после вызова следующего звена, с его результатом.
// Собственные вызовы фич (свои командные буферы, submit и т.д.) идут прямо в DeviceData::vk, мимо
// перехватчиков, и другим фичам не видны.

using EventMask = uint64_t;

// Группы событий для Feature::Subscriptions().
namespace event {
inline constexpr EventMask kBarriers = 1ull << 0;         // OnCmdPipelineBarrier, OnCmdPipelineBarrier2
inline constexpr EventMask kWaitEvents = 1ull << 1;       // OnCmdWaitEvents, OnCmdWaitEvents2
inline constexpr EventMask kSwapchain = 1ull << 2;        // OnCreateSwapchain, OnSwapchainCreated, OnDestroySwapchain
inline constexpr EventMask kPresent = 1ull << 3;          // OnQueuePresent
inline constexpr EventMask kImages = 1ull << 4;           // OnImageCreated, OnDestroyImage
inline constexpr EventMask kImageViews = 1ull << 5;       // OnImageViewCreated, OnDestroyImageView
inline constexpr EventMask kRenderPasses = 1ull << 6;     // OnRenderPassCreated, OnRenderPass2Created, OnDestroyRenderPass
inline constexpr EventMask kFramebuffers = 1ull << 7;     // OnFramebufferCreated, OnDestroyFramebuffer
inline constexpr EventMask kCommandBuffers = 1ull << 8;   // OnAllocateCommandBuffers, OnBegin/OnReset/OnFreeCommandBuffer(s),
                                                          // OnResetCommandPool, OnDestroyCommandPool
inline constexpr EventMask kViewport = 1ull << 9;         // OnCmdSetViewport
inline constexpr EventMask kRenderPassCmds = 1ull << 10;  // OnCmdBegin/Next/EndRenderPass, OnCmdBegin/EndRendering
inline constexpr EventMask kExecuteCommands = 1ull << 11; // OnCmdExecuteCommands
inline constexpr EventMask kSubmit = 1ull << 12;          // OnQueueSubmitted, OnQueueSubmitted2
inline constexpr EventMask kAll = (1ull << 13) - 1;
}  // namespace event

// Барьеры vkCmdPipelineBarrier / vkCmdWaitEvents (общая часть аргументов). Фича может заменить
// значения и указатели; массивы, на которые она перенаправит указатели, должны жить до
// возврата из перехватчика (удобно держать их в thread_local-хранилище фичи).
struct BarrierSet {
  VkPipelineStageFlags srcStageMask;
  VkPipelineStageFlags dstStageMask;
  uint32_t memoryBarrierCount;
  const VkMemoryBarrier* pMemoryBarriers;
  uint32_t bufferMemoryBarrierCount;
  const VkBufferMemoryBarrier* pBufferMemoryBarriers;
  uint32_t imageMemoryBarrierCount;
  const VkImageMemoryBarrier* pImageMemoryBarriers;
};

class Feature {
 public:
  virtual ~Feature() = default;

  virtual const char* Name() const = 0;

  // Номер фичи в списке из features.cpp; им же фича находит своё состояние на устройстве:
  // DeviceData::featureStates[Index()]. Назначает ядро при создании фич.
  size_t Index() const { return index_; }
  void SetIndex(size_t index) { index_ = index; }

  // Группы событий (event::k*), которые нужны фиче. Влияет на то, какие перехватчики ядро отдаёт
  // приложению; события вне маски фича может не получить.
  virtual EventMask Subscriptions() const { return 0; }

  // --- Instance и устройство (всегда) -------------------------------------------------------

  // Вызывается перед vkDestroyInstance (например, чтобы остановить свои фоновые потоки).
  virtual void OnDestroyInstance(InstanceData& /*instance*/) {}

  // Скрыть расширение устройства из vkEnumerateDeviceExtensionProperties.
  virtual bool HidesDeviceExtension(std::string_view /*name*/) const { return false; }

  // Изменить параметры vkCreateDevice до передачи следующему звену цепочки.
  virtual void OnCreateDevice(VkPhysicalDevice /*physicalDevice*/, DeviceCreateEdit& /*edit*/) {}

  // Устройство создано и зарегистрировано в слое.
  virtual void OnDeviceCreated(DeviceData& /*device*/) {}

  // Вызывается перед vkDestroyDevice.
  virtual void OnDestroyDevice(DeviceData& /*device*/) {}

  // Приложение получило очередь (vkGetDeviceQueue / vkGetDeviceQueue2); ядро уже внесло её в
  // карту очередей. Может вызываться повторно для той же очереди.
  virtual void OnGetDeviceQueue(DeviceData& /*device*/, const QueueData& /*queue*/) {}

  // --- Swapchain и present (event::kSwapchain, event::kPresent) -------------------------------

  // Изменить параметры vkCreateSwapchainKHR (копия, ей владеет ядро) до передачи дальше. Цепочку
  // pNext не трогать.
  virtual void OnCreateSwapchain(DeviceData& /*device*/, VkSwapchainCreateInfoKHR& /*info*/) {}
  virtual void OnSwapchainCreated(DeviceData& /*device*/, VkSwapchainKHR /*swapchain*/,
                                  const VkSwapchainCreateInfoKHR& /*info*/) {}
  virtual void OnDestroySwapchain(DeviceData& /*device*/, VkSwapchainKHR /*swapchain*/) {}

  // Перед передачей vkQueuePresentKHR дальше. Фича может записать и отправить свою работу (через
  // DeviceData::vk) и подменить семафоры ожидания в edit.
  virtual void OnQueuePresent(DeviceData& /*device*/, const QueueData& /*queue*/,
                              PresentEdit& /*edit*/) {}

  // --- Ресурсы (event::kImages, kImageViews, kRenderPasses, kFramebuffers) ---------------------

  virtual void OnImageCreated(DeviceData& /*device*/, VkImage /*image*/,
                              const VkImageCreateInfo& /*info*/) {}
  virtual void OnDestroyImage(DeviceData& /*device*/, VkImage /*image*/) {}
  virtual void OnImageViewCreated(DeviceData& /*device*/, VkImageView /*view*/,
                                  const VkImageViewCreateInfo& /*info*/) {}
  virtual void OnDestroyImageView(DeviceData& /*device*/, VkImageView /*view*/) {}
  // vkCreateRenderPass.
  virtual void OnRenderPassCreated(DeviceData& /*device*/, VkRenderPass /*renderPass*/,
                                   const VkRenderPassCreateInfo& /*info*/) {}
  // vkCreateRenderPass2 / vkCreateRenderPass2KHR.
  virtual void OnRenderPass2Created(DeviceData& /*device*/, VkRenderPass /*renderPass*/,
                                    const VkRenderPassCreateInfo2& /*info*/) {}
  virtual void OnDestroyRenderPass(DeviceData& /*device*/, VkRenderPass /*renderPass*/) {}
  virtual void OnFramebufferCreated(DeviceData& /*device*/, VkFramebuffer /*framebuffer*/,
                                    const VkFramebufferCreateInfo& /*info*/) {}
  virtual void OnDestroyFramebuffer(DeviceData& /*device*/, VkFramebuffer /*framebuffer*/) {}

  // --- Командные буферы и пулы (event::kCommandBuffers) --------------------------------------
  // vkResetCommandPool / vkDestroyCommandPool неявно сбрасывают или освобождают все буферы пула:
  // чтобы почистить свои записи о них, фича запоминает пул из OnAllocateCommandBuffers.

  virtual void OnAllocateCommandBuffers(DeviceData& /*device*/,
                                        const VkCommandBufferAllocateInfo& /*info*/,
                                        const VkCommandBuffer* /*commandBuffers*/) {}
  virtual void OnBeginCommandBuffer(DeviceData& /*device*/, VkCommandBuffer /*cb*/,
                                    const VkCommandBufferBeginInfo& /*info*/) {}
  virtual void OnResetCommandBuffer(DeviceData& /*device*/, VkCommandBuffer /*cb*/) {}
  virtual void OnFreeCommandBuffers(DeviceData& /*device*/, VkCommandPool /*pool*/,
                                    uint32_t /*count*/, const VkCommandBuffer* /*commandBuffers*/) {}
  virtual void OnResetCommandPool(DeviceData& /*device*/, VkCommandPool /*pool*/) {}
  virtual void OnDestroyCommandPool(DeviceData& /*device*/, VkCommandPool /*pool*/) {}

  // --- Запись команд (event::kViewport, kRenderPassCmds, kExecuteCommands, kBarriers, kWaitEvents)

  virtual void OnCmdSetViewport(DeviceData& /*device*/, VkCommandBuffer /*cb*/,
                                uint32_t /*firstViewport*/, uint32_t /*count*/,
                                const VkViewport* /*viewports*/) {}
  // vkCmdBeginRenderPass / 2 / 2KHR сведены в одно событие; entryPoint — имя перехваченной функции.
  virtual void OnCmdBeginRenderPass(DeviceData& /*device*/, VkCommandBuffer /*cb*/,
                                    const char* /*entryPoint*/,
                                    const VkRenderPassBeginInfo& /*info*/,
                                    VkSubpassContents /*contents*/) {}
  virtual void OnCmdNextSubpass(DeviceData& /*device*/, VkCommandBuffer /*cb*/,
                                const char* /*entryPoint*/, VkSubpassContents /*contents*/) {}
  virtual void OnCmdEndRenderPass(DeviceData& /*device*/, VkCommandBuffer /*cb*/,
                                  const char* /*entryPoint*/) {}
  // vkCmdBeginRendering / KHR (dynamic rendering).
  virtual void OnCmdBeginRendering(DeviceData& /*device*/, VkCommandBuffer /*cb*/,
                                   const char* /*entryPoint*/, const VkRenderingInfo& /*info*/) {}
  virtual void OnCmdEndRendering(DeviceData& /*device*/, VkCommandBuffer /*cb*/,
                                 const char* /*entryPoint*/) {}
  virtual void OnCmdExecuteCommands(DeviceData& /*device*/, VkCommandBuffer /*cb*/,
                                    uint32_t /*count*/, const VkCommandBuffer* /*secondaries*/) {}

  // Запись барьеров в командный буфер. Фичи вызываются по порядку из features.cpp, и каждая видит
  // правки предыдущих (сначала indirect-barrier-fix, потом наблюдатели).
  // entryPoint — имя перехваченной функции ("vkCmdPipelineBarrier2KHR" и т.п.).
  virtual void OnCmdPipelineBarrier(DeviceData& /*device*/, VkCommandBuffer /*cb*/,
                                    VkDependencyFlags /*dependencyFlags*/, BarrierSet& /*barriers*/) {}
  virtual void OnCmdWaitEvents(DeviceData& /*device*/, VkCommandBuffer /*cb*/, uint32_t /*eventCount*/,
                               const VkEvent* /*pEvents*/, BarrierSet& /*barriers*/) {}
  virtual void OnCmdPipelineBarrier2(DeviceData& /*device*/, VkCommandBuffer /*cb*/,
                                     const char* /*entryPoint*/,
                                     const VkDependencyInfo*& /*dependencyInfo*/) {}
  virtual void OnCmdWaitEvents2(DeviceData& /*device*/, VkCommandBuffer /*cb*/,
                                const char* /*entryPoint*/, uint32_t /*eventCount*/,
                                const VkEvent* /*pEvents*/,
                                const VkDependencyInfo*& /*pDependencyInfos*/) {}

  // --- Отправка (event::kSubmit) ------------------------------------------------------------
  // После вызова следующего звена; result — его результат (при ошибке работа не отправлена).

  virtual void OnQueueSubmitted(DeviceData& /*device*/, const QueueData& /*queue*/,
                                uint32_t /*submitCount*/, const VkSubmitInfo* /*submits*/,
                                VkResult /*result*/) {}
  // vkQueueSubmit2 / vkQueueSubmit2KHR.
  virtual void OnQueueSubmitted2(DeviceData& /*device*/, const QueueData& /*queue*/,
                                 const char* /*entryPoint*/, uint32_t /*submitCount*/,
                                 const VkSubmitInfo2* /*submits*/, VkResult /*result*/) {}

 private:
  size_t index_ = 0;
};

// Создаёт все включённые фичи в порядке, в котором они получают события.
std::vector<std::unique_ptr<Feature>> CreateFeatures(const Config& config);

}  // namespace doomfix
