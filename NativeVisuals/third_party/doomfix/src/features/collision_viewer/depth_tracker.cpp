// Based on doom-eternal-collision-viewer by IlyaTsyganov (commit 43ac57d), used with the author's permission.

#include "features/collision_viewer/depth_tracker.h"

#include <cmath>
#include <cstdint>
#include "runtime.h"

namespace doomfix::cv {
namespace {

// Глубже вторичные буферы не разбираются (защита от циклов в неверных данных).
constexpr uint32_t kMaxNesting = 16;

bool HasArea(const VkViewport& v) { return v.width != 0.0f && v.height != 0.0f; }

// Лучше ли viewport candidate для чтения глубины, чем previous: больший диапазон глубины (например,
// оружие рисуется с урезанным диапазоном), при равном — большая площадь.
bool BetterDepthViewport(const VkViewport& candidate, const VkViewport& previous) {
  if (!HasArea(candidate)) return false;
  const float range = std::fabs(candidate.maxDepth - candidate.minDepth);
  const float oldRange = std::fabs(previous.maxDepth - previous.minDepth);
  return !HasArea(previous) || range > oldRange + 0.001f ||
         (std::fabs(range - oldRange) < 0.001f &&
          std::fabs(candidate.width * candidate.height) >
              std::fabs(previous.width * previous.height) * 1.01f);
}

}  // namespace

bool IsDepthFormat(VkFormat format) {
  switch (format) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
      return true;
    default:
      return false;
  }
}

VkImageAspectFlags DepthAspects(VkFormat format) {
  const bool stencil = format == VK_FORMAT_D16_UNORM_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT ||
                       format == VK_FORMAT_D32_SFLOAT_S8_UINT;
  return VK_IMAGE_ASPECT_DEPTH_BIT | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
}

void DepthTracker::Record::Clear() {
  events.clear();
  views.clear();
  pass = VK_NULL_HANDLE;
  framebuffer = VK_NULL_HANDLE;
  passViewsBegin = passViewsCount = 0;
  viewport = {};
  renderingView = VK_NULL_HANDLE;
  renderingStored = false;
  renderingArea = {};
}

DepthTracker::DepthTracker(bool lateAttach)
    : lateAttach_(lateAttach),scratchEvents_(kMaxNesting + 2), scratchViews_(kMaxNesting + 2) {}

// --- Ресурсы ------------------------------------------------------------------------------------

void DepthTracker::OnImageCreated(VkImage image, const VkImageCreateInfo& info) {
  if (!IsDepthFormat(info.format)) return;
  DepthImage value;
  value.handle = image;
  value.format = info.format;
  value.extent = info.extent;
  value.usage = info.usage;
  value.samples = info.samples;
  value.layers = info.arrayLayers;
  value.mips = info.mipLevels;
  value.layout = info.initialLayout;
  std::lock_guard lock(mutex_);
  images_[image] = value;
}

VkImageView DepthTracker::OnImageDestroyed(VkImage image) {
  std::lock_guard lock(mutex_);
  auto it = images_.find(image);
  if (it == images_.end()) return VK_NULL_HANDLE;
  const VkImageView view = it->second.sampleView;
  images_.erase(it);
  return view;
}

void DepthTracker::OnImageViewCreated(VkImageView view, const VkImageViewCreateInfo& info) {
  std::lock_guard lock(mutex_);
  if (images_.count(info.image)) views_[view] = {info.image, info.subresourceRange};
}

void DepthTracker::OnImageViewDestroyed(VkImageView view) {
  std::lock_guard lock(mutex_);
  views_.erase(view);
}

template <typename Info>
void DepthTracker::RememberPass(VkRenderPass pass, const Info& info) {
  Pass value;
  bool useful = false;
  for (uint32_t i = 0; i < info.attachmentCount; ++i) {
    const auto& a = info.pAttachments[i];
    const bool depth = IsDepthFormat(a.format);
    useful = useful || depth;
    value.attachments.push_back({a.finalLayout, a.storeOp, depth});
  }
  if (!useful) return;
  std::lock_guard lock(mutex_);
  passes_[pass] = std::move(value);
}

void DepthTracker::OnRenderPassCreated(VkRenderPass pass, const VkRenderPassCreateInfo& info) {
  RememberPass(pass, info);
}

void DepthTracker::OnRenderPass2Created(VkRenderPass pass, const VkRenderPassCreateInfo2& info) {
  RememberPass(pass, info);
}

void DepthTracker::OnRenderPassDestroyed(VkRenderPass pass) {
  std::lock_guard lock(mutex_);
  passes_.erase(pass);
}

void DepthTracker::OnFramebufferCreated(VkFramebuffer framebuffer, const VkFramebufferCreateInfo& info) {
  std::lock_guard lock(mutex_);
  if (!passes_.count(info.renderPass)) return;
  Framebuffer value;
  value.extent = {info.width, info.height};
  // У imageless framebuffer'а вложения приходят в vkCmdBeginRenderPass.
  if (info.pAttachments && !(info.flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT)) {
    value.views.assign(info.pAttachments, info.pAttachments + info.attachmentCount);
  }
  framebuffers_[framebuffer] = std::move(value);
}

void DepthTracker::OnFramebufferDestroyed(VkFramebuffer framebuffer) {
  std::lock_guard lock(mutex_);
  framebuffers_.erase(framebuffer);
}

// --- Командные буферы и пулы -----------------------------------------------------------------------

DepthTracker::Shard& DepthTracker::ShardOf(VkCommandBuffer cb) const {
  const auto v = reinterpret_cast<uintptr_t>(cb);
  return shards_[((v >> 4) ^ (v >> 10) ^ (v >> 16)) % kShardCount];
}

template <typename Fn>
void DepthTracker::WithRecord(VkCommandBuffer cb, Fn&& fn) {
  Shard& shard = ShardOf(cb);
  std::lock_guard lock(shard.mutex);
  fn(shard.records[cb]);
}

void DepthTracker::OnAllocated(VkCommandPool pool, uint32_t count, const VkCommandBuffer* buffers) {
  std::lock_guard lock(poolsMutex_);
  auto& set = pools_[pool];
  for (uint32_t i = 0; i < count; ++i) set.insert(buffers[i]);
}

void DepthTracker::OnBegin(VkCommandBuffer cb) {
  WithRecord(cb, [](Record& r) { r.Clear(); });
}

void DepthTracker::OnReset(VkCommandBuffer cb) {
  Shard& shard = ShardOf(cb);
  std::lock_guard lock(shard.mutex);
  auto it = shard.records.find(cb);
  if (it != shard.records.end()) it->second.Clear();
}

void DepthTracker::OnFreed(VkCommandPool pool, uint32_t count, const VkCommandBuffer* buffers) {
  {
    std::lock_guard lock(poolsMutex_);
    auto it = pools_.find(pool);
    if (it != pools_.end()) {
      for (uint32_t i = 0; i < count; ++i) it->second.erase(buffers[i]);
    }
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (!buffers[i]) continue;
    Shard& shard = ShardOf(buffers[i]);
    std::lock_guard lock(shard.mutex);
    shard.records.erase(buffers[i]);
  }
}

void DepthTracker::OnPoolReset(VkCommandPool pool) {
  std::lock_guard lock(poolsMutex_);
  auto it = pools_.find(pool);
  if (it == pools_.end()) return;
  for (VkCommandBuffer cb : it->second) {
    Shard& shard = ShardOf(cb);
    std::lock_guard shardLock(shard.mutex);
    auto record = shard.records.find(cb);
    if (record != shard.records.end()) record->second.Clear();
  }
}

void DepthTracker::OnPoolDestroyed(VkCommandPool pool) {
  std::lock_guard lock(poolsMutex_);
  auto it = pools_.find(pool);
  if (it == pools_.end()) return;
  for (VkCommandBuffer cb : it->second) {
    Shard& shard = ShardOf(cb);
    std::lock_guard shardLock(shard.mutex);
    shard.records.erase(cb);
  }
  pools_.erase(it);
}

// --- Запись команд ---------------------------------------------------------------------------------

void DepthTracker::RecordViewport(VkCommandBuffer cb, uint32_t first, uint32_t count,
                                  const VkViewport* viewports) {
  if (first != 0 || count == 0 || !viewports) return;
  WithRecord(cb, [&](Record& r) {
    r.viewport = viewports[0];
    Event e;
    e.kind = Event::kViewport;
    e.viewport = viewports[0];
    r.events.push_back(e);
  });
}

void DepthTracker::RecordBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo& info) {
  WithRecord(cb, [&](Record& r) {
    r.pass = info.renderPass;
    r.framebuffer = info.framebuffer;
    r.passViewsBegin = r.passViewsCount = 0;
    for (auto* s = static_cast<const VkBaseInStructure*>(info.pNext); s; s = s->pNext) {
      if (s->sType != VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO) continue;
      const auto* attachments = reinterpret_cast<const VkRenderPassAttachmentBeginInfo*>(s);
      r.passViewsBegin = static_cast<uint32_t>(r.views.size());
      r.passViewsCount = attachments->attachmentCount;
      r.views.insert(r.views.end(), attachments->pAttachments,
                     attachments->pAttachments + attachments->attachmentCount);
    }
    Event e;
    e.kind = Event::kBeginPass;
    r.events.push_back(e);
  });
}

void DepthTracker::RecordEndRenderPass(VkCommandBuffer cb) {
  WithRecord(cb, [&](Record& r) {
    Event e;
    e.kind = Event::kEndPass;
    e.pass = r.pass;
    e.framebuffer = r.framebuffer;
    e.viewsBegin = r.passViewsBegin;
    e.viewsCount = r.passViewsCount;
    e.viewport = r.viewport;
    r.events.push_back(e);
    r.pass = VK_NULL_HANDLE;
    r.framebuffer = VK_NULL_HANDLE;
    r.passViewsBegin = r.passViewsCount = 0;
  });
}

void DepthTracker::RecordBeginRendering(VkCommandBuffer cb, const VkRenderingInfo& info) {
  WithRecord(cb, [&](Record& r) {
    Event e;
    e.kind = Event::kBeginRendering;
    e.viewport = {static_cast<float>(info.renderArea.offset.x),
                  static_cast<float>(info.renderArea.offset.y),
                  static_cast<float>(info.renderArea.extent.width),
                  static_cast<float>(info.renderArea.extent.height), 0.0f, 1.0f};
    r.renderingView = VK_NULL_HANDLE;
    if (const VkRenderingAttachmentInfo* depth = info.pDepthAttachment) {
      e.view = depth->imageView;
      e.layout = depth->imageLayout;
      e.stored = depth->storeOp == VK_ATTACHMENT_STORE_OP_STORE;
      r.renderingView = depth->imageView;
      r.renderingStored = e.stored;
      r.renderingArea = e.viewport;
    }
    r.events.push_back(e);
  });
}

void DepthTracker::RecordEndRendering(VkCommandBuffer cb) {
  WithRecord(cb, [&](Record& r) {
    if (!r.renderingView) return;
    // Глубина записана с viewport, который действовал к концу прохода (если его задавали).
    Event e;
    e.kind = Event::kEndRendering;
    e.view = r.renderingView;
    e.stored = r.renderingStored;
    e.viewport = HasArea(r.viewport) ? r.viewport : r.renderingArea;
    r.events.push_back(e);
    r.renderingView = VK_NULL_HANDLE;
  });
}

void DepthTracker::RecordExecuteCommands(VkCommandBuffer cb, uint32_t count,
                                         const VkCommandBuffer* secondaries) {
  if (!count || !secondaries) return;
  WithRecord(cb, [&](Record& r) {
    for (uint32_t i = 0; i < count; ++i) {
      Event e;
      e.kind = Event::kExecute;
      e.secondary = secondaries[i];
      r.events.push_back(e);
    }
  });
}

template <typename Barrier>
void DepthTracker::RecordBarriers(VkCommandBuffer cb, uint32_t count, const Barrier* barriers) {
  // Сначала без блокировки: в большинстве барьеров картинок глубины нет.
  bool any = false;
  for (uint32_t i = 0; i < count && !any; ++i) {
    any = (barriers[i].subresourceRange.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
  }
  if (!any) return;
  WithRecord(cb, [&](Record& r) {
    for (uint32_t i = 0; i < count; ++i) {
      const Barrier& b = barriers[i];
      if (!(b.subresourceRange.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) ||
          b.subresourceRange.baseMipLevel || b.subresourceRange.baseArrayLayer) {
        continue;
      }
      Event e;
      e.kind = Event::kBarrier;
      e.image = b.image;
      e.layout = b.newLayout;
      e.owner = b.dstQueueFamilyIndex;
      // Late attachment may miss cached render-pass creation. An explicit
      // write-to-read dependency still proves that this depth image was
      // produced and preserved for sampling in this submission.
      e.stored = lateAttach_ &&
          (b.srcAccessMask & VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT) &&
          (b.dstAccessMask & (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT)) &&
          b.newLayout != VK_IMAGE_LAYOUT_UNDEFINED;
      r.events.push_back(e);
    }
  });
}

void DepthTracker::RecordImageBarriers(VkCommandBuffer cb, uint32_t count,
                                       const VkImageMemoryBarrier* barriers) {
  if (count && barriers) RecordBarriers(cb, count, barriers);
}

void DepthTracker::RecordImageBarriers2(VkCommandBuffer cb, uint32_t count,
                                        const VkImageMemoryBarrier2* barriers) {
  if (count && barriers) RecordBarriers(cb, count, barriers);
}

// --- Отправка ---------------------------------------------------------------------------------------

bool DepthTracker::ViewImage(VkImageView view, VkImage* image) const {
  auto it = views_.find(view);
  if (it == views_.end()) return false;
  *image = it->second.image;
  return true;
}

void DepthTracker::ApplyLayout(VkImage image, VkImageLayout layout, uint32_t owner) {
  auto it = images_.find(image);
  if (it == images_.end()) return;
  it->second.layout = layout;
  if (owner != VK_QUEUE_FAMILY_IGNORED) it->second.owner = owner;
}

void DepthTracker::ApplyUse(VkImage image, const VkViewport& eventViewport, bool stored,
                            uint32_t family, const Execution& execution, bool replaceViewport) {
  auto it = images_.find(image);
  if (it == images_.end()) return;
  DepthImage& value = it->second;
  const VkViewport& viewport = HasArea(execution.best)      ? execution.best
                               : HasArea(execution.current) ? execution.current
                                                            : eventViewport;
  if (replaceViewport || value.written <= previousPresent_ ||
      BetterDepthViewport(viewport, value.viewport)) {
    value.viewport = viewport;
  }
  value.stored = stored;
  value.written = submitted_;
  value.order = ++order_;
  value.owner = family;
}

void DepthTracker::Apply(VkCommandBuffer cb, uint32_t family, uint32_t nesting, Execution& execution) {
  if (nesting > kMaxNesting || !cb) return;
  // Копия ленты под блокировкой шарда: другой поток может в это время сбросить или освободить
  // буфер (когда GPU его уже выполнил), а вторичный буфер может лежать в том же шарде.
  std::vector<Event>& events = scratchEvents_[nesting];
  std::vector<VkImageView>& views = scratchViews_[nesting];
  {
    Shard& shard = ShardOf(cb);
    std::lock_guard lock(shard.mutex);
    auto it = shard.records.find(cb);
    if (it == shard.records.end()) return;
    events = it->second.events;
    views = it->second.views;
  }
  for (const Event& e : events) {
    switch (e.kind) {
      case Event::kExecute:
        Apply(e.secondary, family, nesting + 1, execution);
        break;
      case Event::kBeginPass:
        execution.best = {};
        break;
      case Event::kViewport:
        execution.current = e.viewport;
        if (BetterDepthViewport(e.viewport, execution.best)) execution.best = e.viewport;
        break;
      case Event::kEndPass: {
        auto pass = passes_.find(e.pass);
        auto fb = framebuffers_.find(e.framebuffer);
        if (pass == passes_.end() || fb == framebuffers_.end()) break;
        const VkImageView* attachmentViews = fb->second.views.data();
        size_t viewCount = fb->second.views.size();
        if (e.viewsCount && e.viewsBegin + e.viewsCount <= views.size()) {
          attachmentViews = views.data() + e.viewsBegin;
          viewCount = e.viewsCount;
        }
        for (size_t i = 0; i < pass->second.attachments.size() && i < viewCount; ++i) {
          const Attachment& attachment = pass->second.attachments[i];
          if (!attachment.depth) continue;
          auto view = views_.find(attachmentViews[i]);
          if (view == views_.end() || view->second.range.baseMipLevel ||
              view->second.range.baseArrayLayer) {
            continue;
          }
          VkViewport viewport = e.viewport;
          if (!HasArea(viewport)) {
            viewport = {0.0f, 0.0f, static_cast<float>(fb->second.extent.width),
                        static_cast<float>(fb->second.extent.height), 0.0f, 1.0f};
          }
          ApplyUse(view->second.image, viewport,
                   attachment.storeOp == VK_ATTACHMENT_STORE_OP_STORE, family, execution);
          ApplyLayout(view->second.image, attachment.finalLayout, VK_QUEUE_FAMILY_IGNORED);
        }
        break;
      }
      case Event::kBeginRendering: {
        execution.best = {};
        VkImage image;
        if (e.view && ViewImage(e.view, &image)) {
          ApplyUse(image, e.viewport, e.stored, family, execution);
          ApplyLayout(image, e.layout, VK_QUEUE_FAMILY_IGNORED);
        }
        break;
      }
      case Event::kEndRendering: {
        VkImage image;
        if (ViewImage(e.view, &image)) ApplyUse(image, e.viewport, e.stored, family, execution, true);
        break;
      }
      case Event::kBarrier:
        if(e.stored) {
          auto image=images_.find(e.image);
          if(image!=images_.end()) {
            const auto& size=image->second.extent;
            VkViewport viewport={0,0,static_cast<float>(size.width),static_cast<float>(size.height),0,1};
            ApplyUse(e.image,viewport,true,family,execution);
          }
        }
        ApplyLayout(e.image, e.layout, e.owner);
        break;
    }
  }
}

void DepthTracker::OnSubmitted(uint32_t family, uint32_t count, const VkCommandBuffer* cbs) {
  std::lock_guard lock(mutex_);
  ++submitted_;
  for (uint32_t i = 0; i < count; ++i) {
    Execution execution;
    Apply(cbs[i], family, 0, execution);
  }
}

void DepthTracker::OnSubmitted(uint32_t family, uint32_t submitCount, const VkSubmitInfo* submits) {
  std::lock_guard lock(mutex_);
  ++submitted_;
  for (uint32_t i = 0; i < submitCount; ++i) {
    for (uint32_t j = 0; j < submits[i].commandBufferCount; ++j) {
      Execution execution;
      Apply(submits[i].pCommandBuffers[j], family, 0, execution);
    }
  }
}

void DepthTracker::OnSubmitted2(uint32_t family, uint32_t submitCount, const VkSubmitInfo2* submits) {
  std::lock_guard lock(mutex_);
  ++submitted_;
  for (uint32_t i = 0; i < submitCount; ++i) {
    for (uint32_t j = 0; j < submits[i].commandBufferInfoCount; ++j) {
      Execution execution;
      Apply(submits[i].pCommandBufferInfos[j].commandBuffer, family, 0, execution);
    }
  }
}

// --- Present ------------------------------------------------------------------------------------------

bool DepthTracker::SelectScreenDepth(uint32_t screenWidth, uint32_t screenHeight, uint32_t family,
                                     DepthImage* out) const {
  if (!screenWidth || !screenHeight) return false;
  const double sw = screenWidth;
  const double sh = screenHeight;
  std::lock_guard lock(mutex_);
  const DepthImage* best = nullptr;
  double bestScore = -1e30;
  double bestRange = -1.0;
  for (const auto& [handle, image] : images_) {
    // Только то, что шейдер может прочитать как sampler2D: один сэмпл, один слой, SAMPLED.
    if (image.samples != VK_SAMPLE_COUNT_1_BIT || !(image.usage & VK_IMAGE_USAGE_SAMPLED_BIT) ||
        image.layers != 1 || !image.stored || image.written <= previousPresent_) {
      continue;
    }
    if (image.layout == VK_IMAGE_LAYOUT_UNDEFINED || image.layout == VK_IMAGE_LAYOUT_PREINITIALIZED) {
      continue;
    }
    if (image.owner != VK_QUEUE_FAMILY_IGNORED && image.owner != family) continue;
    // Глубина экрана: viewport примерно размером и пропорциями с картинку swapchain.
    const double width = std::fabs(image.viewport.width);
    const double height = std::fabs(image.viewport.height);
    if (width < sw * 0.35 || height < sh * 0.35 || width > sw * 1.5 || height > sh * 1.5) continue;
    if (std::fabs(width / height - sw / sh) > 0.04) continue;
    if (image.viewport.minDepth == image.viewport.maxDepth) continue;
    const double score = -std::fabs(std::log(width * height / (sw * sh))) * 100.0;
    // A dynamically scaled world depth is preferable to full-resolution weapon-only depth.
    const double range = std::fabs(image.viewport.maxDepth - image.viewport.minDepth);
    if (!best || range > bestRange + 0.001 ||
        (std::fabs(range - bestRange) < 0.001 && (score > bestScore + 0.001 ||
        (std::fabs(score - bestScore) < 0.001 && image.order > best->order)))) {
      best = &image;
      bestScore = score;
      bestRange = range;
    }
  }
  if (best && out) *out = *best;
  return best != nullptr;
}

bool DepthTracker::SetSampleView(VkImage image, VkImageView view) {
  std::lock_guard lock(mutex_);
  auto it = images_.find(image);
  if (it == images_.end()) return false;
  it->second.sampleView = view;
  return true;
}

void DepthTracker::MarkPresent() {
  std::lock_guard lock(mutex_);
  previousPresent_ = submitted_;
}

std::vector<VkImageView> DepthTracker::TakeSampleViews() {
  std::lock_guard lock(mutex_);
  std::vector<VkImageView> views;
  for (auto& [handle, image] : images_) {
    if (image.sampleView) views.push_back(image.sampleView);
    image.sampleView = VK_NULL_HANDLE;
  }
  return views;
}

size_t DepthTracker::DepthImageCount() const {
  std::lock_guard lock(mutex_);
  return images_.size();
}

uint64_t DepthTracker::SubmitCount() const {
  std::lock_guard lock(mutex_);
  return submitted_;
}

bool DepthTracker::FindImage(VkImage image, DepthImage* out) const {
  std::lock_guard lock(mutex_);
  auto it = images_.find(image);
  if (it == images_.end()) return false;
  if (out) *out = it->second;
  return true;
}

size_t DepthTracker::RecordedBufferCount() const {
  size_t n = 0;
  for (Shard& shard : shards_) {
    std::lock_guard lock(shard.mutex);
    n += shard.records.size();
  }
  return n;
}

}  // namespace doomfix::cv
