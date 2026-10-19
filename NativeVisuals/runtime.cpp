#include "runtime.h"
#include "layer/dispatch.h"
#include "common/log.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <vector>
#include <tlhelp32.h>

extern "C" PFN_vkVoidFunction VKAPI_CALL DoomFix_GetDeviceProcAddr(VkDevice, const char*);
extern "C" PFN_vkVoidFunction VKAPI_CALL DoomFix_GetInstanceProcAddr(VkInstance, const char*);
extern "C" __declspec(dllexport) DesruVisualControl DESRUVisualControl =
    {{'D','E','S','R','U','V','0','2'},2,0,0,0,0,0,0,0,0,0,0,0,0,0x42200000};
extern "C" __declspec(dllexport) volatile LONG DESRUVisualDiagnosticSkipDepth = 0;

namespace {
std::recursive_mutex queueMutex;
LONG started = 0;
uintptr_t image;
struct Range { uintptr_t begin, end; bool code; };
std::vector<Range> ranges;
PFN_vkGetInstanceProcAddr gipa;
PFN_vkGetDeviceProcAddr gdpa;
std::vector<doomfix::QueueData> queues;
std::vector<PFN_vkGetInstanceProcAddr> previousBridges;
void RetirePreviousBridge() {
  HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,GetCurrentProcessId());
  if(snapshot==INVALID_HANDLE_VALUE) return;
  MODULEENTRY32W module{};module.dwSize=sizeof(module);
  if(Module32FirstW(snapshot,&module)) do {
    auto* control=reinterpret_cast<DesruVisualControl*>(GetProcAddress(module.hModule,"DESRUVisualControl"));
    if(!control||control==&DESRUVisualControl||std::memcmp(control->magic,"DESRUV0",7)||(control->version!=1&&control->version!=2)) continue;
    auto old=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(module.hModule,"DoomFix_GetInstanceProcAddr"));
    if(!old) continue;
    InterlockedExchange(&control->requested,0);InterlockedExchange64(&control->lease,0);
    previousBridges.push_back(old);
  } while(Module32NextW(snapshot,&module));
  CloseHandle(snapshot);
}

template<class T> bool Read(uintptr_t p, T& result) {
  __try { result = *reinterpret_cast<const T*>(p); return true; }
  __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
uintptr_t Rip(uintptr_t p, size_t displacement, size_t length) {
  int32_t rel = 0; return Read(p+displacement, rel) ? p+length+rel : 0;
}
bool InImage(uintptr_t p) { return p >= image && p < image+0x8000000; }
uintptr_t LastRip(uintptr_t call, const uint8_t* op, size_t count, size_t lookback=80) {
  uintptr_t result=0;
  for(uintptr_t p=call-lookback;p+count+4<=call;++p)
    if(std::memcmp(reinterpret_cast<void*>(p),op,count)==0) {
      uintptr_t target=Rip(p,count,count+4); if(InImage(target)) result=target;
    }
  return result;
}
void BuildRanges() {
  image=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(image);
  auto nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(image+dos->e_lfanew);
  auto section=IMAGE_FIRST_SECTION(nt);
  // The appended packer image is not the engine. Scan committed original sections only.
  for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i) {
    uintptr_t begin=image+section[i].VirtualAddress;
    uintptr_t end=std::min(begin+section[i].Misc.VirtualSize,image+std::min<DWORD>(nt->OptionalHeader.SizeOfImage,0x8000000));
    for(uintptr_t p=begin;p<end;) {
      MEMORY_BASIC_INFORMATION mbi{}; if(!VirtualQuery(reinterpret_cast<void*>(p),&mbi,sizeof(mbi))) break;
      uintptr_t next=std::min(reinterpret_cast<uintptr_t>(mbi.BaseAddress)+mbi.RegionSize,end);
      if(next<=p) break;
      if(mbi.State==MEM_COMMIT && !(mbi.Protect&(PAGE_GUARD|PAGE_NOACCESS)))
        ranges.push_back({p,next,(section[i].Characteristics&IMAGE_SCN_MEM_EXECUTE)!=0});
      p=next;
    }
  }
}
struct Globals { uintptr_t instance=0,device=0,physical=0; std::vector<std::pair<uintptr_t,uintptr_t>> queueSlots; };
Globals FindGlobals(HMODULE vulkan) {
  std::map<uintptr_t,std::string> functions;
  for(const char* name: {"vkCreateInstance","vkGetDeviceQueue","vkGetPhysicalDeviceMemoryProperties"})
    functions[reinterpret_cast<uintptr_t>(GetProcAddress(vulkan,name))]=name;
  for(auto bridge:previousBridges) for(const char* name:{"vkCreateInstance","vkGetDeviceQueue"})
    if(auto fn=bridge(VK_NULL_HANDLE,name)) functions[reinterpret_cast<uintptr_t>(fn)]=name;
  std::map<uintptr_t,std::string> thunks;
  for(const auto& range:ranges) if(range.code)
    for(uintptr_t p=range.begin;p+6<range.end;++p) {
      const auto* b=reinterpret_cast<uint8_t*>(p);
      if(b[0]!=0xff || b[1]!=0x25) continue;
      uintptr_t pointer=0; if(!Read(Rip(p,2,6),pointer)) continue;
      auto it=functions.find(pointer); if(it!=functions.end()) thunks[p]=it->second;
    }
  Globals out;
  const uint8_t rcx[]={0x48,0x8b,0x0d},r8[]={0x4c,0x8d,0x05},r9[]={0x4c,0x8d,0x0d},edx[]={0x8b,0x15};
  for(const auto& range:ranges) if(range.code)
    for(uintptr_t p=range.begin+96;p+5<range.end;++p) {
      if(*reinterpret_cast<uint8_t*>(p)!=0xe8) continue;
      auto it=thunks.find(Rip(p,1,5)); if(it==thunks.end()) continue;
      if(it->second=="vkCreateInstance") out.instance=LastRip(p,r8,3,96);
      if(it->second=="vkGetPhysicalDeviceMemoryProperties") out.physical=LastRip(p,rcx,3,48);
      if(it->second=="vkGetDeviceQueue") {
        auto dev=LastRip(p,rcx,3,40), q=LastRip(p,r9,3,40), fam=LastRip(p,edx,2,40);
        if(dev&&q&&fam) { out.device=dev; out.queueSlots.emplace_back(q,fam); }
      }
    }
  return out;
}

void InstallHooks(VkInstance instance,VkDevice device,HMODULE vulkan) {
  struct Hook { void* original; void* replacement; };
  std::vector<Hook> hooks;
  auto Add=[&](const char* name) {
    auto replacement=DoomFix_GetDeviceProcAddr(device,name);
    if(!replacement) replacement=DoomFix_GetInstanceProcAddr(instance,name);
    auto original=gdpa(device,name);
    auto exported=GetProcAddress(vulkan,name);
    if(replacement==original) return;
    if(replacement&&original&&replacement!=original) hooks.push_back({reinterpret_cast<void*>(original),reinterpret_cast<void*>(replacement)});
    if(replacement&&exported&&reinterpret_cast<void*>(replacement)!=reinterpret_cast<void*>(exported)) hooks.push_back({reinterpret_cast<void*>(exported),reinterpret_cast<void*>(replacement)});
    auto instanceOriginal=gipa(instance,name);
    if(replacement&&instanceOriginal&&replacement!=instanceOriginal) hooks.push_back({reinterpret_cast<void*>(instanceOriginal),reinterpret_cast<void*>(replacement)});
    for(auto bridge:previousBridges) {
      auto previous=bridge(VK_NULL_HANDLE,name);
      if(replacement&&previous&&previous!=replacement) hooks.push_back({reinterpret_cast<void*>(previous),reinterpret_cast<void*>(replacement)});
    }
  };
#define DESRU_ADD(name) Add("vk" #name);
  DOOMFIX_DEVICE_FUNCS(DESRU_ADD)
#undef DESRU_ADD
  Add("vkGetDeviceProcAddr"); Add("vkGetInstanceProcAddr"); Add("vkCreateDevice"); Add("vkCreateInstance");
  // Patch the game's import/dispatch slots, never Vulkan/driver code or other modules.
  LONG changed=0;
  for(const auto& range:ranges) if(!range.code)
    for(uintptr_t p=(range.begin+7)&~uintptr_t(7);p+8<=range.end;p+=8) {
      void* actual=*reinterpret_cast<void**>(p);
      auto it=std::find_if(hooks.begin(),hooks.end(),[&](const Hook& h){return h.original==actual;});
      if(it==hooks.end()) continue;
      DWORD protection=0;
      if(!VirtualProtect(reinterpret_cast<void*>(p),8,PAGE_READWRITE,&protection)) continue;
      if(InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(p),it->replacement,it->original)==it->original) ++changed;
      DWORD ignored; VirtualProtect(reinterpret_cast<void*>(p),8,protection,&ignored);
    }
  InterlockedExchange(&DESRUVisualControl.hooks,changed);
  DF_LOG_INFO("DESRU: installed %ld game Vulkan dispatch slots",changed);
}
DWORD WINAPI Worker(void*) {
  RetirePreviousBridge();
  BuildRanges();
  HMODULE vulkan=GetModuleHandleW(L"vulkan-1.dll");
  if(!vulkan) { DESRUVisualControl.error=1; DESRUVisualControl.state=-1; return 0; }
  gipa=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(vulkan,"vkGetInstanceProcAddr"));
  gdpa=reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(vulkan,"vkGetDeviceProcAddr"));
  auto globals=FindGlobals(vulkan);
  if(!globals.instance || !globals.device || !globals.physical) { DESRUVisualControl.error=2; DESRUVisualControl.state=-1; return 0; }
  while(true) {
    VkInstance instance{};VkDevice device{};VkPhysicalDevice physical{};
    Read(globals.instance,instance);Read(globals.device,device);Read(globals.physical,physical);
    if(instance&&device&&physical) {
      uintptr_t instanceKey=0,physicalKey=0,deviceKey=0;
      if(!Read(reinterpret_cast<uintptr_t>(instance),instanceKey)||!Read(reinterpret_cast<uintptr_t>(physical),physicalKey)||
          !Read(reinterpret_cast<uintptr_t>(device),deviceKey)||!deviceKey||instanceKey!=physicalKey) {
        DESRUVisualControl.error=5;DESRUVisualControl.state=-1;return 0;
      }
      for(auto slots:globals.queueSlots) {
        VkQueue queue{};uint32_t family=VK_QUEUE_FAMILY_IGNORED;
        Read(slots.first,queue);Read(slots.second,family);
        uintptr_t key=0;
        if(queue&&Read(reinterpret_cast<uintptr_t>(queue),key)&&key==deviceKey) queues.push_back({queue,nullptr,family,0});
      }
      if(desru::AdoptVulkan(instance,physical,device,gipa,gdpa,queues.data(),static_cast<uint32_t>(queues.size()))) {
        DESRUVisualControl.profile=desru::ResolveProfile()?1:0;
        InstallHooks(instance,device,vulkan);
        DESRUVisualControl.state=2;
        break;
      }
    }
    Sleep(100);
  }
  return 0;
}
}

namespace desru {
bool Controlled() { return started!=0; }
bool Lease() { return DESRUVisualControl.lease>static_cast<LONG64>(GetTickCount64()); }
bool Lighting() { return Lease()&&(DESRUVisualControl.requested&kLighting); }
bool VolumeCommandsEnabled() { return Lease() && (DESRUVisualControl.requested & kCheats); }
bool Viewer() { return VolumeCommandsEnabled()&&DESRUVisualControl.error!=4&&(DESRUVisualControl.requested&kViewer); }
std::recursive_mutex& QueueMutex() { return queueMutex; }
void Present() {
  InterlockedIncrement64(&DESRUVisualControl.presents);
  LONG active=Lighting()?kLighting:0;
  if(Viewer()&&DESRUVisualControl.profile&&DESRUVisualControl.swapchains>0) active|=kViewer;
  InterlockedExchange(&DESRUVisualControl.active,active);
}
void Drawn() { InterlockedIncrement64(&DESRUVisualControl.draws); }
}
extern "C" __declspec(dllexport) DWORD WINAPI DesruVisualsStart(void*) {
  if(InterlockedCompareExchange(&started,1,0)) return 0;
  DESRUVisualControl.state=1;
  HANDLE thread=CreateThread(nullptr,0,Worker,nullptr,0,nullptr);
  if(thread) CloseHandle(thread); else { DESRUVisualControl.error=3;DESRUVisualControl.state=-1; }
  return 0;
}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID) {
  if(reason==DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(module);
  return TRUE;
}
