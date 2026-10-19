// Based on doom-eternal-collision-viewer by IlyaTsyganov (commit 43ac57d), used with the author's permission.
//
// Трекер буфера глубины игры. Следит за картинками глубины, их view, проходами и framebuffer'ами,
// записывает для каждого командного буфера ленту событий (viewport, проходы, барьеры, вторичные
// буферы) и при отправке проигрывает ленты: узнаёт layout каждой картинки глубины, когда её последний
// раз писали и сохраняли, с каким viewport (в том числе reverse-Z) и на каком семействе очередей.
// В present выбирает буфер глубины экрана, который можно читать в шейдере оверлея.
//
// Без вызовов Vulkan: только учёт. Создание sample view и барьеры делает рендерер.
//
// Потоки и блокировки:
//   - запись команд (Record*) идёт с потоков игры, часто параллельно. Ленты лежат в шардированной
//     карте «командный буфер -> лента»: каждый шард со своим мьютексом, глобальной блокировки нет;
//   - создание/уничтожение ресурсов, отправка (OnSubmitted) и present (Select*, MarkPresent) — под
//     одним мьютексом состояния: это редкие или и так последовательные вызовы;
//   - хэндлы в событиях записи разбираются при отправке, а не при записи: поток записи не трогает
//     общие таблицы ресурсов.

#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace doomfix::cv {

bool IsDepthFormat(VkFormat format);
VkImageAspectFlags DepthAspects(VkFormat format);

// Состояние картинки глубины по уже отправленной работе.
struct DepthImage {
  VkImage handle = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkExtent3D extent = {};
  VkImageUsageFlags usage = 0;
  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
  uint32_t layers = 0;
  uint32_t mips = 0;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  uint32_t owner = VK_QUEUE_FAMILY_IGNORED;  // семейство очередей, которому принадлежит картинка
  VkViewport viewport = {};                  // лучший viewport последней записи глубины
  uint64_t written = 0;                      // номер отправки, в которой её последний раз писали
  uint64_t order = 0;                        // порядковый номер этой записи среди всех картинок
  bool stored = false;                       // глубина сохранена (storeOp STORE), её можно читать
  VkImageView sampleView = VK_NULL_HANDLE;   // view для шейдера оверлея (создаёт рендерер)
};

class DepthTracker {
 public:
  explicit DepthTracker(bool lateAttach=false);
  DepthTracker(const DepthTracker&) = delete;
  DepthTracker& operator=(const DepthTracker&) = delete;

  // --- Ресурсы (мьютекс состояния) ---------------------------------------------------------
  void OnImageCreated(VkImage image, const VkImageCreateInfo& info);
  // Возвращает sample view картинки (если рендерер его создавал): его нужно уничтожить.
  VkImageView OnImageDestroyed(VkImage image);
  void OnImageViewCreated(VkImageView view, const VkImageViewCreateInfo& info);
  void OnImageViewDestroyed(VkImageView view);
  void OnRenderPassCreated(VkRenderPass pass, const VkRenderPassCreateInfo& info);
  void OnRenderPass2Created(VkRenderPass pass, const VkRenderPassCreateInfo2& info);
  void OnRenderPassDestroyed(VkRenderPass pass);
  void OnFramebufferCreated(VkFramebuffer framebuffer, const VkFramebufferCreateInfo& info);
  void OnFramebufferDestroyed(VkFramebuffer framebuffer);

  // --- Командные буферы и пулы ----------------------------------------------------------------
  void OnAllocated(VkCommandPool pool, uint32_t count, const VkCommandBuffer* buffers);
  void OnBegin(VkCommandBuffer cb);
  void OnReset(VkCommandBuffer cb);
  void OnFreed(VkCommandPool pool, uint32_t count, const VkCommandBuffer* buffers);
  void OnPoolReset(VkCommandPool pool);
  void OnPoolDestroyed(VkCommandPool pool);

  // --- Запись команд (шард командного буфера) -------------------------------------------------
  void RecordViewport(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkViewport* viewports);
  void RecordBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo& info);
  void RecordEndRenderPass(VkCommandBuffer cb);
  void RecordBeginRendering(VkCommandBuffer cb, const VkRenderingInfo& info);
  void RecordEndRendering(VkCommandBuffer cb);
  void RecordExecuteCommands(VkCommandBuffer cb, uint32_t count, const VkCommandBuffer* secondaries);
  // Барьеры только читаются: смена layout картинок глубины и передача владения.
  void RecordImageBarriers(VkCommandBuffer cb, uint32_t count, const VkImageMemoryBarrier* barriers);
  void RecordImageBarriers2(VkCommandBuffer cb, uint32_t count, const VkImageMemoryBarrier2* barriers);

  // --- Отправка и present (мьютекс состояния) ---------------------------------------------------
  // Успешная отправка буферов cbs на очередь семейства family (одна отправка = один номер).
  void OnSubmitted(uint32_t family, uint32_t count, const VkCommandBuffer* cbs);
  // То же для VkSubmitInfo / VkSubmitInfo2 целиком (все пакеты — одна отправка).
  void OnSubmitted(uint32_t family, uint32_t submitCount, const VkSubmitInfo* submits);
  void OnSubmitted2(uint32_t family, uint32_t submitCount, const VkSubmitInfo2* submits);

  // Свежий (записан после прошлого present и сохранён) буфер глубины размером с экран, который можно
  // читать в шейдере на семействе family. Копия записи или false, если подходящего нет.
  bool SelectScreenDepth(uint32_t screenWidth, uint32_t screenHeight, uint32_t family,
                         DepthImage* out) const;
  // Запомнить sample view картинки (рендерер создал его сам). false — картинки уже нет.
  bool SetSampleView(VkImage image, VkImageView view);
  // Граница кадра: всё, что отправлено до этого места, относится к уже показанному кадру.
  void MarkPresent();

  // Все sample view (при уничтожении устройства их уничтожает рендерер). Очищает их у картинок.
  std::vector<VkImageView> TakeSampleViews();

  // Для логов и тестов.
  size_t DepthImageCount() const;
  uint64_t SubmitCount() const;
  bool FindImage(VkImage image, DepthImage* out) const;
  size_t RecordedBufferCount() const;

 private:
  struct Event {
    enum Kind : uint8_t {
      kViewport,         // vkCmdSetViewport (первый viewport)
      kBeginPass,        // начало прохода: лучший viewport считается заново
      kEndPass,          // конец прохода v1/v2: глубина из вложений framebuffer'а
      kBeginRendering,   // dynamic rendering: начало, view глубины
      kEndRendering,     // dynamic rendering: конец, viewport на момент конца
      kBarrier,          // смена layout / владельца картинки
      kExecute,          // вызов вторичного буфера
    } kind = kViewport;
    VkViewport viewport = {};
    // kEndPass: проход, framebuffer и (для imageless framebuffer) диапазон views в Record::views.
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    uint32_t viewsBegin = 0;
    uint32_t viewsCount = 0;
    // kBeginRendering / kEndRendering: view глубины; kBarrier: картинка.
    VkImageView view = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t owner = VK_QUEUE_FAMILY_IGNORED;
    bool stored = false;
    VkCommandBuffer secondary = VK_NULL_HANDLE;
  };

  // Лента одного командного буфера и состояние его записи.
  struct Record {
    std::vector<Event> events;
    std::vector<VkImageView> views;  // вложения imageless framebuffer'ов
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    uint32_t passViewsBegin = 0;
    uint32_t passViewsCount = 0;
    VkViewport viewport = {};  // последний viewport в этом буфере
    // dynamic rendering: view глубины текущего прохода, её storeOp и область рисования.
    VkImageView renderingView = VK_NULL_HANDLE;
    bool renderingStored = false;
    VkViewport renderingArea = {};

    void Clear();
  };

  static constexpr size_t kShardCount = 64;
  struct Shard {
    std::mutex mutex;
    std::unordered_map<VkCommandBuffer, Record> records;
  };

  struct View {
    VkImage image = VK_NULL_HANDLE;
    VkImageSubresourceRange range = {};
  };
  struct Attachment {
    VkImageLayout finalLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkAttachmentStoreOp storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    bool depth = false;
  };
  struct Pass {
    std::vector<Attachment> attachments;
  };
  struct Framebuffer {
    std::vector<VkImageView> views;
    VkExtent2D extent = {};
  };
  // Лучший viewport во время проигрывания (наследуется вторичными буферами).
  struct Execution {
    VkViewport current = {};
    VkViewport best = {};
  };

  Shard& ShardOf(VkCommandBuffer cb) const;
  template <typename Fn>
  void WithRecord(VkCommandBuffer cb, Fn&& fn);
  template <typename Barrier>
  void RecordBarriers(VkCommandBuffer cb, uint32_t count, const Barrier* barriers);
  template <typename Info>
  void RememberPass(VkRenderPass pass, const Info& info);

  // Под mutex_.
  void Apply(VkCommandBuffer cb, uint32_t family, uint32_t nesting, Execution& execution);
  // replaceViewport — viewport этой записи главнее прежнего (конец dynamic rendering: в начале прохода
  // был известен только renderArea).
  void ApplyUse(VkImage image, const VkViewport& eventViewport, bool stored, uint32_t family,
                const Execution& execution, bool replaceViewport = false);
  void ApplyLayout(VkImage image, VkImageLayout layout, uint32_t owner);
  bool ViewImage(VkImageView view, VkImage* image) const;

  mutable std::array<Shard, kShardCount> shards_;

  std::mutex poolsMutex_;
  std::unordered_map<VkCommandPool, std::unordered_set<VkCommandBuffer>> pools_;

  mutable std::mutex mutex_;  // всё ниже
  std::unordered_map<VkImage, DepthImage> images_;
  std::unordered_map<VkImageView, View> views_;
  std::unordered_map<VkRenderPass, Pass> passes_;
  std::unordered_map<VkFramebuffer, Framebuffer> framebuffers_;
  uint64_t submitted_ = 0;
  uint64_t previousPresent_ = 0;
  uint64_t order_ = 0;
  bool lateAttach_ = false;
  // Копии лент при проигрывании, по уровню вложенности (без выделений памяти на каждой отправке).
  std::vector<std::vector<Event>> scratchEvents_;
  std::vector<std::vector<VkImageView>> scratchViews_;
};

}  // namespace doomfix::cv
