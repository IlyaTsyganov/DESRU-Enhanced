// Included after the layer core so the direct-load bridge reuses the same
// dispatch maps and feature events as the original Vulkan layer.
namespace desru {
VkResult VKAPI_CALL CopyLoaderDispatch(VkDevice device,void* object) {
  if(!device||!object) return VK_ERROR_INITIALIZATION_FAILED;
  *reinterpret_cast<void**>(object)=*reinterpret_cast<void**>(device);
  return VK_SUCCESS;
}
bool AdoptVulkan(VkInstance handle,VkPhysicalDevice physical,VkDevice device,
                 PFN_vkGetInstanceProcAddr gipa,PFN_vkGetDeviceProcAddr gdpa,
                 const doomfix::QueueData* queues,uint32_t count) {
  using namespace doomfix;
  EnsureInitialized();
  if(g_devices.Find(DispatchKey(device))) return true;
  InstanceData* instance=g_instances.Find(DispatchKey(handle));
  if(!instance) {
    auto data=std::make_unique<InstanceData>();data->handle=handle;data->vk.Load(handle,gipa);
    instance=g_instances.Insert(DispatchKey(handle),std::move(data));
  }
  auto data=std::make_unique<DeviceData>();
  data->handle=device;data->physicalDevice=physical;data->instance=instance;
  data->vk.Load(device,gdpa);data->setDeviceLoaderData=CopyLoaderDispatch;
  VkPhysicalDeviceProperties properties{};
  instance->vk.GetPhysicalDeviceProperties(physical,&properties);
  data->deviceName=properties.deviceName;data->apiVersion=properties.apiVersion;
  instance->vk.GetPhysicalDeviceMemoryProperties(physical,&data->memoryProperties);
  uint32_t families=0;instance->vk.GetPhysicalDeviceQueueFamilyProperties(physical,&families,nullptr);
  if(!families||families>64) return false;
  data->queueFamilies.resize(families);
  instance->vk.GetPhysicalDeviceQueueFamilyProperties(physical,&families,data->queueFamilies.data());
  for(uint32_t i=0;i<count;++i) if(queues[i].family<families &&
      (data->queueFamilies[queues[i].family].queueFlags&VK_QUEUE_GRAPHICS_BIT)) {
    data->sharedGraphicsQueue=queues[i].handle;data->sharedGraphicsFamily=queues[i].family;break;
  }
  data->featureStates.resize(g_features.size());
  DeviceData* adopted=g_devices.Insert(DispatchKey(device),std::move(data));
  for(const auto& feature:g_features) feature->OnDeviceCreated(*adopted);
  for(uint32_t i=0;i<count;++i) { auto queue=queues[i];queue.device=adopted;g_queues.Insert(queue); }
  DF_LOG_INFO("DESRU: adopted existing Vulkan device %s with %u queues",properties.deviceName,count);
  return true;
}
VkResult NativeCreateDevice(VkPhysicalDevice physical,const VkDeviceCreateInfo* info,
                            const VkAllocationCallbacks* allocator,VkDevice* device) {
  using namespace doomfix;
  auto module=GetModuleHandleW(L"vulkan-1.dll");
  auto gipa=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(module,"vkGetInstanceProcAddr"));
  auto gdpa=reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(module,"vkGetDeviceProcAddr"));
  auto create=reinterpret_cast<PFN_vkCreateDevice>(GetProcAddress(module,"vkCreateDevice"));
  InstanceData* instance=g_instances.Find(DispatchKey(physical));
  if(!instance) return VK_ERROR_INITIALIZATION_FAILED;
  DeviceCreateEdit edit(*info);
  uint32_t count=0;instance->vk.GetPhysicalDeviceQueueFamilyProperties(physical,&count,nullptr);
  std::vector<VkQueueFamilyProperties> families(count);
  instance->vk.GetPhysicalDeviceQueueFamilyProperties(physical,&count,families.data());
  edit.SetQueueFamilies(std::move(families));
  for(const auto& feature:g_features) feature->OnCreateDevice(physical,edit);
  const auto* final=edit.Result();
  VkResult result=create(physical,final,allocator,device);
  if(result!=VK_SUCCESS) return result;
  std::vector<QueueData> queues;
  auto get=reinterpret_cast<PFN_vkGetDeviceQueue>(gdpa(*device,"vkGetDeviceQueue"));
  for(uint32_t i=0;i<final->queueCreateInfoCount;++i) {
    const auto& ci=final->pQueueCreateInfos[i];
    for(uint32_t j=0;j<ci.queueCount;++j) {
      VkQueue queue{};get(*device,ci.queueFamilyIndex,j,&queue);
      if(queue) queues.push_back({queue,nullptr,ci.queueFamilyIndex,j});
    }
  }
  AdoptVulkan(instance->handle,physical,*device,gipa,gdpa,queues.data(),static_cast<uint32_t>(queues.size()));
  return result;
}
}
