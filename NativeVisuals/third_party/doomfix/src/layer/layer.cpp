// Ядро слоя: согласование интерфейса с loader'ом, vkGet*ProcAddr, построение цепочки вызовов,
// учёт VkInstance/VkDevice и раздача событий фичам (src/features).

#include <vulkan/vk_layer.h>

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "common/config.h"
#include "common/log.h"
#include "common/util.h"
#include "features/feature.h"
#include "layer/chain_diag.h"
#include "layer/device_create_edit.h"
#include "layer/dispatch.h"
#include "layer/present_edit.h"
#include "layer/vk_util.h"
#include "runtime.h"

extern "C" {
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL DoomFix_GetInstanceProcAddr(VkInstance instance,
                                                                     const char* pName);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL DoomFix_GetDeviceProcAddr(VkDevice device,
                                                                   const char* pName);
}

namespace doomfix {
namespace {

std::once_flag g_initOnce;
std::vector<std::unique_ptr<Feature>> g_features;
HandleMap<InstanceData> g_instances;
HandleMap<DeviceData> g_devices;
QueueMap g_queues;
// Объединение Feature::Subscriptions() всех фич. Пишется один раз внутри g_initOnce; все, кто его
// читает, сначала вызывают EnsureInitialized(), поэтому отдельная синхронизация не нужна.
EventMask g_interest = 0;

std::string DescribeEvents(EventMask mask) {
  static const struct {
    EventMask bit;
    const char* name;
  } kNames[] = {
      {event::kBarriers, "barriers"},         {event::kWaitEvents, "wait-events"},
      {event::kSwapchain, "swapchain"},       {event::kPresent, "present"},
      {event::kImages, "images"},             {event::kImageViews, "image-views"},
      {event::kRenderPasses, "render-passes"}, {event::kFramebuffers, "framebuffers"},
      {event::kCommandBuffers, "command-buffers"}, {event::kViewport, "viewport"},
      {event::kRenderPassCmds, "render-pass-commands"},
      {event::kExecuteCommands, "execute-commands"}, {event::kSubmit, "submit"},
  };
  std::string out;
  for (const auto& n : kNames) {
    if (!(mask & n.bit)) continue;
    if (!out.empty()) out += ", ";
    out += n.name;
  }
  return out.empty() ? "(none)" : out;
}

void EnsureInitialized() {
  std::call_once(g_initOnce, [] {
    Config config = GetConfig();
    if(desru::Controlled()) { config.enabled=true; config.collisionViewer=true; config.indirectFixMode=IndirectFixMode::On; }
    // Без DOOMFIX_ENABLE=1 (например, если слой включили принудительно) работаем прозрачно:
    // фич нет, маска пустая, отдаются только перехватчики, нужные самому ядру.
    if (!config.enabled) return;
    log::Open(config.logDir, config.logLevel);
    DF_LOG_INFO("doomfix %s (%s) loaded into %s, pid %lu", DOOMFIX_VERSION, DOOMFIX_LAYER_NAME,
                ToUtf8(ExecutablePath()).c_str(), GetCurrentProcessId());
    g_features = CreateFeatures(config);
    for (size_t i = 0; i < g_features.size(); ++i) g_features[i]->SetIndex(i);
    for (const auto& feature : g_features) {
      DF_LOG_INFO("feature: %s (events: %s)", feature->Name(),
                  DescribeEvents(feature->Subscriptions()).c_str());
      g_interest |= feature->Subscriptions();
    }
    DF_LOG_INFO("intercepted event groups: %s", DescribeEvents(g_interest).c_str());
  });
}

// Вызывает fn(feature) для каждой фичи по порядку из features.cpp.
template <typename Fn>
void ForEachFeature(Fn&& fn) {
  for (const auto& feature : g_features) fn(*feature);
}

bool IsDeviceExtensionHidden(std::string_view name) {
  return std::any_of(g_features.begin(), g_features.end(),
                     [&](const auto& f) { return f->HidesDeviceExtension(name); });
}

// --- Логирование ----------------------------------------------------------------------------

void LogNameList(const char* title, uint32_t count, const char* const* names,
                 const DeviceCreateEdit* edit = nullptr) {
  DF_LOG_INFO("  %s (%u):", title, count);
  for (uint32_t i = 0; i < count; ++i) {
    const bool removed = edit && !edit->HasExtension(names[i]);
    DF_LOG_INFO("    %s%s", names[i], removed ? "   <-- REMOVED" : "");
  }
}

void LogPNextChain(const char* title, const void* pNext) {
  std::string chain;
  for (auto* s = static_cast<const VkBaseInStructure*>(pNext); s; s = s->pNext) {
    if (s->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) continue;  // служебная loader'а
    if (!chain.empty()) chain += ", ";
    chain += DescribeStructureType(s->sType);
  }
  DF_LOG_INFO("  %s: %s", title, chain.empty() ? "(empty)" : chain.c_str());
}

// "family 0 x1 (graphics|compute|transfer, of 16), family 2 x1 (compute|transfer, of 8)".
void LogQueues(const char* title, uint32_t count, const VkDeviceQueueCreateInfo* queues,
               const std::vector<VkQueueFamilyProperties>& families) {
  std::string text;
  for (uint32_t i = 0; i < count; ++i) {
    const VkDeviceQueueCreateInfo& q = queues[i];
    std::string caps;
    uint32_t available = 0;
    if (q.queueFamilyIndex < families.size()) {
      const VkQueueFlags flags = families[q.queueFamilyIndex].queueFlags;
      if (flags & VK_QUEUE_GRAPHICS_BIT) caps += "graphics|";
      if (flags & VK_QUEUE_COMPUTE_BIT) caps += "compute|";
      if (flags & VK_QUEUE_TRANSFER_BIT) caps += "transfer|";
      if (!caps.empty()) caps.pop_back();
      available = families[q.queueFamilyIndex].queueCount;
    }
    if (!text.empty()) text += ", ";
    text += "family " + std::to_string(q.queueFamilyIndex) + " x" + std::to_string(q.queueCount) + " (" +
            caps + ", of " + std::to_string(available) + (q.flags ? ", protected" : "") + ")";
  }
  DF_LOG_INFO("  %s: %s", title, text.empty() ? "(none)" : text.c_str());
}

// --- Цепочка слоёв ------------------------------------------------------------------------

template <typename CreateInfo, typename LinkInfo>
LinkInfo* FindLinkInfo(const CreateInfo* createInfo, VkStructureType type) {
  for (auto* s = static_cast<const VkBaseInStructure*>(createInfo->pNext); s; s = s->pNext) {
    if (s->sType != type) continue;
    auto* info = reinterpret_cast<LinkInfo*>(const_cast<VkBaseInStructure*>(s));
    if (info->function == VK_LAYER_LINK_INFO) return info;
  }
  return nullptr;
}

// --- Instance -----------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                              const VkAllocationCallbacks* pAllocator,
                                              VkInstance* pInstance) {
  EnsureInitialized();

  auto* chain = FindLinkInfo<VkInstanceCreateInfo, VkLayerInstanceCreateInfo>(
      pCreateInfo, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO);
  if(desru::Controlled()&&!chain) {
    auto module=GetModuleHandleW(L"vulkan-1.dll");
    auto create=reinterpret_cast<PFN_vkCreateInstance>(GetProcAddress(module,"vkCreateInstance"));
    VkResult result=create(pCreateInfo,pAllocator,pInstance);
    if(result==VK_SUCCESS) {
      auto data=std::make_unique<InstanceData>();data->handle=*pInstance;
      data->vk.Load(*pInstance,reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(module,"vkGetInstanceProcAddr")));
      g_instances.Insert(DispatchKey(*pInstance),std::move(data));
    }
    return result;
  }
  if (!chain || !chain->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;

  const PFN_vkGetInstanceProcAddr nextGipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  auto nextCreate =
      reinterpret_cast<PFN_vkCreateInstance>(nextGipa(VK_NULL_HANDLE, "vkCreateInstance"));
  if (!nextCreate) return VK_ERROR_INITIALIZATION_FAILED;

  if (const VkApplicationInfo* app = pCreateInfo->pApplicationInfo) {
    DF_LOG_INFO("vkCreateInstance: app=\"%s\" v%u, engine=\"%s\" v%u, apiVersion=%s",
                app->pApplicationName ? app->pApplicationName : "", app->applicationVersion,
                app->pEngineName ? app->pEngineName : "", app->engineVersion,
                ApiVersionString(app->apiVersion).c_str());
  } else {
    DF_LOG_INFO("vkCreateInstance: no VkApplicationInfo");
  }
  LogNameList("instance extensions requested by app", pCreateInfo->enabledExtensionCount,
              pCreateInfo->ppEnabledExtensionNames);
  LogNameList("instance layers requested by app", pCreateInfo->enabledLayerCount,
              pCreateInfo->ppEnabledLayerNames);

  std::vector<const void*> below;
  for (const VkLayerInstanceLink* link = chain->u.pLayerInfo; link; link = link->pNext) {
    below.push_back(reinterpret_cast<const void*>(link->pfnNextGetInstanceProcAddr));
  }
  LogLayersBelow("vkCreateInstance", below);

  chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
  const VkResult result = nextCreate(pCreateInfo, pAllocator, pInstance);
  DF_LOG_INFO("vkCreateInstance -> %d", static_cast<int>(result));
  if (result != VK_SUCCESS) return result;

  auto data = std::make_unique<InstanceData>();
  data->handle = *pInstance;
  data->vk.Load(*pInstance, nextGipa);
  g_instances.Insert(DispatchKey(*pInstance), std::move(data));
  return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance,
                                           const VkAllocationCallbacks* pAllocator) {
  if (!instance) return;
  std::unique_ptr<InstanceData> data = g_instances.Remove(DispatchKey(instance));
  if (!data) return;
  DF_LOG_INFO("vkDestroyInstance");
  ForEachFeature([&](Feature& f) { f.OnDestroyInstance(*data); });
  data->vk.DestroyInstance(instance, pAllocator);
}

std::string PhysicalDeviceName(InstanceData& instance, VkPhysicalDevice physicalDevice) {
  VkPhysicalDeviceProperties props = {};
  instance.vk.GetPhysicalDeviceProperties(physicalDevice, &props);
  return props.deviceName;
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceExtensionProperties(
    VkPhysicalDevice physicalDevice, const char* pLayerName, uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties) {
  // Собственных расширений у слоя нет.
  if (pLayerName && std::strcmp(pLayerName, DOOMFIX_LAYER_NAME) == 0) {
    *pPropertyCount = 0;
    return VK_SUCCESS;
  }

  InstanceData* instance = g_instances.Find(DispatchKey(physicalDevice));
  if (!instance) return VK_ERROR_INITIALIZATION_FAILED;
  const auto next = instance->vk.EnumerateDeviceExtensionProperties;
  if (pLayerName) return next(physicalDevice, pLayerName, pPropertyCount, pProperties);

  // Полный список от нижних звеньев (двухфазный запрос, повтор при VK_INCOMPLETE).
  std::vector<VkExtensionProperties> all;
  VkResult result;
  do {
    uint32_t count = 0;
    result = next(physicalDevice, nullptr, &count, nullptr);
    if (result != VK_SUCCESS) return result;
    all.resize(count);
    result = next(physicalDevice, nullptr, &count, all.data());
    all.resize(count);
  } while (result == VK_INCOMPLETE);
  if (result != VK_SUCCESS) return result;

  std::vector<VkExtensionProperties> visible;
  std::string hidden;
  visible.reserve(all.size());
  for (const VkExtensionProperties& ext : all) {
    if (IsDeviceExtensionHidden(ext.extensionName)) {
      if (!hidden.empty()) hidden += ", ";
      hidden += ext.extensionName;
    } else {
      visible.push_back(ext);
    }
  }

  bool firstTime;
  {
    std::lock_guard lock(instance->mutex);
    firstTime = instance->extensionListLogged.insert(physicalDevice).second;
  }
  if (firstTime) {
    DF_LOG_INFO("vkEnumerateDeviceExtensionProperties [%s]: %zu reported below, %zu visible "
                "to app, hidden: %s",
                PhysicalDeviceName(*instance, physicalDevice).c_str(), all.size(), visible.size(),
                hidden.empty() ? "(none)" : hidden.c_str());
  }

  if (!pProperties) {
    *pPropertyCount = static_cast<uint32_t>(visible.size());
    return VK_SUCCESS;
  }
  const uint32_t copied = std::min(*pPropertyCount, static_cast<uint32_t>(visible.size()));
  std::copy_n(visible.begin(), copied, pProperties);
  *pPropertyCount = copied;
  return copied < visible.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

// --- Device -------------------------------------------------------------------------------

PFN_vkSetDeviceLoaderData FindLoaderDataCallback(const VkDeviceCreateInfo* createInfo) {
  for (auto* s = static_cast<const VkBaseInStructure*>(createInfo->pNext); s; s = s->pNext) {
    if (s->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) continue;
    auto* info = reinterpret_cast<const VkLayerDeviceCreateInfo*>(s);
    if (info->function == VK_LOADER_DATA_CALLBACK) return info->u.pfnSetDeviceLoaderData;
  }
  return nullptr;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physicalDevice,
                                            const VkDeviceCreateInfo* pCreateInfo,
                                            const VkAllocationCallbacks* pAllocator,
                                            VkDevice* pDevice) {
  InstanceData* instance = g_instances.Find(DispatchKey(physicalDevice));
  auto* chain = FindLinkInfo<VkDeviceCreateInfo, VkLayerDeviceCreateInfo>(
      pCreateInfo, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO);
  if(desru::Controlled() && !chain) return desru::NativeCreateDevice(physicalDevice,pCreateInfo,pAllocator,pDevice);
  if (!instance || !chain || !chain->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;

  const PFN_vkGetInstanceProcAddr nextGipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  const PFN_vkGetDeviceProcAddr nextGdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
  auto nextCreate =
      reinterpret_cast<PFN_vkCreateDevice>(nextGipa(instance->handle, "vkCreateDevice"));
  const PFN_vkSetDeviceLoaderData setDeviceLoaderData = FindLoaderDataCallback(pCreateInfo);
  if (!nextCreate) return VK_ERROR_INITIALIZATION_FAILED;

  const std::string deviceName = PhysicalDeviceName(*instance, physicalDevice);
  DF_LOG_INFO("vkCreateDevice [%s]", deviceName.c_str());

  std::vector<const void*> below;
  for (const VkLayerDeviceLink* link = chain->u.pLayerInfo; link; link = link->pNext) {
    below.push_back(reinterpret_cast<const void*>(link->pfnNextGetDeviceProcAddr));
  }
  LogLayersBelow("vkCreateDevice", below);
  LogPNextChain("pNext chain from app", pCreateInfo->pNext);

  VkResult result;
  std::vector<std::string> enabledExtensions;
  {
    // edit восстанавливает правки чужих структур в деструкторе — строго после вызова вниз.
    DeviceCreateEdit edit(*pCreateInfo);
    uint32_t familyCount = 0;
    instance->vk.GetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    instance->vk.GetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, families.data());
    families.resize(familyCount);
    edit.SetQueueFamilies(std::move(families));
    LogQueues("queues requested by app", pCreateInfo->queueCreateInfoCount, pCreateInfo->pQueueCreateInfos,
              edit.QueueFamilies());
    for (const auto& feature : g_features) feature->OnCreateDevice(physicalDevice, edit);

    LogNameList("device extensions requested by app", pCreateInfo->enabledExtensionCount,
                pCreateInfo->ppEnabledExtensionNames, &edit);
    const VkDeviceCreateInfo* finalInfo = edit.Result();
    LogPNextChain("pNext chain passed down", finalInfo->pNext);
    LogQueues("queues passed down", finalInfo->queueCreateInfoCount, finalInfo->pQueueCreateInfos,
              edit.QueueFamilies());
    DF_LOG_INFO("  passing %u extensions down the chain", finalInfo->enabledExtensionCount);

    for (const char* ext : edit.Extensions()) enabledExtensions.emplace_back(ext);

    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    result = nextCreate(physicalDevice, finalInfo, pAllocator, pDevice);
  }
  DF_LOG_INFO("vkCreateDevice -> %d", static_cast<int>(result));
  if (result != VK_SUCCESS) return result;

  auto data = std::make_unique<DeviceData>();
  data->handle = *pDevice;
  data->physicalDevice = physicalDevice;
  data->instance = instance;
  data->deviceName = deviceName;
  data->enabledExtensions = std::move(enabledExtensions);
  data->vk.Load(*pDevice, nextGdpa);
  data->setDeviceLoaderData = setDeviceLoaderData;
  VkPhysicalDeviceProperties props = {};
  instance->vk.GetPhysicalDeviceProperties(physicalDevice, &props);
  data->apiVersion = props.apiVersion;
  instance->vk.GetPhysicalDeviceMemoryProperties(physicalDevice, &data->memoryProperties);
  uint32_t familyCount = 0;
  instance->vk.GetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, nullptr);
  data->queueFamilies.resize(familyCount);
  instance->vk.GetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount,
                                                      data->queueFamilies.data());
  data->queueFamilies.resize(familyCount);
  data->featureStates.resize(g_features.size());
  if (!data->setDeviceLoaderData) {
    DF_LOG_WARN("vkCreateDevice: the loader gave no SetDeviceLoaderData callback");
  }
  DeviceData* device = g_devices.Insert(DispatchKey(*pDevice), std::move(data));
  for (const auto& feature : g_features) feature->OnDeviceCreated(*device);
  return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator) {
  if (!device) return;
  std::unique_ptr<DeviceData> data = g_devices.Remove(DispatchKey(device));
  if (!data) return;
  DF_LOG_INFO("vkDestroyDevice [%s]", data->deviceName.c_str());
  for (const auto& feature : g_features) feature->OnDestroyDevice(*data);
  g_queues.RemoveDevice(data.get());
  data->vk.DestroyDevice(device, pAllocator);
}

// --- Очереди (нужны самому ядру, отдаются всегда) ---------------------------------------------

void RegisterQueue(DeviceData& device, VkQueue queue, uint32_t family, uint32_t index) {
  const QueueData data{queue, &device, family, index};
  g_queues.Insert(data);
  ForEachFeature([&](Feature& f) { f.OnGetDeviceQueue(device, data); });
}

// Очередь из карты; если приложение получило её в обход перехватчика, семейство неизвестно.
QueueData FindQueue(DeviceData& device, VkQueue queue) {
  QueueData data = g_queues.Find(queue);
  if (!data.device) data.device = &device;
  return data;
}

VKAPI_ATTR void VKAPI_CALL GetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex,
                                          uint32_t queueIndex, VkQueue* pQueue) {
  DeviceData* data = g_devices.Find(DispatchKey(device));
  if (!data) return;
  data->vk.GetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);
  if (*pQueue) RegisterQueue(*data, *pQueue, queueFamilyIndex, queueIndex);
}

VKAPI_ATTR void VKAPI_CALL GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* pQueueInfo,
                                           VkQueue* pQueue) {
  DeviceData* data = g_devices.Find(DispatchKey(device));
  if (!data) return;
  data->vk.GetDeviceQueue2(device, pQueueInfo, pQueue);
  if (*pQueue) RegisterQueue(*data, *pQueue, pQueueInfo->queueFamilyIndex, pQueueInfo->queueIndex);
}

// --- Swapchain и present ---------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchainKHR(VkDevice device,
                                                  const VkSwapchainCreateInfoKHR* pCreateInfo,
                                                  const VkAllocationCallbacks* pAllocator,
                                                  VkSwapchainKHR* pSwapchain) {
  DeviceData* data = g_devices.Find(DispatchKey(device));
  if (!data) return VK_ERROR_INITIALIZATION_FAILED;
  VkSwapchainCreateInfoKHR info = *pCreateInfo;
  ForEachFeature([&](Feature& f) { f.OnCreateSwapchain(*data, info); });
  const VkResult result = data->vk.CreateSwapchainKHR(device, &info, pAllocator, pSwapchain);
  if (result == VK_SUCCESS) {
    ForEachFeature([&](Feature& f) { f.OnSwapchainCreated(*data, *pSwapchain, info); });
  }
  return result;
}

VKAPI_ATTR void VKAPI_CALL DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                               const VkAllocationCallbacks* pAllocator) {
  DeviceData* data = g_devices.Find(DispatchKey(device));
  if (!data) return;
  if (swapchain) ForEachFeature([&](Feature& f) { f.OnDestroySwapchain(*data, swapchain); });
  data->vk.DestroySwapchainKHR(device, swapchain, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
  std::lock_guard lock(desru::QueueMutex());
  if(desru::Controlled()) desru::Present();
  DeviceData* data = g_devices.Find(DispatchKey(queue));
  if (!data) return VK_ERROR_DEVICE_LOST;
  const QueueData q = FindQueue(*data, queue);
  PresentEdit edit(*pPresentInfo);
  ForEachFeature([&](Feature& f) { f.OnQueuePresent(*data, q, edit); });
  return data->vk.QueuePresentKHR(queue, edit.Result());
}

// --- Ресурсы ----------------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL CreateImage(VkDevice device, const VkImageCreateInfo* pCreateInfo,
                                           const VkAllocationCallbacks* pAllocator, VkImage* pImage) {
  DeviceData* data = g_devices.Find(DispatchKey(device));
  if (!data) return VK_ERROR_INITIALIZATION_FAILED;
  const VkResult result = data->vk.CreateImage(device, pCreateInfo, pAllocator, pImage);
  if (result == VK_SUCCESS) {
    ForEachFeature([&](Feature& f) { f.OnImageCreated(*data, *pImage, *pCreateInfo); });
  }
  return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyImage(VkDevice device, VkImage image,
                                        const VkAllocationCallbacks* pAllocator) {
  DeviceData* data = g_devices.Find(DispatchKey(device));
  if (!data) return;
  if (image) ForEachFeature([&](Feature& f) { f.OnDestroyImage(*data, image); });
  data->vk.DestroyImage(device, image, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateImageView(VkDevice device,
                                               const VkImageViewCreateInfo* pCreateInfo,
                                               const VkAllocationCallbacks* pAllocator,
                                               VkImageView* pView) {
  DeviceData* data = g_devices.Find(DispatchKey(device));
  if (!data) return VK_ERROR_INITIALIZATION_FAILED;
  const VkResult result = data->vk.CreateImageView(device, pCreateInfo, pAllocator, pView);
  if (result == VK_SUCCESS) {
    ForEachFeature([&](Feature& f) { f.OnImageViewCreated(*data, *pView, *pCreateInfo); });
  }
  return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyImageView(VkDevice device, VkImageView imageView,
                                            const VkAllocationCallbacks* pAllocator) {
  DeviceData* data = g_devices.Find(DispatchKey(device));
  if (!data) return;
  if (imageView) ForEachFeature([&](Feature& f) { f.OnDestroyImageView(*data, imageView); });
  data->vk.DestroyImageView(device, imageView, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass(VkDevice device,
                                                const VkRenderPassCreateInfo* pCreateInfo,
                                                const VkAllocationCallbacks* pAllocator,
                                                VkRenderPass* pRenderPass) {
  DeviceData* data = g_devices.Find(DispatchKey(device));
  if (!data) return VK_ERROR_INITIALIZATION_FAILED;
  const VkResult result = data->vk.CreateRenderPass(device, pCreateInfo, pAllocator, pRenderPass);
  if (result == VK_SUCCESS) {
    ForEachFeature([&](Feature& f) { f.OnRenderPassCreated(*data, *pRenderPass, *pCreateInfo); });
  }
  return result;
}

template <PFN_vkCreateRenderPass2 DeviceDispatch::*Next>
VkResult CreateRenderPass2T(VkDevice device, const VkRenderPassCreateInfo2* pCreateInfo,
                            const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass) {
  DeviceData* data = g_devices.Find(DispatchKey(device));
  if (!data) return VK_ERROR_INITIALIZATION_FAILED;
  const VkResult result = (data->vk.*Next)(device, pCreateInfo, pAllocator, pRenderPass);
  if (result == VK_SUCCESS) {
    ForEachFeature([&](Feature& f) { f.OnRenderPass2Created(*data, *pRenderPass, *pCreateInfo); });
  }
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass2(VkDevice device,
                                                 const VkRenderPassCreateInfo2* pCreateInfo,
                                                 const VkAllocationCallbacks* pAllocator,
                                                 VkRenderPass* pRenderPass) {
  return CreateRenderPass2T<&DeviceDispatch::CreateRenderPass2>(device, pCreateInfo, pAllocator,
                                                                pRenderPass);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass2KHR(VkDevice device,
                                                    const VkRenderPassCreateInfo2* pCreateInfo,
                                                    const VkAllocationCallbacks* pAllocator,
                                                    VkRenderPass* pRenderPass) {
  return CreateRenderPass2T<&DeviceDispatch::CreateRenderPass2KHR>(device, pCreateInfo, pAllocator,
                                                                   pRenderPass);
}

VKAPI_ATTR void VKAPI_CALL DestroyRenderPass(VkDevice device, VkRenderPass renderPass,
                                             const VkAllocationCallbacks* pAllocator) {
  DeviceData* data = g_devices.Find(DispatchKey(device));
  if (!data) return;
  if (renderPass) ForEachFeature([&](Feature& f) { f.OnDestroyRenderPass(*data, renderPass); });
  data->vk.DestroyRenderPass(device, renderPass, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateFramebuffer(VkDevice device,
                                                 const VkFramebufferCreateInfo* pCreateInfo,
                                                 const VkAllocationCallbacks* pAllocator,
                                                 VkFramebuffer* pFramebuffer) {
  DeviceData* data = g_devices.Find(DispatchKey(device));
  if (!data) return VK_ERROR_INITIALIZATION_FAILED;
  const VkResult result = data->vk.CreateFramebuffer(device, pCreateInfo, pAllocator, pFramebuffer);
  if (result == VK_SUCCESS) {
    ForEachFeature([&](Feature& f) { f.OnFramebufferCreated(*data, *pFramebuffer, *pCreateInfo); });
  }
  return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyFramebuffer(VkDevice device, VkFramebuffer framebuffer,
                                              const VkAllocationCallbacks* pAllocator) {
  DeviceData* data = g_devices.Find(DispatchKey(device));
  if (!data) return;
  if (framebuffer) ForEachFeature([&](Feature& f) { f.OnDestroyFramebuffer(*data, framebuffer); });
  data->vk.DestroyFramebuffer(device, framebuffer, pAllocator);
}

// --- Командные буферы и пулы ------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL AllocateCommandBuffers(VkDevice device,
                                                      const VkCommandBufferAllocateInfo* pAllocateInfo,
                                                      VkCommandBuffer* pCommandBuffers) {
  DeviceData* data = g_devices.Find(DispatchKey(device));
  if (!data) return VK_ERROR_INITIALIZATION_FAILED;
  const VkResult result = data->vk.AllocateCommandBuffers(device, pAllocateInfo, pCommandBuffers);
  if (result == VK_SUCCESS) {
    ForEachFeature(
        [&](Feature& f) { f.OnAllocateCommandBuffers(*data, *pAllocateInfo, pCommandBuffers); });
  }
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL BeginCommandBuffer(VkCommandBuffer commandBuffer,
                                                  const VkCommandBufferBeginInfo* pBeginInfo) {
  DeviceData* data = g_devices.Find(DispatchKey(commandBuffer));
  if (!data) return VK_ERROR_INITIALIZATION_FAILED;
  ForEachFeature([&](Feature& f) { f.OnBeginCommandBuffer(*data, commandBuffer, *pBeginInfo); });
  return data->vk.BeginCommandBuffer(commandBuffer, pBeginInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL ResetCommandBuffer(VkCommandBuffer commandBuffer,
                                                  VkCommandBufferResetFlags flags) {
  DeviceData* data = g_devices.Find(DispatchKey(commandBuffer));
  if (!data) return VK_ERROR_INITIALIZATION_FAILED;
  ForEachFeature([&](Feature& f) { f.OnResetCommandBuffer(*data, commandBuffer); });
  return data->vk.ResetCommandBuffer(commandBuffer, flags);
}

VKAPI_ATTR void VKAPI_CALL FreeCommandBuffers(VkDevice device, VkCommandPool commandPool,
                                              uint32_t commandBufferCount,
                                              const VkCommandBuffer* pCommandBuffers) {
  DeviceData* data = g_devices.Find(DispatchKey(device));
  if (!data) return;
  ForEachFeature([&](Feature& f) {
    f.OnFreeCommandBuffers(*data, commandPool, commandBufferCount, pCommandBuffers);
  });
  data->vk.FreeCommandBuffers(device, commandPool, commandBufferCount, pCommandBuffers);
}

VKAPI_ATTR VkResult VKAPI_CALL ResetCommandPool(VkDevice device, VkCommandPool commandPool,
                                                VkCommandPoolResetFlags flags) {
  DeviceData* data = g_devices.Find(DispatchKey(device));
  if (!data) return VK_ERROR_INITIALIZATION_FAILED;
  ForEachFeature([&](Feature& f) { f.OnResetCommandPool(*data, commandPool); });
  return data->vk.ResetCommandPool(device, commandPool, flags);
}

VKAPI_ATTR void VKAPI_CALL DestroyCommandPool(VkDevice device, VkCommandPool commandPool,
                                              const VkAllocationCallbacks* pAllocator) {
  DeviceData* data = g_devices.Find(DispatchKey(device));
  if (!data) return;
  if (commandPool) ForEachFeature([&](Feature& f) { f.OnDestroyCommandPool(*data, commandPool); });
  data->vk.DestroyCommandPool(device, commandPool, pAllocator);
}

// --- Запись команд (кроме барьеров) ----------------------------------------------------------
// Вызываются при записи командных буферов (в том числе многопоточной): только поиск устройства
// под разделяемой блокировкой, без логирования и аллокаций; вся логика — в фичах.

VKAPI_ATTR void VKAPI_CALL CmdSetViewport(VkCommandBuffer commandBuffer, uint32_t firstViewport,
                                          uint32_t viewportCount, const VkViewport* pViewports) {
  DeviceData* data = g_devices.Find(DispatchKey(commandBuffer));
  if (!data) return;
  ForEachFeature([&](Feature& f) {
    f.OnCmdSetViewport(*data, commandBuffer, firstViewport, viewportCount, pViewports);
  });
  data->vk.CmdSetViewport(commandBuffer, firstViewport, viewportCount, pViewports);
}

VKAPI_ATTR void VKAPI_CALL CmdBeginRenderPass(VkCommandBuffer commandBuffer,
                                              const VkRenderPassBeginInfo* pRenderPassBegin,
                                              VkSubpassContents contents) {
  DeviceData* data = g_devices.Find(DispatchKey(commandBuffer));
  if (!data) return;
  ForEachFeature([&](Feature& f) {
    f.OnCmdBeginRenderPass(*data, commandBuffer, "vkCmdBeginRenderPass", *pRenderPassBegin,
                           contents);
  });
  data->vk.CmdBeginRenderPass(commandBuffer, pRenderPassBegin, contents);
}

template <PFN_vkCmdBeginRenderPass2 DeviceDispatch::*Next>
void CmdBeginRenderPass2T(VkCommandBuffer commandBuffer,
                          const VkRenderPassBeginInfo* pRenderPassBegin,
                          const VkSubpassBeginInfo* pSubpassBeginInfo, const char* entryPoint) {
  DeviceData* data = g_devices.Find(DispatchKey(commandBuffer));
  if (!data) return;
  ForEachFeature([&](Feature& f) {
    f.OnCmdBeginRenderPass(*data, commandBuffer, entryPoint, *pRenderPassBegin,
                           pSubpassBeginInfo->contents);
  });
  (data->vk.*Next)(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
}

VKAPI_ATTR void VKAPI_CALL CmdBeginRenderPass2(VkCommandBuffer commandBuffer,
                                               const VkRenderPassBeginInfo* pRenderPassBegin,
                                               const VkSubpassBeginInfo* pSubpassBeginInfo) {
  CmdBeginRenderPass2T<&DeviceDispatch::CmdBeginRenderPass2>(
      commandBuffer, pRenderPassBegin, pSubpassBeginInfo, "vkCmdBeginRenderPass2");
}

VKAPI_ATTR void VKAPI_CALL CmdBeginRenderPass2KHR(VkCommandBuffer commandBuffer,
                                                  const VkRenderPassBeginInfo* pRenderPassBegin,
                                                  const VkSubpassBeginInfo* pSubpassBeginInfo) {
  CmdBeginRenderPass2T<&DeviceDispatch::CmdBeginRenderPass2KHR>(
      commandBuffer, pRenderPassBegin, pSubpassBeginInfo, "vkCmdBeginRenderPass2KHR");
}

VKAPI_ATTR void VKAPI_CALL CmdNextSubpass(VkCommandBuffer commandBuffer, VkSubpassContents contents) {
  DeviceData* data = g_devices.Find(DispatchKey(commandBuffer));
  if (!data) return;
  ForEachFeature(
      [&](Feature& f) { f.OnCmdNextSubpass(*data, commandBuffer, "vkCmdNextSubpass", contents); });
  data->vk.CmdNextSubpass(commandBuffer, contents);
}

template <PFN_vkCmdNextSubpass2 DeviceDispatch::*Next>
void CmdNextSubpass2T(VkCommandBuffer commandBuffer, const VkSubpassBeginInfo* pSubpassBeginInfo,
                      const VkSubpassEndInfo* pSubpassEndInfo, const char* entryPoint) {
  DeviceData* data = g_devices.Find(DispatchKey(commandBuffer));
  if (!data) return;
  ForEachFeature([&](Feature& f) {
    f.OnCmdNextSubpass(*data, commandBuffer, entryPoint, pSubpassBeginInfo->contents);
  });
  (data->vk.*Next)(commandBuffer, pSubpassBeginInfo, pSubpassEndInfo);
}

VKAPI_ATTR void VKAPI_CALL CmdNextSubpass2(VkCommandBuffer commandBuffer,
                                           const VkSubpassBeginInfo* pSubpassBeginInfo,
                                           const VkSubpassEndInfo* pSubpassEndInfo) {
  CmdNextSubpass2T<&DeviceDispatch::CmdNextSubpass2>(commandBuffer, pSubpassBeginInfo,
                                                     pSubpassEndInfo, "vkCmdNextSubpass2");
}

VKAPI_ATTR void VKAPI_CALL CmdNextSubpass2KHR(VkCommandBuffer commandBuffer,
                                              const VkSubpassBeginInfo* pSubpassBeginInfo,
                                              const VkSubpassEndInfo* pSubpassEndInfo) {
  CmdNextSubpass2T<&DeviceDispatch::CmdNextSubpass2KHR>(commandBuffer, pSubpassBeginInfo,
                                                        pSubpassEndInfo, "vkCmdNextSubpass2KHR");
}

VKAPI_ATTR void VKAPI_CALL CmdEndRenderPass(VkCommandBuffer commandBuffer) {
  DeviceData* data = g_devices.Find(DispatchKey(commandBuffer));
  if (!data) return;
  ForEachFeature(
      [&](Feature& f) { f.OnCmdEndRenderPass(*data, commandBuffer, "vkCmdEndRenderPass"); });
  data->vk.CmdEndRenderPass(commandBuffer);
}

template <PFN_vkCmdEndRenderPass2 DeviceDispatch::*Next>
void CmdEndRenderPass2T(VkCommandBuffer commandBuffer, const VkSubpassEndInfo* pSubpassEndInfo,
                        const char* entryPoint) {
  DeviceData* data = g_devices.Find(DispatchKey(commandBuffer));
  if (!data) return;
  ForEachFeature([&](Feature& f) { f.OnCmdEndRenderPass(*data, commandBuffer, entryPoint); });
  (data->vk.*Next)(commandBuffer, pSubpassEndInfo);
}

VKAPI_ATTR void VKAPI_CALL CmdEndRenderPass2(VkCommandBuffer commandBuffer,
                                             const VkSubpassEndInfo* pSubpassEndInfo) {
  CmdEndRenderPass2T<&DeviceDispatch::CmdEndRenderPass2>(commandBuffer, pSubpassEndInfo,
                                                         "vkCmdEndRenderPass2");
}

VKAPI_ATTR void VKAPI_CALL CmdEndRenderPass2KHR(VkCommandBuffer commandBuffer,
                                                const VkSubpassEndInfo* pSubpassEndInfo) {
  CmdEndRenderPass2T<&DeviceDispatch::CmdEndRenderPass2KHR>(commandBuffer, pSubpassEndInfo,
                                                            "vkCmdEndRenderPass2KHR");
}

template <PFN_vkCmdBeginRendering DeviceDispatch::*Next>
void CmdBeginRenderingT(VkCommandBuffer commandBuffer, const VkRenderingInfo* pRenderingInfo,
                        const char* entryPoint) {
  DeviceData* data = g_devices.Find(DispatchKey(commandBuffer));
  if (!data) return;
  ForEachFeature(
      [&](Feature& f) { f.OnCmdBeginRendering(*data, commandBuffer, entryPoint, *pRenderingInfo); });
  (data->vk.*Next)(commandBuffer, pRenderingInfo);
}

VKAPI_ATTR void VKAPI_CALL CmdBeginRendering(VkCommandBuffer commandBuffer,
                                             const VkRenderingInfo* pRenderingInfo) {
  CmdBeginRenderingT<&DeviceDispatch::CmdBeginRendering>(commandBuffer, pRenderingInfo,
                                                         "vkCmdBeginRendering");
}

VKAPI_ATTR void VKAPI_CALL CmdBeginRenderingKHR(VkCommandBuffer commandBuffer,
                                                const VkRenderingInfo* pRenderingInfo) {
  CmdBeginRenderingT<&DeviceDispatch::CmdBeginRenderingKHR>(commandBuffer, pRenderingInfo,
                                                            "vkCmdBeginRenderingKHR");
}

template <PFN_vkCmdEndRendering DeviceDispatch::*Next>
void CmdEndRenderingT(VkCommandBuffer commandBuffer, const char* entryPoint) {
  DeviceData* data = g_devices.Find(DispatchKey(commandBuffer));
  if (!data) return;
  ForEachFeature([&](Feature& f) { f.OnCmdEndRendering(*data, commandBuffer, entryPoint); });
  (data->vk.*Next)(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL CmdEndRendering(VkCommandBuffer commandBuffer) {
  CmdEndRenderingT<&DeviceDispatch::CmdEndRendering>(commandBuffer, "vkCmdEndRendering");
}

VKAPI_ATTR void VKAPI_CALL CmdEndRenderingKHR(VkCommandBuffer commandBuffer) {
  CmdEndRenderingT<&DeviceDispatch::CmdEndRenderingKHR>(commandBuffer, "vkCmdEndRenderingKHR");
}

VKAPI_ATTR void VKAPI_CALL CmdExecuteCommands(VkCommandBuffer commandBuffer,
                                              uint32_t commandBufferCount,
                                              const VkCommandBuffer* pCommandBuffers) {
  DeviceData* data = g_devices.Find(DispatchKey(commandBuffer));
  if (!data) return;
  ForEachFeature([&](Feature& f) {
    f.OnCmdExecuteCommands(*data, commandBuffer, commandBufferCount, pCommandBuffers);
  });
  data->vk.CmdExecuteCommands(commandBuffer, commandBufferCount, pCommandBuffers);
}

// --- Отправка ---------------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(VkQueue queue, uint32_t submitCount,
                                           const VkSubmitInfo* pSubmits, VkFence fence) {
  std::lock_guard lock(desru::QueueMutex());
  DeviceData* data = g_devices.Find(DispatchKey(queue));
  if (!data) return VK_ERROR_DEVICE_LOST;
  const VkResult result = data->vk.QueueSubmit(queue, submitCount, pSubmits, fence);
  const QueueData q = FindQueue(*data, queue);
  ForEachFeature([&](Feature& f) { f.OnQueueSubmitted(*data, q, submitCount, pSubmits, result); });
  return result;
}

template <PFN_vkQueueSubmit2 DeviceDispatch::*Next>
VkResult QueueSubmit2T(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* pSubmits,
                       VkFence fence, const char* entryPoint) {
  std::lock_guard lock(desru::QueueMutex());
  DeviceData* data = g_devices.Find(DispatchKey(queue));
  if (!data) return VK_ERROR_DEVICE_LOST;
  const VkResult result = (data->vk.*Next)(queue, submitCount, pSubmits, fence);
  const QueueData q = FindQueue(*data, queue);
  ForEachFeature([&](Feature& f) {
    f.OnQueueSubmitted2(*data, q, entryPoint, submitCount, pSubmits, result);
  });
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit2(VkQueue queue, uint32_t submitCount,
                                            const VkSubmitInfo2* pSubmits, VkFence fence) {
  return QueueSubmit2T<&DeviceDispatch::QueueSubmit2>(queue, submitCount, pSubmits, fence,
                                                      "vkQueueSubmit2");
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit2KHR(VkQueue queue, uint32_t submitCount,
                                               const VkSubmitInfo2* pSubmits, VkFence fence) {
  return QueueSubmit2T<&DeviceDispatch::QueueSubmit2KHR>(queue, submitCount, pSubmits, fence,
                                                         "vkQueueSubmit2KHR");
}

// --- Запись барьеров ------------------------------------------------------------------------
// Вызываются при записи командных буферов (в том числе многопоточной), поэтому без логирования
// и аллокаций; вся логика — в фичах.

VKAPI_ATTR void VKAPI_CALL CmdPipelineBarrier(
    VkCommandBuffer commandBuffer, VkPipelineStageFlags srcStageMask,
    VkPipelineStageFlags dstStageMask, VkDependencyFlags dependencyFlags,
    uint32_t memoryBarrierCount, const VkMemoryBarrier* pMemoryBarriers,
    uint32_t bufferMemoryBarrierCount, const VkBufferMemoryBarrier* pBufferMemoryBarriers,
    uint32_t imageMemoryBarrierCount, const VkImageMemoryBarrier* pImageMemoryBarriers) {
  DeviceData* device = g_devices.Find(DispatchKey(commandBuffer));
  if (!device) return;
  BarrierSet b{srcStageMask,          dstStageMask,         memoryBarrierCount,
               pMemoryBarriers,       bufferMemoryBarrierCount, pBufferMemoryBarriers,
               imageMemoryBarrierCount, pImageMemoryBarriers};
  for (const auto& feature : g_features) {
    feature->OnCmdPipelineBarrier(*device, commandBuffer, dependencyFlags, b);
  }
  device->vk.CmdPipelineBarrier(commandBuffer, b.srcStageMask, b.dstStageMask, dependencyFlags,
                                b.memoryBarrierCount, b.pMemoryBarriers, b.bufferMemoryBarrierCount,
                                b.pBufferMemoryBarriers, b.imageMemoryBarrierCount,
                                b.pImageMemoryBarriers);
}

VKAPI_ATTR void VKAPI_CALL CmdWaitEvents(
    VkCommandBuffer commandBuffer, uint32_t eventCount, const VkEvent* pEvents,
    VkPipelineStageFlags srcStageMask, VkPipelineStageFlags dstStageMask,
    uint32_t memoryBarrierCount, const VkMemoryBarrier* pMemoryBarriers,
    uint32_t bufferMemoryBarrierCount, const VkBufferMemoryBarrier* pBufferMemoryBarriers,
    uint32_t imageMemoryBarrierCount, const VkImageMemoryBarrier* pImageMemoryBarriers) {
  DeviceData* device = g_devices.Find(DispatchKey(commandBuffer));
  if (!device) return;
  BarrierSet b{srcStageMask,          dstStageMask,         memoryBarrierCount,
               pMemoryBarriers,       bufferMemoryBarrierCount, pBufferMemoryBarriers,
               imageMemoryBarrierCount, pImageMemoryBarriers};
  for (const auto& feature : g_features) {
    feature->OnCmdWaitEvents(*device, commandBuffer, eventCount, pEvents, b);
  }
  device->vk.CmdWaitEvents(commandBuffer, eventCount, pEvents, b.srcStageMask, b.dstStageMask,
                           b.memoryBarrierCount, b.pMemoryBarriers, b.bufferMemoryBarrierCount,
                           b.pBufferMemoryBarriers, b.imageMemoryBarrierCount,
                           b.pImageMemoryBarriers);
}

template <PFN_vkCmdPipelineBarrier2 DeviceDispatch::*Next>
VKAPI_ATTR void VKAPI_CALL CmdPipelineBarrier2T(VkCommandBuffer commandBuffer,
                                                const VkDependencyInfo* pDependencyInfo,
                                                const char* entryPoint) {
  DeviceData* device = g_devices.Find(DispatchKey(commandBuffer));
  if (!device) return;
  for (const auto& feature : g_features) {
    feature->OnCmdPipelineBarrier2(*device, commandBuffer, entryPoint, pDependencyInfo);
  }
  (device->vk.*Next)(commandBuffer, pDependencyInfo);
}

VKAPI_ATTR void VKAPI_CALL CmdPipelineBarrier2(VkCommandBuffer commandBuffer,
                                               const VkDependencyInfo* pDependencyInfo) {
  CmdPipelineBarrier2T<&DeviceDispatch::CmdPipelineBarrier2>(commandBuffer, pDependencyInfo,
                                                             "vkCmdPipelineBarrier2");
}

VKAPI_ATTR void VKAPI_CALL CmdPipelineBarrier2KHR(VkCommandBuffer commandBuffer,
                                                  const VkDependencyInfo* pDependencyInfo) {
  CmdPipelineBarrier2T<&DeviceDispatch::CmdPipelineBarrier2KHR>(commandBuffer, pDependencyInfo,
                                                                "vkCmdPipelineBarrier2KHR");
}

template <PFN_vkCmdWaitEvents2 DeviceDispatch::*Next>
VKAPI_ATTR void VKAPI_CALL CmdWaitEvents2T(VkCommandBuffer commandBuffer, uint32_t eventCount,
                                           const VkEvent* pEvents,
                                           const VkDependencyInfo* pDependencyInfos,
                                           const char* entryPoint) {
  DeviceData* device = g_devices.Find(DispatchKey(commandBuffer));
  if (!device) return;
  for (const auto& feature : g_features) {
    feature->OnCmdWaitEvents2(*device, commandBuffer, entryPoint, eventCount, pEvents,
                              pDependencyInfos);
  }
  (device->vk.*Next)(commandBuffer, eventCount, pEvents, pDependencyInfos);
}

VKAPI_ATTR void VKAPI_CALL CmdWaitEvents2(VkCommandBuffer commandBuffer, uint32_t eventCount,
                                          const VkEvent* pEvents,
                                          const VkDependencyInfo* pDependencyInfos) {
  CmdWaitEvents2T<&DeviceDispatch::CmdWaitEvents2>(commandBuffer, eventCount, pEvents,
                                                   pDependencyInfos, "vkCmdWaitEvents2");
}

VKAPI_ATTR void VKAPI_CALL CmdWaitEvents2KHR(VkCommandBuffer commandBuffer, uint32_t eventCount,
                                             const VkEvent* pEvents,
                                             const VkDependencyInfo* pDependencyInfos) {
  CmdWaitEvents2T<&DeviceDispatch::CmdWaitEvents2KHR>(commandBuffer, eventCount, pEvents,
                                                      pDependencyInfos, "vkCmdWaitEvents2KHR");
}

// --- Таблицы перехвата ----------------------------------------------------------------------

struct NamedHook {
  const char* name;
  PFN_vkVoidFunction function;
  // Группы событий (event::k*), ради которых нужен перехват; 0 — нужен самому ядру, отдаётся всегда.
  EventMask events = 0;
  // Для функций, которых может не быть у устройства (расширения, Vulkan 1.1+): хук отдаётся,
  // только если функция есть у следующего звена. nullptr — функция есть всегда.
  bool (*available)(const DeviceDispatch&) = nullptr;
};

#define DOOMFIX_HOOK(name, events) \
  { "vk" #name, reinterpret_cast<PFN_vkVoidFunction>(&name), events }
#define DOOMFIX_OPTIONAL_HOOK(name, events)                                  \
  {                                                                          \
    "vk" #name, reinterpret_cast<PFN_vkVoidFunction>(&name), events,         \
        [](const DeviceDispatch& d) { return d.name != nullptr; }            \
  }

const NamedHook kInstanceHooks[] = {
    {"vkGetInstanceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(&DoomFix_GetInstanceProcAddr)},
    DOOMFIX_HOOK(CreateInstance, 0),
    DOOMFIX_HOOK(DestroyInstance, 0),
    DOOMFIX_HOOK(EnumerateDeviceExtensionProperties, 0),
    DOOMFIX_HOOK(CreateDevice, 0),
};

const NamedHook kDeviceHooks[] = {
    {"vkGetDeviceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(&DoomFix_GetDeviceProcAddr)},
    // Нужны самому ядру.
    DOOMFIX_HOOK(DestroyDevice, 0),
    DOOMFIX_HOOK(GetDeviceQueue, 0),
    DOOMFIX_OPTIONAL_HOOK(GetDeviceQueue2, 0),
    // Swapchain и present.
    DOOMFIX_OPTIONAL_HOOK(CreateSwapchainKHR, event::kSwapchain),
    DOOMFIX_OPTIONAL_HOOK(DestroySwapchainKHR, event::kSwapchain),
    DOOMFIX_OPTIONAL_HOOK(QueuePresentKHR, event::kPresent),
    // Ресурсы.
    DOOMFIX_HOOK(CreateImage, event::kImages),
    DOOMFIX_HOOK(DestroyImage, event::kImages),
    DOOMFIX_HOOK(CreateImageView, event::kImageViews),
    DOOMFIX_HOOK(DestroyImageView, event::kImageViews),
    DOOMFIX_HOOK(CreateRenderPass, event::kRenderPasses),
    DOOMFIX_OPTIONAL_HOOK(CreateRenderPass2, event::kRenderPasses),
    DOOMFIX_OPTIONAL_HOOK(CreateRenderPass2KHR, event::kRenderPasses),
    DOOMFIX_HOOK(DestroyRenderPass, event::kRenderPasses),
    DOOMFIX_HOOK(CreateFramebuffer, event::kFramebuffers),
    DOOMFIX_HOOK(DestroyFramebuffer, event::kFramebuffers),
    // Командные буферы и пулы.
    DOOMFIX_HOOK(AllocateCommandBuffers, event::kCommandBuffers),
    DOOMFIX_HOOK(BeginCommandBuffer, event::kCommandBuffers),
    DOOMFIX_HOOK(ResetCommandBuffer, event::kCommandBuffers),
    DOOMFIX_HOOK(FreeCommandBuffers, event::kCommandBuffers),
    DOOMFIX_HOOK(ResetCommandPool, event::kCommandBuffers),
    DOOMFIX_HOOK(DestroyCommandPool, event::kCommandBuffers),
    // Запись команд.
    DOOMFIX_HOOK(CmdSetViewport, event::kViewport),
    DOOMFIX_HOOK(CmdBeginRenderPass, event::kRenderPassCmds),
    DOOMFIX_OPTIONAL_HOOK(CmdBeginRenderPass2, event::kRenderPassCmds),
    DOOMFIX_OPTIONAL_HOOK(CmdBeginRenderPass2KHR, event::kRenderPassCmds),
    DOOMFIX_HOOK(CmdNextSubpass, event::kRenderPassCmds),
    DOOMFIX_OPTIONAL_HOOK(CmdNextSubpass2, event::kRenderPassCmds),
    DOOMFIX_OPTIONAL_HOOK(CmdNextSubpass2KHR, event::kRenderPassCmds),
    DOOMFIX_HOOK(CmdEndRenderPass, event::kRenderPassCmds),
    DOOMFIX_OPTIONAL_HOOK(CmdEndRenderPass2, event::kRenderPassCmds),
    DOOMFIX_OPTIONAL_HOOK(CmdEndRenderPass2KHR, event::kRenderPassCmds),
    DOOMFIX_OPTIONAL_HOOK(CmdBeginRendering, event::kRenderPassCmds),
    DOOMFIX_OPTIONAL_HOOK(CmdBeginRenderingKHR, event::kRenderPassCmds),
    DOOMFIX_OPTIONAL_HOOK(CmdEndRendering, event::kRenderPassCmds),
    DOOMFIX_OPTIONAL_HOOK(CmdEndRenderingKHR, event::kRenderPassCmds),
    DOOMFIX_HOOK(CmdExecuteCommands, event::kExecuteCommands),
    DOOMFIX_HOOK(CmdPipelineBarrier, event::kBarriers),
    DOOMFIX_OPTIONAL_HOOK(CmdPipelineBarrier2, event::kBarriers),
    DOOMFIX_OPTIONAL_HOOK(CmdPipelineBarrier2KHR, event::kBarriers),
    DOOMFIX_HOOK(CmdWaitEvents, event::kWaitEvents),
    DOOMFIX_OPTIONAL_HOOK(CmdWaitEvents2, event::kWaitEvents),
    DOOMFIX_OPTIONAL_HOOK(CmdWaitEvents2KHR, event::kWaitEvents),
    // Отправка.
    DOOMFIX_HOOK(QueueSubmit, event::kSubmit),
    DOOMFIX_OPTIONAL_HOOK(QueueSubmit2, event::kSubmit),
    DOOMFIX_OPTIONAL_HOOK(QueueSubmit2KHR, event::kSubmit),
};

// Нужен ли перехват хоть одной фиче (или самому ядру).
bool Wanted(const NamedHook& hook) { return hook.events == 0 || (g_interest & hook.events) != 0; }

#undef DOOMFIX_HOOK
#undef DOOMFIX_OPTIONAL_HOOK

template <size_t N>
const NamedHook* FindHookEntry(const NamedHook (&hooks)[N], const char* name) {
  for (const NamedHook& hook : hooks) {
    if (std::strcmp(hook.name, name) == 0) return &hook;
  }
  return nullptr;
}

template <size_t N>
PFN_vkVoidFunction FindHook(const NamedHook (&hooks)[N], const char* name) {
  const NamedHook* hook = FindHookEntry(hooks, name);
  return hook ? hook->function : nullptr;
}

}  // namespace
}  // namespace doomfix

// --- Экспортируемые точки входа ----------------------------------------------------------------

extern "C" {

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL DoomFix_GetInstanceProcAddr(VkInstance instance,
                                                                     const char* pName) {
  using namespace doomfix;
  if (!pName) return nullptr;
  EnsureInitialized();  // маска интересов должна быть известна до первого ответа
  if (auto fn = FindHook(kInstanceHooks, pName)) return fn;
  InstanceData* data = instance ? g_instances.Find(DispatchKey(instance)) : nullptr;
  // По спецификации vkGetInstanceProcAddr обязан отдавать и функции уровня устройства.
  // Необязательные отдаём, только если их знает следующее звено; ненужные фичам — не перехватываем.
  const NamedHook* hook = FindHookEntry(kDeviceHooks, pName);
  if (hook && Wanted(*hook)) {
    if (!hook->available || !data) return hook->function;
    return data->vk.GetInstanceProcAddr(instance, pName) ? hook->function : nullptr;
  }
  if (!instance) return nullptr;
  return data ? data->vk.GetInstanceProcAddr(instance, pName) : nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL DoomFix_GetDeviceProcAddr(VkDevice device,
                                                                   const char* pName) {
  using namespace doomfix;
  if (!pName || !device) return nullptr;
  EnsureInitialized();
  DeviceData* data = g_devices.Find(DispatchKey(device));
  // Перехват, ненужный ни одной фиче, не отдаём: приложение вызывает следующее звено напрямую.
  const NamedHook* hook = FindHookEntry(kDeviceHooks, pName);
  if (hook && Wanted(*hook)) {
    // Функцию, не включённую у устройства, нельзя отдавать: по спецификации ответ — NULL.
    if (!hook->available || (data && hook->available(data->vk))) return hook->function;
    return nullptr;
  }
  return data ? data->vk.GetDeviceProcAddr(device, pName) : nullptr;
}

VKAPI_ATTR VkResult VKAPI_CALL
DoomFix_NegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct) {
  if (!pVersionStruct || pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT ||
      pVersionStruct->loaderLayerInterfaceVersion < 2) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  pVersionStruct->loaderLayerInterfaceVersion = 2;
  pVersionStruct->pfnGetInstanceProcAddr = DoomFix_GetInstanceProcAddr;
  pVersionStruct->pfnGetDeviceProcAddr = DoomFix_GetDeviceProcAddr;
  pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
  doomfix::EnsureInitialized();
  return VK_SUCCESS;
}

}  // extern "C"
#include "native_adopt.inc"
