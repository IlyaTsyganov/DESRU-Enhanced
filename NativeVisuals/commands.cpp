#include "runtime.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <charconv>
#include <cmath>
#include "features/collision_viewer/game_memory/memory_reader.h"

namespace {
using PrintFunction=void(*)(int,int,const char*,va_list);
using AddFunction=void(*)(void*,const char*,void(*)(const void*),const char*,void*,int);
PrintFunction printFunction=nullptr;
void Print(const char* format,...) {
  if(!printFunction) return;
  va_list arguments;va_start(arguments,format);printFunction(1,1,format,arguments);va_end(arguments);
}
void Execute(const void* raw,LONG bit,const char* name) {
  if(!desru::VolumeCommandsEnabled()) { Print("%s: enable cheats in DESRU first.\n",name);return; }
  const int count=*static_cast<const int*>(raw);
  const auto* argv=reinterpret_cast<const char* const*>(static_cast<const char*>(raw)+8);
  if(count==1) { Print("\"%s\" is:\"%d\" default:\"%d\"\nRequires cheats enabled in DESRU. desru_volumes_show controls the viewer; desru_volumes_fill controls fill.\n",name,(DESRUVisualControl.requested&bit)?1:0,bit==desru::kViewer?0:1);return; }
  if(count!=2 || !argv[1] || (std::strcmp(argv[1],"0")&&std::strcmp(argv[1],"1"))) {
    Print("Usage: %s <0|1>\n",name);return;
  }
  if(argv[1][0]=='1') InterlockedOr(&DESRUVisualControl.requested,bit);
  else InterlockedAnd(&DESRUVisualControl.requested,~bit);
}
void Triggers(const void* args) { Execute(args,4,"desru_triggers_show"); }
void Collision(const void* args) { Execute(args,8,"desru_collision_show"); }
void Walls(const void* args) { Execute(args,16,"desru_invisible_walls_show"); }
void Show(const void* args) { Execute(args,desru::kViewer,"desru_volumes_show"); }
void Fill(const void* args) { Execute(args,desru::kFill,"desru_volumes_fill"); }
void Distance(const void* raw) {
  const char* name="desru_volumes_render_distance";
  if(!desru::VolumeCommandsEnabled()) { Print("%s: enable cheats in DESRU first.\n",name);return; }
  int count=*static_cast<const int*>(raw);const auto* argv=reinterpret_cast<const char* const*>(static_cast<const char*>(raw)+8);
  if(count==1) {
    LONG bits=DESRUVisualControl.distanceBits;float value;std::memcpy(&value,&bits,4);
    Print("\"%s\" is:\"%g\" default:\"40\"\nCollision viewer rendering distance in metres. Intersecting objects are shown in full.\n",name,value);return;
  }
  float value=-1;
  if(count==2&&argv[1]) {
    const char* end=argv[1]+std::strlen(argv[1]);auto result=std::from_chars(argv[1],end,value);
    if(result.ec!=std::errc{}||result.ptr!=end) value=-1;
  }
  if(!std::isfinite(value)||value<0) { Print("Usage: %s <distance in metres, 0 or greater>\n",name);return; }
  LONG bits;std::memcpy(&bits,&value,4);InterlockedExchange(&DESRUVisualControl.distanceBits,bits);
}
bool ReplaceCallback(uintptr_t system,const char* name,void(*callback)(const void*)) {
  doomfix::cv::MemoryReader memory;uintptr_t registry=0,list=0;int count=0;
  if(!memory.Get(system+16,registry)||!memory.Get(registry,list)||!memory.Get(registry+8,count)||count<0||count>65536) return false;
  for(int i=0;i<count;++i) {
    uintptr_t command=0,text=0;char existing[64]{};
    if(!memory.Get(list+i*8,command)||!memory.Get(command,text)||!memory.Read(text,existing,sizeof(existing)-1)) continue;
    if(std::strcmp(existing,name)) continue;
    // These names belong to DESRU. Keep engine-owned command records
    // and replace only our callback when reconnecting an updated bridge.
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(command+8),reinterpret_cast<void*>(callback));
    return true;
  }
  return false;
}
}
extern "C" __declspec(dllexport) DWORD WINAPI DesruVisualsRegister(void* argument) {
  static bool registered=false;
  auto* values=static_cast<uintptr_t*>(argument);
  if(!values || !values[0] || !values[1] || !values[2]) return 1;
  printFunction=reinterpret_cast<PrintFunction>(values[2]);
  if(registered) return 0;
  auto add=reinterpret_cast<AddFunction>(values[1]);
  const char* help="<0|1>: show/hide volumes, fill or category; requires cheats enabled in DESRU";
  auto registerCommand=[&](const char* name,void(*callback)(const void*)) {
    if(!ReplaceCallback(values[0],name,callback)) add(reinterpret_cast<void*>(values[0]),name,callback,help,nullptr,2);
  };
  registerCommand("desru_triggers_show",Triggers);
  registerCommand("desru_collision_show",Collision);
  registerCommand("desru_invisible_walls_show",Walls);
  registerCommand("desru_volumes_show",Show);
  registerCommand("desru_volumes_fill",Fill);
  if(!ReplaceCallback(values[0],"desru_volumes_render_distance",Distance))
    add(reinterpret_cast<void*>(values[0]),"desru_volumes_render_distance",Distance,"<metres>: collision viewer rendering distance (default 40); requires cheats enabled in DESRU",nullptr,2);
  registered=true;return 0;
}
