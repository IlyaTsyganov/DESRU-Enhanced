#pragma once
#include <windows.h>
#ifdef CreateSemaphore
#undef CreateSemaphore
#endif
#include <cstdint>
#include <mutex>
#include <vulkan/vulkan.h>

// Cross-process protocol. DESRU owns request/lease; the game owns observed status.
// Native code and GPU callbacks stay loaded until game exit. An expired lease turns
// off both visual effects without removing callbacks from a running render thread.
struct alignas(8) DesruVisualControl {
  char magic[8];
  uint32_t version;
  volatile LONG requested;
  volatile LONG active;
  volatile LONG state;
  volatile LONG error;
  volatile LONG profile;
  volatile LONG64 lease;
  volatile LONG64 presents;
  volatile LONG64 barriers;
  volatile LONG64 patched;
  volatile LONG64 draws;
  volatile LONG swapchains;
  volatile LONG hooks;
  volatile LONG distanceBits;
};
extern "C" __declspec(dllexport) DesruVisualControl DESRUVisualControl;
// Session-only diagnostic; no game cvars or saved preferences are changed.
extern "C" __declspec(dllexport) volatile LONG DESRUVisualDiagnosticSkipDepth;
extern "C" __declspec(dllexport) DWORD WINAPI DesruVisualsStart(void*);
namespace doomfix { struct DeviceData; struct QueueData; }
namespace desru {
inline constexpr LONG kLighting = 1, kViewer = 2, kFill = 32, kCheats = 64;
bool VolumeCommandsEnabled();
bool Lighting();
bool Viewer();
bool Controlled();
void Present();
void Drawn();
bool ResolveProfile();
bool ResolveCamera(uintptr_t renderer);
bool ReadLiveCamera(float* matrix,float* position);
bool DecodeCamera(const float* matrix,float* position);
void ResolveMap(uintptr_t map);
std::recursive_mutex& QueueMutex();
bool AdoptVulkan(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                 PFN_vkGetInstanceProcAddr gipa, PFN_vkGetDeviceProcAddr gdpa,
                 const doomfix::QueueData* queues, uint32_t count);
VkResult NativeCreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
                            const VkAllocationCallbacks* allocator, VkDevice* device);
}
