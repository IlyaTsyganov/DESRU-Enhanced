#pragma once

#include <vulkan/vulkan.h>

#include <utility>
#include <vector>

namespace doomfix {

// Изменяемая копия VkPresentInfoKHR на время одного вызова vkQueuePresentKHR. Ей владеет ядро;
// фичи могут подменить список семафоров ожидания (например, дождаться своей отрисовки поверх кадра
// вместо исходных семафоров игры). Остальные поля и цепочка pNext передаются как есть.
class PresentEdit {
 public:
  explicit PresentEdit(const VkPresentInfoKHR& original)
      : original_(original),
        info_(original),
        waitSemaphores_(original.pWaitSemaphores,
                        original.pWaitSemaphores + original.waitSemaphoreCount) {}
  PresentEdit(const PresentEdit&) = delete;
  PresentEdit& operator=(const PresentEdit&) = delete;

  // Что передало приложение.
  const VkPresentInfoKHR& Original() const { return original_; }

  // Текущий список семафоров ожидания (с учётом правок предыдущих фич).
  const std::vector<VkSemaphore>& WaitSemaphores() const { return waitSemaphores_; }
  void SetWaitSemaphores(std::vector<VkSemaphore> semaphores) {
    waitSemaphores_ = std::move(semaphores);
    changed_ = true;
  }
  bool Changed() const { return changed_; }

  // Итоговая структура для следующего звена. Действительна, пока жив объект.
  const VkPresentInfoKHR* Result() {
    if (!changed_) return &original_;
    info_ = original_;
    info_.waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores_.size());
    info_.pWaitSemaphores = waitSemaphores_.empty() ? nullptr : waitSemaphores_.data();
    return &info_;
  }

 private:
  const VkPresentInfoKHR original_;
  VkPresentInfoKHR info_;
  std::vector<VkSemaphore> waitSemaphores_;
  bool changed_ = false;
};

}  // namespace doomfix
