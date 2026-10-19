// Проверка, что процесс — DOOM Eternal 1.1, под которую написан профиль exe_profile.h. Делается один
// раз при старте фичи, до любых других чтений памяти игры. На другой версии (или в другой программе)
// источник «память игры» не создаётся: viewer ничего не рисует, фикс освещения работает как обычно.
//
// Полный SHA-256 exe (≈450 МБ) не считается: это секунды чтения диска при каждом запуске. Заголовок PE
// (время сборки и размер образа) и сигнатура кода однозначно отличают сборки и стоят микросекунды.

#pragma once

#include <cstdint>
#include <string>

namespace doomfix::cv {

struct ExeVersion {
  bool known = false;      // это exe 1.1: можно читать память по профилю
  std::string description; // для лога: что найдено
};

// base — начало образа exe в памяти (GetModuleHandleW(nullptr)).
ExeVersion CheckExeVersion(uintptr_t base);

}  // namespace doomfix::cv
