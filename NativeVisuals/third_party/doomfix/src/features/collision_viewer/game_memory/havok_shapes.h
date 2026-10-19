// Based on doom-eternal-collision-viewer by IlyaTsyganov (commit 43ac57d), used with the author's permission.
//
// Разбор форм столкновений Havok (hknp*) из памяти игры в треугольники и рёбра контура:
//   - hknpCompoundShape — набор экземпляров дочерних форм с матрицами (рекурсивно);
//   - hknpCompressedMeshShape — сжатая сетка уровня (секции, упакованные и общие вершины);
//   - выпуклые многогранники (и box) — вершины, грани, индексы углов.
// В контур попадают только настоящие рёбра: у сжатой сетки ребро между двумя треугольниками одной
// плоскости (диагональ четырёхугольника) пропускается, у многогранника рёбра — края граней.
// Раскладки — exe_profile.h.

#pragma once

#include <cstdint>
#include <string>

#include "features/collision_viewer/game_memory/memory_reader.h"
#include "features/collision_viewer/game_memory/shape_contents.h"
#include "features/collision_viewer/scene/scene_types.h"

namespace doomfix::cv {

class ShapeDecoder {
 public:
  // Предел вершин треугольников на одну форму (как в исходном viewer): больше — форма пропускается.
  static constexpr size_t kMaxTriangleVertices = 60000;

  explicit ShapeDecoder(MemoryReader memory = {}, ShapeContentsFilter* contents = nullptr)
      : memory_(memory), contents_(contents) {}

  // Добавляет форму по адресу address (с преобразованием transform) в geometry. false — форма не
  // разобрана (тип не поддерживается, данные неверны или недоступны); причина — Error().
  bool Decode(uintptr_t address, const Transform& transform, Geometry& geometry, unsigned depth = 0);

  const std::string& Error() const { return error_; }

 private:
  bool Compressed(uintptr_t address, const Transform& transform, Geometry& geometry);
  bool Convex(uintptr_t address, const unsigned char* header, const Transform& transform, Geometry& geometry);
  bool Fail(const char* message) {
    error_ = message;
    return false;
  }

  MemoryReader memory_;
  ShapeContentsFilter* contents_ = nullptr;
  std::string error_;
};

// Читает hkArray (указатель + uint32 число) по адресу address в output; false — число больше limit
// или память недоступна.
template <typename T, typename Vector>
bool ReadHavokArray(const MemoryReader& memory, uintptr_t address, Vector& output, uint32_t limit = 100000) {
  uintptr_t data = 0;
  uint32_t count = 0;
  if (!memory.Get(address, data) || !memory.Get(address + 8, count) || count > limit) return false;
  output.resize(count);
  return !count || memory.Read(data, output.data(), size_t(count) * sizeof(T));
}

// Матрица экземпляра hknpCompoundShape (float[16] по столбцам и масштаб) -> Transform. false — числа
// неверные или слишком большие.
bool ReadInstanceTransform(const unsigned char* row, Transform& local);

}  // namespace doomfix::cv
