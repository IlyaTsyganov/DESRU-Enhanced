// Based on doom-eternal-collision-viewer by IlyaTsyganov (commit 43ac57d), used with the author's permission.

#include "features/collision_viewer/game_memory/memory_reader.h"

#include <windows.h>

#include <cstring>

namespace doomfix::cv {

// Только простые типы: в функции с __try не может быть объектов C++ с деструкторами.
bool GuardedCopy(uintptr_t address, void* output, size_t bytes) noexcept {
  if (address < 0x10000 || address > 0x7fffffffffffULL || bytes > 0x1000000) return false;
  if (bytes == 0) return true;
  __try {
    std::memcpy(output, reinterpret_cast<const void*>(address), bytes);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

}  // namespace doomfix::cv
