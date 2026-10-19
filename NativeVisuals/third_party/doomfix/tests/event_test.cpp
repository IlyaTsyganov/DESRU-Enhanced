// Проверка диспетчеризации событий ядра через настоящий loader, без игры.
//
// Тест запускает сам себя (--child) трижды, каждый раз с DOOMFIX_ENABLE=1, DOOMFIX_INDIRECT_FIX=on и
// своим DOOMFIX_LOG_DIR:
//   trace    — с DOOMFIX_TRACE_EVENTS=1: отладочная фича event-trace считает все события ядра;
//   plain    — без неё: как у игроков (с collision-viewer, он включён по умолчанию);
//   noviewer — ещё и с DOOMFIX_COLLISION_VIEWER=0: остаётся только фикс.
// Дочерний процесс создаёт ресурсы, записывает командные буферы (проходы v1/v2, dynamic rendering,
// вторичный буфер, барьеры), отправляет пустые буферы через vkQueueSubmit/vkQueueSubmit2 и, если
// есть окно и swapchain, делает один present. Сколько раз он вызвал каждую функцию, он пишет в
// expected.txt; родитель сравнивает это с итоговым отчётом event-trace в логе слоя.
// Без DOOMFIX_TRACE_EVENTS в логе не должно быть event-trace, а итоговые счётчики indirect-barrier-fix
// должны совпасть с прогоном trace во всех прогонах (новые события и collision-viewer не меняют
// поведение фикса). Ещё проверяется маска интересов: с event-trace или collision-viewer
// vkCmdSetViewport должен вести в doomfix_layer.dll, без обоих — мимо слоя (если doomfix — верхний
// слой; иначе проверка пропускается).
// Проверяется установленный слой (install.bat / scripts\install.ps1), а не DLL из build\bin.
//
//   build\bin\doomfix_event_test.exe     код выхода 0 = всё совпало

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
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

PFN_vkGetInstanceProcAddr gipa;

#define CHECK(x)                                                     \
  do {                                                               \
    VkResult r_ = (x);                                               \
    if (r_ != VK_SUCCESS) {                                          \
      std::printf("FAILED %s -> %d (line %d)\n", #x, r_, __LINE__); \
      std::exit(1);                                                  \
    }                                                                \
  } while (0)

template <typename T>
T Load(VkInstance instance, const char* name) {
  return reinterpret_cast<T>(gipa(instance, name));
}

std::string ModuleOf(PFN_vkVoidFunction fn) {
  HMODULE module = nullptr;
  if (!fn || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                     GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 reinterpret_cast<LPCWSTR>(fn), &module)) {
    return "<none>";
  }
  char path[MAX_PATH] = {};
  GetModuleFileNameA(module, path, MAX_PATH);
  return fs::path(path).filename().string();
}

bool HasExtension(const std::vector<VkExtensionProperties>& list, const char* name) {
  for (const VkExtensionProperties& e : list) {
    if (std::strcmp(e.extensionName, name) == 0) return true;
  }
  return false;
}

uint32_t FindMemoryType(const VkPhysicalDeviceMemoryProperties& props, uint32_t bits) {
  for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
    if ((bits & (1u << i)) &&
        (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
      return i;
    }
  }
  for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
    if (bits & (1u << i)) return i;
  }
  return UINT32_MAX;
}

// --- Дочерний процесс ------------------------------------------------------------------------------

int RunChild(const fs::path& outDir) {
  std::map<std::string, int> expected;  // событие event-trace -> сколько раз вызвано
  int fixIndirect = 0;                  // барьеры с DRAW_INDIRECT в dst

  HMODULE loader = LoadLibraryW(L"vulkan-1.dll");
  if (!loader) {
    std::printf("vulkan-1.dll not found\n");
    return 1;
  }
  gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));

  // Instance: с surface, если loader и драйвер его дают.
  auto enumInstExt = Load<PFN_vkEnumerateInstanceExtensionProperties>(
      nullptr, "vkEnumerateInstanceExtensionProperties");
  uint32_t count = 0;
  CHECK(enumInstExt(nullptr, &count, nullptr));
  std::vector<VkExtensionProperties> instExts(count);
  CHECK(enumInstExt(nullptr, &count, instExts.data()));
  const bool surfaceExt = HasExtension(instExts, VK_KHR_SURFACE_EXTENSION_NAME) &&
                          HasExtension(instExts, VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
  const char* instanceExtensions[] = {VK_KHR_SURFACE_EXTENSION_NAME,
                                      VK_KHR_WIN32_SURFACE_EXTENSION_NAME};

  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "doomfix_event_test";
  app.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  ici.enabledExtensionCount = surfaceExt ? 2 : 0;
  ici.ppEnabledExtensionNames = instanceExtensions;
  VkInstance instance;
  CHECK(Load<PFN_vkCreateInstance>(nullptr, "vkCreateInstance")(&ici, nullptr, &instance));

  auto enumPd = Load<PFN_vkEnumeratePhysicalDevices>(instance, "vkEnumeratePhysicalDevices");
  enumPd(instance, &count, nullptr);
  std::vector<VkPhysicalDevice> pds(count);
  enumPd(instance, &count, pds.data());
  VkPhysicalDevice pd = pds.at(0);
  VkPhysicalDeviceProperties props;
  Load<PFN_vkGetPhysicalDeviceProperties>(instance, "vkGetPhysicalDeviceProperties")(pd, &props);
  std::printf("  device: %s, api %u.%u\n", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
              VK_API_VERSION_MINOR(props.apiVersion));
  if (props.apiVersion < VK_API_VERSION_1_2) {
    std::printf("  FAIL: the test needs a Vulkan 1.2 device (vkCreateRenderPass2)\n");
    return 1;
  }
  VkPhysicalDeviceMemoryProperties memProps;
  Load<PFN_vkGetPhysicalDeviceMemoryProperties>(instance, "vkGetPhysicalDeviceMemoryProperties")(
      pd, &memProps);
  uint32_t familyCount = 0;
  auto getFamilies = Load<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
      instance, "vkGetPhysicalDeviceQueueFamilyProperties");
  getFamilies(pd, &familyCount, nullptr);
  std::vector<VkQueueFamilyProperties> families(familyCount);
  getFamilies(pd, &familyCount, families.data());
  if (!(families.at(0).queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
    std::printf("  FAIL: queue family 0 has no graphics\n");
    return 1;
  }

  // Возможности 1.3: synchronization2 и dynamic rendering — если есть.
  VkPhysicalDeviceVulkan13Features have13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceFeatures2 have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  const bool api13 = props.apiVersion >= VK_API_VERSION_1_3;
  if (api13) {
    have.pNext = &have13;
    Load<PFN_vkGetPhysicalDeviceFeatures2>(instance, "vkGetPhysicalDeviceFeatures2")(pd, &have);
  }
  const bool sync2 = api13 && have13.synchronization2;
  const bool dynRendering = api13 && have13.dynamicRendering;

  auto enumDevExt = Load<PFN_vkEnumerateDeviceExtensionProperties>(
      instance, "vkEnumerateDeviceExtensionProperties");
  CHECK(enumDevExt(pd, nullptr, &count, nullptr));
  std::vector<VkExtensionProperties> devExts(count);
  CHECK(enumDevExt(pd, nullptr, &count, devExts.data()));
  const bool swapchainExt = surfaceExt && HasExtension(devExts, VK_KHR_SWAPCHAIN_EXTENSION_NAME);

  VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  f13.synchronization2 = sync2;
  f13.dynamicRendering = dynRendering;
  const float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = 0;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.pNext = api13 ? &f13 : nullptr;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = swapchainExt ? 1 : 0;
  dci.ppEnabledExtensionNames = deviceExtensions;
  VkDevice device;
  CHECK(Load<PFN_vkCreateDevice>(instance, "vkCreateDevice")(pd, &dci, nullptr, &device));
  expected["OnDeviceCreated"]++;

  auto gdpa = Load<PFN_vkGetDeviceProcAddr>(instance, "vkGetDeviceProcAddr");
  // Куда ведёт vkCmdSetViewport: в doomfix (перехват) или мимо (маска интересов).
  std::ofstream(outDir / "resolve.txt") << ModuleOf(gdpa(device, "vkCmdSetViewport")) << "\n";
#define DEV(name) auto name = reinterpret_cast<PFN_vk##name>(gdpa(device, "vk" #name))
  DEV(GetDeviceQueue);
  DEV(GetDeviceQueue2);
  DEV(QueueWaitIdle);
  DEV(QueueSubmit);
  DEV(QueueSubmit2);
  DEV(CreateImage);
  DEV(DestroyImage);
  DEV(GetImageMemoryRequirements);
  DEV(AllocateMemory);
  DEV(FreeMemory);
  DEV(BindImageMemory);
  DEV(CreateImageView);
  DEV(DestroyImageView);
  DEV(CreateRenderPass);
  DEV(CreateRenderPass2);
  DEV(DestroyRenderPass);
  DEV(CreateFramebuffer);
  DEV(DestroyFramebuffer);
  DEV(CreateCommandPool);
  DEV(DestroyCommandPool);
  DEV(ResetCommandPool);
  DEV(AllocateCommandBuffers);
  DEV(FreeCommandBuffers);
  DEV(BeginCommandBuffer);
  DEV(EndCommandBuffer);
  DEV(ResetCommandBuffer);
  DEV(CmdSetViewport);
  DEV(CmdBeginRenderPass);
  DEV(CmdNextSubpass);
  DEV(CmdEndRenderPass);
  DEV(CmdBeginRenderPass2);
  DEV(CmdNextSubpass2);
  DEV(CmdEndRenderPass2);
  DEV(CmdBeginRendering);
  DEV(CmdEndRendering);
  DEV(CmdExecuteCommands);
  DEV(CmdPipelineBarrier);
  DEV(CmdPipelineBarrier2);
  DEV(CreateSemaphore);
  DEV(DestroySemaphore);
  DEV(CreateSwapchainKHR);
  DEV(DestroySwapchainKHR);
  DEV(GetSwapchainImagesKHR);
  DEV(AcquireNextImageKHR);
  DEV(QueuePresentKHR);
  DEV(DestroyDevice);
#undef DEV

  // Очереди.
  VkQueue queue = VK_NULL_HANDLE, queue2 = VK_NULL_HANDLE;
  GetDeviceQueue(device, 0, 0, &queue);
  expected["OnGetDeviceQueue"]++;
  if (GetDeviceQueue2) {
    VkDeviceQueueInfo2 qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2};
    qi.queueFamilyIndex = 0;
    qi.queueIndex = 0;
    GetDeviceQueue2(device, &qi, &queue2);
    expected["OnGetDeviceQueue"]++;
    if (queue2 != queue) {
      std::printf("  FAIL: vkGetDeviceQueue2 returned another queue\n");
      return 1;
    }
  }

  // Ресурсы: depth-картинка 64x64, view, проходы v1 и v2 (по два подпрохода), framebuffer.
  const VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
  VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.format = depthFormat;
  imageInfo.extent = {64, 64, 1};
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = 1;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  VkImage image;
  CHECK(CreateImage(device, &imageInfo, nullptr, &image));
  expected["OnImageCreated"]++;
  VkMemoryRequirements req;
  GetImageMemoryRequirements(device, image, &req);
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = FindMemoryType(memProps, req.memoryTypeBits);
  VkDeviceMemory memory;
  CHECK(AllocateMemory(device, &mai, nullptr, &memory));
  CHECK(BindImageMemory(device, image, memory, 0));

  VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  viewInfo.image = image;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format = depthFormat;
  viewInfo.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
  VkImageView view;
  CHECK(CreateImageView(device, &viewInfo, nullptr, &view));
  expected["OnImageViewCreated"]++;

  VkAttachmentDescription att{};
  att.format = depthFormat;
  att.samples = VK_SAMPLE_COUNT_1_BIT;
  att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  // Проходы оставляют картинку в том layout'е, в котором её ждёт dynamic rendering ниже.
  att.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpasses[2] = {};
  for (VkSubpassDescription& sp : subpasses) {
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.pDepthStencilAttachment = &ref;
  }
  // deps[0]: между подпроходами. deps[1]: от предыдущего прохода на той же картинке (второй проход
  // начинается с перехода из UNDEFINED, без этой зависимости sync validation видит WRITE_AFTER_WRITE).
  VkSubpassDependency deps[2] = {};
  for (VkSubpassDependency& dep : deps) {
    dep.srcStageMask = dep.dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                          VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  }
  deps[0].srcSubpass = 0;
  deps[0].dstSubpass = 1;
  deps[1].srcSubpass = VK_SUBPASS_EXTERNAL;
  deps[1].dstSubpass = 0;
  VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rpi.attachmentCount = 1;
  rpi.pAttachments = &att;
  rpi.subpassCount = 2;
  rpi.pSubpasses = subpasses;
  rpi.dependencyCount = 2;
  rpi.pDependencies = deps;
  VkRenderPass pass1;
  CHECK(CreateRenderPass(device, &rpi, nullptr, &pass1));
  expected["OnRenderPassCreated"]++;

  VkAttachmentDescription2 att2{VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2};
  att2.format = att.format;
  att2.samples = att.samples;
  att2.loadOp = att.loadOp;
  att2.storeOp = att.storeOp;
  att2.stencilLoadOp = att.stencilLoadOp;
  att2.stencilStoreOp = att.stencilStoreOp;
  att2.initialLayout = att.initialLayout;
  att2.finalLayout = att.finalLayout;
  VkAttachmentReference2 ref2{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
  ref2.attachment = 0;
  ref2.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  ref2.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  VkSubpassDescription2 subpasses2[2] = {{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2},
                                         {VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2}};
  for (VkSubpassDescription2& sp : subpasses2) {
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.pDepthStencilAttachment = &ref2;
  }
  VkSubpassDependency2 deps2[2] = {{VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2},
                                   {VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2}};
  for (int i = 0; i < 2; ++i) {
    deps2[i].srcSubpass = deps[i].srcSubpass;
    deps2[i].dstSubpass = deps[i].dstSubpass;
    deps2[i].srcStageMask = deps[i].srcStageMask;
    deps2[i].dstStageMask = deps[i].dstStageMask;
    deps2[i].srcAccessMask = deps[i].srcAccessMask;
    deps2[i].dstAccessMask = deps[i].dstAccessMask;
  }
  VkRenderPassCreateInfo2 rpi2{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
  rpi2.attachmentCount = 1;
  rpi2.pAttachments = &att2;
  rpi2.subpassCount = 2;
  rpi2.pSubpasses = subpasses2;
  rpi2.dependencyCount = 2;
  rpi2.pDependencies = deps2;
  VkRenderPass pass2;
  CHECK(CreateRenderPass2(device, &rpi2, nullptr, &pass2));
  expected["OnRenderPass2Created"]++;

  VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fbi.renderPass = pass1;
  fbi.attachmentCount = 1;
  fbi.pAttachments = &view;
  fbi.width = fbi.height = 64;
  fbi.layers = 1;
  VkFramebuffer framebuffer;
  CHECK(CreateFramebuffer(device, &fbi, nullptr, &framebuffer));
  expected["OnFramebufferCreated"]++;

  // Командные буферы: пул, два первичных и один вторичный.
  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = 0;
  VkCommandPool pool;
  CHECK(CreateCommandPool(device, &pci, nullptr, &pool));
  VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = pool;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = 2;
  VkCommandBuffer primary[2];
  CHECK(AllocateCommandBuffers(device, &cai, primary));
  expected["OnAllocateCommandBuffers"]++;
  cai.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
  cai.commandBufferCount = 1;
  VkCommandBuffer secondary;
  CHECK(AllocateCommandBuffers(device, &cai, &secondary));
  expected["OnAllocateCommandBuffers"]++;

  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  VkCommandBufferInheritanceInfo inherit{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
  VkCommandBufferBeginInfo beginSecondary{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  beginSecondary.pInheritanceInfo = &inherit;
  CHECK(BeginCommandBuffer(secondary, &beginSecondary));
  expected["OnBeginCommandBuffer"]++;
  CHECK(EndCommandBuffer(secondary));

  // Первичный буфер A: только записывается (не отправляется).
  VkCommandBuffer cb = primary[0];
  CHECK(BeginCommandBuffer(cb, &begin));
  expected["OnBeginCommandBuffer"]++;
  const VkViewport viewport{0, 0, 64, 64, 1, 0};  // обратная Z, как у DOOM
  CmdSetViewport(cb, 0, 1, &viewport);
  expected["OnCmdSetViewport"]++;
  VkClearValue clear{};
  VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rbi.renderPass = pass1;
  rbi.framebuffer = framebuffer;
  rbi.renderArea.extent = {64, 64};
  rbi.clearValueCount = 1;
  rbi.pClearValues = &clear;
  CmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
  CmdNextSubpass(cb, VK_SUBPASS_CONTENTS_INLINE);
  CmdEndRenderPass(cb);
  rbi.renderPass = pass2;  // совместим с framebuffer'ом pass1
  VkSubpassBeginInfo sbi{VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO};
  sbi.contents = VK_SUBPASS_CONTENTS_INLINE;
  VkSubpassEndInfo sei{VK_STRUCTURE_TYPE_SUBPASS_END_INFO};
  CmdBeginRenderPass2(cb, &rbi, &sbi);
  CmdNextSubpass2(cb, &sbi, &sei);
  CmdEndRenderPass2(cb, &sei);
  expected["OnCmdBeginRenderPass"] += 2;
  expected["OnCmdNextSubpass"] += 2;
  expected["OnCmdEndRenderPass"] += 2;
  if (dynRendering) {
    VkRenderingAttachmentInfo depthAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAtt.imageView = view;
    depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea.extent = {64, 64};
    ri.layerCount = 1;
    ri.pDepthAttachment = &depthAtt;
    CmdBeginRendering(cb, &ri);
    CmdEndRendering(cb);
    expected["OnCmdBeginRendering"]++;
    expected["OnCmdEndRendering"]++;
  }
  CmdExecuteCommands(cb, 1, &secondary);
  expected["OnCmdExecuteCommands"]++;
  // Барьер биннинга из захвата игры (как случай 1 в barrier_test): фикс должен его править.
  const VkAccessFlags gameAccess =
      VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  const VkMemoryBarrier gameMem{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, gameAccess, gameAccess};
  CmdPipelineBarrier(cb,
                     VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     0, 1, &gameMem, 0, nullptr, 0, nullptr);
  expected["OnCmdPipelineBarrier"]++;
  ++fixIndirect;
  if (sync2) {
    VkMemoryBarrier2 m2{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    m2.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    m2.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    m2.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    m2.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &m2;
    CmdPipelineBarrier2(cb, &di);
    expected["OnCmdPipelineBarrier2"]++;
    ++fixIndirect;
  }
  CHECK(EndCommandBuffer(cb));
  CHECK(ResetCommandBuffer(cb, 0));
  expected["OnResetCommandBuffer"]++;

  // Первичный буфер B: пустой, отправляется через vkQueueSubmit и vkQueueSubmit2.
  VkCommandBuffer empty = primary[1];
  CHECK(BeginCommandBuffer(empty, &begin));
  expected["OnBeginCommandBuffer"]++;
  CHECK(EndCommandBuffer(empty));
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &empty;
  CHECK(QueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
  expected["OnQueueSubmitted"]++;
  CHECK(QueueWaitIdle(queue));
  if (sync2) {
    CHECK(BeginCommandBuffer(empty, &begin));
    expected["OnBeginCommandBuffer"]++;
    CHECK(EndCommandBuffer(empty));
    VkCommandBufferSubmitInfo cbsi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbsi.commandBuffer = empty;
    VkSubmitInfo2 si2{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si2.commandBufferInfoCount = 1;
    si2.pCommandBufferInfos = &cbsi;
    CHECK(QueueSubmit2(queue, 1, &si2, VK_NULL_HANDLE));
    expected["OnQueueSubmitted2"]++;
    CHECK(QueueWaitIdle(queue));
  }

  // Swapchain и present в скрытом окне — если есть surface.
  std::string swapchainNote = "skipped: no VK_KHR_win32_surface/VK_KHR_swapchain";
  if (swapchainExt) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"doomfix_event_test";
    RegisterClassW(&wc);
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"doomfix_event_test", WS_POPUP, 0, 0, 64,
                                  64, nullptr, nullptr, wc.hInstance, nullptr);
    VkWin32SurfaceCreateInfoKHR wsi{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    wsi.hinstance = wc.hInstance;
    wsi.hwnd = window;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkBool32 supported = VK_FALSE;
    VkSurfaceCapabilitiesKHR caps{};
    if (window &&
        Load<PFN_vkCreateWin32SurfaceKHR>(instance, "vkCreateWin32SurfaceKHR")(
            instance, &wsi, nullptr, &surface) == VK_SUCCESS &&
        Load<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(instance,
                                                       "vkGetPhysicalDeviceSurfaceSupportKHR")(
            pd, 0, surface, &supported) == VK_SUCCESS &&
        supported &&
        Load<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
            instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")(pd, surface, &caps) ==
            VK_SUCCESS) {
      uint32_t formatCount = 0;
      auto getFormats = Load<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(
          instance, "vkGetPhysicalDeviceSurfaceFormatsKHR");
      getFormats(pd, surface, &formatCount, nullptr);
      std::vector<VkSurfaceFormatKHR> formats(formatCount);
      getFormats(pd, surface, &formatCount, formats.data());
      VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
      sci.surface = surface;
      sci.minImageCount = std::max(2u, caps.minImageCount);
      if (caps.maxImageCount) sci.minImageCount = std::min(sci.minImageCount, caps.maxImageCount);
      sci.imageFormat = formats.at(0).format;
      sci.imageColorSpace = formats.at(0).colorSpace;
      sci.imageExtent = caps.currentExtent.width == UINT32_MAX ? VkExtent2D{64, 64} : caps.currentExtent;
      sci.imageArrayLayers = 1;
      sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
      sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
      sci.preTransform = caps.currentTransform;
      sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
      sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
      sci.clipped = VK_TRUE;
      VkSwapchainKHR swapchain = VK_NULL_HANDLE;
      const VkResult created = CreateSwapchainKHR(device, &sci, nullptr, &swapchain);
      expected["OnCreateSwapchain"]++;
      if (created == VK_SUCCESS) {
        expected["OnSwapchainCreated"]++;
        uint32_t imageCount = 0;
        GetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr);
        std::vector<VkImage> images(imageCount);
        GetSwapchainImagesKHR(device, swapchain, &imageCount, images.data());
        VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore acquired, rendered;
        CHECK(CreateSemaphore(device, &semInfo, nullptr, &acquired));
        CHECK(CreateSemaphore(device, &semInfo, nullptr, &rendered));
        uint32_t index = 0;
        const VkResult acquire =
            AcquireNextImageKHR(device, swapchain, 2000000000ull, acquired, VK_NULL_HANDLE, &index);
        if (acquire == VK_SUCCESS || acquire == VK_SUBOPTIMAL_KHR) {
          CHECK(BeginCommandBuffer(empty, &begin));
          expected["OnBeginCommandBuffer"]++;
          VkImageMemoryBarrier toPresent{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
          toPresent.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
          toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
          toPresent.srcQueueFamilyIndex = toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          toPresent.image = images[index];
          toPresent.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
          CmdPipelineBarrier(empty, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &toPresent);
          expected["OnCmdPipelineBarrier"]++;
          CHECK(EndCommandBuffer(empty));
          const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
          VkSubmitInfo psi{VK_STRUCTURE_TYPE_SUBMIT_INFO};
          psi.waitSemaphoreCount = 1;
          psi.pWaitSemaphores = &acquired;
          psi.pWaitDstStageMask = &waitStage;
          psi.commandBufferCount = 1;
          psi.pCommandBuffers = &empty;
          psi.signalSemaphoreCount = 1;
          psi.pSignalSemaphores = &rendered;
          CHECK(QueueSubmit(queue, 1, &psi, VK_NULL_HANDLE));
          expected["OnQueueSubmitted"]++;
          VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
          present.waitSemaphoreCount = 1;
          present.pWaitSemaphores = &rendered;
          present.swapchainCount = 1;
          present.pSwapchains = &swapchain;
          present.pImageIndices = &index;
          const VkResult presented = QueuePresentKHR(queue, &present);
          expected["OnQueuePresent"]++;
          swapchainNote = "present -> " + std::to_string(presented);
        } else {
          swapchainNote = "swapchain created, acquire -> " + std::to_string(acquire);
        }
        CHECK(QueueWaitIdle(queue));
        DestroySemaphore(device, acquired, nullptr);
        DestroySemaphore(device, rendered, nullptr);
        DestroySwapchainKHR(device, swapchain, nullptr);
        expected["OnDestroySwapchain"]++;
      } else {
        swapchainNote = "vkCreateSwapchainKHR -> " + std::to_string(created);
      }
    } else {
      swapchainNote = "skipped: no surface for the hidden window";
    }
    if (surface) {
      Load<PFN_vkDestroySurfaceKHR>(instance, "vkDestroySurfaceKHR")(instance, surface, nullptr);
    }
    if (window) DestroyWindow(window);
  }
  std::printf("  swapchain/present: %s\n", swapchainNote.c_str());

  FreeCommandBuffers(device, pool, 1, &secondary);
  expected["OnFreeCommandBuffers"]++;
  CHECK(ResetCommandPool(device, pool, 0));
  expected["OnResetCommandPool"]++;
  DestroyCommandPool(device, pool, nullptr);
  expected["OnDestroyCommandPool"]++;
  DestroyFramebuffer(device, framebuffer, nullptr);
  expected["OnDestroyFramebuffer"]++;
  DestroyRenderPass(device, pass1, nullptr);
  DestroyRenderPass(device, pass2, nullptr);
  expected["OnDestroyRenderPass"] += 2;
  DestroyImageView(device, view, nullptr);
  expected["OnDestroyImageView"]++;
  DestroyImage(device, image, nullptr);
  expected["OnDestroyImage"]++;
  FreeMemory(device, memory, nullptr);
  DestroyDevice(device, nullptr);  // indirect-barrier-fix пишет итоговый отчёт ("final")
  expected["OnDestroyDevice"]++;
  Load<PFN_vkDestroyInstance>(instance, "vkDestroyInstance")(instance, nullptr);
  expected["OnDestroyInstance"]++;  // event-trace пишет итоговый отчёт ("final")

  std::ofstream out(outDir / "expected.txt");
  out << "fixIndirect=" << fixIndirect << "\n";
  for (const auto& [name, n] : expected) out << name << "=" << n << "\n";
  return 0;
}

// --- Родитель ----------------------------------------------------------------------------------------

int g_failures = 0;

void Fail(const std::string& message) {
  std::printf("  FAIL: %s\n", message.c_str());
  ++g_failures;
}

void Ok(const std::string& message) { std::printf("  ok   %s\n", message.c_str()); }

bool RunSelf(const fs::path& logDir, bool trace, bool viewer = true) {
  fs::create_directories(logDir);
  SetEnvironmentVariableW(L"DOOMFIX_ENABLE", L"1");
  SetEnvironmentVariableW(L"DOOMFIX_LOG_DIR", logDir.c_str());
  SetEnvironmentVariableW(L"DOOMFIX_INDIRECT_FIX", L"on");
  SetEnvironmentVariableW(L"DOOMFIX_LOG_LEVEL", L"info");
  SetEnvironmentVariableW(L"DOOMFIX_TRACE_EVENTS", trace ? L"1" : nullptr);
  SetEnvironmentVariableW(L"DOOMFIX_COLLISION_VIEWER", viewer ? nullptr : L"0");
  SetEnvironmentVariableW(L"DOOMFIX_CV_TEST", nullptr);
  SetEnvironmentVariableW(L"DOOMFIX_CV_OCCLUSION_TEST", nullptr);

  std::fflush(stdout);  // дочерний процесс пишет в ту же консоль
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --child \"" + logDir.wstring() + L"\"";
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
    Fail("cannot start a child process, error " + std::to_string(GetLastError()));
    return false;
  }
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  if (code != 0) {
    Fail("child process exited with code " + std::to_string(code));
    return false;
  }
  return true;
}

struct RunResult {
  bool ok = false;
  std::vector<std::string> log;
  std::map<std::string, int> expected;
  std::string resolve;      // модуль, в который ведёт vkCmdSetViewport
  std::string fixFinal;     // итоговый отчёт indirect-barrier-fix: "dst N patched M calls: ..."
};

RunResult ReadRun(const fs::path& logDir) {
  RunResult r;
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
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    r.log.push_back(line);
  }
  std::ifstream ex(logDir / "expected.txt");
  for (std::string line; std::getline(ex, line);) {
    const size_t eq = line.find('=');
    if (eq != std::string::npos) r.expected[line.substr(0, eq)] = std::stoi(line.substr(eq + 1));
  }
  std::ifstream(logDir / "resolve.txt") >> r.resolve;
  // Приращения "(+N)" зависят от того, успел ли пройти периодический отчёт, поэтому не сравниваются.
  static const std::regex fixFinal(
      R"(indirect-barrier-fix \[on\] final: barriers with DRAW_INDIRECT dst (\d+) .*patched (\d+) .*calls: (.*))");
  for (const std::string& line : r.log) {
    std::smatch m;
    if (std::regex_search(line, m, fixFinal)) {
      r.fixFinal = "dst " + m[1].str() + " patched " + m[2].str() + " calls: " + m[3].str();
    }
  }
  r.ok = !r.expected.empty();
  if (!r.ok) Fail("no expected.txt from the child in " + logDir.string());
  return r;
}

bool LogHas(const RunResult& r, const std::string& text) {
  for (const std::string& line : r.log) {
    if (line.find(text) != std::string::npos) return true;
  }
  return false;
}

void CheckTrace(const RunResult& r) {
  // Итоговый отчёт event-trace: "event-trace final: OnA=1, OnB=2".
  std::map<std::string, int> got;
  bool found = false;
  static const std::regex finalReport(R"(event-trace final: (.*))");
  static const std::regex item(R"((\w+)=(\d+))");
  for (const std::string& line : r.log) {
    std::smatch m;
    if (!std::regex_search(line, m, finalReport)) continue;
    found = true;
    got.clear();
    const std::string list = m[1];
    for (auto it = std::sregex_iterator(list.begin(), list.end(), item); it != std::sregex_iterator();
         ++it) {
      got[(*it)[1].str()] = std::stoi((*it)[2].str());
    }
  }
  if (!found) {
    Fail("no 'event-trace final' report in the layer log");
    return;
  }
  for (const auto& [name, n] : r.expected) {
    if (name == "fixIndirect") continue;
    const int have = got.count(name) ? got[name] : 0;
    if (have == n) {
      Ok(name + " = " + std::to_string(n));
    } else {
      Fail(name + ": expected " + std::to_string(n) + ", event-trace counted " + std::to_string(have));
    }
  }
  for (const auto& [name, n] : got) {
    if (!r.expected.count(name)) Fail("unexpected event " + name + " = " + std::to_string(n));
  }
  if (LogHas(r, "loader data callback: yes")) {
    Ok("DeviceData has the loader's SetDeviceLoaderData callback");
  } else {
    Fail("event-trace did not report a SetDeviceLoaderData callback");
  }
  if (LogHas(r, "event-trace: first vkQueueSubmit on queue family 0 index 0 -> 0")) {
    Ok("vkQueueSubmit: the queue map knows family 0 index 0");
  } else {
    Fail("vkQueueSubmit did not see the queue as family 0 index 0");
  }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc > 2 && std::wcscmp(argv[1], L"--child") == 0) return RunChild(argv[2]);

  wchar_t temp[MAX_PATH];
  GetTempPathW(MAX_PATH, temp);
  const fs::path root = fs::path(temp) / L"doomfix_event_test" / std::to_wstring(GetCurrentProcessId());

  std::printf("trace (DOOMFIX_TRACE_EVENTS=1)\n");
  RunResult trace, plain;
  if (RunSelf(root / L"trace", true)) trace = ReadRun(root / L"trace");
  if (trace.ok) CheckTrace(trace);

  // Фикс: те же итоговые счётчики, что и с event-trace, и ожидаемое число барьеров.
  auto checkFix = [&](RunResult& run, const char* label) {
    const int indirect = run.expected["fixIndirect"];
    const std::string want =
        "dst " + std::to_string(indirect) + " patched " + std::to_string(indirect) + " calls: ";
    if (run.fixFinal.rfind(want, 0) == 0) {
      Ok("indirect-barrier-fix: " + run.fixFinal);
    } else {
      Fail("indirect-barrier-fix final report: expected '" + want + "...', got '" + run.fixFinal + "'");
    }
    if (trace.ok && trace.fixFinal != run.fixFinal) {
      Fail(std::string("indirect-barrier-fix counted differently (") + label + "):\n        trace: " +
           trace.fixFinal + "\n        " + label + ": " + run.fixFinal);
    } else if (trace.ok) {
      Ok(std::string("indirect-barrier-fix counts are the same with event-trace and ") + label);
    }
  };
  const bool topLayer = trace.ok && trace.resolve == "DESRU.Visuals.dll";

  std::printf("plain (no DOOMFIX_TRACE_EVENTS; collision-viewer is on by default)\n");
  if (RunSelf(root / L"plain", false)) plain = ReadRun(root / L"plain");
  if (plain.ok) {
    if (LogHas(plain, "event-trace")) {
      Fail("event-trace is active without DOOMFIX_TRACE_EVENTS");
    } else {
      Ok("event-trace is off");
    }
    if (LogHas(plain, "feature: collision-viewer") && !LogHas(plain, "The viewer is turned off")) {
      Ok("collision-viewer is loaded and reported no errors");
    } else {
      Fail("collision-viewer is not loaded or reported an error");
    }
    // collision-viewer следит за viewport: перехват ему нужен.
    if (topLayer && plain.resolve != "DESRU.Visuals.dll") {
      Fail("vkCmdSetViewport bypasses the layer although collision-viewer needs it");
    }
    checkFix(plain, "collision-viewer");
  }

  std::printf("noviewer (DOOMFIX_COLLISION_VIEWER=0)\n");
  RunResult noviewer;
  if (RunSelf(root / L"noviewer", false, false)) noviewer = ReadRun(root / L"noviewer");
  if (noviewer.ok) {
    if (LogHas(noviewer, "collision-viewer")) {
      Fail("collision-viewer is active with DOOMFIX_COLLISION_VIEWER=0");
    } else {
      Ok("collision-viewer is off");
    }
    // Маска интересов: без event-trace и collision-viewer запись viewport никому не нужна.
    if (topLayer) {
      if (noviewer.resolve != "DESRU.Visuals.dll") {
        Ok("vkCmdSetViewport bypasses the layer (" + noviewer.resolve + ")");
      } else {
        Fail("vkCmdSetViewport is intercepted although no feature needs it");
      }
    } else {
      std::printf("  skip vkCmdSetViewport resolves to %s with event-trace: doomfix is not the top layer\n",
                  trace.resolve.c_str());
    }
    checkFix(noviewer, "no collision-viewer");
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
