#include "runtime.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

extern "C" DWORD WINAPI DesruVisualsRegister(void*);

namespace {
using Callback = void (*)(const void*);
std::map<std::string, Callback> callbacks;
std::string printed;
int failures = 0;
void Check(bool ok, const char* name) {
  std::printf("%s %s\n", ok ? "ok" : "FAIL", name);
  failures += !ok;
}
void Print(int, int, const char* format, va_list args) {
  char text[2048]{};
  std::vsnprintf(text, sizeof(text), format, args);
  printed = text;
}
void Add(void*, const char* name, Callback callback, const char*, void*, int) {
  callbacks[name] = callback;
}
void Execute(const char* name, const char* value = nullptr) {
  struct Args {
    int count, padding;
    const char* argv[2];
  } args = {value ? 2 : 1, 0, {name, value}};
  callbacks.at(name)(&args);
}
}  // namespace

int main() {
  auto* shared = reinterpret_cast<DesruVisualControl*>(GetProcAddress(GetModuleHandleW(L"DESRU.Visuals.dll"), "DESRUVisualControl"));
  if (!shared) return 2;
  auto& control = *shared;
  uintptr_t registry[4]{};
  uintptr_t registration[] = {reinterpret_cast<uintptr_t>(registry),
                             reinterpret_cast<uintptr_t>(Add),
                             reinterpret_cast<uintptr_t>(Print)};
  Check(DesruVisualsRegister(registration) == 0 && callbacks.size() == 6,
        "all six volume commands register without starting Vulkan");
  Check(control.state == 0, "registration does not start rendering");
  control.lease = GetTickCount64() + 60000;
  control.requested = 28 | desru::kFill;
  for (const auto& entry : callbacks)
    Execute(entry.first.c_str(), entry.first == "desru_volumes_render_distance" ? "5" : "1");
  Check(control.requested == (28 | desru::kFill) &&
            control.distanceBits == 0x42200000 &&
            printed.find("enable cheats") != std::string::npos,
        "every volume command is blocked without cheats");
  control.requested |= desru::kCheats;
  Execute("desru_volumes_show");
  Check(printed.find("is:\"0\"") != std::string::npos &&
            printed.find("default:\"0\"") != std::string::npos,
        "viewer visibility reports a hidden default");
  Execute("desru_volumes_fill", "0");
  Execute("desru_collision_show", "0");
  Execute("desru_volumes_render_distance", "73.5");
  float distance;
  LONG bits = control.distanceBits;
  std::memcpy(&distance, &bits, 4);
  Check(!(control.requested & (2 | 8 | 32)) && distance == 73.5f,
        "fill, categories and distance can change while the viewer is hidden");
  Execute("desru_volumes_show", "1");
  Check(desru::Viewer() && !(control.requested & 32),
        "show starts an outlines-only viewer without changing fill");
  for (const char* bad : {"-1", "2", "true", "1junk", ""}) {
    LONG before = control.requested;
    Execute("desru_volumes_fill", bad);
    Execute("desru_volumes_show", bad);
    Check(control.requested == before, "invalid booleans do not change volume controls");
  }
  Execute("desru_volumes_fill", "1");
  Execute("desru_volumes_show", "0");
  Check(!desru::Viewer() && (control.requested & 32),
        "hide preserves fill preference");
  Execute("desru_volumes_show", "1");
  control.requested &= ~desru::kCheats;
  Check(!desru::Viewer(), "removing cheat permission immediately prevents drawing");
  control.requested |= desru::kCheats;
  control.lease = 0;
  LONG before = control.requested;
  Execute("desru_volumes_show", "0");
  Check(control.requested == before && !desru::Viewer(),
        "expired DESRU heartbeat locks commands and rendering");
  return failures ? 1 : 0;
}
