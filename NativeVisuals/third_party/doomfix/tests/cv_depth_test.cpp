// Based on doom-eternal-collision-viewer by IlyaTsyganov (commit 43ac57d), used with the author's permission.
//
// Модульный тест трекера глубины collision-viewer (src/features/collision_viewer/depth_tracker.h), только
// CPU, с выдуманными хэндлами. Перенос tests/depth_tracker_tests.cpp исходного viewer плюс проверки
// того, что изменилось при переносе: ленты по командным буферам, пулы, многопоточная запись.
//
//   ctest --test-dir build-release            или   build-release\bin\doomfix_cv_depth_test.exe

#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "features/collision_viewer/depth_tracker.h"

namespace {

using doomfix::cv::DepthImage;
using doomfix::cv::DepthTracker;

int g_failures = 0;

void Check(bool ok, const char* what) {
  std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_failures;
}

template <typename Handle>
Handle H(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

VkImageCreateInfo DepthImageInfo(uint32_t width, uint32_t height,
                                 VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT) {
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = VK_FORMAT_D32_SFLOAT;
  info.extent = {width, height, 1};
  info.mipLevels = info.arrayLayers = 1;
  info.samples = samples;
  info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  return info;
}

VkImageViewCreateInfo ViewInfo(VkImage image, uint32_t baseLayer = 0) {
  VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  info.image = image;
  info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  info.format = VK_FORMAT_D32_SFLOAT;
  info.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, baseLayer, 1};
  return info;
}

// Проход с одним вложением глубины.
void CreatePass(DepthTracker& t, VkRenderPass pass, VkAttachmentStoreOp store,
                VkImageLayout finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL) {
  VkAttachmentDescription attachment = {};
  attachment.format = VK_FORMAT_D32_SFLOAT;
  attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  attachment.storeOp = store;
  attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  attachment.finalLayout = finalLayout;
  VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  info.attachmentCount = 1;
  info.pAttachments = &attachment;
  t.OnRenderPassCreated(pass, info);
}

void CreateFramebuffer(DepthTracker& t, VkFramebuffer fb, VkRenderPass pass, const VkImageView* view,
                       uint32_t size, bool imageless = false) {
  VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  info.flags = imageless ? VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT : 0;
  info.renderPass = pass;
  info.attachmentCount = 1;
  info.pAttachments = imageless ? nullptr : view;
  info.width = info.height = size;
  info.layers = 1;
  t.OnFramebufferCreated(fb, info);
}

void RecordPass(DepthTracker& t, VkCommandBuffer cb, VkRenderPass pass, VkFramebuffer fb,
                const VkImageView* imagelessView = nullptr) {
  VkRenderPassAttachmentBeginInfo attachments{VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO};
  attachments.attachmentCount = 1;
  attachments.pAttachments = imagelessView;
  VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  begin.pNext = imagelessView ? &attachments : nullptr;
  begin.renderPass = pass;
  begin.framebuffer = fb;
  t.RecordBeginRenderPass(cb, begin);
  t.RecordEndRenderPass(cb);
}

void Submit(DepthTracker& t, uint32_t family, VkCommandBuffer cb) { t.OnSubmitted(family, 1, &cb); }

// Перенос depth_tracker_tests.cpp: отправленные layout, viewport вторичного буфера, обратная глубина,
// свежесть, семейство очередей, несохранённая и мультисэмпловая глубина, уничтоженная картинка.
void OriginalScenario() {
  std::printf("original depth tracker scenario\n");
  DepthTracker t;
  const VkImage image = H<VkImage>(1);
  const VkCommandBuffer primary = H<VkCommandBuffer>(2);
  const VkCommandBuffer secondary = H<VkCommandBuffer>(3);
  const VkImageView view = H<VkImageView>(4);
  const VkRenderPass pass = H<VkRenderPass>(5);
  const VkFramebuffer fb = H<VkFramebuffer>(6);
  t.OnImageCreated(image, DepthImageInfo(128, 128));
  t.OnImageViewCreated(view, ViewInfo(image));
  CreatePass(t, pass, VK_ATTACHMENT_STORE_OP_STORE);
  CreateFramebuffer(t, fb, pass, &view, 128);

  // Вторичный буфер: viewport мира с обратной глубиной (1..0, перевёрнутый по y), затем оружие с
  // урезанным диапазоном — выбран должен быть первый.
  t.OnBegin(secondary);
  const VkViewport world = {0, 128, 128, -128, 1, 0};
  VkViewport weapon = world;
  weapon.maxDepth = 0.9f;
  t.RecordViewport(secondary, 0, 1, &world);
  t.RecordViewport(secondary, 0, 1, &weapon);
  t.OnBegin(primary);
  VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  begin.renderPass = pass;
  begin.framebuffer = fb;
  t.RecordBeginRenderPass(primary, begin);
  t.RecordExecuteCommands(primary, 1, &secondary);
  t.RecordEndRenderPass(primary);

  Submit(t, 7, primary);
  DepthImage tracked;
  Check(t.FindImage(image, &tracked), "image is tracked");
  Check(tracked.viewport.height == -128 && tracked.viewport.minDepth == 1 && tracked.viewport.maxDepth == 0,
        "secondary viewport and reversed depth kept");
  Check(tracked.layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL && tracked.stored &&
            tracked.written == 1 && tracked.owner == 7,
        "submitted layout, store, submit number and queue family");
  DepthImage chosen;
  Check(t.SelectScreenDepth(128, 128, 7, &chosen) && chosen.handle == image, "fresh depth is selected");
  t.MarkPresent();
  Check(!t.SelectScreenDepth(128, 128, 7, nullptr), "stale depth withholds the overlay");
  Submit(t, 7, primary);
  Check(t.SelectScreenDepth(128, 128, 7, nullptr), "re-submitted command buffer refreshes depth");
  Check(!t.SelectScreenDepth(128, 128, 9, nullptr), "depth of another queue family is not selected");

  // Не сохранённая глубина (storeOp DONT_CARE).
  const VkRenderPass discardPass = H<VkRenderPass>(7);
  const VkFramebuffer discardFb = H<VkFramebuffer>(8);
  const VkCommandBuffer discard = H<VkCommandBuffer>(9);
  CreatePass(t, discardPass, VK_ATTACHMENT_STORE_OP_DONT_CARE);
  CreateFramebuffer(t, discardFb, discardPass, &view, 128);
  t.OnBegin(discard);
  RecordPass(t, discard, discardPass, discardFb);
  Submit(t, 7, discard);
  Check(!t.SelectScreenDepth(128, 128, 7, nullptr), "discarded depth is not selected");
  Submit(t, 7, primary);
  Check(t.SelectScreenDepth(128, 128, 7, nullptr), "stored again: selected again");

  // Мультисэмпловую глубину шейдер не может читать как sampler2D.
  const VkImage msaa = H<VkImage>(10);
  const VkImageView msaaView = H<VkImageView>(11);
  const VkFramebuffer msaaFb = H<VkFramebuffer>(12);
  const VkCommandBuffer msaaCb = H<VkCommandBuffer>(13);
  t.OnImageCreated(msaa, DepthImageInfo(128, 128, VK_SAMPLE_COUNT_4_BIT));
  t.OnImageViewCreated(msaaView, ViewInfo(msaa));
  CreateFramebuffer(t, msaaFb, pass, &msaaView, 128);
  t.OnBegin(msaaCb);
  RecordPass(t, msaaCb, pass, msaaFb);
  t.MarkPresent();
  Submit(t, 7, msaaCb);
  Check(t.FindImage(msaa, &tracked) && tracked.stored && tracked.written == t.SubmitCount(),
        "multisampled depth is tracked");
  Check(!t.SelectScreenDepth(128, 128, 7, nullptr), "multisampled depth is not selected");

  // Уничтоженная картинка не возвращается из истории команд.
  t.OnImageDestroyed(image);
  Submit(t, 7, primary);
  Check(!t.FindImage(image, nullptr), "destroyed image is not resurrected by command history");
}

void BarrierScenario() {
  std::printf("barriers\n");
  DepthTracker t;
  const VkImage image = H<VkImage>(21);
  const VkCommandBuffer cb = H<VkCommandBuffer>(22);
  t.OnImageCreated(image, DepthImageInfo(64, 64));
  t.OnBegin(cb);
  VkImageMemoryBarrier barriers[2] = {{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER},
                                      {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}};
  barriers[0].image = image;
  barriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
  barriers[0].srcQueueFamilyIndex = 0;
  barriers[0].dstQueueFamilyIndex = 3;
  barriers[0].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
  barriers[1] = barriers[0];  // не первый слой: не учитывается
  barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  barriers[1].subresourceRange.baseArrayLayer = 1;
  t.RecordImageBarriers(cb, 2, barriers);
  DepthImage tracked;
  t.FindImage(image, &tracked);
  Check(tracked.layout == VK_IMAGE_LAYOUT_UNDEFINED, "nothing changes before submit");
  Submit(t, 0, cb);
  t.FindImage(image, &tracked);
  Check(tracked.layout == VK_IMAGE_LAYOUT_GENERAL && tracked.owner == 3,
        "barrier layout and queue family transfer applied on submit");

  VkImageMemoryBarrier2 b2{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b2.image = image;
  b2.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  b2.srcQueueFamilyIndex = b2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b2.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
  t.OnBegin(cb);
  t.RecordImageBarriers2(cb, 1, &b2);
  Submit(t, 0, cb);
  t.FindImage(image, &tracked);
  Check(tracked.layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL && tracked.owner == 3,
        "synchronization2 barrier: layout applied, owner kept for IGNORED");
}

void DynamicRenderingScenario() {
  std::printf("dynamic rendering and imageless framebuffers\n");
  DepthTracker t;
  const VkImage image = H<VkImage>(31);
  const VkImageView view = H<VkImageView>(32);
  const VkCommandBuffer cb = H<VkCommandBuffer>(33);
  t.OnImageCreated(image, DepthImageInfo(256, 128));
  t.OnImageViewCreated(view, ViewInfo(image));
  VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  depth.imageView = view;
  depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
  info.renderArea = {{0, 0}, {256, 128}};
  info.layerCount = 1;
  info.pDepthAttachment = &depth;
  t.OnBegin(cb);
  t.RecordBeginRendering(cb, info);
  const VkViewport reversed = {0, 0, 256, 128, 1, 0};
  t.RecordViewport(cb, 0, 1, &reversed);
  t.RecordEndRendering(cb);
  Submit(t, 0, cb);
  DepthImage tracked;
  t.FindImage(image, &tracked);
  Check(tracked.stored && tracked.layout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        "dynamic rendering: stored, attachment layout");
  Check(tracked.viewport.minDepth == 1 && tracked.viewport.maxDepth == 0 && tracked.viewport.width == 256,
        "dynamic rendering: viewport set inside the pass");
  Check(t.SelectScreenDepth(256, 128, 0, nullptr), "dynamic rendering depth is selected");

  // Imageless framebuffer: views приходят в vkCmdBeginRenderPass.
  const VkImage image2 = H<VkImage>(34);
  const VkImageView view2 = H<VkImageView>(35);
  const VkRenderPass pass = H<VkRenderPass>(36);
  const VkFramebuffer fb = H<VkFramebuffer>(37);
  const VkCommandBuffer cb2 = H<VkCommandBuffer>(38);
  t.OnImageCreated(image2, DepthImageInfo(64, 64));
  t.OnImageViewCreated(view2, ViewInfo(image2));
  CreatePass(t, pass, VK_ATTACHMENT_STORE_OP_STORE, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
  CreateFramebuffer(t, fb, pass, nullptr, 64, true);
  t.OnBegin(cb2);
  RecordPass(t, cb2, pass, fb, &view2);
  Submit(t, 0, cb2);
  t.FindImage(image2, &tracked);
  Check(tracked.stored && tracked.written == t.SubmitCount() &&
            tracked.layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        "imageless framebuffer: attachment from VkRenderPassAttachmentBeginInfo");

  // View не первого слоя не считается глубиной экрана.
  const VkImageView layerView = H<VkImageView>(39);
  const VkFramebuffer layerFb = H<VkFramebuffer>(40);
  const uint64_t before = tracked.written;
  t.OnImageViewCreated(layerView, ViewInfo(image2, 1));
  CreateFramebuffer(t, layerFb, pass, &layerView, 64);
  t.OnBegin(cb2);
  RecordPass(t, cb2, pass, layerFb);
  Submit(t, 0, cb2);
  t.FindImage(image2, &tracked);
  Check(tracked.written == before, "view of another array layer is ignored");
}

void LifetimeScenario() {
  std::printf("command buffer and pool lifetime\n");
  DepthTracker t;
  const VkImage image = H<VkImage>(51);
  const VkImageView view = H<VkImageView>(52);
  const VkRenderPass pass = H<VkRenderPass>(53);
  const VkFramebuffer fb = H<VkFramebuffer>(54);
  const VkCommandPool pool = H<VkCommandPool>(55);
  const VkCommandBuffer cbs[3] = {H<VkCommandBuffer>(56), H<VkCommandBuffer>(57), H<VkCommandBuffer>(58)};
  t.OnImageCreated(image, DepthImageInfo(64, 64));
  t.OnImageViewCreated(view, ViewInfo(image));
  CreatePass(t, pass, VK_ATTACHMENT_STORE_OP_STORE);
  CreateFramebuffer(t, fb, pass, &view, 64);
  t.OnAllocated(pool, 3, cbs);
  for (VkCommandBuffer cb : cbs) {
    t.OnBegin(cb);
    RecordPass(t, cb, pass, fb);
  }
  Check(t.RecordedBufferCount() == 3, "three recorded command buffers");

  DepthImage tracked;
  t.OnPoolReset(pool);
  Submit(t, 0, cbs[0]);
  t.FindImage(image, &tracked);
  Check(tracked.written == 0, "vkResetCommandPool clears the recorded events");
  t.OnBegin(cbs[0]);
  RecordPass(t, cbs[0], pass, fb);
  t.OnReset(cbs[0]);
  Submit(t, 0, cbs[0]);
  t.FindImage(image, &tracked);
  Check(tracked.written == 0, "vkResetCommandBuffer clears the recorded events");

  t.OnFreed(pool, 1, &cbs[1]);
  Check(t.RecordedBufferCount() == 2, "vkFreeCommandBuffers removes the record");
  t.OnPoolDestroyed(pool);
  Check(t.RecordedBufferCount() == 0, "vkDestroyCommandPool removes the records of the pool");

  // Цикл вторичных буферов (неверные данные) не зацикливает проигрывание.
  const VkCommandBuffer loop = H<VkCommandBuffer>(59);
  t.OnBegin(loop);
  t.RecordExecuteCommands(loop, 1, &loop);
  RecordPass(t, loop, pass, fb);
  Submit(t, 0, loop);
  t.FindImage(image, &tracked);
  Check(tracked.written == t.SubmitCount(), "self-executing command buffer stops at the nesting limit");

  // Sample view: отдаётся при уничтожении картинки и при остановке.
  const VkImageView sample = H<VkImageView>(60);
  Check(t.SetSampleView(image, sample), "sample view stored");
  Check(t.TakeSampleViews().size() == 1 && t.OnImageDestroyed(image) == VK_NULL_HANDLE,
        "sample views taken once");
  Check(!t.SetSampleView(image, sample), "no sample view for a destroyed image");
}

// Запись с нескольких потоков одновременно с отправками: без глобальной блокировки, без гонок
// по общим таблицам (проверяется отсутствием падений и итоговыми числами).
void ThreadScenario() {
  std::printf("multithreaded recording\n");
  DepthTracker t;
  const VkImage image = H<VkImage>(71);
  const VkImageView view = H<VkImageView>(72);
  const VkRenderPass pass = H<VkRenderPass>(73);
  const VkFramebuffer fb = H<VkFramebuffer>(74);
  t.OnImageCreated(image, DepthImageInfo(128, 128));
  t.OnImageViewCreated(view, ViewInfo(image));
  CreatePass(t, pass, VK_ATTACHMENT_STORE_OP_STORE);
  CreateFramebuffer(t, fb, pass, &view, 128);
  constexpr int kThreads = 8;
  constexpr int kRounds = 2000;
  std::vector<std::thread> threads;
  for (int n = 0; n < kThreads; ++n) {
    threads.emplace_back([&, n] {
      const VkCommandPool pool = H<VkCommandPool>(1000 + n);
      std::vector<VkCommandBuffer> cbs;
      for (int i = 0; i < 4; ++i) cbs.push_back(H<VkCommandBuffer>(0x10000 + n * 16 + i));
      t.OnAllocated(pool, static_cast<uint32_t>(cbs.size()), cbs.data());
      const VkViewport viewport = {0, 0, 128, 128, 0, 1};
      VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      barrier.image = image;
      barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
      barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
      for (int round = 0; round < kRounds; ++round) {
        VkCommandBuffer cb = cbs[round % cbs.size()];
        t.OnBegin(cb);
        for (int k = 0; k < 8; ++k) t.RecordViewport(cb, 0, 1, &viewport);
        RecordPass(t, cb, pass, fb);
        t.RecordImageBarriers(cb, 1, &barrier);
        t.OnSubmitted(0, 1, &cb);
        if (round % 97 == 0) t.OnPoolReset(pool);
      }
      t.OnPoolDestroyed(pool);
    });
  }
  for (std::thread& thread : threads) thread.join();
  DepthImage tracked;
  t.FindImage(image, &tracked);
  Check(t.SubmitCount() == uint64_t(kThreads) * kRounds, "every submit counted");
  Check(tracked.stored && tracked.layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL &&
            tracked.written <= t.SubmitCount() && tracked.written > 0,
        "depth state consistent after parallel recording");
  Check(t.RecordedBufferCount() == 0, "all records removed with their pools");
}

}  // namespace

void LateAttachScenario() {
  DepthTracker t(true);auto image=H<VkImage>(901);auto cb=H<VkCommandBuffer>(902);
  t.OnImageCreated(image,DepthImageInfo(1920,1080));
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.image=image;barrier.oldLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  barrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
  barrier.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};
  barrier.srcAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
  t.OnBegin(cb);t.RecordImageBarriers(cb,1,&barrier);
  Check(!t.SelectScreenDepth(1920,1080,0,nullptr),"late attachment waits for submitted depth writes");
  t.OnSubmitted(0,1,&cb);
  Check(t.SelectScreenDepth(1920,1080,0,nullptr),"late attachment adopts explicit depth write-to-read dependencies without cached render passes");
  t.MarkPresent();
  Check(!t.SelectScreenDepth(1920,1080,0,nullptr),"late-adopted depth still expires after present");
  barrier.srcAccessMask=VK_ACCESS_SHADER_READ_BIT;
  t.OnBegin(cb);t.RecordImageBarriers(cb,1,&barrier);t.OnSubmitted(0,1,&cb);
  Check(!t.SelectScreenDepth(1920,1080,0,nullptr),"a read-only barrier cannot fabricate a fresh depth buffer");
}

void ScaledWorldScenario() {
  DepthTracker t;
  const auto pass = H<VkRenderPass>(950);
  const auto cb = H<VkCommandBuffer>(951);
  CreatePass(t, pass, VK_ATTACHMENT_STORE_OP_STORE);
  for (int i = 0; i < 2; ++i) {
    const auto image = H<VkImage>(960 + i);
    const auto view = H<VkImageView>(970 + i);
    const auto fb = H<VkFramebuffer>(980 + i);
    const uint32_t size = i == 0 ? 96 : 128;
    t.OnImageCreated(image, DepthImageInfo(size, size));
    t.OnImageViewCreated(view, ViewInfo(image));
    CreateFramebuffer(t, fb, pass, &view, size);
    t.OnBegin(cb);
    const VkViewport viewport = {0, 0, float(size), float(size), 0, i == 0 ? 1.0f : 0.1f};
    t.RecordViewport(cb, 0, 1, &viewport);
    RecordPass(t, cb, pass, fb);
    Submit(t, 0, cb);
  }
  DepthImage chosen;
  Check(t.SelectScreenDepth(128, 128, 0, &chosen) && chosen.handle == H<VkImage>(960),
        "scaled full-range world depth wins over full-resolution weapon depth");
  t.OnImageDestroyed(H<VkImage>(960));
  Check(t.SelectScreenDepth(128, 128, 0, &chosen) && chosen.handle == H<VkImage>(961),
        "depth selection still handles a sole restricted-range candidate");
}

int main() {
  ScaledWorldScenario();
  LateAttachScenario();
  OriginalScenario();
  BarrierScenario();
  DynamicRenderingScenario();
  LifetimeScenario();
  ThreadScenario();
  if (g_failures) {
    std::printf("\n%d check(s) FAILED\n", g_failures);
    return 1;
  }
  std::printf("\nPASSED\n");
  return 0;
}
