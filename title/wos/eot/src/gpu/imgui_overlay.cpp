// gpu/imgui_overlay.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "gpu/imgui_overlay.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

#include <plume_render_interface.h>
#include <plume_render_interface_builders.h>

#if defined(EOT_D3D12)
#include "shaders/imgui_ps.hlsl.dxil.h"
#include "shaders/imgui_vs.hlsl.dxil.h"
#else
#include "shaders/imgui_ps.hlsl.spirv.h"
#include "shaders/imgui_vs.hlsl.spirv.h"
#endif

#include "core/logging.h"
#include "gpu/backend.h"
#include "gpu/render_thread.h"
#include "gpu/device.h"
#include "gpu/resources.h"

namespace eot::gpu {

void DestroyHostTexture(VideoState &s, HostTexture &host);

namespace {

OverlayDrawHook g_hook;
thread_local bool g_video_lock_borrowed = false;

struct PendingTextureRetirements {
  std::mutex mutex;
  std::vector<HostTexture> hosts;
};

PendingTextureRetirements &PendingRetirements() {
  static auto *queue = new PendingTextureRetirements;
  return *queue;
}

void QueueTextureRetirement(HostTexture host) {
  auto &pending = PendingRetirements();
  std::lock_guard lock(pending.mutex);
  pending.hosts.push_back(std::move(host));
}

void DrainTextureRetirementsLocked(VideoState &s) {
  std::vector<HostTexture> hosts;
  {
    auto &pending = PendingRetirements();
    std::lock_guard lock(pending.mutex);
    hosts.swap(pending.hosts);
  }
  for (auto &host : hosts)
    DestroyHostTexture(s, host);
}

struct Ortho {
  float scale[2];
  float translate[2];
};
struct Slots {
  u32 texture;
  u32 sampler;
};

class OverlayTexture final : public rex::ui::ImmediateTexture {
public:
  OverlayTexture(u32 width, u32 height, HostTexture host, u32 slot)
      : rex::ui::ImmediateTexture(width, height), host_(std::move(host)), slot_(slot) {}

  ~OverlayTexture() override {
    auto &s = state();
    if (g_video_lock_borrowed) {
      DestroyHostTexture(s, host_);
      return;
    }
    QueueTextureRetirement(std::move(host_));
  }

  u32 slot() const { return slot_; }

private:
  HostTexture host_;
  u32 slot_;
};

class OverlayDrawer final : public rex::ui::ImmediateDrawer {
public:
  ~OverlayDrawer() override = default;

  std::unique_ptr<rex::ui::ImmediateTexture> CreateTexture(u32 width, u32 height,
                                                           rex::ui::ImmediateTextureFilter filter,
                                                           bool is_repeated,
                                                           const u8 *data) override;
  void Begin(rex::ui::UIDrawContext &ctx, float coord_width, float coord_height) override;
  void BeginDrawBatch(const rex::ui::ImmediateDrawBatch &batch) override;
  void Draw(const rex::ui::ImmediateDraw &draw) override;
  void EndDrawBatch() override;
  void End() override;

private:
  bool EnsureResources(VideoState &s);

  plume::RenderCommandList *cmd_ = nullptr;
  std::unique_ptr<plume::RenderPipelineLayout> layout_;
  std::unique_ptr<plume::RenderPipeline> pipeline_;
  std::unique_ptr<plume::RenderShader> vs_, ps_;
  bool failed_ = false;
  bool batch_indexed_ = false;
  float coord_width_ = 0.0f, coord_height_ = 0.0f;
  u32 target_width_ = 0, target_height_ = 0;
};

bool OverlayDrawer::EnsureResources(VideoState &s) {
  if (pipeline_)
    return true;
  if (failed_ || !s.ready || !s.device)
    return false;

  vs_ = s.device->createShader(EOT_SHADER_BLOB(imgui_vs), "main", kHostShaderFormat);
  ps_ = s.device->createShader(EOT_SHADER_BLOB(imgui_ps), "main", kHostShaderFormat);
  if (!vs_ || !ps_) {
    EOT_ERROR("[overlay] createShader failed; overlays will not draw");
    failed_ = true;
    return false;
  }

  plume::RenderPipelineLayoutBuilder layout_builder;
  layout_builder.begin(false, true);
  plume::RenderDescriptorSetBuilder texture_set;
  texture_set.begin();
  texture_set.addTexture(0, kBindlessTextureCount);
  texture_set.end(true, kBindlessTextureCount);
  layout_builder.addDescriptorSet(texture_set);
  plume::RenderDescriptorSetBuilder sampler_set;
  sampler_set.begin();
  sampler_set.addSampler(0, kBindlessSamplerCount);
  sampler_set.end(true, kBindlessSamplerCount);
  layout_builder.addDescriptorSet(sampler_set);
  layout_builder.addPushConstant(0, 2, sizeof(Ortho), plume::RenderShaderStageFlag::VERTEX);
#if defined(EOT_D3D12)
  layout_builder.addPushConstant(1, 2, sizeof(Slots), plume::RenderShaderStageFlag::PIXEL);
#else
  layout_builder.addPushConstant(1, 2, sizeof(Slots), plume::RenderShaderStageFlag::PIXEL,
                                 sizeof(Ortho));
#endif
  layout_builder.end();
  layout_ = layout_builder.create(s.device.get());
  if (!layout_) {
    EOT_ERROR("[overlay] createPipelineLayout failed; overlays will not draw");
    failed_ = true;
    return false;
  }

  const plume::RenderInputSlot slot(0, sizeof(rex::ui::ImmediateVertex));
  const plume::RenderInputElement elements[] = {
      {"POSITION", 0, 0, plume::RenderFormat::R32G32_FLOAT, 0, 0},
      {"TEXCOORD", 0, 1, plume::RenderFormat::R32G32_FLOAT, 0, 8},
      {"COLOR", 0, 2, plume::RenderFormat::R8G8B8A8_UNORM, 0, 16},
  };

  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = layout_.get();
  desc.vertexShader = vs_.get();
  desc.pixelShader = ps_.get();
  desc.inputSlots = &slot;
  desc.inputSlotsCount = 1;
  desc.inputElements = elements;
  desc.inputElementsCount = static_cast<u32>(std::size(elements));
  desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  desc.cullMode = plume::RenderCullMode::NONE;
  desc.depthEnabled = false;
  desc.depthWriteEnabled = false;
  desc.depthFunction = plume::RenderComparisonFunction::ALWAYS;
  desc.renderTargetCount = 1;
  desc.renderTargetFormat[0] = plume::RenderFormat::B8G8R8A8_UNORM;
  desc.depthTargetFormat = plume::RenderFormat::UNKNOWN;
  plume::RenderBlendDesc blend;
  blend.blendEnabled = true;
  blend.srcBlend = plume::RenderBlend::SRC_ALPHA;
  blend.dstBlend = plume::RenderBlend::INV_SRC_ALPHA;
  blend.blendOp = plume::RenderBlendOperation::ADD;
  blend.srcBlendAlpha = plume::RenderBlend::ONE;
  blend.dstBlendAlpha = plume::RenderBlend::INV_SRC_ALPHA;
  blend.blendOpAlpha = plume::RenderBlendOperation::ADD;
  blend.renderTargetWriteMask = 0xF;
  desc.renderTargetBlend[0] = blend;

  pipeline_ = CreateHostGraphicsPipeline(s.device.get(), desc, "overlay");
  if (!pipeline_) {
    EOT_ERROR("[overlay] createGraphicsPipeline failed; overlays will not draw");
    failed_ = true;
    return false;
  }
  EOT_DEBUG("[overlay] ready");
  return true;
}

std::unique_ptr<rex::ui::ImmediateTexture>
OverlayDrawer::CreateTexture(u32 width, u32 height, rex::ui::ImmediateTextureFilter filter,
                             bool is_repeated, const u8 *data) {
  (void)filter;
  (void)is_repeated;
  auto &s = state();
  if (!s.ready || !s.device || !width || !height)
    return nullptr;

  plume::RenderTextureDesc desc;
  desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
  desc.width = width;
  desc.height = height;
  desc.depth = 1;
  desc.mipLevels = 1;
  desc.arraySize = 1;
  desc.format = plume::RenderFormat::R8G8B8A8_UNORM;
  desc.committed = true;

  HostTexture host;
  host.texture = CreateHostTexture(s.device.get(), desc, "overlay-texture");
  if (!host.valid())
    return nullptr;
  host.desc = desc;
  host.format = desc.format;
  host.viewDimension = plume::RenderTextureViewDimension::TEXTURE_2D;
  host.width = width;
  host.height = height;
  host.depth = 1;
  host.mipLevels = 1;
  host.arraySize = 1;
  host.layout = plume::RenderTextureLayout::UNKNOWN;

  UploadAlloc staging;
  u64 pitch = 0;
  if (data) {
    const u32 row = width * 4;
    pitch = (row + kTextureRowPitchAlignment - 1) & ~u64(kTextureRowPitchAlignment - 1);
    if (!s.command_list_open || !UploadAllocate(pitch * height, kTexturePlacementAlignment,
                                                &staging))
      return nullptr;
    for (u32 y = 0; y < height; ++y)
      std::memcpy(staging.cpu + y * pitch, data + size_t(y) * row, row);
  }

  const u32 slot = BindTextureSRVLocked(s, host);
  if (slot == kInvalidDescriptorIndex)
    return nullptr;

  if (data) {
    TransitionLocked(s, host, plume::RenderTextureLayout::COPY_DEST);
    s.command_list->copyTextureRegion(
        plume::RenderTextureCopyLocation::Subresource(host.texture.get(), 0, 0),
        plume::RenderTextureCopyLocation::PlacedFootprint(
            staging.buffer, host.format, width, height, 1, static_cast<u32>(pitch / 4),
            staging.offset),
        0, 0, 0);
  }
  TransitionLocked(s, host, plume::RenderTextureLayout::SHADER_READ);
  return std::make_unique<OverlayTexture>(width, height, std::move(host), slot);
}

void OverlayDrawer::Begin(rex::ui::UIDrawContext &ctx, float coord_width, float coord_height) {
  rex::ui::ImmediateDrawer::Begin(ctx, coord_width, coord_height);
  cmd_ = nullptr;

  auto &s = state();
  if (!EnsureResources(s))
    return;
  auto &overlay_ctx = static_cast<OverlayDrawContext &>(ctx);
  cmd_ = overlay_ctx.command_list();
  if (!cmd_)
    return;

  coord_width_ = coordinate_space_width();
  coord_height_ = coordinate_space_height();
  target_width_ = ctx.render_target_width();
  target_height_ = ctx.render_target_height();

  cmd_->setFramebuffer(overlay_ctx.framebuffer());
  const plume::RenderViewport viewport(0.0f, 0.0f, static_cast<float>(target_width_),
                                       static_cast<float>(target_height_), 0.0f, 1.0f);
  cmd_->setViewports(&viewport, 1);
  cmd_->setGraphicsPipelineLayout(layout_.get());
  cmd_->setPipeline(pipeline_.get());
  cmd_->setGraphicsDescriptorSet(s.texture_descriptor_set.get(), 0);
  cmd_->setGraphicsDescriptorSet(s.sampler_descriptor_set.get(), 1);

  const Ortho ortho{{2.0f / coord_width_, -2.0f / coord_height_}, {-1.0f, 1.0f}};
  cmd_->setGraphicsPushConstants(0, &ortho, 0, sizeof(ortho));
}

void OverlayDrawer::BeginDrawBatch(const rex::ui::ImmediateDrawBatch &batch) {
  if (!cmd_)
    return;
  batch_indexed_ = false;

  if (batch.vertex_count > 0 && batch.vertices) {
    UploadAlloc vertices;
    const u64 bytes = u64(batch.vertex_count) * sizeof(rex::ui::ImmediateVertex);
    if (!UploadBytes(batch.vertices, bytes, sizeof(rex::ui::ImmediateVertex), &vertices)) {
      cmd_ = nullptr;
      return;
    }
    const plume::RenderVertexBufferView view(
        plume::RenderBufferReference(vertices.buffer, vertices.offset), static_cast<u32>(bytes));
    const plume::RenderInputSlot slot(0, sizeof(rex::ui::ImmediateVertex));
    cmd_->setVertexBuffers(0, &view, 1, &slot);
  }
  if (batch.index_count > 0 && batch.indices) {
    UploadAlloc indices;
    const u64 bytes = u64(batch.index_count) * sizeof(uint16_t);
    if (!UploadBytes(batch.indices, bytes, sizeof(uint16_t), &indices)) {
      cmd_ = nullptr;
      return;
    }
    const plume::RenderIndexBufferView index_view(
        plume::RenderBufferReference(indices.buffer, indices.offset), static_cast<u32>(bytes),
        plume::RenderFormat::R16_UINT);
    cmd_->setIndexBuffer(&index_view);
    batch_indexed_ = true;
  }
}

void OverlayDrawer::Draw(const rex::ui::ImmediateDraw &draw) {
  if (!cmd_ || draw.count <= 0)
    return;
  if (draw.primitive_type != rex::ui::ImmediatePrimitiveType::kTriangles)
    return;

  const float sx = coord_width_ > 0.0f ? static_cast<float>(target_width_) / coord_width_ : 1.0f;
  const float sy = coord_height_ > 0.0f ? static_cast<float>(target_height_) / coord_height_ : 1.0f;
  i32 left = 0, top = 0, right = static_cast<i32>(target_width_),
      bottom = static_cast<i32>(target_height_);
  if (draw.scissor) {
    if (draw.scissor_right <= draw.scissor_left || draw.scissor_bottom <= draw.scissor_top)
      return;
    left = std::max(0, static_cast<i32>(draw.scissor_left * sx));
    top = std::max(0, static_cast<i32>(draw.scissor_top * sy));
    right = std::min(right, static_cast<i32>(draw.scissor_right * sx));
    bottom = std::min(bottom, static_cast<i32>(draw.scissor_bottom * sy));
    if (right <= left || bottom <= top)
      return;
  }
  const plume::RenderRect scissor(left, top, right, bottom);
  cmd_->setScissors(&scissor, 1);

  const auto *texture = static_cast<const OverlayTexture *>(draw.texture);
  const Slots slots{texture ? texture->slot() : kInvalidDescriptorIndex, kSamplerLinearClamp};
  cmd_->setGraphicsPushConstants(1, &slots, 0, sizeof(slots));

  if (batch_indexed_)
    cmd_->drawIndexedInstanced(static_cast<u32>(draw.count), 1, static_cast<u32>(draw.index_offset),
                               draw.base_vertex, 0);
  else
    cmd_->drawInstanced(static_cast<u32>(draw.count), 1, static_cast<u32>(draw.base_vertex), 0);
}

void OverlayDrawer::EndDrawBatch() { batch_indexed_ = false; }

void OverlayDrawer::End() {
  cmd_ = nullptr;
  rex::ui::ImmediateDrawer::End();
}

}

OverlayVideoLockScope::OverlayVideoLockScope() : previous_(g_video_lock_borrowed) {
  g_video_lock_borrowed = true;
}

OverlayVideoLockScope::~OverlayVideoLockScope() { g_video_lock_borrowed = previous_; }

std::unique_ptr<rex::ui::ImmediateDrawer> CreateOverlayDrawer() {
  return std::make_unique<OverlayDrawer>();
}

void SetOverlayDrawHook(OverlayDrawHook hook) { g_hook = std::move(hook); }

void RunOverlayDrawHook(plume::RenderCommandList *cmd, plume::RenderFramebuffer *framebuffer,
                        u32 width, u32 height) {
  DrainTextureRetirementsLocked(state());
  if (g_hook)
    g_hook(cmd, framebuffer, width, height);
}

}
