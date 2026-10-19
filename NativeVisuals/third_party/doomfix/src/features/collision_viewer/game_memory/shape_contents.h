#pragma once

#include <optional>
#include <unordered_map>
#include <vector>

#include "features/collision_viewer/game_memory/memory_reader.h"

namespace doomfix::cv {

// The world contains query/projectile-only shapes as well as player obstacles.
// Resolve engine contentFlags through the validated idHavokShape table. Missing
// metadata stays visible; arbitrary Havok userData is never treated as flags.
class ShapeContentsFilter {
 public:
  explicit ShapeContentsFilter(MemoryReader memory = {}) : memory_(memory) {}
  void Clear() { tables_.clear(); }
  bool AllowsShape(uintptr_t shape);
  bool AllowsTag(uintptr_t shape, uint16_t tag);
  static bool BlocksPlayer(uint32_t contents);

 private:
  struct Table {
    uintptr_t data = 0;
    std::vector<uint32_t> contents;
  };
  const Table* Lookup(uintptr_t shape, size_t* index);
  MemoryReader memory_;
  std::unordered_map<uintptr_t, Table> tables_;
};

}  // namespace doomfix::cv
