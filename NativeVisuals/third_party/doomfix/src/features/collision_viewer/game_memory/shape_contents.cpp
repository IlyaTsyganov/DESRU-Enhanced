#include "features/collision_viewer/game_memory/shape_contents.h"

#include <cstring>

#include "features/collision_viewer/game_memory/exe_profile.h"

namespace doomfix::cv {
using namespace exe11;

bool ShapeContentsFilter::BlocksPlayer(uint32_t contents) {
  return (contents & (kContentsSolid | kContentsPlayerClip)) != 0 &&
         (contents & kContentsNoCollision) == 0;
}

const ShapeContentsFilter::Table* ShapeContentsFilter::Lookup(uintptr_t shape,
                                                            size_t* index) {
  uintptr_t entry = 0, wrapper = 0;
  if (!memory_.Get(shape + kShapeUserData, entry) ||
      !memory_.Get(entry + kShapeDataRoot, wrapper) || !wrapper)
    return nullptr;
  auto found = tables_.find(wrapper);
  if (found == tables_.end()) {
    Table table;
    uint32_t count = 0;
    if (memory_.Get(wrapper + kShapeDataList, table.data) &&
        memory_.Get(wrapper + kShapeDataCount, count) && count &&
        count <= 100000 && table.data && entry >= table.data &&
        (entry - table.data) % kShapeDataSize == 0 &&
        (entry - table.data) / kShapeDataSize < count) {
      std::vector<unsigned char> rows(size_t(count) * kShapeDataSize);
      if (memory_.Read(table.data, rows.data(), rows.size())) {
        table.contents.reserve(count);
        for (size_t i = 0; i < count; ++i) {
          uintptr_t owner = 0;
          uint32_t contents = 0;
          std::memcpy(&owner, rows.data() + i * kShapeDataSize + kShapeDataRoot, 8);
          if (owner != wrapper) {
            table.contents.clear();
            break;
          }
          std::memcpy(&contents, rows.data() + i * kShapeDataSize + kShapeDataContents, 4);
          table.contents.push_back(contents);
        }
      }
    }
    found = tables_.emplace(wrapper, std::move(table)).first;
  }
  const Table& table = found->second;
  if (entry < table.data || (entry - table.data) % kShapeDataSize != 0)
    return nullptr;
  *index = (entry - table.data) / kShapeDataSize;
  return *index < table.contents.size() ? &table : nullptr;
}

bool ShapeContentsFilter::AllowsShape(uintptr_t shape) {
  size_t index = 0;
  const Table* table = Lookup(shape, &index);
  return !table || BlocksPlayer(table->contents[index]);
}

bool ShapeContentsFilter::AllowsTag(uintptr_t shape, uint16_t tag) {
  size_t index = 0;
  const Table* table = Lookup(shape, &index);
  // Havok's invalid tag inherits the parent shape's data.
  if (!table || tag == 0xffff || tag >= table->contents.size()) return true;
  return BlocksPlayer(table->contents[tag]);
}

}  // namespace doomfix::cv
