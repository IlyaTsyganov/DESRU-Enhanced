// Based on doom-eternal-collision-viewer by IlyaTsyganov (commit 43ac57d), used with the author's permission.
//
// Чтение памяти игры (слой работает в процессе игры): копирование под SEH. Синхронизации с игрой нет —
// снимок может оказаться «рваным»; читатели перепроверяют то, на что опираются (идентичность уровня,
// сущности). Неверный адрес даёт false, а не падение.

#pragma once

#include <cstddef>
#include <cstdint>

namespace doomfix::cv {

// Копирует bytes байт из address в output; false — адрес вне пользовательского пространства, размер
// больше 16 МБ или память недоступна (исключение доступа поймано __try/__except).
bool GuardedCopy(uintptr_t address, void* output, size_t bytes) noexcept;

struct MemoryReader {
  bool Read(uintptr_t address, void* output, size_t bytes) const noexcept {
    return GuardedCopy(address, output, bytes);
  }
  template <typename T>
  bool Get(uintptr_t address, T& value) const noexcept {
    return Read(address, &value, sizeof(value));
  }
};

}  // namespace doomfix::cv
