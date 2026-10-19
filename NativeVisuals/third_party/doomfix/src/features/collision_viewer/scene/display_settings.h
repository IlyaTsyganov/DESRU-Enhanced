// Based on doom-eternal-collision-viewer by IlyaTsyganov (commit 43ac57d), used with the author's permission.
//
// Что показывать и клавиши F5–F10 (как в README исходного viewer):
//   F5  — контуры <-> контуры с заливкой (для всех групп);
//   F6 / F7 / F8 — группы триггеров / коллизий / стен;
//   F9  — дальность 40 -> 100 -> 200 -> 20 -> 40 м;
//   F10 — весь оверлей.
// Старт: все три группы, контуры с заливкой, 40 м. Настройки живут, пока не закрыта игра (между
// уровнями сохраняются).
//
// Клавиши опрашиваются GetAsyncKeyState в каждом present по фронту нажатия и только когда окно игры на
// переднем плане. Ни хуков окна, ни перехвата сообщений: нажатия доходят и до игры.

#pragma once

#include <cstdint>
#include <string>

namespace doomfix::cv {

struct DisplaySettings {
  bool overlay = true;   // F10
  bool triggers = true;  // F6
  bool collision = true; // F7
  bool walls = true;     // F8
  bool fill = true;      // F5: заливка (контуры есть всегда)
  float radius = 40.0f;  // F9

  // "overlay=on triggers=on ..." — для лога.
  std::string Describe() const;
};

// Следующая дальность для F9: 20 -> 40 -> 100 -> 200 -> 20.
float NextRadius(float current);

enum class DisplayKey { kF5, kF6, kF7, kF8, kF9, kF10 };
constexpr int kDisplayKeys = 6;

// Применить нажатие клавиши.
void ApplyKey(DisplaySettings& settings, DisplayKey key);

// Опрос клавиш по фронту нажатия. Чтение клавиш и окна можно подменить (тесты).
class KeyPoller {
 public:
  using KeyDown = bool (*)(int virtualKey);
  using Foreground = bool (*)();
  KeyPoller();
  KeyPoller(KeyDown keyDown, Foreground foreground) : keyDown_(keyDown), foreground_(foreground) {}

  // Вызывает onPress для каждой клавиши, нажатой с прошлого опроса (если окно процесса на переднем
  // плане). Возвращает число нажатий.
  template <typename Fn>
  int Poll(Fn&& onPress) {
    int pressed = 0;
    const bool active = foreground_();
    for (int i = 0; i < kDisplayKeys; ++i) {
      const bool down = keyDown_(VirtualKey(static_cast<DisplayKey>(i)));
      if (down && !down_[i] && active) {
        onPress(static_cast<DisplayKey>(i));
        ++pressed;
      }
      down_[i] = down;
    }
    return pressed;
  }

  static int VirtualKey(DisplayKey key);
  static const char* Name(DisplayKey key);

 private:
  KeyDown keyDown_;
  Foreground foreground_;
  bool down_[kDisplayKeys] = {};
};

}  // namespace doomfix::cv
