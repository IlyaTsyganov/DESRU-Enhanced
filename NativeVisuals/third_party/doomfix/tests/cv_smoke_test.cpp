// Based on doom-eternal-collision-viewer by IlyaTsyganov (commit 43ac57d), used with the author's permission.
//
// GPU-смоук оверлея collision-viewer через настоящий loader, без игры (перенос tests/vulkan_smoke.cpp
// и tools/test_vulkan.ps1 исходного viewer). Проверяется установленный слой (scripts\install.ps1).
//
// Тест запускает сам себя (--child <случай> <папка>) для каждого случая с DOOMFIX_ENABLE=1, своим
// DOOMFIX_LOG_DIR и тестовыми переменными DOOMFIX_CV_*. Дочерний процесс, как игра: создаёт скрытое
// окно, swapchain (только с TRANSFER_DST — COLOR_ATTACHMENT и TRANSFER_SRC добавляет слой), картинку
// глубины D32 с SAMPLED, и три кадра подряд: проход глубины (очистка и «стена» нужной глубины),
// очистка картинки swapchain в чёрный, submit, present. В тестовом режиме слой рисует свою сцену
// поверх кадра, читает кадр обратно и пишет в лог строку "collision-viewer test result: ...".
// Родитель разбирает последнюю такую строку и проверяет пиксели:
//   smoke            треугольник и линия видны (центр экрана не чёрный);
//   console 0.5 / 1  выше «консоли» ни одного пикселя, ниже — есть (при 1 — нет);
//   occlusion        hidden/reverse/inverted — всё скрыто стеной; partial/inverted-partial — видно
//                    только справа; front/inverted-front — видно с обеих сторон; везде один оттенок
//                    зелёного (окклюзия по пикселю, без смешения);
//   pattern          тестовый рисунок для игры: рамка зелёная (буфер глубины найден);
//   scene            DOOMFIX_CV_TEST=scene: настоящий источник «память игры» над поддельной памятью
//                    (маленький уровень, game_memory/fake_game.h), 60 кадров: видны коллизии (зелёные),
//                    триггер (голубой) и стена (оранжевая) в обеих половинах; в логе — сцена и сущности;
//   scene-occluded   то же со «стеной» глубины (reverse-Z) в левой половине: слева ничего;
//   scene-console    то же с консолью «игры» на 50 %: выше неё ничего;
//   default          без тестовых переменных: в логе «found the screen depth buffer», ничего не рисуется;
//   off              DOOMFIX_COLLISION_VIEWER=0: фичи нет в логе.
//   cross-exclusive, cross-concurrent  как DOOM Eternal: рисование на графической очереди, present — с
//                    очереди без графики (EXCLUSIVE с передачей владения или CONCURRENT); слой рисует на
//                    своей графической очереди; проверяется partial. Пропускаются, если у драйвера нет
//                    такого семейства очередей (SwiftShader).
// Во всех случаях с viewer в логе не должно быть ошибок collision-viewer.
//
//   build\bin\doomfix_cv_smoke_test.exe     код выхода 0 = всё прошло

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

#define CHECK(x)                                                     \
  do {                                                               \
    VkResult r_ = (x);                                               \
    if (r_ != VK_SUCCESS && r_ != VK_SUBOPTIMAL_KHR) {               \
      std::printf("FAILED %s -> %d (line %d)\n", #x, r_, __LINE__); \
      std::exit(1);                                                  \
    }                                                                \
  } while (0)

// --- Дочерний процесс ---------------------------------------------------------------------------

// Код выхода дочернего процесса: случай невозможен на этом драйвере.
constexpr int kSkip = 3;

// presentMode: "same" — present с графической очереди; "exclusive"/"concurrent" — с очереди без графики.
// occlusion — глубина кадра: как в DOOMFIX_CV_OCCLUSION_TEST, плюс "inverted-none" (reverse-Z без стены).
int RunChild(const std::string& occlusion, const std::string& presentMode, int frames) {
  HMODULE loader = LoadLibraryW(L"vulkan-1.dll");
  if (!loader) {
    std::printf("vulkan-1.dll not found\n");
    return 1;
  }
  auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
  const char* extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "doomfix_cv_smoke_test";
  app.apiVersion = VK_API_VERSION_1_1;  // как у игры
  VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  instanceInfo.pApplicationInfo = &app;
  instanceInfo.enabledExtensionCount = 2;
  instanceInfo.ppEnabledExtensionNames = extensions;
  VkInstance instance;
  CHECK(reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"))(&instanceInfo, nullptr,
                                                                                 &instance));
#define INST(name) auto name = reinterpret_cast<PFN_vk##name>(gipa(instance, "vk" #name))
  INST(EnumeratePhysicalDevices);
  INST(GetPhysicalDeviceQueueFamilyProperties);
  INST(GetPhysicalDeviceSurfaceSupportKHR);
  INST(CreateWin32SurfaceKHR);
  INST(GetPhysicalDeviceSurfaceCapabilitiesKHR);
  INST(GetPhysicalDeviceSurfaceFormatsKHR);
  INST(GetPhysicalDeviceMemoryProperties);
  INST(GetPhysicalDeviceProperties);
  INST(CreateDevice);
  INST(DestroySurfaceKHR);
  INST(DestroyInstance);
  auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(instance, "vkGetDeviceProcAddr"));

  WNDCLASSW wc{};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"doomfix_cv_smoke_test";
  RegisterClassW(&wc);
  HWND window = CreateWindowExW(0, wc.lpszClassName, L"doomfix_cv_smoke_test", WS_POPUP, 0, 0, 128, 128,
                                nullptr, nullptr, wc.hInstance, nullptr);
  if (!window) {
    std::printf("cannot create the hidden test window\n");
    return 1;
  }
  VkWin32SurfaceCreateInfoKHR surfaceInfo{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
  surfaceInfo.hinstance = wc.hInstance;
  surfaceInfo.hwnd = window;
  VkSurfaceKHR surface;
  CHECK(CreateWin32SurfaceKHR(instance, &surfaceInfo, nullptr, &surface));

  uint32_t count = 0;
  CHECK(EnumeratePhysicalDevices(instance, &count, nullptr));
  std::vector<VkPhysicalDevice> physicals(count);
  CHECK(EnumeratePhysicalDevices(instance, &count, physicals.data()));
  const bool cross = presentMode != "same";
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  uint32_t family = UINT32_MAX;         // графика (и present, если не cross)
  uint32_t presentFamily = UINT32_MAX;  // cross: present без графики
  for (VkPhysicalDevice candidate : physicals) {
    uint32_t n = 0;
    GetPhysicalDeviceQueueFamilyProperties(candidate, &n, nullptr);
    std::vector<VkQueueFamilyProperties> props(n);
    GetPhysicalDeviceQueueFamilyProperties(candidate, &n, props.data());
    uint32_t graphics = UINT32_MAX;
    uint32_t other = UINT32_MAX;
    for (uint32_t i = 0; i < n; ++i) {
      VkBool32 supported = VK_FALSE;
      GetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface, &supported);
      const bool hasGraphics = (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
      if (hasGraphics && (supported || cross) && graphics == UINT32_MAX) graphics = i;
      if (!hasGraphics && supported && other == UINT32_MAX) other = i;
    }
    if (graphics != UINT32_MAX && (!cross || other != UINT32_MAX)) {
      physical = candidate;
      family = graphics;
      presentFamily = cross ? other : graphics;
      break;
    }
  }
  if (!physical) {
    if (cross) {
      std::printf("    no queue family that presents without graphics: skipped\n");
      return kSkip;
    }
    std::printf("no graphics queue that can present to the window\n");
    return 1;
  }
  VkPhysicalDeviceProperties properties;
  GetPhysicalDeviceProperties(physical, &properties);
  std::printf("    device: %s\n", properties.deviceName);

  const float priority = 1.0f;
  VkDeviceQueueCreateInfo queueInfos[2] = {{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO},
                                           {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}};
  queueInfos[0].queueFamilyIndex = family;
  queueInfos[0].queueCount = 1;
  queueInfos[0].pQueuePriorities = &priority;
  queueInfos[1] = queueInfos[0];
  queueInfos[1].queueFamilyIndex = presentFamily;
  const char* deviceExtension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
  VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  deviceInfo.queueCreateInfoCount = cross ? 2 : 1;
  deviceInfo.pQueueCreateInfos = queueInfos;
  deviceInfo.enabledExtensionCount = 1;
  deviceInfo.ppEnabledExtensionNames = &deviceExtension;
  VkDevice device;
  CHECK(CreateDevice(physical, &deviceInfo, nullptr, &device));
#define DEV(name) auto name = reinterpret_cast<PFN_vk##name>(gdpa(device, "vk" #name))
  DEV(GetDeviceQueue);
  DEV(CreateSwapchainKHR);
  DEV(GetSwapchainImagesKHR);
  DEV(AcquireNextImageKHR);
  DEV(QueuePresentKHR);
  DEV(DestroySwapchainKHR);
  DEV(CreateCommandPool);
  DEV(AllocateCommandBuffers);
  DEV(BeginCommandBuffer);
  DEV(EndCommandBuffer);
  DEV(ResetCommandBuffer);
  DEV(DestroyCommandPool);
  DEV(CmdPipelineBarrier);
  DEV(CmdClearColorImage);
  DEV(CreateSemaphore);
  DEV(DestroySemaphore);
  DEV(QueueSubmit);
  DEV(DeviceWaitIdle);
  DEV(DestroyDevice);
  DEV(CreateImage);
  DEV(DestroyImage);
  DEV(GetImageMemoryRequirements);
  DEV(AllocateMemory);
  DEV(FreeMemory);
  DEV(BindImageMemory);
  DEV(CreateImageView);
  DEV(DestroyImageView);
  DEV(CreateRenderPass);
  DEV(DestroyRenderPass);
  DEV(CreateFramebuffer);
  DEV(DestroyFramebuffer);
  DEV(CmdBeginRenderPass);
  DEV(CmdEndRenderPass);
  DEV(CmdClearAttachments);
  DEV(CmdSetViewport);
  VkQueue queue;
  GetDeviceQueue(device, family, 0, &queue);
  VkQueue presentQueue = queue;
  if (cross) GetDeviceQueue(device, presentFamily, 0, &presentQueue);

  VkSurfaceCapabilitiesKHR caps;
  CHECK(GetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps));
  CHECK(GetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, nullptr));
  std::vector<VkSurfaceFormatKHR> formats(count);
  CHECK(GetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats.data()));
  VkSurfaceFormatKHR format = formats.at(0);
  for (const VkSurfaceFormatKHR& f : formats) {
    if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) {
      format = f;
      break;
    }
  }
  VkExtent2D extent = caps.currentExtent;
  if (extent.width == UINT32_MAX) extent = {128, 128};
  VkSwapchainCreateInfoKHR swapInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  swapInfo.surface = surface;
  swapInfo.minImageCount = std::max(2u, caps.minImageCount);
  if (caps.maxImageCount) swapInfo.minImageCount = std::min(swapInfo.minImageCount, caps.maxImageCount);
  swapInfo.imageFormat = format.format;
  swapInfo.imageColorSpace = format.colorSpace;
  swapInfo.imageExtent = extent;
  swapInfo.imageArrayLayers = 1;
  swapInfo.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;  // остальное — дело слоя
  const uint32_t sharingFamilies[2] = {family, presentFamily};
  swapInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (presentMode == "concurrent") {
    swapInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
    swapInfo.queueFamilyIndexCount = 2;
    swapInfo.pQueueFamilyIndices = sharingFamilies;
  }
  swapInfo.preTransform = caps.currentTransform;
  swapInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  swapInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  swapInfo.clipped = VK_TRUE;
  VkSwapchainKHR swapchain;
  CHECK(CreateSwapchainKHR(device, &swapInfo, nullptr, &swapchain));
  CHECK(GetSwapchainImagesKHR(device, swapchain, &count, nullptr));
  std::vector<VkImage> images(count);
  CHECK(GetSwapchainImagesKHR(device, swapchain, &count, images.data()));

  // Буфер глубины «игры»: D32, сохраняется и остаётся в DEPTH_STENCIL_READ_ONLY_OPTIMAL.
  const bool scaled = occlusion == "inverted-jitter-scaled";
  const VkExtent2D depthExtent = scaled ? VkExtent2D{96, 96} : extent;
  VkImageCreateInfo depthInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  depthInfo.imageType = VK_IMAGE_TYPE_2D;
  depthInfo.format = VK_FORMAT_D32_SFLOAT;
  depthInfo.extent = {depthExtent.width, depthExtent.height, 1};
  depthInfo.mipLevels = depthInfo.arrayLayers = 1;
  depthInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  depthInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  depthInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  VkImage depthImage;
  CHECK(CreateImage(device, &depthInfo, nullptr, &depthImage));
  VkMemoryRequirements requirements;
  GetImageMemoryRequirements(device, depthImage, &requirements);
  VkPhysicalDeviceMemoryProperties memoryProperties;
  GetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
  uint32_t memoryType = UINT32_MAX;
  for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
    if (!(requirements.memoryTypeBits & (1u << i))) continue;
    memoryType = i;
    if (memoryProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) break;
  }
  VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocation.allocationSize = requirements.size;
  allocation.memoryTypeIndex = memoryType;
  VkDeviceMemory depthMemory;
  CHECK(AllocateMemory(device, &allocation, nullptr, &depthMemory));
  CHECK(BindImageMemory(device, depthImage, depthMemory, 0));
  VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  viewInfo.image = depthImage;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format = depthInfo.format;
  viewInfo.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
  VkImageView depthView;
  CHECK(CreateImageView(device, &viewInfo, nullptr, &depthView));
  VkAttachmentDescription attachment = {};
  attachment.format = depthInfo.format;
  attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
  const VkAttachmentReference reference = {0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass = {};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.pDepthStencilAttachment = &reference;
  // Запись глубины видна чтению в шейдере оверлея (его отправка идёт после, на той же очереди).
  VkSubpassDependency dependency = {};
  dependency.srcSubpass = 0;
  dependency.dstSubpass = VK_SUBPASS_EXTERNAL;
  dependency.srcStageMask =
      VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  dependency.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  dependency.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  dependency.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  VkRenderPassCreateInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  passInfo.attachmentCount = 1;
  passInfo.pAttachments = &attachment;
  passInfo.subpassCount = 1;
  passInfo.pSubpasses = &subpass;
  passInfo.dependencyCount = 1;
  passInfo.pDependencies = &dependency;
  VkRenderPass depthPass;
  CHECK(CreateRenderPass(device, &passInfo, nullptr, &depthPass));
  VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  framebufferInfo.renderPass = depthPass;
  framebufferInfo.attachmentCount = 1;
  framebufferInfo.pAttachments = &depthView;
  framebufferInfo.width = depthExtent.width;
  framebufferInfo.height = depthExtent.height;
  framebufferInfo.layers = 1;
  VkFramebuffer depthFramebuffer;
  CHECK(CreateFramebuffer(device, &framebufferInfo, nullptr, &depthFramebuffer));

  VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  poolInfo.queueFamilyIndex = family;
  VkCommandPool pool;
  CHECK(CreateCommandPool(device, &poolInfo, nullptr, &pool));
  VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  allocate.commandPool = pool;
  allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocate.commandBufferCount = 1;
  VkCommandBuffer commands;
  CHECK(AllocateCommandBuffers(device, &allocate, &commands));
  // EXCLUSIVE с present на другом семействе: картинку принимает очередь present (acquire).
  const bool exclusiveCross = presentMode == "exclusive";
  VkCommandPool presentPool = VK_NULL_HANDLE;
  VkCommandBuffer presentCommands = VK_NULL_HANDLE;
  if (exclusiveCross) {
    VkCommandPoolCreateInfo presentPoolInfo = poolInfo;
    presentPoolInfo.queueFamilyIndex = presentFamily;
    CHECK(CreateCommandPool(device, &presentPoolInfo, nullptr, &presentPool));
    VkCommandBufferAllocateInfo presentAllocate = allocate;
    presentAllocate.commandPool = presentPool;
    CHECK(AllocateCommandBuffers(device, &presentAllocate, &presentCommands));
  }
  VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  VkSemaphore acquired;
  VkSemaphore ready;
  VkSemaphore handedOver;
  CHECK(CreateSemaphore(device, &semaphoreInfo, nullptr, &acquired));
  CHECK(CreateSemaphore(device, &semaphoreInfo, nullptr, &ready));
  CHECK(CreateSemaphore(device, &semaphoreInfo, nullptr, &handedOver));

  // Глубина по случаю: очистка 1 (reverse-Z — 0), «стена» 0.95 (reverse-Z и перевёрнутый viewport —
  // 0.05) на весь экран или левую половину.
  const bool inverted = occlusion.rfind("inverted", 0) == 0;
  const bool reverseViewport = occlusion == "reverse";
  const bool partial = occlusion == "partial" || occlusion == "inverted-partial";
  const bool jitter = occlusion.find("jitter") != std::string::npos;
  const bool wall = !occlusion.empty() && occlusion != "none" && occlusion != "inverted-none" &&
                    occlusion.rfind("range-", 0) != 0 && occlusion != "dense-object" && !jitter;
  std::vector<VkImageLayout> layouts(images.size(), VK_IMAGE_LAYOUT_UNDEFINED);
  for (int frame = 0; frame < frames; ++frame) {
    uint32_t index = 0;
    CHECK(AcquireNextImageKHR(device, swapchain, 2000000000ull, acquired, VK_NULL_HANDLE, &index));
    CHECK(ResetCommandBuffer(commands, 0));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(BeginCommandBuffer(commands, &begin));
    if (reverseViewport || scaled) {
      const VkViewport reversed = {0, 0, static_cast<float>(depthExtent.width), static_cast<float>(depthExtent.height),
                                   reverseViewport ? 1.0f : 0.0f, reverseViewport ? 0.0f : 1.0f};
      CmdSetViewport(commands, 0, 1, &reversed);
    }
    VkClearValue clear = {};
    clear.depthStencil = {inverted ? 0.0f : 1.0f, 0};
    VkRenderPassBeginInfo passBegin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    passBegin.renderPass = depthPass;
    passBegin.framebuffer = depthFramebuffer;
    passBegin.renderArea.extent = depthExtent;
    passBegin.clearValueCount = 1;
    passBegin.pClearValues = &clear;
    CmdBeginRenderPass(commands, &passBegin, VK_SUBPASS_CONTENTS_INLINE);
    if (wall) {
      VkClearAttachment wallClear = {};
      wallClear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
      wallClear.clearValue.depthStencil = {(inverted || reverseViewport) ? 0.05f : 0.95f, 0};
      VkClearRect rect = {};
      rect.layerCount = 1;
      rect.rect.extent = {partial ? extent.width / 2 : extent.width, extent.height};
      CmdClearAttachments(commands, 1, &wallClear, 1, &rect);
    }
    if (jitter) {
      for (uint32_t y = 0; y < depthExtent.height; ++y) {
        for (uint32_t x = 0; x < depthExtent.width; ++x) {
          // Alternate the half-pixel sample offset each frame, including with a stationary camera.
          const float offset = ((x + y + frame) & 1) ? 0.5f : -0.5f;
          const float sx = 2.0f * (x + 0.5f + offset) / depthExtent.width - 1.0f;
          const float slope = occlusion.find("grazing") != std::string::npos ? 1.5f : 0.8f;
          const float distance = occlusion.find("occluded") != std::string::npos && x < depthExtent.width / 2 ?
                                 1.0f : 20.0f / std::max(0.00001f, 1.0f - slope * sx);
          VkClearAttachment plane = {};
          plane.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
          plane.clearValue.depthStencil = {inverted ? 0.05f / distance : 1.0f - 0.05f / distance, 0};
          VkClearRect rect = {};
          rect.layerCount = 1;
          rect.rect.offset = {static_cast<int32_t>(x), static_cast<int32_t>(y)};
          rect.rect.extent = {1, 1};
          CmdClearAttachments(commands, 1, &plane, 1, &rect);
        }
      }
    }
    CmdEndRenderPass(commands);
    // Кадр «игры» — чёрный.
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = images[index];
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    CmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                       nullptr, 0, nullptr, 1, &barrier);
    const VkClearColorValue black = {{0, 0, 0, 1}};
    CmdClearColorImage(commands, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1,
                       &barrier.subresourceRange);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = 0;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    if (exclusiveCross) {  // release: отдать картинку семейству present
      barrier.srcQueueFamilyIndex = family;
      barrier.dstQueueFamilyIndex = presentFamily;
    }
    CmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                       nullptr, 0, nullptr, 1, &barrier);
    CHECK(EndCommandBuffer(commands));
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &acquired;
    submit.pWaitDstStageMask = &stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &ready;
    CHECK(QueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    VkSemaphore presentWait = ready;
    if (exclusiveCross) {
      // acquire на очереди present (те же layout, что в release).
      CHECK(ResetCommandBuffer(presentCommands, 0));
      CHECK(BeginCommandBuffer(presentCommands, &begin));
      barrier.srcAccessMask = 0;
      CmdPipelineBarrier(presentCommands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
      CHECK(EndCommandBuffer(presentCommands));
      const VkPipelineStageFlags allCommands = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
      VkSubmitInfo handOver{VK_STRUCTURE_TYPE_SUBMIT_INFO};
      handOver.waitSemaphoreCount = 1;
      handOver.pWaitSemaphores = &ready;
      handOver.pWaitDstStageMask = &allCommands;
      handOver.commandBufferCount = 1;
      handOver.pCommandBuffers = &presentCommands;
      handOver.signalSemaphoreCount = 1;
      handOver.pSignalSemaphores = &handedOver;
      CHECK(QueueSubmit(presentQueue, 1, &handOver, VK_NULL_HANDLE));
      presentWait = handedOver;
    }
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &presentWait;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain;
    present.pImageIndices = &index;
    CHECK(QueuePresentKHR(presentQueue, &present));
    CHECK(DeviceWaitIdle(device));
  }

  DestroySemaphore(device, acquired, nullptr);
  DestroySemaphore(device, ready, nullptr);
  DestroySemaphore(device, handedOver, nullptr);
  DestroyCommandPool(device, pool, nullptr);
  if (presentPool) DestroyCommandPool(device, presentPool, nullptr);
  DestroySwapchainKHR(device, swapchain, nullptr);
  DestroyFramebuffer(device, depthFramebuffer, nullptr);
  DestroyRenderPass(device, depthPass, nullptr);
  DestroyImageView(device, depthView, nullptr);
  DestroyImage(device, depthImage, nullptr);
  FreeMemory(device, depthMemory, nullptr);
  DestroyDevice(device, nullptr);
  DestroySurfaceKHR(instance, surface, nullptr);
  DestroyInstance(instance, nullptr);
  DestroyWindow(window);
  UnregisterClassW(wc.lpszClassName, wc.hInstance);
  return 0;
}

// --- Родитель -------------------------------------------------------------------------------------

int g_failures = 0;

void Fail(const std::string& message) {
  std::printf("  FAIL: %s\n", message.c_str());
  ++g_failures;
}

void Ok(const std::string& message) { std::printf("  ok   %s\n", message.c_str()); }

struct Case {
  const char* name;
  const wchar_t* viewer;       // DOOMFIX_COLLISION_VIEWER (nullptr — не задана)
  const wchar_t* test;         // DOOMFIX_CV_TEST
  const char* occlusion;       // DOOMFIX_CV_OCCLUSION_TEST и глубина в дочернем процессе
  const wchar_t* console;      // DOOMFIX_CV_TEST_CONSOLE_FRACTION
  const wchar_t* readback;     // DOOMFIX_CV_TEST_READBACK
  const wchar_t* present = L"same";  // очередь present в дочернем процессе: same / exclusive / concurrent
  const char* depth = nullptr;       // глубина кадра в дочернем процессе (по умолчанию = occlusion)
  int frames = 3;                    // кадров в дочернем процессе
};

struct Run {
  bool ok = false;
  std::vector<std::string> log;
  std::map<std::string, std::string> result;  // последняя строка "collision-viewer test result"
  int results = 0;
  std::vector<std::map<std::string, std::string>> frames;
};

// 1 — прошёл, 0 — ошибка, -1 — случай невозможен на этом драйвере.
int RunChildProcess(const Case& c, const fs::path& logDir) {
  fs::create_directories(logDir);
  SetEnvironmentVariableW(L"DOOMFIX_ENABLE", L"1");
  SetEnvironmentVariableW(L"DOOMFIX_LOG_DIR", logDir.c_str());
  SetEnvironmentVariableW(L"DOOMFIX_LOG_LEVEL", L"info");
  SetEnvironmentVariableW(L"DOOMFIX_INDIRECT_FIX", L"on");
  SetEnvironmentVariableW(L"DOOMFIX_TRACE_EVENTS", nullptr);
  SetEnvironmentVariableW(L"DOOMFIX_COLLISION_VIEWER", c.viewer);
  SetEnvironmentVariableW(L"DOOMFIX_CV_TEST", c.test);
  const std::wstring occlusion(c.occlusion, c.occlusion + std::strlen(c.occlusion));
  SetEnvironmentVariableW(L"DOOMFIX_CV_OCCLUSION_TEST", occlusion.empty() ? nullptr : occlusion.c_str());
  SetEnvironmentVariableW(L"DOOMFIX_CV_TEST_CONSOLE_FRACTION", c.console);
  SetEnvironmentVariableW(L"DOOMFIX_CV_TEST_READBACK", c.readback);

  std::fflush(stdout);
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  const std::string depthName = c.depth ? c.depth : (occlusion.empty() ? "none" : c.occlusion);
  std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --child \"" +
                     std::wstring(depthName.begin(), depthName.end()) + L"\" " + c.present + L" " +
                     std::to_wstring(c.frames);
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
    Fail("cannot start a child process, error " + std::to_string(GetLastError()));
    return 0;
  }
  if (WaitForSingleObject(pi.hProcess, 60000) != WAIT_OBJECT_0) {
    TerminateProcess(pi.hProcess, 1);
    Fail("the child process did not finish in 60 s");
  }
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  if (code == kSkip) return -1;
  if (code != 0) {
    Fail("child process exited with code " + std::to_string(code));
    return 0;
  }
  return 1;
}

Run ReadRun(const fs::path& logDir) {
  Run r;
  std::vector<fs::path> logs;
  for (const auto& e : fs::directory_iterator(logDir)) {
    if (e.path().extension() == L".log") logs.push_back(e.path());
  }
  if (logs.size() != 1) {
    Fail("expected one layer log in " + logDir.string() + ", found " + std::to_string(logs.size()) +
         " (is the layer installed? run install.bat)");
    return r;
  }
  std::ifstream in(logs[0]);
  const std::string marker = "collision-viewer test result: ";
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    r.log.push_back(line);
    const size_t at = line.find(marker);
    if (at == std::string::npos) continue;
    ++r.results;
    r.result.clear();
    std::istringstream items(line.substr(at + marker.size()));
    for (std::string item; items >> item;) {
      const size_t eq = item.find('=');
      if (eq != std::string::npos) r.result[item.substr(0, eq)] = item.substr(eq + 1);
    }
    r.frames.push_back(r.result);
  }
  r.ok = true;
  return r;
}

bool LogHas(const Run& r, const std::string& text) {
  for (const std::string& line : r.log) {
    if (line.find(text) != std::string::npos) return true;
  }
  return false;
}

long Num(const Run& r, const char* key) {
  auto it = r.result.find(key);
  return it == r.result.end() ? -1 : std::strtol(it->second.c_str(), nullptr, 10);
}

// "r,g,b,a" из результата.
std::vector<int> Pixel(const Run& r, const char* key) {
  std::vector<int> out;
  auto it = r.result.find(key);
  if (it == r.result.end()) return out;
  std::istringstream in(it->second);
  for (std::string part; std::getline(in, part, ',');) out.push_back(std::atoi(part.c_str()));
  return out;
}

void Expect(bool ok, const std::string& what, const Run& r) {
  if (ok) {
    Ok(what);
    return;
  }
  std::string details;
  for (const auto& [key, value] : r.result) details += key + "=" + value + " ";
  Fail(what + (details.empty() ? " (no result line)" : " (" + details + ")"));
}

void CheckCase(const Case& c, const Run& r) {
  const std::string name = c.name;
  const bool viewerOff = c.viewer && std::wcscmp(c.viewer, L"0") == 0;
  if (viewerOff) {
    Expect(!LogHas(r, "collision-viewer"), "DOOMFIX_COLLISION_VIEWER=0: no collision-viewer in the log", r);
    Expect(LogHas(r, "intercepted event groups: barriers, wait-events"),
           "DOOMFIX_COLLISION_VIEWER=0: the interest mask is the fix's only", r);
    return;
  }
  Expect(LogHas(r, "feature: collision-viewer"), "collision-viewer is loaded", r);
  Expect(!LogHas(r, "The viewer is turned off"), "no collision-viewer errors", r);
  Expect(LogHas(r, "found the screen depth buffer"), "the depth buffer of the test program is found", r);
  if (!c.test && c.occlusion[0] == '\0') {
    // Для справки (и для scripts\test_validation.ps1): какие слои стоят под doomfix.
    for (const std::string& line : r.log) {
      const size_t at = line.find("vkCreateDevice: chain below doomfix: ");
      if (at != std::string::npos) std::printf("  info %s\n", line.c_str() + at);
    }
    Expect(r.results == 0 && !LogHas(r, "overlay renderer ready"),
           "without a test mode nothing is drawn and no overlay resources are created", r);
    Expect(LogHas(r, "swapchain image usage 0x"), "COLOR_ATTACHMENT added to the swapchain usage", r);
    return;
  }
  const auto depth = r.result.find("depth");
  Expect(r.results > 0 && depth != r.result.end() && depth->second == "found",
         name + ": frames were drawn and read back", r);
  const long green = Num(r, "green");
  const long cyan = Num(r, "cyan");
  const long left = Num(r, "leftColored");
  const long right = Num(r, "rightColored");
  const long greens = Num(r, "uniqueGreens");
  const long orange = Num(r, "orange");
  if (name == "smoke") {
    const std::vector<int> center = Pixel(r, "center");
    Expect(center.size() == 4 && (center[0] > 10 || center[1] > 10 || center[2] > 10) && Num(r, "lit") > 0,
           "smoke: overlay pixels are present (center is lit)", r);
  } else if (name == "console 0.5") {
    Expect(Num(r, "consoleTop") > 0 && Num(r, "consoleLeak") == 0 && Num(r, "belowConsole") > 0,
           "console 50%: nothing above the console, overlay below it", r);
  } else if (name == "console 1") {
    Expect(Num(r, "consoleTop") == Num(r, "height") && Num(r, "consoleLeak") == 0 && Num(r, "belowConsole") == 0,
           "console 100%: nothing drawn", r);
  } else if (name == "hidden" || name == "reverse" || name == "inverted") {
    Expect(green == 0 && cyan == 0, name + ": everything behind the wall is hidden", r);
  } else if (name == "partial" || name == "inverted-partial" || name.rfind("cross", 0) == 0) {
    Expect(green > 0 && cyan > 0 && left == 0 && right > 0 && greens == 1,
           name + ": hidden on the left behind the wall, visible on the right", r);
    if (name.rfind("cross", 0) == 0) {
      Expect(LogHas(r, "(no graphics); the overlay draws on its own graphics queue"),
             name + ": present without graphics, the overlay draws on its own queue", r);
      const bool handOver = LogHas(r, "are passed between queue families");
      Expect(name == "cross-exclusive" ? handOver : !handOver,
             name + (name == "cross-exclusive" ? ": images handed over between queue families"
                                                : ": concurrent images need no hand-over"),
             r);
    }
  } else if (name == "front" || name == "inverted-front") {
    Expect(green > 0 && cyan > 0 && left > 0 && right > 0 && greens == 1,
           name + ": in front of the wall, visible on both sides", r);
  } else if (name == "range-crossing") {
    Expect(green == 5776 && cyan == 76 && left > 0 && right > 0,
           "whole object: crossing faces and contours remain complete beyond the radius/far plane", r);
  } else if (name == "dense-object") {
    Expect(green == 5776 && cyan == 76 && Num(r, "triangles") == 300006,
           "dense object: GPU upload/draw includes geometry beyond the old 250000-vertex limit", r);
  } else if (name.rfind("coplanar-jitter", 0) == 0) {
    const bool occluded = name.find("occluded") != std::string::npos;
    bool continuous = r.frames.size() == static_cast<size_t>(c.frames);
    for (const auto& frame : r.frames) {
      continuous &= std::stol(frame.at("green")) > (occluded ? 2800 : 5600) &&
                    std::stol(frame.at("cyan")) > (occluded ? 25 : 50) &&
                    std::stol(frame.at("uniqueGreens")) == 1;
      if (occluded) continuous &= std::stol(frame.at("leftColored")) == 0;
    }
    Expect(continuous,
           "jittered depth: coplanar fill and contours remain continuous without checkerboard holes", r);
  } else if (name == "scene") {
    Expect(green > 0 && cyan > 0 && orange > 0 && left > 0 && right > 0,
           "scene: collision (green), trigger (cyan) and wall (orange) drawn on both halves", r);
    Expect(LogHas(r, "scene ready: game/sp/test_a/test_a, 2 collision instances, 2 unique shapes, 14 triangles"),
           "scene: the world was decoded in the background (2 instances, 14 triangles)", r);
    Expect(LogHas(r, "entities: 1 triggers, 1 invisible walls"), "scene: one trigger and one wall found", r);
    Expect(LogHas(r, "gameplay: overlay shown"), "scene: gameplay detected (no menu)", r);
  } else if (name == "scene-occluded") {
    Expect(green > 0 && cyan > 0 && orange > 0 && left == 0 && right > 0,
           "scene-occluded: hidden on the left behind the game's depth, visible on the right", r);
  } else if (name == "scene-console") {
    Expect(Num(r, "consoleTop") > 0 && Num(r, "consoleLeak") == 0 && Num(r, "belowConsole") > 0,
           "scene-console: nothing above the game's console, the scene below it", r);
  } else if (name == "pattern") {
    const std::vector<int> edge = Pixel(r, "edge");
    Expect(green > 0 && edge.size() == 4 && edge[1] > 150 && edge[0] < 100,
           "pattern: green frame (depth buffer found) and lit pixels", r);
  }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc > 3 && std::wcscmp(argv[1], L"--child") == 0) {
    std::string occlusion;  // имена случаев — ASCII
    std::string presentMode;
    for (const wchar_t* c = argv[2]; *c; ++c) occlusion += static_cast<char>(*c);
    for (const wchar_t* c = argv[3]; *c; ++c) presentMode += static_cast<char>(*c);
    const int frames = argc > 4 ? std::max(1, _wtoi(argv[4])) : 3;
    return RunChild(occlusion == "none" ? "" : occlusion, presentMode, frames);
  }

  static const Case kCases[] = {
      {"default", nullptr, nullptr, "", nullptr, nullptr},
      {"off", L"0", L"smoke", "", nullptr, nullptr},
      {"smoke", nullptr, L"smoke", "", nullptr, nullptr},
      {"console 0.5", nullptr, L"smoke", "", L"0.5", nullptr},
      {"console 1", nullptr, L"smoke", "", L"1", nullptr},
      {"hidden", nullptr, nullptr, "hidden", nullptr, nullptr},
      {"partial", nullptr, nullptr, "partial", nullptr, nullptr},
      {"front", nullptr, nullptr, "front", nullptr, nullptr},
      {"reverse", nullptr, nullptr, "reverse", nullptr, nullptr},
      {"inverted", nullptr, nullptr, "inverted", nullptr, nullptr},
      {"inverted-partial", nullptr, nullptr, "inverted-partial", nullptr, nullptr},
      {"inverted-front", nullptr, nullptr, "inverted-front", nullptr, nullptr},
      {"range-crossing", nullptr, nullptr, "range-crossing", nullptr, nullptr},
      {"dense-object", nullptr, nullptr, "dense-object", nullptr, nullptr},
      {"coplanar-jitter", nullptr, nullptr, "coplanar-jitter", nullptr, nullptr, L"same", nullptr, 4},
      {"coplanar-jitter-reverse", nullptr, nullptr, "coplanar-jitter-reverse", nullptr, nullptr,
       L"same", "inverted-jitter", 4},
      {"coplanar-jitter-scaled", nullptr, nullptr, "coplanar-jitter-reverse", nullptr, nullptr,
       L"same", "inverted-jitter-scaled", 4},
      {"coplanar-jitter-occluded", nullptr, nullptr, "coplanar-jitter-reverse", nullptr, nullptr,
       L"same", "inverted-jitter-occluded", 4},
      {"coplanar-jitter-grazing", nullptr, nullptr, "coplanar-jitter-grazing", nullptr, nullptr,
       L"same", "inverted-jitter-grazing", 4},
      {"coplanar-jitter-grazing-occluded", nullptr, nullptr, "coplanar-jitter-grazing", nullptr, nullptr,
       L"same", "inverted-jitter-grazing-occluded", 4},
      {"pattern", nullptr, L"pattern", "", nullptr, L"1"},
      {"cross-exclusive", nullptr, nullptr, "partial", nullptr, nullptr, L"exclusive"},
      {"cross-concurrent", nullptr, nullptr, "partial", nullptr, nullptr, L"concurrent"},
      {"scene", nullptr, L"scene", "", nullptr, nullptr, L"same", "inverted-none", 60},
      {"scene-occluded", nullptr, L"scene", "", nullptr, nullptr, L"same", "inverted-partial", 60},
      {"scene-console", nullptr, L"scene", "", L"0.5", nullptr, L"same", "inverted-none", 60},
  };

  wchar_t temp[MAX_PATH];
  GetTempPathW(MAX_PATH, temp);
  const fs::path root = fs::path(temp) / L"doomfix_cv_smoke" / std::to_wstring(GetCurrentProcessId());
  int index = 0;
  for (const Case& c : kCases) {
    std::printf("%s\n", c.name);
    const fs::path dir = root / std::to_wstring(index++);
    const int started = RunChildProcess(c, dir);
    if (started < 0) {
      std::printf("  skip %s: the driver has no queue family that presents without graphics\n", c.name);
      continue;
    }
    if (!started) continue;
    const Run run = ReadRun(dir);
    if (run.ok) CheckCase(c, run);
  }

  if (g_failures) {
    std::printf("\n%d check(s) FAILED. Layer logs: %ls\n", g_failures, root.c_str());
    return 1;
  }
  std::error_code ec;
  fs::remove_all(root, ec);
  std::printf("\nPASSED\n");
  return 0;
}
