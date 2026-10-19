// Based on doom-eternal-collision-viewer by IlyaTsyganov (commit 43ac57d), used with the author's permission.
//
// Поддельная «память DOOM Eternal 1.1» в своём процессе: те же объекты и поля, что читает источник
// «память игры» (exe_profile.h), с маленьким уровнем. Нужна для проверок без игры:
//   - CPU-тест tests/cv_scene_test.cpp (перенос viewer_behavior_tests.cpp исходного viewer);
//   - GPU-смоук DOOMFIX_CV_TEST=scene: слой рисует этот уровень настоящим кодом источника (чтение
//     памяти, фоновый поток, разбор Havok, BVH, отбор, бюджет) поверх кадра тестовой программы.
//
// Образ exe — зарезервированная область адресов размера kImageSize; страницы выделяются только там,
// куда пишут (чтение невыделенной страницы ловит SEH, как и в игре). Остальные объекты — в своей
// «куче» по тому же принципу.
//
// Уровень (камера в начале координат смотрит вдоль +z, единица — метр):
//   коллизии (мир Havok — составная форма): плита-ящик x -1.5..1.5, y -1.2..-0.8, z 3..4 и сжатая
//     сетка — четырёхугольник «потолок» y = 1, x -1.5..1.5, z 3..4 (2 треугольника, 4 ребра контура);
//   стена playerclip: ящик x -1.5..1.5, y -0.3..0.3, z 3.5..3.6;
//   триггер (idTriggerHurt): ящик x -1.5..1.5, y 0.4..0.7, z 3..3.3.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "features/collision_viewer/overlay_types.h"

namespace doomfix::cv {

class FakeGame {
 public:
  static constexpr size_t kImageSize = 0x6000000;
  static constexpr size_t kHeapSize = 0x1000000;

  FakeGame();
  ~FakeGame();
  FakeGame(const FakeGame&) = delete;
  FakeGame& operator=(const FakeGame&) = delete;

  bool Valid() const { return image_ != nullptr && heap_ != nullptr; }
  uintptr_t Base() const { return reinterpret_cast<uintptr_t>(image_); }

  // Запись в поддельную память (страницы выделяются по мере надобности).
  void Write(uintptr_t address, const void* data, size_t size);
  template <typename T>
  void Put(uintptr_t address, T value) {
    Write(address, &value, sizeof(value));
  }
  template <typename T>
  T Get(uintptr_t address) const {
    T value{};
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
  }

  // Объект в «куче» (обнулён; маленькие — выделены целиком).
  uintptr_t Allocate(size_t size);

  // Камера по умолчанию: позиция (0, 0, 0), взгляд вдоль +z, поле зрения 90°, обратная глубина.
  static Camera DefaultCamera();
  void SetCamera(const Camera& camera);
  void SetConsole(float fraction, bool forced);
  void SetMapName(const std::string& name);

  // Адреса объектов (для тестов, которые меняют состояние).
  struct Objects {
    uintptr_t system = 0, map = 0, player = 0, menus = 0, menu = 0;
    uintptr_t renderer = 0, view = 0;
    uintptr_t playerPhysics = 0, havok = 0, worldWrapper = 0, world = 0;
    uintptr_t floorShape = 0, meshShape = 0;
    uintptr_t entities = 0;  // список сущностей (2 элемента)
    uintptr_t wall = 0, wallPhysics = 0, wallWrapper = 0, wallShape = 0;
    uintptr_t trigger = 0, triggerPhysics = 0, triggerWrapper = 0, triggerShape = 0, triggerVtable = 0;
    uintptr_t mapName = 0;
  } objects;

  // Выпуклый ящик hknp с полуразмерами (hx, hy, hz) в своих координатах (8 вершин, 6 граней).
  uintptr_t MakeBox(float hx, float hy, float hz);

 private:
  void Commit(uintptr_t address, size_t size);
  void Build();

  unsigned char* image_ = nullptr;
  unsigned char* heap_ = nullptr;
  size_t heapUsed_ = 0;
};

}  // namespace doomfix::cv
