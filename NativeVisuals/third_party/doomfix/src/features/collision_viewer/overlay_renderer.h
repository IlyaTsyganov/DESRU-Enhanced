// Based on doom-eternal-collision-viewer by IlyaTsyganov (commit 43ac57d), used with the author's permission.
//
// Рендерер оверлея: рисует сцену источника поверх картинки swapchain внутри vkQueuePresentKHR.
//
// Как это устроено (как в исходном viewer):
//   - свой render pass (LOAD/STORE, PRESENT_SRC -> PRESENT_SRC), два пайплайна (заливка треугольниками и
//     контуры линиями), push-константы с камерой, буфер глубины игры читается в шейдере (reverse-Z);
//   - на каждую картинку swapchain свой кадр: командный буфер, семафор, фенс, буфер вершин;
//   - свой vkQueueSubmit ждёт семафоры present'а игры и сигналит свой семафор; present ждёт уже его
//     (PresentEdit::SetWaitSemaphores);
//   - картинка глубины игры на время отрисовки переводится в DEPTH_STENCIL_READ_ONLY_OPTIMAL и
//     возвращается в свой layout тем же командным буфером.
// Present с очереди без графики (DOOM Eternal 1.1 показывает кадр с compute-очереди): оверлей рисуется на
// своей графической очереди, которую фича просит у драйвера при создании устройства (SetOverlayQueue), а
// картинка swapchain при EXCLUSIVE передаётся между семействами очередей барьерами (release/acquire):
// очередь present -> своя очередь -> очередь present, три отправки, связанные семафорами.
//
// Отличия от исходного viewer: present никогда не ждёт GPU — если кадр оверлея для этой картинки ещё
// занят, оверлей в этом кадре пропускается; ресурсы создаются только когда есть что рисовать; все
// вызовы Vulkan — через DeviceData::vk; ошибки — исключения, которые фича превращает в выключение viewer.
//
// Все методы — под одним мьютексом рендерера (present, создание/уничтожение swapchain и картинок).

#pragma once

#include <vulkan/vulkan.h>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "features/collision_viewer/depth_tracker.h"
#include "features/collision_viewer/overlay_types.h"
#include "features/collision_viewer/sources.h"
#include "features/collision_viewer/test_scene.h"

namespace doomfix {
struct DeviceData;
struct QueueData;
class PresentEdit;
}  // namespace doomfix

namespace doomfix::cv {

class OverlayRenderer {
 public:
  OverlayRenderer(DeviceData& device, DepthTracker& tracker, ICameraSource& camera, ISceneSource& scene,
                  const TestOptions& test);
  OverlayRenderer(const OverlayRenderer&) = delete;
  OverlayRenderer& operator=(const OverlayRenderer&) = delete;

  // Своя графическая очередь (полученная фичей при создании устройства) для present с очереди без
  // графики. VK_NULL_HANDLE — её нет: тогда оверлей рисуется только при present с графической очереди.
  void SetOverlayQueue(VkQueue queue, uint32_t family);

  // Перед vkCreateSwapchainKHR: добавить в imageUsage то, что нужно оверлею (COLOR_ATTACHMENT, в
  // тестовом режиме с чтением кадра — TRANSFER_SRC), если поверхность это поддерживает.
  void PrepareSwapchain(VkSwapchainCreateInfoKHR& info);
  void OnSwapchainCreated(VkSwapchainKHR swapchain, const VkSwapchainCreateInfoKHR& info);
  void OnSwapchainDestroyed(VkSwapchainKHR swapchain);

  // vkQueuePresentKHR: нарисовать оверлей и подменить семафоры ожидания в edit. Бросает исключение
  // при ошибке; если исключение вылетело до отправки своей работы, edit не изменён.
  void Present(const QueueData& queue, PresentEdit& edit);
  // Граница кадра для трекера глубины: вызывается в каждом present, что бы ни случилось.
  void EndPresent();

  // Игра уничтожает картинку глубины; sampleView — наш view для неё (или VK_NULL_HANDLE). Ждёт кадры
  // оверлея, которые её читают, и уничтожает view.
  void OnDepthImageDestroyed(VkImage image, VkImageView sampleView);

  // Перед vkDestroyDevice: уничтожить все свои объекты. Не бросает.
  void Shutdown();

 private:
  struct Frame {
    VkImage image = VK_NULL_HANDLE;  // картинка swapchain
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkSemaphore complete = VK_NULL_HANDLE;  // сигналит наш submit, ждёт present
    VkFence fence = VK_NULL_HANDLE;
    VkBuffer vertices = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
    void* vertexMapped = nullptr;
    uint32_t vertexCapacity = 0;
    VkDescriptorSet depthSet = VK_NULL_HANDLE;
    VkImageView boundView = VK_NULL_HANDLE;  // что записано в depthSet
    VkImage boundDepth = VK_NULL_HANDLE;     // картинка глубины, которую читал последний submit
    VkBuffer readback = VK_NULL_HANDLE;      // тестовый режим: копия кадра для проверки пикселей
    VkDeviceMemory readbackMemory = VK_NULL_HANDLE;
    void* readbackMapped = nullptr;
    // Передача картинки между семействами (present с очереди без графики, EXCLUSIVE): командные буферы
    // семейства present (release — отдать картинку своей очереди, acquire — вернуть) и семафоры между
    // тремя отправками.
    VkCommandBuffer release = VK_NULL_HANDLE;
    VkCommandBuffer acquire = VK_NULL_HANDLE;
    VkSemaphore released = VK_NULL_HANDLE;
    VkSemaphore drawn = VK_NULL_HANDLE;
  };

  struct Swapchain {
    VkSwapchainKHR handle = VK_NULL_HANDLE;
    VkExtent2D extent = {};
    VkFormat format = VK_FORMAT_UNDEFINED;
    bool colorAttachment = false;  // можно рисовать в картинки
    bool transferSrc = false;      // можно читать картинки (тестовый режим)
    VkSharingMode sharing = VK_SHARING_MODE_EXCLUSIVE;
    std::vector<uint32_t> sharingFamilies;  // для VK_SHARING_MODE_CONCURRENT
    std::vector<VkImage> images;
    bool initialized = false;
    bool failed = false;           // инициализация не удалась: не пробовать снова
    uint32_t family = VK_QUEUE_FAMILY_IGNORED;  // семейство, на котором рисует оверлей
    // Передача между семействами: семейство present, для которого записаны Frame::release/acquire.
    uint32_t transferFamily = VK_QUEUE_FAMILY_IGNORED;
    VkCommandPool transferPool = VK_NULL_HANDLE;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline fill = VK_NULL_HANDLE;
    VkPipeline lines = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    // Картинка-заглушка для дескриптора, пока буфер глубины игры не найден.
    VkImage dummyImage = VK_NULL_HANDLE;
    VkDeviceMemory dummyMemory = VK_NULL_HANDLE;
    VkImageView dummyView = VK_NULL_HANDLE;
    bool dummyReady = false;  // переведена в SHADER_READ_ONLY_OPTIMAL
    std::vector<Frame> frames;
  };

  void Initialize(Swapchain& swap, uint32_t family);
  // Нужна ли передача картинки между семействами presentFamily и swap.family.
  bool NeedsTransfer(const Swapchain& swap, uint32_t presentFamily) const;
  // Командные буферы и семафоры передачи для семейства present; false — уже подготовлены для другого.
  bool PrepareTransfer(Swapchain& swap, uint32_t presentFamily);
  VkPipeline CreatePipeline(Swapchain& swap, VkShaderModule vertex, VkShaderModule fragment,
                            VkPrimitiveTopology topology);
  void CreateHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer* buffer,
                        VkDeviceMemory* memory, void** mapped);
  void DestroyHostBuffer(VkBuffer* buffer, VkDeviceMemory* memory, void** mapped);
  void EnsureVertexCapacity(Frame& frame, uint32_t count);
  // Ждёт кадры оверлея (не дольше timeout); false — какой-то кадр так и не завершился.
  bool WaitFrames(const Swapchain& swap, uint64_t timeoutNs, VkImage onlyReading = VK_NULL_HANDLE);
  void Destroy(Swapchain& swap);
  void LogDepthChoice(bool found, const DepthImage& depth);
  void LogStatistics(bool force = false);

  DeviceData& device_;
  DepthTracker& tracker_;
  ICameraSource& camera_;
  ISceneSource& scene_;
  TestOptions test_;

  std::mutex mutex_;
  std::unordered_map<VkSwapchainKHR, Swapchain> swapchains_;
  OverlayScene sceneData_;  // переиспользуется между кадрами
  uint64_t frameNumber_ = 0;

  // Статистика и логи (под mutex_).
  using Clock = std::chrono::steady_clock;
  uint64_t drawn_ = 0;
  uint64_t skippedBusy_ = 0;
  uint64_t reportedDrawn_ = 0;
  uint64_t reportedSkipped_ = 0;
  Clock::time_point lastStatistics_ = Clock::now();
  VkImage loggedDepth_ = VK_NULL_HANDLE;
  bool everFoundDepth_ = false;
  bool lostLogged_ = false;
  bool waitingLogged_ = false;
  uint32_t depthChanges_ = 0;
  Clock::time_point firstPresent_ = {};
  Clock::time_point lastDepthLog_ = {};
  Clock::time_point lastDepthSeen_ = {};
  bool warnedQueue_ = false;
  VkQueue overlayQueue_ = VK_NULL_HANDLE;
  uint32_t overlayFamily_ = VK_QUEUE_FAMILY_IGNORED;
  std::vector<uint32_t> loggedPresentFamilies_;
};

}  // namespace doomfix::cv
