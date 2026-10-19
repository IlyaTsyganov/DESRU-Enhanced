// Based on doom-eternal-collision-viewer by IlyaTsyganov (commit 43ac57d), used with the author's permission.

#include "features/collision_viewer/overlay_renderer.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

#include "common/log.h"
#include "features/collision_viewer/overlay_math.h"
#include "features/collision_viewer/readback.h"
#include "features/collision_viewer/shaders_spirv.h"
#include "layer/dispatch.h"
#include "layer/present_edit.h"
#include "runtime.h"

namespace doomfix::cv {
namespace {

constexpr uint32_t kInitialVertexCapacity = 4096;
constexpr auto kReportInterval = std::chrono::seconds(10);
// Сколько ждать кадры оверлея при уничтожении swapchain/картинки/устройства (не в present).
constexpr uint64_t kDestroyWaitNs = 1000000000ull;
// Тестовый режим: сколько ждать отрисованный кадр, чтобы прочитать пиксели.
constexpr uint64_t kReadbackWaitNs = 5000000000ull;

void Check(VkResult result, const char* what) {
  if (result != VK_SUCCESS) {
    throw std::runtime_error(std::string(what) + " failed: VkResult " + std::to_string(result));
  }
}

bool IsBgra(VkFormat format) {
  return format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
}

bool IsReadableFormat(VkFormat format) {
  return IsBgra(format) || format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB;
}

uint32_t FindMemoryType(const VkPhysicalDeviceMemoryProperties& props, uint32_t bits,
                        VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred) {
  uint32_t fallback = UINT32_MAX;
  for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
    if (!(bits & (1u << i))) continue;
    const VkMemoryPropertyFlags flags = props.memoryTypes[i].propertyFlags;
    if ((flags & required) != required) continue;
    if ((flags & preferred) == preferred) return i;
    if (fallback == UINT32_MAX) fallback = i;
  }
  return fallback;
}

// Барьер картинки глубины игры: перед отрисовкой — в DEPTH_STENCIL_READ_ONLY_OPTIMAL для чтения в
// шейдере, после — обратно в layout, в котором её оставила игра.
void DepthSamplingBarrier(const DeviceDispatch& vk, VkCommandBuffer cb, const DepthImage& image,
                          bool restore) {
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.image = image.handle;
  barrier.subresourceRange = {DepthAspects(image.format), 0, 1, 0, 1};
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.oldLayout = restore ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : image.layout;
  barrier.newLayout = restore ? image.layout : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
  barrier.srcAccessMask =
      restore ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  barrier.dstAccessMask =
      restore ? VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT;
  vk.CmdPipelineBarrier(cb,
                        restore ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        restore ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        0, 0, nullptr, 0, nullptr, 1, &barrier);
}

// Передача картинки swapchain между семействами очередей (release на одной очереди, acquire на
// другой); layout остаётся PRESENT_SRC_KHR.
void OwnershipBarrier(const DeviceDispatch& vk, VkCommandBuffer cb, VkImage image, uint32_t from, uint32_t to,
                      VkPipelineStageFlags srcStage, VkAccessFlags srcAccess, VkPipelineStageFlags dstStage,
                      VkAccessFlags dstAccess) {
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.srcAccessMask = srcAccess;
  barrier.dstAccessMask = dstAccess;
  barrier.oldLayout = barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  barrier.srcQueueFamilyIndex = from;
  barrier.dstQueueFamilyIndex = to;
  barrier.image = image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vk.CmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

}  // namespace

OverlayRenderer::OverlayRenderer(DeviceData& device, DepthTracker& tracker, ICameraSource& camera,
                                 ISceneSource& scene, const TestOptions& test)
    : device_(device), tracker_(tracker), camera_(camera), scene_(scene), test_(test) {}

void OverlayRenderer::SetOverlayQueue(VkQueue queue, uint32_t family) {
  std::lock_guard lock(mutex_);
  overlayQueue_ = queue;
  overlayFamily_ = family;
}

// --- Swapchain ------------------------------------------------------------------------------------

void OverlayRenderer::PrepareSwapchain(VkSwapchainCreateInfoKHR& info) {
  const auto caps = device_.instance ? device_.instance->vk.GetPhysicalDeviceSurfaceCapabilitiesKHR : nullptr;
  VkSurfaceCapabilitiesKHR surface = {};
  if (!caps || caps(device_.physicalDevice, info.surface, &surface) != VK_SUCCESS) {
    DF_LOG_WARN("collision-viewer: cannot query the surface; swapchain image usage left as is");
    return;
  }
  VkImageUsageFlags wanted = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  if (test_.readback) wanted |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  const VkImageUsageFlags add = wanted & ~info.imageUsage & surface.supportedUsageFlags;
  const VkImageUsageFlags missing = wanted & ~info.imageUsage & ~surface.supportedUsageFlags;
  info.imageUsage |= add;
  if (add) {
    DF_LOG_INFO("collision-viewer: swapchain image usage 0x%x (the overlay added 0x%x)", info.imageUsage,
                add);
  }
  if (missing & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) {
    DF_LOG_WARN("collision-viewer: the surface does not allow drawing into swapchain images "
                "(no COLOR_ATTACHMENT usage); the overlay will not be drawn on this swapchain");
  }
}

void OverlayRenderer::OnSwapchainCreated(VkSwapchainKHR swapchain, const VkSwapchainCreateInfoKHR& info) {
  Swapchain swap;
  swap.handle = swapchain;
  swap.extent = info.imageExtent;
  swap.format = info.imageFormat;
  swap.colorAttachment = (info.imageUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0;
  swap.transferSrc = (info.imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
  swap.sharing = info.imageSharingMode;
  if (info.imageSharingMode == VK_SHARING_MODE_CONCURRENT && info.pQueueFamilyIndices) {
    swap.sharingFamilies.assign(info.pQueueFamilyIndices,
                                info.pQueueFamilyIndices + info.queueFamilyIndexCount);
  }
  if (device_.vk.GetSwapchainImagesKHR) {
    uint32_t count = 0;
    device_.vk.GetSwapchainImagesKHR(device_.handle, swapchain, &count, nullptr);
    swap.images.resize(count);
    if (device_.vk.GetSwapchainImagesKHR(device_.handle, swapchain, &count, swap.images.data()) !=
        VK_SUCCESS) {
      swap.images.clear();
    }
    swap.images.resize(std::min<size_t>(count, swap.images.size()));
  }
  std::string families;
  for (uint32_t f : swap.sharingFamilies) families += (families.empty() ? "" : ",") + std::to_string(f);
  DF_LOG_INFO("collision-viewer: swapchain %ux%u, format %d, %zu images, usage 0x%x, color attachment: %s, "
              "sharing %s%s%s",
              swap.extent.width, swap.extent.height, static_cast<int>(swap.format), swap.images.size(),
              info.imageUsage, swap.colorAttachment ? "yes" : "no",
              swap.sharing == VK_SHARING_MODE_CONCURRENT ? "concurrent" : "exclusive",
              families.empty() ? "" : ", families ", families.c_str());
  std::lock_guard lock(mutex_);
  swapchains_[swapchain] = std::move(swap);
}

void OverlayRenderer::OnSwapchainDestroyed(VkSwapchainKHR swapchain) {
  std::lock_guard lock(mutex_);
  auto it = swapchains_.find(swapchain);
  if (it == swapchains_.end()) return;
  Destroy(it->second);
  swapchains_.erase(it);
}

// --- Ресурсы ---------------------------------------------------------------------------------------

void OverlayRenderer::CreateHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer* buffer,
                                       VkDeviceMemory* memory, void** mapped) {
  const DeviceDispatch& vk = device_.vk;
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = size;
  info.usage = usage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  Check(vk.CreateBuffer(device_.handle, &info, nullptr, buffer), "vkCreateBuffer");
  VkMemoryRequirements requirements = {};
  vk.GetBufferMemoryRequirements(device_.handle, *buffer, &requirements);
  const VkMemoryPropertyFlags hostFlags =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  const uint32_t type = FindMemoryType(device_.memoryProperties, requirements.memoryTypeBits, hostFlags,
                                       hostFlags);
  if (type == UINT32_MAX) throw std::runtime_error("no host-visible coherent memory type");
  VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocate.allocationSize = requirements.size;
  allocate.memoryTypeIndex = type;
  Check(vk.AllocateMemory(device_.handle, &allocate, nullptr, memory), "vkAllocateMemory");
  Check(vk.BindBufferMemory(device_.handle, *buffer, *memory, 0), "vkBindBufferMemory");
  Check(vk.MapMemory(device_.handle, *memory, 0, VK_WHOLE_SIZE, 0, mapped), "vkMapMemory");
}

void OverlayRenderer::DestroyHostBuffer(VkBuffer* buffer, VkDeviceMemory* memory, void** mapped) {
  const DeviceDispatch& vk = device_.vk;
  if (*mapped) vk.UnmapMemory(device_.handle, *memory);
  if (*buffer) vk.DestroyBuffer(device_.handle, *buffer, nullptr);
  if (*memory) vk.FreeMemory(device_.handle, *memory, nullptr);
  *buffer = VK_NULL_HANDLE;
  *memory = VK_NULL_HANDLE;
  *mapped = nullptr;
}

void OverlayRenderer::EnsureVertexCapacity(Frame& frame, uint32_t count) {
  if (count <= frame.vertexCapacity && frame.vertices) return;
  const uint64_t grown = uint64_t(frame.vertexCapacity) + frame.vertexCapacity / 2;
  const uint32_t capacity = static_cast<uint32_t>(std::min<uint64_t>(std::numeric_limits<uint32_t>::max(),
      std::max<uint64_t>(std::max(kInitialVertexCapacity, count), grown)));
  // Кадр свободен (его фенс уже проверен), буфер можно пересоздать.
  DestroyHostBuffer(&frame.vertices, &frame.vertexMemory, &frame.vertexMapped);
  frame.vertexCapacity = 0;
  CreateHostBuffer(VkDeviceSize(capacity) * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                   &frame.vertices, &frame.vertexMemory, &frame.vertexMapped);
  frame.vertexCapacity = capacity;
}

VkPipeline OverlayRenderer::CreatePipeline(Swapchain& swap, VkShaderModule vertex,
                                           VkShaderModule fragment, VkPrimitiveTopology topology) {
  VkPipelineShaderStageCreateInfo stages[2] = {};
  for (auto& stage : stages) {
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.pName = "main";
  }
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vertex;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fragment;
  const VkVertexInputBindingDescription binding = {0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
  const VkVertexInputAttributeDescription attributes[2] = {
      {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
      {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, color)}};
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vi.vertexBindingDescriptionCount = 1;
  vi.pVertexBindingDescriptions = &binding;
  vi.vertexAttributeDescriptionCount = 2;
  vi.pVertexAttributeDescriptions = attributes;
  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = topology;
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = 1;
  vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  // Глубину оверлей не пишет и не проверяет аппаратно: окклюзию по глубине игры считает шейдер.
  VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  VkPipelineColorBlendAttachmentState blend = {};
  blend.blendEnable = VK_TRUE;
  blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blend.colorBlendOp = VK_BLEND_OP_ADD;
  blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blend.alphaBlendOp = VK_BLEND_OP_ADD;
  blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                         VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo color{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  color.attachmentCount = 1;
  color.pAttachments = &blend;
  const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamicStates;
  VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.stageCount = 2;
  info.pStages = stages;
  info.pVertexInputState = &vi;
  info.pInputAssemblyState = &ia;
  info.pViewportState = &vp;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &ms;
  info.pDepthStencilState = &depth;
  info.pColorBlendState = &color;
  info.pDynamicState = &dynamic;
  info.layout = swap.layout;
  info.renderPass = swap.pass;
  VkPipeline pipeline = VK_NULL_HANDLE;
  Check(device_.vk.CreateGraphicsPipelines(device_.handle, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline),
        "vkCreateGraphicsPipelines");
  return pipeline;
}

void OverlayRenderer::Initialize(Swapchain& swap, uint32_t family) {
  const DeviceDispatch& vk = device_.vk;
  const VkDevice dev = device_.handle;
  if (!device_.setDeviceLoaderData) {
    throw std::runtime_error("the loader gave no SetDeviceLoaderData callback; own command buffers "
                             "cannot be used");
  }
  if (swap.images.empty()) throw std::runtime_error("swapchain has no images");
  swap.family = family;
  swap.frames.resize(swap.images.size());
  for (size_t i = 0; i < swap.images.size(); ++i) swap.frames[i].image = swap.images[i];

  // Render pass поверх готового кадра: LOAD/STORE, картинка остаётся в PRESENT_SRC.
  VkAttachmentDescription attachment = {};
  attachment.format = swap.format;
  attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachment.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  const VkAttachmentReference reference = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass = {};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &reference;
  VkSubpassDependency dependency = {};
  dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
  dependency.dstSubpass = 0;
  dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  pass.attachmentCount = 1;
  pass.pAttachments = &attachment;
  pass.subpassCount = 1;
  pass.pSubpasses = &subpass;
  pass.dependencyCount = 1;
  pass.pDependencies = &dependency;
  Check(vk.CreateRenderPass(dev, &pass, nullptr, &swap.pass), "vkCreateRenderPass");

  // Дескриптор: буфер глубины игры (или заглушка) как sampler2D.
  const VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                                VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo setLayout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  setLayout.bindingCount = 1;
  setLayout.pBindings = &binding;
  Check(vk.CreateDescriptorSetLayout(dev, &setLayout, nullptr, &swap.setLayout),
        "vkCreateDescriptorSetLayout");
  const auto frameCount = static_cast<uint32_t>(swap.frames.size());
  const VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, frameCount};
  VkDescriptorPoolCreateInfo descriptorPool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  descriptorPool.maxSets = frameCount;
  descriptorPool.poolSizeCount = 1;
  descriptorPool.pPoolSizes = &poolSize;
  Check(vk.CreateDescriptorPool(dev, &descriptorPool, nullptr, &swap.descriptorPool),
        "vkCreateDescriptorPool");
  std::vector<VkDescriptorSetLayout> layouts(frameCount, swap.setLayout);
  std::vector<VkDescriptorSet> sets(frameCount);
  VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  setInfo.descriptorPool = swap.descriptorPool;
  setInfo.descriptorSetCount = frameCount;
  setInfo.pSetLayouts = layouts.data();
  Check(vk.AllocateDescriptorSets(dev, &setInfo, sets.data()), "vkAllocateDescriptorSets");
  VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
  sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler.maxLod = 0.0f;
  Check(vk.CreateSampler(dev, &sampler, nullptr, &swap.sampler), "vkCreateSampler");

  const VkPushConstantRange range = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                     sizeof(PushConstants)};
  VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout.setLayoutCount = 1;
  layout.pSetLayouts = &swap.setLayout;
  layout.pushConstantRangeCount = 1;
  layout.pPushConstantRanges = &range;
  Check(vk.CreatePipelineLayout(dev, &layout, nullptr, &swap.layout), "vkCreatePipelineLayout");

  VkShaderModule vertex = VK_NULL_HANDLE;
  VkShaderModule fragment = VK_NULL_HANDLE;
  VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  shader.codeSize = sizeof(kVertexShader);
  shader.pCode = kVertexShader;
  Check(vk.CreateShaderModule(dev, &shader, nullptr, &vertex), "vkCreateShaderModule (vertex)");
  shader.codeSize = sizeof(kFragmentShader);
  shader.pCode = kFragmentShader;
  try {
    Check(vk.CreateShaderModule(dev, &shader, nullptr, &fragment), "vkCreateShaderModule (fragment)");
    swap.fill = CreatePipeline(swap, vertex, fragment, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    swap.lines = CreatePipeline(swap, vertex, fragment, VK_PRIMITIVE_TOPOLOGY_LINE_LIST);
  } catch (...) {
    vk.DestroyShaderModule(dev, vertex, nullptr);
    if (fragment) vk.DestroyShaderModule(dev, fragment, nullptr);
    throw;
  }
  vk.DestroyShaderModule(dev, vertex, nullptr);
  vk.DestroyShaderModule(dev, fragment, nullptr);

  // Заглушка 1x1 для дескриптора, пока нет буфера глубины игры (шейдер её не читает).
  VkImageCreateInfo dummy{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  dummy.imageType = VK_IMAGE_TYPE_2D;
  dummy.format = VK_FORMAT_R8G8B8A8_UNORM;
  dummy.extent = {1, 1, 1};
  dummy.mipLevels = dummy.arrayLayers = 1;
  dummy.samples = VK_SAMPLE_COUNT_1_BIT;
  dummy.tiling = VK_IMAGE_TILING_OPTIMAL;
  dummy.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
  dummy.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  dummy.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  Check(vk.CreateImage(dev, &dummy, nullptr, &swap.dummyImage), "vkCreateImage (placeholder)");
  VkMemoryRequirements requirements = {};
  vk.GetImageMemoryRequirements(dev, swap.dummyImage, &requirements);
  const uint32_t type = FindMemoryType(device_.memoryProperties, requirements.memoryTypeBits, 0,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (type == UINT32_MAX) throw std::runtime_error("no memory type for the placeholder image");
  VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocate.allocationSize = requirements.size;
  allocate.memoryTypeIndex = type;
  Check(vk.AllocateMemory(dev, &allocate, nullptr, &swap.dummyMemory), "vkAllocateMemory (placeholder)");
  Check(vk.BindImageMemory(dev, swap.dummyImage, swap.dummyMemory, 0), "vkBindImageMemory");
  VkImageViewCreateInfo dummyView{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  dummyView.image = swap.dummyImage;
  dummyView.viewType = VK_IMAGE_VIEW_TYPE_2D;
  dummyView.format = dummy.format;
  dummyView.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  Check(vk.CreateImageView(dev, &dummyView, nullptr, &swap.dummyView), "vkCreateImageView (placeholder)");

  VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool.queueFamilyIndex = family;
  Check(vk.CreateCommandPool(dev, &pool, nullptr, &swap.pool), "vkCreateCommandPool");
  std::vector<VkCommandBuffer> buffers(frameCount);
  VkCommandBufferAllocateInfo buffersInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  buffersInfo.commandPool = swap.pool;
  buffersInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  buffersInfo.commandBufferCount = frameCount;
  Check(vk.AllocateCommandBuffers(dev, &buffersInfo, buffers.data()), "vkAllocateCommandBuffers");

  const bool readback = test_.readback && swap.transferSrc && IsReadableFormat(swap.format);
  if (test_.readback && !readback) {
    DF_LOG_WARN("collision-viewer test: cannot read swapchain images back (usage 0x%s, format %d)",
                swap.transferSrc ? "TRANSFER_SRC" : "-", static_cast<int>(swap.format));
  }
  for (uint32_t i = 0; i < frameCount; ++i) {
    Frame& frame = swap.frames[i];
    frame.commands = buffers[i];
    frame.depthSet = sets[i];
    // Свой командный буфер — диспетчеризуемый объект: без таблицы loader'а его нельзя передать вниз.
    Check(device_.setDeviceLoaderData(dev, frame.commands), "SetDeviceLoaderData");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = frame.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = swap.format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    Check(vk.CreateImageView(dev, &view, nullptr, &frame.view), "vkCreateImageView");
    VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebuffer.renderPass = swap.pass;
    framebuffer.attachmentCount = 1;
    framebuffer.pAttachments = &frame.view;
    framebuffer.width = swap.extent.width;
    framebuffer.height = swap.extent.height;
    framebuffer.layers = 1;
    Check(vk.CreateFramebuffer(dev, &framebuffer, nullptr, &frame.framebuffer), "vkCreateFramebuffer");
    EnsureVertexCapacity(frame, kInitialVertexCapacity);
    if (readback) {
      CreateHostBuffer(VkDeviceSize(swap.extent.width) * swap.extent.height * 4,
                       VK_BUFFER_USAGE_TRANSFER_DST_BIT, &frame.readback, &frame.readbackMemory,
                       &frame.readbackMapped);
    }
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    Check(vk.CreateSemaphore(dev, &semaphore, nullptr, &frame.complete), "vkCreateSemaphore");
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    Check(vk.CreateFence(dev, &fence, nullptr, &frame.fence), "vkCreateFence");
  }
  swap.initialized = true;
  DF_LOG_INFO("collision-viewer: overlay renderer ready: swapchain %ux%u, %u images, queue family %u%s",
              swap.extent.width, swap.extent.height, frameCount, family,
              readback ? ", reading frames back (test)" : "");
}

bool OverlayRenderer::NeedsTransfer(const Swapchain& swap, uint32_t presentFamily) const {
  if (presentFamily == swap.family) return false;
  if (swap.sharing != VK_SHARING_MODE_CONCURRENT) return true;
  const auto& f = swap.sharingFamilies;
  const bool both = std::find(f.begin(), f.end(), presentFamily) != f.end() &&
                    std::find(f.begin(), f.end(), swap.family) != f.end();
  return !both;
}

bool OverlayRenderer::PrepareTransfer(Swapchain& swap, uint32_t presentFamily) {
  if (swap.transferFamily == presentFamily) return true;
  if (swap.transferFamily != VK_QUEUE_FAMILY_IGNORED) return false;  // уже под другое семейство
  const DeviceDispatch& vk = device_.vk;
  const VkDevice dev = device_.handle;
  VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool.queueFamilyIndex = presentFamily;
  Check(vk.CreateCommandPool(dev, &pool, nullptr, &swap.transferPool), "vkCreateCommandPool (present family)");
  const auto frameCount = static_cast<uint32_t>(swap.frames.size());
  std::vector<VkCommandBuffer> buffers(size_t(frameCount) * 2);
  VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  allocate.commandPool = swap.transferPool;
  allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocate.commandBufferCount = frameCount * 2;
  Check(vk.AllocateCommandBuffers(dev, &allocate, buffers.data()), "vkAllocateCommandBuffers (present family)");
  swap.transferFamily = presentFamily;  // с этого места Destroy освободит пул
  for (uint32_t i = 0; i < frameCount; ++i) {
    Frame& frame = swap.frames[i];
    frame.release = buffers[size_t(i) * 2];
    frame.acquire = buffers[size_t(i) * 2 + 1];
    Check(device_.setDeviceLoaderData(dev, frame.release), "SetDeviceLoaderData");
    Check(device_.setDeviceLoaderData(dev, frame.acquire), "SetDeviceLoaderData");
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    Check(vk.CreateSemaphore(dev, &semaphore, nullptr, &frame.released), "vkCreateSemaphore");
    Check(vk.CreateSemaphore(dev, &semaphore, nullptr, &frame.drawn), "vkCreateSemaphore");
    // Картинка и семейства у кадра постоянные: буферы записываются один раз и отправляются каждый кадр.
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    Check(vk.BeginCommandBuffer(frame.release, &begin), "vkBeginCommandBuffer");
    OwnershipBarrier(vk, frame.release, frame.image, presentFamily, swap.family, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                     0, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    Check(vk.EndCommandBuffer(frame.release), "vkEndCommandBuffer");
    Check(vk.BeginCommandBuffer(frame.acquire, &begin), "vkBeginCommandBuffer");
    OwnershipBarrier(vk, frame.acquire, frame.image, swap.family, presentFamily, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                     0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0);
    Check(vk.EndCommandBuffer(frame.acquire), "vkEndCommandBuffer");
  }
  DF_LOG_INFO("collision-viewer: swapchain images are passed between queue families %u (present) and %u "
              "(overlay) for each overlay frame",
              presentFamily, swap.family);
  return true;
}

bool OverlayRenderer::WaitFrames(const Swapchain& swap, uint64_t timeoutNs, VkImage onlyReading) {
  bool ok = true;
  for (const Frame& frame : swap.frames) {
    if (!frame.fence) continue;
    if (onlyReading && frame.boundDepth != onlyReading) continue;
    if (device_.vk.WaitForFences(device_.handle, 1, &frame.fence, VK_TRUE, timeoutNs) != VK_SUCCESS) {
      ok = false;
    }
  }
  return ok;
}

void OverlayRenderer::Destroy(Swapchain& swap) {
  const DeviceDispatch& vk = device_.vk;
  const VkDevice dev = device_.handle;
  if (!WaitFrames(swap, kDestroyWaitNs)) {
    // Уничтожать занятые GPU объекты нельзя; лучше оставить их (утечка), чем уронить игру.
    DF_LOG_ERROR("collision-viewer: overlay frames did not finish in 1 s; their objects are leaked");
    swap.frames.clear();
    return;
  }
  for (Frame& frame : swap.frames) {
    DestroyHostBuffer(&frame.vertices, &frame.vertexMemory, &frame.vertexMapped);
    DestroyHostBuffer(&frame.readback, &frame.readbackMemory, &frame.readbackMapped);
    if (frame.fence) vk.DestroyFence(dev, frame.fence, nullptr);
    if (frame.complete) vk.DestroySemaphore(dev, frame.complete, nullptr);
    if (frame.released) vk.DestroySemaphore(dev, frame.released, nullptr);
    if (frame.drawn) vk.DestroySemaphore(dev, frame.drawn, nullptr);
    if (frame.framebuffer) vk.DestroyFramebuffer(dev, frame.framebuffer, nullptr);
    if (frame.view) vk.DestroyImageView(dev, frame.view, nullptr);
  }
  swap.frames.clear();
  if (swap.pool) vk.DestroyCommandPool(dev, swap.pool, nullptr);
  if (swap.transferPool) vk.DestroyCommandPool(dev, swap.transferPool, nullptr);
  swap.transferPool = VK_NULL_HANDLE;
  swap.transferFamily = VK_QUEUE_FAMILY_IGNORED;
  if (swap.fill) vk.DestroyPipeline(dev, swap.fill, nullptr);
  if (swap.lines) vk.DestroyPipeline(dev, swap.lines, nullptr);
  if (swap.layout) vk.DestroyPipelineLayout(dev, swap.layout, nullptr);
  if (swap.descriptorPool) vk.DestroyDescriptorPool(dev, swap.descriptorPool, nullptr);
  if (swap.sampler) vk.DestroySampler(dev, swap.sampler, nullptr);
  if (swap.setLayout) vk.DestroyDescriptorSetLayout(dev, swap.setLayout, nullptr);
  if (swap.pass) vk.DestroyRenderPass(dev, swap.pass, nullptr);
  if (swap.dummyView) vk.DestroyImageView(dev, swap.dummyView, nullptr);
  if (swap.dummyImage) vk.DestroyImage(dev, swap.dummyImage, nullptr);
  if (swap.dummyMemory) vk.FreeMemory(dev, swap.dummyMemory, nullptr);
  swap.pool = VK_NULL_HANDLE;
  swap.fill = swap.lines = VK_NULL_HANDLE;
  swap.layout = VK_NULL_HANDLE;
  swap.descriptorPool = VK_NULL_HANDLE;
  swap.sampler = VK_NULL_HANDLE;
  swap.setLayout = VK_NULL_HANDLE;
  swap.pass = VK_NULL_HANDLE;
  swap.dummyView = VK_NULL_HANDLE;
  swap.dummyImage = VK_NULL_HANDLE;
  swap.dummyMemory = VK_NULL_HANDLE;
  swap.dummyReady = false;
  swap.initialized = false;
}

void OverlayRenderer::OnDepthImageDestroyed(VkImage image, VkImageView sampleView) {
  std::lock_guard lock(mutex_);
  bool ok = true;
  for (auto& [handle, swap] : swapchains_) {
    ok = WaitFrames(swap, kDestroyWaitNs, image) && ok;
    for (Frame& frame : swap.frames) {
      if (frame.boundView == sampleView) frame.boundView = VK_NULL_HANDLE;
      if (frame.boundDepth == image) frame.boundDepth = VK_NULL_HANDLE;
    }
  }
  if (!sampleView) return;
  if (ok) {
    device_.vk.DestroyImageView(device_.handle, sampleView, nullptr);
  } else {
    DF_LOG_ERROR("collision-viewer: an overlay frame reading a destroyed depth image did not finish; "
                 "its view is leaked");
  }
}

void OverlayRenderer::Shutdown() {
  std::lock_guard lock(mutex_);
  LogStatistics(true);
  for (auto& [handle, swap] : swapchains_) Destroy(swap);
  swapchains_.clear();
  for (VkImageView view : tracker_.TakeSampleViews()) {
    device_.vk.DestroyImageView(device_.handle, view, nullptr);
  }
}

// --- Present ----------------------------------------------------------------------------------------

void OverlayRenderer::LogDepthChoice(bool found, const DepthImage& depth) {
  const auto now = Clock::now();
  if (firstPresent_ == Clock::time_point{}) firstPresent_ = now;
  if (found) {
    lastDepthSeen_ = now;
    lostLogged_ = false;
    if (depth.handle == loggedDepth_) return;
    // Игра может чередовать буферы глубины по кадрам: не чаще раза в 10 с, остальные смены — счётчиком.
    if (everFoundDepth_ && now - lastDepthLog_ < kReportInterval) {
      ++depthChanges_;
      return;
    }
    DF_LOG_INFO("collision-viewer: %s screen depth buffer %p: format %d, %ux%u, viewport (%.0f, %.0f, "
                "%.0f, %.0f) depth %.3f..%.3f, layout %d, queue family %u, submit %llu (%u changes since "
                "the last report)",
                everFoundDepth_ ? "now using" : "found the", reinterpret_cast<void*>(depth.handle),
                static_cast<int>(depth.format), depth.extent.width, depth.extent.height, depth.viewport.x,
                depth.viewport.y, depth.viewport.width, depth.viewport.height, depth.viewport.minDepth,
                depth.viewport.maxDepth, static_cast<int>(depth.layout), depth.owner,
                static_cast<unsigned long long>(depth.written), depthChanges_);
    everFoundDepth_ = true;
    loggedDepth_ = depth.handle;
    lastDepthLog_ = now;
    depthChanges_ = 0;
    return;
  }
  if (everFoundDepth_ && !lostLogged_ && now - lastDepthSeen_ >= kReportInterval) {
    DF_LOG_INFO("collision-viewer: no fresh screen depth buffer for 10 s (%zu depth images tracked, "
                "%llu submits)",
                tracker_.DepthImageCount(), static_cast<unsigned long long>(tracker_.SubmitCount()));
    lostLogged_ = true;
    loggedDepth_ = VK_NULL_HANDLE;
  } else if (!everFoundDepth_ && !waitingLogged_ && now - firstPresent_ >= kReportInterval) {
    DF_LOG_INFO("collision-viewer: still waiting for a screen depth buffer after 10 s of presents (%zu "
                "depth images tracked, %llu submits)",
                tracker_.DepthImageCount(), static_cast<unsigned long long>(tracker_.SubmitCount()));
    waitingLogged_ = true;
  }
}

void OverlayRenderer::LogStatistics(bool force) {
  const auto now = Clock::now();
  if (!force && now - lastStatistics_ < kReportInterval) return;
  lastStatistics_ = now;
  if (drawn_ == reportedDrawn_ && skippedBusy_ == reportedSkipped_) return;
  DF_LOG_INFO("collision-viewer: overlay frames drawn %llu (+%llu), skipped because the previous overlay "
              "frame was still on the GPU %llu (+%llu)",
              static_cast<unsigned long long>(drawn_),
              static_cast<unsigned long long>(drawn_ - reportedDrawn_),
              static_cast<unsigned long long>(skippedBusy_),
              static_cast<unsigned long long>(skippedBusy_ - reportedSkipped_));
  reportedDrawn_ = drawn_;
  reportedSkipped_ = skippedBusy_;
}

void OverlayRenderer::EndPresent() { tracker_.MarkPresent(); }

void OverlayRenderer::Present(const QueueData& queue, PresentEdit& edit) {
  std::lock_guard lock(mutex_);
  const VkPresentInfoKHR& present = edit.Original();
  if (present.swapchainCount != 1 || !present.pSwapchains || !present.pImageIndices) return;
  auto found = swapchains_.find(present.pSwapchains[0]);
  if (found == swapchains_.end()) return;
  Swapchain& swap = found->second;
  const uint32_t imageIndex = present.pImageIndices[0];

  // Где рисовать: на очереди present, если она умеет графику, иначе на своей графической очереди.
  const uint32_t presentFamily = queue.family;
  const bool graphics = presentFamily < device_.queueFamilies.size() &&
                        (device_.queueFamilies[presentFamily].queueFlags & VK_QUEUE_GRAPHICS_BIT);
  VkQueue drawQueue = queue.handle;
  uint32_t family = presentFamily;
  if (!graphics) {
    if (!overlayQueue_) {
      if (!warnedQueue_) {
        DF_LOG_WARN("collision-viewer: present on a queue without graphics (family %u) and no own graphics "
                    "queue; the overlay is not drawn",
                    presentFamily);
        warnedQueue_ = true;
      }
      return;
    }
    drawQueue = overlayQueue_;
    family = overlayFamily_;
  }
  if (std::find(loggedPresentFamilies_.begin(), loggedPresentFamilies_.end(), presentFamily) ==
      loggedPresentFamilies_.end()) {
    loggedPresentFamilies_.push_back(presentFamily);
    DF_LOG_INFO("collision-viewer: present on queue family %u (%s); the overlay draws on %s, family %u",
                presentFamily, graphics ? "graphics" : "no graphics",
                graphics ? "the present queue" : "its own graphics queue", family);
  }

  DepthImage depth;
  const bool depthFound = tracker_.SelectScreenDepth(swap.extent.width, swap.extent.height, family, &depth);
  LogDepthChoice(depthFound, depth);
  LogStatistics();

  // Что рисовать. Без камеры (пустой источник, меню игры) — ничего, и своих ресурсов не создаём.
  Camera camera;
  if (!camera_.ReadCamera(camera)) return;
  FrameInfo info;
  info.width = swap.extent.width;
  info.height = swap.extent.height;
  info.frame = frameNumber_++;
  info.depthFound = depthFound;
  sceneData_.Clear();
  if (!scene_.BuildScene(info, camera, sceneData_) || sceneData_.draws.empty()) return;
  const uint32_t consoleTop = ConsoleClipTop(sceneData_.consoleFraction, swap.extent.height);
  if (consoleTop >= swap.extent.height && !test_.Active()) return;

  if (!swap.colorAttachment || swap.failed || imageIndex >= swap.images.size()) return;
  if (swap.initialized && swap.family != family) return;  // ресурсы созданы под другое семейство
  if (!swap.initialized) {
    try {
      Initialize(swap, family);
    } catch (...) {
      swap.failed = true;
      Destroy(swap);
      throw;
    }
  }

  const bool transfer = NeedsTransfer(swap, presentFamily);
  if (transfer && !PrepareTransfer(swap, presentFamily)) return;
  Frame& frame = swap.frames[imageIndex];
  const DeviceDispatch& vk = device_.vk;
  const VkDevice dev = device_.handle;
  // Прошлый кадр оверлея для этой картинки ещё на GPU — пропустить, а не ждать.
  if (vk.WaitForFences(dev, 1, &frame.fence, VK_TRUE, 0) != VK_SUCCESS) {
    ++skippedBusy_;
    return;
  }

  // Upload the complete admitted objects; do not silently truncate distant faces or draws.
  if (sceneData_.vertices.size() > std::numeric_limits<uint32_t>::max())
    throw std::length_error("Overlay exceeds Vulkan vertex indexing");
  const auto vertexCount = static_cast<uint32_t>(sceneData_.vertices.size());
  EnsureVertexCapacity(frame, vertexCount);
  std::memcpy(frame.vertexMapped, sceneData_.vertices.data(), size_t(vertexCount) * sizeof(Vertex));

  // Буфер глубины игры для шейдера.
  DepthSampleInfo depthInfo;
  VkImageView depthView = swap.dummyView;
  VkImageLayout depthViewLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  if (depthFound) {
    if (!depth.sampleView) {
      VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      view.image = depth.handle;
      view.viewType = VK_IMAGE_VIEW_TYPE_2D;
      view.format = depth.format;
      view.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
      Check(vk.CreateImageView(dev, &view, nullptr, &depth.sampleView), "vkCreateImageView (depth)");
      if (!tracker_.SetSampleView(depth.handle, depth.sampleView)) {
        vk.DestroyImageView(dev, depth.sampleView, nullptr);  // картинку уже уничтожили
        return;
      }
    }
    depthInfo.width = depth.extent.width;
    depthInfo.height = depth.extent.height;
    depthInfo.viewport = depth.viewport;
    depthView = depth.sampleView;
    depthViewLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
  }
  if (frame.boundView != depthView) {
    // Набор не используется: фенс этого кадра уже сигнален.
    VkDescriptorImageInfo image = {swap.sampler, depthView, depthViewLayout};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = frame.depthSet;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vk.UpdateDescriptorSets(dev, 1, &write, 0, nullptr);
    frame.boundView = depthView;
  }

  // Запись.
  const VkCommandBuffer cb = frame.commands;
  Check(vk.ResetCommandBuffer(cb, 0), "vkResetCommandBuffer");
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  Check(vk.BeginCommandBuffer(cb, &begin), "vkBeginCommandBuffer");
  if (!swap.dummyReady) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = swap.dummyImage;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                          0, nullptr, 0, nullptr, 1, &barrier);
  }
  if (transfer) {
    // Принять картинку от семейства present.
    OwnershipBarrier(vk, cb, frame.image, presentFamily, family, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
  }
  if (depthFound) DepthSamplingBarrier(vk, cb, depth, false);
  VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  pass.renderPass = swap.pass;
  pass.framebuffer = frame.framebuffer;
  pass.renderArea.extent = swap.extent;
  vk.CmdBeginRenderPass(cb, &pass, VK_SUBPASS_CONTENTS_INLINE);
  const VkViewport viewport = {0.0f, 0.0f, static_cast<float>(swap.extent.width),
                               static_cast<float>(swap.extent.height), 0.0f, 1.0f};
  const VkRect2D scissor = {{0, static_cast<int32_t>(consoleTop)},
                            {swap.extent.width, swap.extent.height - consoleTop}};
  vk.CmdSetViewport(cb, 0, 1, &viewport);
  vk.CmdSetScissor(cb, 0, 1, &scissor);
  const VkDeviceSize offset = 0;
  vk.CmdBindVertexBuffers(cb, 0, 1, &frame.vertices, &offset);
  vk.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, swap.layout, 0, 1, &frame.depthSet, 0,
                           nullptr);
  VkPipeline bound = VK_NULL_HANDLE;
  uint32_t pushedView = UINT32_MAX;
  uint32_t triangles = 0;
  uint32_t lines = 0;
  for (const OverlayDraw& draw : sceneData_.draws) {
    if (draw.view >= sceneData_.views.size() || draw.firstVertex >= vertexCount) continue;
    const uint32_t count = std::min(draw.vertexCount, vertexCount - draw.firstVertex);
    if (!count) continue;
    const VkPipeline pipeline = draw.primitive == Primitive::kTriangles ? swap.fill : swap.lines;
    if (pipeline != bound) {
      vk.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      bound = pipeline;
    }
    if (draw.view != pushedView) {
      const PushConstants push = BuildPushConstants(sceneData_.views[draw.view],
                                                    depthFound ? &depthInfo : nullptr,
                                                    swap.extent.width, swap.extent.height);
      vk.CmdPushConstants(cb, swap.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                          sizeof(push), &push);
      pushedView = draw.view;
    }
    vk.CmdDraw(cb, count, 1, draw.firstVertex, 0);
    (draw.primitive == Primitive::kTriangles ? triangles : lines) += count;
  }
  vk.CmdEndRenderPass(cb);
  if (depthFound) DepthSamplingBarrier(vk, cb, depth, true);
  const bool readback = frame.readback != VK_NULL_HANDLE;
  if (readback) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = frame.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkBufferImageCopy region = {};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {swap.extent.width, swap.extent.height, 1};
    vk.CmdCopyImageToBuffer(cb, frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, frame.readback, 1,
                            &region);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = 0;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vk.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 1, &host, 0,
                          nullptr, 1, &barrier);
  }
  if (transfer) {
    // Вернуть картинку семейству present.
    OwnershipBarrier(vk, cb, frame.image, family, presentFamily, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
  }
  Check(vk.EndCommandBuffer(cb), "vkEndCommandBuffer");

  // Отправка: ждём то, чего ждал бы present, и отдаём present свой семафор. Если present идёт с другой
  // очереди и картинку нужно передавать: release на очереди present -> отрисовка на своей очереди ->
  // acquire на очереди present (очередь present сейчас наша: игра держит её на время vkQueuePresentKHR).
  const std::vector<VkSemaphore> waits = edit.WaitSemaphores();
  const std::vector<VkPipelineStageFlags> stages(std::max<size_t>(waits.size(), 1),
                                                 VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
  auto submitInfo = [&](uint32_t waitCount, const VkSemaphore* wait, const VkCommandBuffer* buffer,
                        const VkSemaphore* signal) {
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = waitCount;
    submit.pWaitSemaphores = waitCount ? wait : nullptr;
    submit.pWaitDstStageMask = waitCount ? stages.data() : nullptr;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = buffer;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = signal;
    return submit;
  };
  const auto waitCount = static_cast<uint32_t>(waits.size());
  Check(vk.ResetFences(dev, 1, &frame.fence), "vkResetFences");
  // Если отправка не ушла, фенс остаётся несигнальным, и кадр больше не используется: исключение
  // выключает viewer. Present при этом ждёт то, что уже точно будет сигнализировано.
  if (drawQueue == queue.handle) {
    const VkSubmitInfo submit = submitInfo(waitCount, waits.data(), &cb, &frame.complete);
    Check(vk.QueueSubmit(drawQueue, 1, &submit, frame.fence), "vkQueueSubmit (overlay)");
  } else if (!transfer) {
    // Картинка общая (CONCURRENT): передавать не нужно, но последний сигнал для present всё равно идёт
    // с очереди present (пустая отправка) — так present упорядочен с прошлым present этой картинки и
    // для проверки синхронизации validation.
    if (!frame.drawn) {
      VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      Check(vk.CreateSemaphore(dev, &semaphore, nullptr, &frame.drawn), "vkCreateSemaphore");
    }
    const VkSubmitInfo draw = submitInfo(waitCount, waits.data(), &cb, &frame.drawn);
    Check(vk.QueueSubmit(drawQueue, 1, &draw, VK_NULL_HANDLE), "vkQueueSubmit (overlay)");
    VkSubmitInfo handOver = submitInfo(1, &frame.drawn, nullptr, &frame.complete);
    handOver.commandBufferCount = 0;
    if (const VkResult r = vk.QueueSubmit(queue.handle, 1, &handOver, frame.fence); r != VK_SUCCESS) {
      edit.SetWaitSemaphores({frame.drawn});
      Check(r, "vkQueueSubmit (overlay signal)");
    }
  } else {
    const VkSubmitInfo release = submitInfo(waitCount, waits.data(), &frame.release, &frame.released);
    Check(vk.QueueSubmit(queue.handle, 1, &release, VK_NULL_HANDLE), "vkQueueSubmit (overlay release)");
    // Семафоры игры израсходованы: дальше при ошибке present ждёт последний сигнальный семафор.
    const VkSubmitInfo draw = submitInfo(1, &frame.released, &cb, &frame.drawn);
    if (const VkResult r = vk.QueueSubmit(drawQueue, 1, &draw, VK_NULL_HANDLE); r != VK_SUCCESS) {
      edit.SetWaitSemaphores({frame.released});
      Check(r, "vkQueueSubmit (overlay)");
    }
    const VkSubmitInfo acquire = submitInfo(1, &frame.drawn, &frame.acquire, &frame.complete);
    if (const VkResult r = vk.QueueSubmit(queue.handle, 1, &acquire, frame.fence); r != VK_SUCCESS) {
      edit.SetWaitSemaphores({frame.drawn});
      Check(r, "vkQueueSubmit (overlay acquire)");
    }
  }
  // С этого места семафоры игры уже израсходованы нашей работой: present обязан ждать наш.
  edit.SetWaitSemaphores({frame.complete});
  if (!swap.dummyReady) swap.dummyReady = true;
  frame.boundDepth = depthFound ? depth.handle : VK_NULL_HANDLE;
  ++drawn_;
  if(desru::Controlled()) desru::Drawn();
  if (drawn_ == 1) {
    DF_LOG_INFO("collision-viewer: first overlay frame drawn (%u triangle vertices, %u line vertices, "
                "depth buffer %s)",
                triangles, lines, depthFound ? "found" : "not found");
  }

  if (readback) {
    if (vk.WaitForFences(dev, 1, &frame.fence, VK_TRUE, kReadbackWaitNs) != VK_SUCCESS) {
      DF_LOG_ERROR("collision-viewer test: the overlay frame did not finish in 5 s");
      return;
    }
    const ReadbackStats stats =
        AnalyzeReadback(static_cast<const uint8_t*>(frame.readbackMapped), swap.extent.width,
                        swap.extent.height, IsBgra(swap.format), consoleTop);
    DF_LOG_INFO("collision-viewer test result: mode=%s occlusion=%s frame=%llu depth=%s triangles=%u "
                "lines=%u %s",
                TestModeName(test_.mode), OcclusionCaseName(test_.occlusion),
                static_cast<unsigned long long>(info.frame), depthFound ? "found" : "none", triangles,
                lines, stats.Format().c_str());
  }
}

}  // namespace doomfix::cv
