#include "features/collision_viewer/game_memory/version_check.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "features/collision_viewer/game_memory/exe_profile.h"
#include "features/collision_viewer/game_memory/memory_reader.h"

namespace doomfix::cv {

ExeVersion CheckExeVersion(uintptr_t base) {
  ExeVersion result;
  const MemoryReader memory;
  IMAGE_DOS_HEADER dos = {};
  IMAGE_NT_HEADERS64 nt = {};
  if (!memory.Get(base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 ||
      !memory.Get(base + static_cast<uintptr_t>(dos.e_lfanew), nt) || nt.Signature != IMAGE_NT_SIGNATURE) {
    result.description = "no PE header at the image base";
    return result;
  }
  char text[256];
  std::snprintf(text, sizeof(text), "PE TimeDateStamp 0x%08lx, SizeOfImage 0x%lx, machine 0x%x",
                static_cast<unsigned long>(nt.FileHeader.TimeDateStamp),
                static_cast<unsigned long>(nt.OptionalHeader.SizeOfImage), nt.FileHeader.Machine);
  result.description = text;
  if (nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || nt.FileHeader.TimeDateStamp != exe11::kTimeDateStamp ||
      nt.OptionalHeader.SizeOfImage != exe11::kSizeOfImage) {
    return result;
  }
  uint8_t code[sizeof(exe11::kSetStringBytes)] = {};
  if (!memory.Read(base + exe11::kSetStringRva, code, sizeof(code)) ||
      std::memcmp(code, exe11::kSetStringBytes, sizeof(code)) != 0) {
    result.description += ", but the code signature differs";
    return result;
  }
  result.known = true;
  result.description += ", code signature matches";
  return result;
}

}  // namespace doomfix::cv
