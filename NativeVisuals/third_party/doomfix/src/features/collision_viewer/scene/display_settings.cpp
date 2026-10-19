// Based on doom-eternal-collision-viewer by IlyaTsyganov (commit 43ac57d), used with the author's permission.

#include "features/collision_viewer/scene/display_settings.h"

#include <windows.h>

#include <cstdio>

namespace doomfix::cv {
namespace {

bool AsyncKeyDown(int virtualKey) { return (GetAsyncKeyState(virtualKey) & 0x8000) != 0; }

// Окно на переднем плане принадлежит этому процессу (игре).
bool ProcessInForeground() {
  DWORD pid = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &pid);
  return pid == GetCurrentProcessId();
}

const char* OnOff(bool value) { return value ? "on" : "off"; }

}  // namespace

std::string DisplaySettings::Describe() const {
  char text[160];
  std::snprintf(text, sizeof(text), "overlay=%s triggers=%s collision=%s walls=%s style=%s range=%.0fm",
                OnOff(overlay), OnOff(triggers), OnOff(collision), OnOff(walls),
                fill ? "contours+fill" : "contours", radius);
  return text;
}

float NextRadius(float current) {
  return current < 30.0f ? 40.0f : (current < 50.0f ? 100.0f : (current < 150.0f ? 200.0f : 20.0f));
}

void ApplyKey(DisplaySettings& settings, DisplayKey key) {
  switch (key) {
    case DisplayKey::kF5:
      settings.fill = !settings.fill;
      break;
    case DisplayKey::kF6:
      settings.triggers = !settings.triggers;
      break;
    case DisplayKey::kF7:
      settings.collision = !settings.collision;
      break;
    case DisplayKey::kF8:
      settings.walls = !settings.walls;
      break;
    case DisplayKey::kF9:
      settings.radius = NextRadius(settings.radius);
      break;
    case DisplayKey::kF10:
      settings.overlay = !settings.overlay;
      break;
  }
}

KeyPoller::KeyPoller() : keyDown_(AsyncKeyDown), foreground_(ProcessInForeground) {}

int KeyPoller::VirtualKey(DisplayKey key) { return VK_F5 + static_cast<int>(key); }

const char* KeyPoller::Name(DisplayKey key) {
  static const char* const kNames[kDisplayKeys] = {"F5", "F6", "F7", "F8", "F9", "F10"};
  return kNames[static_cast<int>(key)];
}

}  // namespace doomfix::cv
