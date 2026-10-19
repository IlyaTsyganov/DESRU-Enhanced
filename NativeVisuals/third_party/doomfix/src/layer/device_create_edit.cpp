#include "layer/device_create_edit.h"

#include <algorithm>

#include "common/log.h"
#include "common/util.h"
#include "layer/vk_util.h"

namespace doomfix {

DeviceCreateEdit::DeviceCreateEdit(const VkDeviceCreateInfo& original)
    : info_(original),
      extensions_(original.ppEnabledExtensionNames,
                  original.ppEnabledExtensionNames + original.enabledExtensionCount) {
  for (uint32_t i = 0; i < original.queueCreateInfoCount; ++i) {
    const VkDeviceQueueCreateInfo& q = original.pQueueCreateInfos[i];
    queues_.push_back(q);
    priorities_.emplace_back(q.pQueuePriorities, q.pQueuePriorities + q.queueCount);
  }
}

DeviceCreateEdit::~DeviceCreateEdit() {
  for (auto it = patches_.rbegin(); it != patches_.rend(); ++it) {
    it->node->pNext = it->originalNext;
  }
}

bool DeviceCreateEdit::HasExtension(std::string_view name) const {
  return std::any_of(extensions_.begin(), extensions_.end(),
                     [&](const char* ext) { return name == ext; });
}

bool DeviceCreateEdit::RemoveExtension(std::string_view name) {
  const auto end = std::remove_if(extensions_.begin(), extensions_.end(),
                                  [&](const char* ext) { return name == ext; });
  const bool found = end != extensions_.end();
  extensions_.erase(end, extensions_.end());
  return found;
}

const VkBaseInStructure* DeviceCreateEdit::FindStruct(VkStructureType type) const {
  for (auto* s = static_cast<const VkBaseInStructure*>(info_.pNext); s; s = s->pNext) {
    if (s->sType == type) return s;
  }
  return nullptr;
}

int DeviceCreateEdit::RemoveStructs(VkStructureType type) {
  auto* const head = reinterpret_cast<VkBaseOutStructure*>(&info_);
  int removed = 0;
  VkBaseOutStructure* prev = head;
  while (VkBaseOutStructure* cur = prev->pNext) {
    if (cur->sType != type) {
      prev = cur;
      continue;
    }
    if (prev != head) {
      if (!IsWritable(&prev->pNext)) {
        DF_LOG_ERROR("cannot unlink %s: preceding %s is in read-only memory",
                     DescribeStructureType(type).c_str(),
                     DescribeStructureType(prev->sType).c_str());
        prev = cur;
        continue;
      }
      DF_LOG_DEBUG("temporarily patching pNext of %s (restored after vkCreateDevice)",
                   DescribeStructureType(prev->sType).c_str());
      patches_.push_back({prev, prev->pNext});
    }
    prev->pNext = cur->pNext;
    ++removed;
  }
  return removed;
}

bool DeviceCreateEdit::AddQueue(uint32_t family, uint32_t* index) {
  if (family >= families_.size()) return false;
  // Защищённые очереди (flags) — отдельные описания; берём обычное описание этого семейства.
  for (size_t i = 0; i < queues_.size(); ++i) {
    VkDeviceQueueCreateInfo& q = queues_[i];
    if (q.queueFamilyIndex != family || q.flags != 0) continue;
    if (q.queueCount >= families_[family].queueCount) return false;
    *index = q.queueCount;
    priorities_[i].push_back(priorities_[i].empty() ? 1.0f : priorities_[i].front());
    ++q.queueCount;
    return true;
  }
  if (!families_[family].queueCount) return false;
  VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  q.queueFamilyIndex = family;
  q.queueCount = 1;
  queues_.push_back(q);
  priorities_.push_back({1.0f});
  *index = 0;
  return true;
}

const VkDeviceCreateInfo* DeviceCreateEdit::Result() {
  for (size_t i = 0; i < queues_.size(); ++i) queues_[i].pQueuePriorities = priorities_[i].data();
  info_.queueCreateInfoCount = static_cast<uint32_t>(queues_.size());
  info_.pQueueCreateInfos = queues_.empty() ? nullptr : queues_.data();
  info_.enabledExtensionCount = static_cast<uint32_t>(extensions_.size());
  info_.ppEnabledExtensionNames = extensions_.empty() ? nullptr : extensions_.data();
  return &info_;
}

}  // namespace doomfix
