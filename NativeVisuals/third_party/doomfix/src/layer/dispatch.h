#pragma once

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include <iterator>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace doomfix {

// Функции следующего звена цепочки, которые вызывает слой. Чтобы добавить функцию, допишите
// X(Имя): поле в таблице и его загрузка появятся автоматически. Функции расширений (surface)
// могут отсутствовать: тогда указатель остаётся nullptr.
#define DOOMFIX_INSTANCE_FUNCS(X)           \
  X(DestroyInstance)                        \
  X(EnumerateDeviceExtensionProperties)     \
  X(GetPhysicalDeviceProperties)            \
  X(GetPhysicalDeviceMemoryProperties)      \
  X(GetPhysicalDeviceQueueFamilyProperties) \
  X(GetPhysicalDeviceSurfaceCapabilitiesKHR)

// Функции Vulkan 1.0 устройства: есть у любого устройства.
#define DOOMFIX_DEVICE_FUNCS_CORE(X)                                                            \
  /* устройство и очереди */                                                                    \
  X(DestroyDevice) X(GetDeviceQueue) X(DeviceWaitIdle) X(QueueSubmit) X(QueueWaitIdle)          \
  /* ресурсы */                                                                                 \
  X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(BindImageMemory)               \
  X(CreateImageView) X(DestroyImageView) X(CreateBuffer) X(DestroyBuffer)                       \
  X(GetBufferMemoryRequirements) X(BindBufferMemory) X(AllocateMemory) X(FreeMemory)            \
  X(MapMemory) X(UnmapMemory) X(CreateSampler) X(DestroySampler)                                \
  /* проходы и пайплайны */                                                                     \
  X(CreateRenderPass) X(DestroyRenderPass) X(CreateFramebuffer) X(DestroyFramebuffer)           \
  X(CreateShaderModule) X(DestroyShaderModule) X(CreatePipelineLayout)                          \
  X(DestroyPipelineLayout) X(CreateGraphicsPipelines) X(DestroyPipeline)                        \
  X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreateDescriptorPool)            \
  X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets)                    \
  /* синхронизация */                                                                           \
  X(CreateSemaphore) X(DestroySemaphore) X(CreateFence) X(DestroyFence) X(WaitForFences)        \
  X(ResetFences)                                                                                \
  /* командные буферы */                                                                        \
  X(CreateCommandPool) X(DestroyCommandPool) X(ResetCommandPool) X(AllocateCommandBuffers)      \
  X(FreeCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) X(ResetCommandBuffer)         \
  /* запись команд */                                                                           \
  X(CmdPipelineBarrier) X(CmdWaitEvents) X(CmdSetViewport) X(CmdSetScissor)                     \
  X(CmdBeginRenderPass) X(CmdNextSubpass) X(CmdEndRenderPass) X(CmdExecuteCommands)             \
  X(CmdBindPipeline) X(CmdBindVertexBuffers) X(CmdBindDescriptorSets) X(CmdPushConstants)       \
  X(CmdDraw) X(CmdCopyImageToBuffer)

// Функции новых версий API (1.1–1.3) и расширений: могут отсутствовать у устройства, тогда
// указатель остаётся nullptr, а хук для них не отдаётся (см. DOOMFIX_OPTIONAL_HOOK в layer.cpp).
#define DOOMFIX_DEVICE_FUNCS_OPTIONAL(X)                                                        \
  X(GetDeviceQueue2)                                                                            \
  X(CreateSwapchainKHR) X(DestroySwapchainKHR) X(GetSwapchainImagesKHR) X(QueuePresentKHR)      \
  X(CreateRenderPass2) X(CreateRenderPass2KHR)                                                  \
  X(CmdBeginRenderPass2) X(CmdBeginRenderPass2KHR) X(CmdNextSubpass2) X(CmdNextSubpass2KHR)     \
  X(CmdEndRenderPass2) X(CmdEndRenderPass2KHR)                                                  \
  X(CmdBeginRendering) X(CmdBeginRenderingKHR) X(CmdEndRendering) X(CmdEndRenderingKHR)         \
  X(CmdPipelineBarrier2) X(CmdPipelineBarrier2KHR) X(CmdWaitEvents2) X(CmdWaitEvents2KHR)       \
  X(QueueSubmit2) X(QueueSubmit2KHR)

#define DOOMFIX_DEVICE_FUNCS(X) \
  DOOMFIX_DEVICE_FUNCS_CORE(X)  \
  DOOMFIX_DEVICE_FUNCS_OPTIONAL(X)

struct InstanceDispatch {
  PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
#define DOOMFIX_DECLARE(name) PFN_vk##name name = nullptr;
  DOOMFIX_INSTANCE_FUNCS(DOOMFIX_DECLARE)
#undef DOOMFIX_DECLARE

  void Load(VkInstance instance, PFN_vkGetInstanceProcAddr gipa) {
    GetInstanceProcAddr = gipa;
#define DOOMFIX_LOAD(name) name = reinterpret_cast<PFN_vk##name>(gipa(instance, "vk" #name));
    DOOMFIX_INSTANCE_FUNCS(DOOMFIX_LOAD)
#undef DOOMFIX_LOAD
  }
};

struct DeviceDispatch {
  PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
#define DOOMFIX_DECLARE(name) PFN_vk##name name = nullptr;
  DOOMFIX_DEVICE_FUNCS(DOOMFIX_DECLARE)
#undef DOOMFIX_DECLARE

  void Load(VkDevice device, PFN_vkGetDeviceProcAddr gdpa) {
    GetDeviceProcAddr = gdpa;
#define DOOMFIX_LOAD(name) name = reinterpret_cast<PFN_vk##name>(gdpa(device, "vk" #name));
    DOOMFIX_DEVICE_FUNCS(DOOMFIX_LOAD)
#undef DOOMFIX_LOAD
  }
};

struct InstanceData {
  VkInstance handle = VK_NULL_HANDLE;
  InstanceDispatch vk;

  std::mutex mutex;  // защищает поля ниже
  std::set<VkPhysicalDevice> extensionListLogged;
};

// Данные фичи, привязанные к устройству (DeviceData::featureStates).
struct FeatureState {
  virtual ~FeatureState() = default;
};

struct DeviceData {
  VkQueue sharedGraphicsQueue = VK_NULL_HANDLE;
  uint32_t sharedGraphicsFamily = VK_QUEUE_FAMILY_IGNORED;
  VkDevice handle = VK_NULL_HANDLE;
  VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
  InstanceData* instance = nullptr;
  DeviceDispatch vk;
  std::string deviceName;
  std::vector<std::string> enabledExtensions;  // то, что реально ушло вниз по цепочке

  // Колбэк loader'а (VkLayerDeviceCreateInfo с VK_LOADER_DATA_CALLBACK). Его нужно вызвать для
  // каждого диспетчеризуемого объекта, который слой создаёт сам (например, свой VkCommandBuffer),
  // иначе в нём нет указателя на таблицу loader'а. nullptr, если loader его не дал.
  PFN_vkSetDeviceLoaderData setDeviceLoaderData = nullptr;
  uint32_t apiVersion = 0;  // VkPhysicalDeviceProperties::apiVersion
  VkPhysicalDeviceMemoryProperties memoryProperties = {};
  std::vector<VkQueueFamilyProperties> queueFamilies;

  // Состояние фич на этом устройстве, по номеру фичи (Feature::Index()). Фича заполняет свой слот в
  // OnDeviceCreated, пока устройство ещё не видно приложению; дальше слот только читается, поэтому
  // событиям записи команд не нужна блокировка, чтобы найти своё состояние. Уничтожается вместе с
  // DeviceData — после OnDestroyDevice и vkDestroyDevice.
  std::vector<std::unique_ptr<FeatureState>> featureStates;
};

// Очередь, полученная приложением через vkGetDeviceQueue/vkGetDeviceQueue2.
struct QueueData {
  VkQueue handle = VK_NULL_HANDLE;
  DeviceData* device = nullptr;
  uint32_t family = VK_QUEUE_FAMILY_IGNORED;  // VK_QUEUE_FAMILY_IGNORED — очередь неизвестна
  uint32_t index = 0;
};

// Потокобезопасная таблица "ключ диспетчеризации -> данные".
template <typename T>
class HandleMap {
 public:
  T* Insert(void* key, std::unique_ptr<T> value) {
    std::unique_lock lock(mutex_);
    auto& slot = map_[key];
    slot = std::move(value);
    return slot.get();
  }

  T* Find(void* key) const {
    std::shared_lock lock(mutex_);
    auto it = map_.find(key);
    return it == map_.end() ? nullptr : it->second.get();
  }

  std::unique_ptr<T> Remove(void* key) {
    std::unique_lock lock(mutex_);
    auto it = map_.find(key);
    if (it == map_.end()) return nullptr;
    std::unique_ptr<T> value = std::move(it->second);
    map_.erase(it);
    return value;
  }

 private:
  mutable std::shared_mutex mutex_;
  std::unordered_map<void*, std::unique_ptr<T>> map_;
};

// Таблица VkQueue -> QueueData. Ключ — сам хэндл очереди (ключ диспетчеризации у очереди общий с
// устройством). Чтение (submit, present) — под разделяемой блокировкой.
class QueueMap {
 public:
  void Insert(const QueueData& queue) {
    std::unique_lock lock(mutex_);
    map_[queue.handle] = queue;
  }

  // Копия записи; family == VK_QUEUE_FAMILY_IGNORED, если очередь не зарегистрирована.
  QueueData Find(VkQueue queue) const {
    std::shared_lock lock(mutex_);
    auto it = map_.find(queue);
    return it == map_.end() ? QueueData{queue} : it->second;
  }

  void RemoveDevice(const DeviceData* device) {
    std::unique_lock lock(mutex_);
    for (auto it = map_.begin(); it != map_.end();) {
      it = it->second.device == device ? map_.erase(it) : std::next(it);
    }
  }

 private:
  mutable std::shared_mutex mutex_;
  std::unordered_map<VkQueue, QueueData> map_;
};

}  // namespace doomfix
