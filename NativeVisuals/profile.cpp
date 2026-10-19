#include "runtime.h"
#include "features/collision_viewer/game_memory/exe_profile.h"
#include "features/collision_viewer/game_memory/memory_reader.h"
#include "common/log.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {
using namespace doomfix::cv::exe11;
doomfix::cv::MemoryReader memory;
uintptr_t base=0;
uintptr_t resolvedMatrix=0;
struct Segment { uintptr_t start,end; };
std::vector<Segment> segments;
bool Pointer(uintptr_t p) { return p>=0x10000&&p<0x0000800000000000&&(p&7)==0; }
std::vector<uintptr_t> Find(const void* value,size_t size,size_t alignment=1) {
  std::vector<uintptr_t> result;
  for(auto segment:segments) {
    const auto* p=reinterpret_cast<const uint8_t*>(segment.start);
    const auto* end=reinterpret_cast<const uint8_t*>(segment.end);
    const auto* needle=static_cast<const uint8_t*>(value);
    while(p+size<=end) {
      auto* found=std::search(p,end,needle,needle+size);if(found==end) break;
      uintptr_t at=reinterpret_cast<uintptr_t>(found);
      if(at%alignment==0) result.push_back(at);
      p=found+1;
    }
  }
  return result;
}
uintptr_t Vtable(const char* name) {
  std::set<uintptr_t> tables;
  for(auto text:Find(name,std::strlen(name)+1)) {
    uint32_t descriptor=static_cast<uint32_t>(text-base-16);
    for(auto reference:Find(&descriptor,4)) {
      uint32_t loc[6]{};auto locator=reference-12;
      if(!memory.Read(locator,loc,sizeof(loc))||loc[0]!=1||loc[1]||loc[2]||loc[5]!=locator-base) continue;
      for(auto pointer:Find(&locator,8,8)) tables.insert(pointer+8);
    }
  }
  return tables.size()==1?*tables.begin():0;
}
std::map<std::string,uintptr_t> Fields(const char* type) {
  std::map<std::string,uintptr_t> result;
  for(auto text:Find(type,std::strlen(type)+1)) {
    for(auto root:Find(&text,8,8)) {
      uint32_t size=0;uintptr_t list=0;
      if(!memory.Get(root+16,size)||!size||size>0x1000000||!memory.Get(root+32,list)||!Pointer(list)) continue;
      for(unsigned i=0;i<4096;++i) {
        uintptr_t name=0;uint32_t offset=0,bytes=0;
        if(!memory.Get(list+i*72+16,name)||!name) break;
        if(!memory.Get(list+i*72+24,offset)||!memory.Get(list+i*72+28,bytes)||offset>=size) break;
        char field[128]{};if(!memory.Read(name,field,sizeof(field)-1)) break;
        if(std::memchr(field,0,sizeof(field))) result[field]=offset;
      }
      if(!result.empty()) return result;
    }
  }
  return result;
}
void Field(const char* type,const char* name,uintptr_t& destination) {
  static std::map<std::string,std::map<std::string,uintptr_t>> cache;
  auto it=cache.find(type);if(it==cache.end()) it=cache.emplace(type,Fields(type)).first;
  auto f=it->second.find(name);if(f!=it->second.end()&&f->second) destination=f->second;
}
bool CameraAt(uintptr_t view,uintptr_t position) {
  float m[16]{},p[3]{};
  if(!Pointer(view)||!memory.Read(view+kViewMatrix,m,sizeof(m))||!memory.Read(view+position,p,sizeof(p))) return false;
  for(float v:m) if(!std::isfinite(v)) return false;
  for(float v:p) if(!std::isfinite(v)) return false;
  float norm=m[12]*m[12]+m[13]*m[13]+m[14]*m[14];
  float w=m[12]*p[0]+m[13]*p[1]+m[14]*p[2]+m[15];
  float n0=0,n1=0,d01=0,d03=0,d13=0,x=m[3],y=m[7];
  for(unsigned i=0;i<3;++i) { n0+=m[i]*m[i];n1+=m[i+4]*m[i+4];d01+=m[i]*m[i+4];d03+=m[i]*m[i+12];d13+=m[i+4]*m[i+12];x+=m[i]*p[i];y+=m[i+4]*p[i]; }
  return norm>0.99f&&norm<1.01f&&n0>0.01f&&n0<100&&n1>0.01f&&n1<100&&
    std::fabs(d01)<0.01f&&std::fabs(d03)<0.01f&&std::fabs(d13)<0.01f&&
    std::fabs(w)<0.05f&&std::fabs(x)<0.05f&&std::fabs(y)<0.05f;
}
}

namespace desru {
bool DecodeCamera(const float* m,float* position) {
  for(unsigned i=0;i<16;++i) if(!std::isfinite(m[i])||std::fabs(m[i])>1e7f) return false;
  float n0=0,n1=0,n3=0,d01=0,d03=0,d13=0,a=0;
  for(unsigned i=0;i<3;++i) { n0+=m[i]*m[i];n1+=m[i+4]*m[i+4];n3+=m[i+12]*m[i+12];d01+=m[i]*m[i+4];d03+=m[i]*m[i+12];d13+=m[i+4]*m[i+12];a+=m[i+8]*m[i+12]; }
  if(n0<0.01f||n0>100||n1<0.01f||n1>100||n3<0.999f||n3>1.001f||std::fabs(d01)>0.001f||std::fabs(d03)>0.001f||std::fabs(d13)>0.001f) return false;
  a/=n3;
  if(a<0.99f||a>1.02f) return false;
  for(unsigned i=0;i<3;++i) if(std::fabs(m[i+8]-a*m[i+12])>0.001f) return false;
  float nearDepth=m[11]-a*m[15];if(nearDepth>=-0.0001f||nearDepth<-2) return false;
  for(unsigned i=0;i<3;++i) position[i]=-m[3]*m[i]/n0-m[7]*m[i+4]/n1-m[15]*m[i+12]/n3;
  return true;
}
bool ReadLiveCamera(float* matrix,float* position) {
  if(!kResolvedRenderer||!ResolveCamera(kResolvedRenderer)) return false;
  if(resolvedMatrix) return memory.Read(resolvedMatrix,matrix,64)&&DecodeCamera(matrix,position);
  uintptr_t list=0,view=kResolvedView;
  if(!view&&(!memory.Get(kResolvedRenderer+kRendererViews,list)||!memory.Get(list,view))) return false;
  return memory.Read(view+kViewMatrix,matrix,64)&&memory.Read(view+kViewPosition,position,12);
}
bool ResolveProfile() {
  base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
  auto* sections=IMAGE_FIRST_SECTION(nt);
  for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i) {
    uintptr_t p=base+sections[i].VirtualAddress;
    uintptr_t end=std::min(p+sections[i].Misc.VirtualSize,base+0x8000000);
    while(p<end) {
      MEMORY_BASIC_INFORMATION mbi{};if(!VirtualQuery(reinterpret_cast<void*>(p),&mbi,sizeof(mbi))) break;
      uintptr_t next=std::min(reinterpret_cast<uintptr_t>(mbi.BaseAddress)+mbi.RegionSize,end);
      if(next<=p) break;
      if(mbi.State==MEM_COMMIT&&!(mbi.Protect&(PAGE_GUARD|PAGE_NOACCESS))) segments.push_back({p,next});
      p=next;
    }
  }
  uintptr_t renderer=Vtable(".?AVidRenderSystemLocal@@"),system=Vtable(".?AVidGameSystemLocal@@"),console=Vtable(".?AVidConsoleLocal@@");
  auto player=Vtable(".?AVidPlayer@@"),dynamic=Vtable(".?AVidDynamicEntity@@");
  if(player) kPlayerVtableRva=player-base;
  if(dynamic) kDynamicEntityVtableRva=dynamic-base;
  if(console) kConsoleVtableRva=console-base;
  // Discover singleton objects by their actual RTTI vtables; no EXE version whitelist.
  for(auto segment:segments) for(uintptr_t p=(segment.start+7)&~uintptr_t(7);p+8<=segment.end;p+=8) {
    uintptr_t value=*reinterpret_cast<uintptr_t*>(p),table=0;
    if(value==console&&!kResolvedConsole) kResolvedConsole=p;
    if(!Pointer(value)||!memory.Get(value,table)) continue;
    if(table==renderer&&!kResolvedRenderer) { kResolvedRenderer=value;kRendererRva=p-base; }
    if(table==system&&!kResolvedGameSystem) { kResolvedGameSystem=value;kGameSystemRva=p-base; }
  }
  Field("idRenderView","viewProjectionMatrix",kViewMatrix);
  auto views=Fields("idRenderView");auto render=views.find("r");
  if(render!=views.end()) kViewPosition=render->second+0x88;
  Field("idGameSystemLocal","state",kGameSystemState);
  Field("idGameSystemLocal","mapInstance",kGameSystemMap);
  Field("idEntity","physics",kEntityPhysics);
  Field("havokShapeData_t","rootShape",kShapeDataRoot);
  Field("havokShapeData_t","contentFlags",kShapeDataContents);
  Field("idHavokShape","shapeData",kShapeDataList);kShapeDataCount=kShapeDataList+8;
  Field("idPlayer","playerHud",kPlayerHud);
  Field("idHUD","gameWasPaused",kHudPaused);
  Field("idHUD","menus",kHudMenus);kHudMenuCount=kHudMenus+8;
  DF_LOG_INFO("DESRU viewer profile: renderer=%p game=%p player-vtable=%p matrix=0x%llx",reinterpret_cast<void*>(kResolvedRenderer),reinterpret_cast<void*>(kResolvedGameSystem),reinterpret_cast<void*>(player),static_cast<unsigned long long>(kViewMatrix));
  return kResolvedRenderer&&kResolvedGameSystem&&player;
}
bool ResolveCamera(uintptr_t renderer) {
  static uintptr_t lastRenderer=0;static bool ready=false;static ULONGLONG next=0;
  if(renderer==lastRenderer&&ready) {
    if(resolvedMatrix) {
      float matrix[16],position[3];
      if(memory.Read(resolvedMatrix,matrix,64)&&DecodeCamera(matrix,position)) return true;
    }
    uintptr_t list=0,view=kResolvedView;
    if(!view&&memory.Get(renderer+kRendererViews,list)) memory.Get(list,view);
    if(CameraAt(view,kViewPosition)) return true;
    ready=false;kResolvedView=0;resolvedMatrix=0;
  }
  if(GetTickCount64()<next) return false;
  lastRenderer=renderer;next=GetTickCount64()+500;
  // Renderer-owned projection snapshots are also present in builds that do
  // not publish idRenderView pointers. Recover the camera from its orthogonal
  // view axes and translation; validate the perspective/near-plane relation.
  for(uintptr_t offset=0;offset<0x800000;offset+=16) {
    float matrix[16],position[3];
    if(!memory.Read(renderer+offset,matrix,sizeof(matrix))||!DecodeCamera(matrix,position)) continue;
    resolvedMatrix=renderer+offset;ready=true;
    DF_LOG_INFO("DESRU viewer: resolved live projection at renderer+0x%llx",static_cast<unsigned long long>(offset));
    return true;
  }
  // Some engine builds embed the active idRenderView in the renderer instead
  // of publishing a list. Validate the complete camera transform, including
  // orthogonal axes and the camera mapping to the projection origin.
  for(uintptr_t offset=0;offset<0x800000;offset+=16) {
    if(!CameraAt(renderer+offset,kViewPosition)) continue;
    kResolvedView=renderer+offset;ready=true;
    DF_LOG_INFO("DESRU viewer: resolved embedded live view at renderer+0x%llx",static_cast<unsigned long long>(offset));
    return true;
  }
  // A renderer owns an array of idRenderView pointers followed by its count.
  // Validate the matrix and camera position before accepting a candidate array.
  for(uintptr_t offset=0;offset<0x800000;offset+=8) {
    uintptr_t list=0,view=0;int count=0;
    if(!memory.Get(renderer+offset,list)||!Pointer(list)||!memory.Get(renderer+offset+8,count)||count<1||count>32||!memory.Get(list,view)) continue;
    if(!CameraAt(view,kViewPosition)) continue;
    kRendererViews=offset;kRendererViewCount=offset+8;ready=true;
    DF_LOG_INFO("DESRU viewer: resolved live camera array at renderer+0x%llx",static_cast<unsigned long long>(offset));
    return true;
  }
  return false;
}
void ResolveMap(uintptr_t map) {
  static uintptr_t previous=0;static ULONGLONG next=0;
  if(map==previous||GetTickCount64()<next) return;
  next=GetTickCount64()+500;
  uintptr_t player=0;
  for(uintptr_t offset=0;offset<0x10000;offset+=8) {
    uintptr_t value=0,table=0;
    if(memory.Get(map+offset,value)&&Pointer(value)&&memory.Get(value,table)&&table==base+kPlayerVtableRva) { kMapPlayer=offset;player=value;break; }
  }
  if(!player) return;
  uint32_t bestCount=0;uintptr_t bestOffset=0;
  for(uintptr_t offset=0x1000;offset<0x100000;offset+=8) {
    uintptr_t list=0;uint32_t count=0;
    if(!memory.Get(map+offset,list)||!Pointer(list)||!memory.Get(map+offset+8,count)||count<16||count>32768||count<=bestCount) continue;
    bool containsPlayer=false;
    for(uint32_t i=0;i<count;++i) { uintptr_t entity=0;if(!memory.Get(list+i*32+8,entity)) break;if(entity==player) { containsPlayer=true;break; } }
    // Filtered update/physics lists also contain the player. The complete
    // entity registry is the largest validated EntityEntry array.
    if(containsPlayer) { bestOffset=offset;bestCount=count; }
  }
  if(bestOffset) { kMapEntities=bestOffset;kMapEntityCount=bestOffset+8; }
  previous=map;
  DF_LOG_INFO("DESRU viewer: resolved map player+0x%llx entities+0x%llx",static_cast<unsigned long long>(kMapPlayer),static_cast<unsigned long long>(kMapEntities));
}
}
