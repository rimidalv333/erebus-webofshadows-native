// gpu/frame_ring.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include <plume_render_interface.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#if defined(EOT_D3D12)
#include <plume_d3d12.h>
#endif

#include "core/logging.h"
#include "gpu/render_thread.h"
#include "gpu/device.h"
#include "gpu/draw.h"
#include "gpu/gpu_timing.h"
#include "gpu/memory_report.h"
#include "gpu/format.h"

namespace eot::gpu {

void DestroyHostTexture(VideoState &s, HostTexture &host);

namespace {

bool SameTextureShape(const HostTexture &pooled, const HostTexture &want,
                      const plume::RenderTextureDesc &d) {
  const plume::RenderTextureDesc &p = pooled.desc;
  return p.dimension == d.dimension && p.width == d.width && p.height == d.height &&
         p.depth == d.depth && p.mipLevels == d.mipLevels && p.arraySize == d.arraySize &&
         p.format == d.format && p.flags == d.flags &&
         p.multisampling.sampleCount == d.multisampling.sampleCount &&
         pooled.viewDimension == want.viewDimension &&
         pooled.isDepth == want.isDepth;
}

constexpr f64 kHostTexturePoolSeconds = 10.0;
constexpr size_t kHostTexturePoolMax = 192;
constexpr u64 kHostTexturePoolMaxBytes = 256ull << 20;

void TrimHostTexturePool(VideoState &s) {
  while (!s.host_texture_pool.empty() && s.host_texture_pool_bytes > kHostTexturePoolMaxBytes) {
    auto &oldest = s.host_texture_pool.front();
    s.host_texture_pool_bytes -= std::min(s.host_texture_pool_bytes, oldest.bytes);
    DestroyHostTexture(s, oldest.host);
    s.host_texture_pool.erase(s.host_texture_pool.begin());
  }
}

}

void BeginCommandList(VideoState &s) {
  if (s.command_list_open)
    return;
  if (s.shutting_down.load(std::memory_order_acquire) || !s.pipeline_layout)
    return;
  const u32 cur = s.frame.load(std::memory_order_relaxed);
  s.command_list = s.command_lists[cur].get();
  s.command_list->begin();
  if (!s.null_texture_barriers_submitted) {
    plume::RenderTextureBarrier barriers[kNullTextureDescriptorCount];
    for (u32 i = 0; i < kNullTextureDescriptorCount; ++i) {
      barriers[i] = plume::RenderTextureBarrier(s.null_textures[i].get(),
                                                plume::RenderTextureLayout::SHADER_READ);
    }
    s.command_list->barriers(plume::RenderBarrierStage::NONE, barriers,
                             kNullTextureDescriptorCount);
    s.null_texture_barriers_submitted = true;
  }
  s.command_list->setGraphicsPipelineLayout(s.pipeline_layout.get());
  s.command_list->setGraphicsDescriptorSet(s.texture_descriptor_set.get(), 0);
  s.command_list->setGraphicsDescriptorSet(s.texture_descriptor_set.get(), 1);
  s.command_list->setGraphicsDescriptorSet(s.texture_descriptor_set.get(), 2);
  s.command_list->setGraphicsDescriptorSet(s.sampler_descriptor_set.get(), 3);
  s.command_list->setGraphicsDescriptorSet(s.texture_descriptor_set.get(), 4);
  s.command_list_open = true;
  GpuTimingFrameBegin(s, s.command_list, cur);
#if EOT_GPU_PROFILING
  SetGPUProfilerCommandList(static_cast<plume::D3D12CommandList *>(s.command_list)->d3d);
#endif
  s.bound_framebuffer = nullptr;
  s.bound_pipeline = nullptr;
  s.bound_draw_targets_valid = false;
  for (auto &stream : s.bound_vertex_streams)
    stream.valid = false;
  s.bound_index_stream.valid = false;
  for (u32 i = 0; i < 3; ++i) {
    s.bound_root_buffer[i] = nullptr;
    s.bound_root_offset[i] = 0;
  }
  s.bound_spec = ~0u;
}

void SubmitOpenListLocked(VideoState &s) {
  if (!s.command_list_open)
    return;
  const u32 cur = s.frame.load(std::memory_order_relaxed);
  FlushGeometryStaging(s);
  GpuTimingFrameEnd(s.command_list);
  s.command_lists[cur]->end();
  s.command_list_open = false;
  s.bound_framebuffer = nullptr;
  s.bound_pipeline = nullptr;
  s.bound_draw_targets_valid = false;
  for (auto &stream : s.bound_vertex_streams)
    stream.valid = false;
  s.bound_index_stream.valid = false;
  for (u32 i = 0; i < 3; ++i) {
    s.bound_root_buffer[i] = nullptr;
    s.bound_root_offset[i] = 0;
  }
  s.bound_spec = ~0u;
  const plume::RenderCommandList *lists[] = {s.command_lists[cur].get()};
  s.queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, s.fences[cur].get());
  s.command_list_submitted[cur] = true;
}

void AdvanceAndWaitReused(VideoState &s) {
  const u32 slot = s.next_frame;
  s.frame.store(slot, std::memory_order_relaxed);
  s.next_frame = (slot + 1) % kNumFrames;
  s.command_list = s.command_lists[slot].get();
  if (s.command_list_submitted[slot]) {
    s.queue->waitForCommandFence(s.fences[slot].get());
    s.command_list_submitted[slot] = false;
    GpuTimingCollect(s, slot);
#if EOT_GPU_PROFILING
    if (auto *ctx = GpuProfilerCtx()) {
      TracyD3D12NewFrame(ctx);
      TracyD3D12Collect(ctx);
    }
#endif
  }
  s.view_graveyard[slot].clear();
  s.framebuffer_graveyard[slot].clear();
  s.texture_graveyard[slot].clear();
  s.buffer_graveyard[slot].clear();
  DrainDescriptorSlotsLocked(s, slot);
  UploadRingResetFrame(slot);
}

void ParkTexture(VideoState &s, std::unique_ptr<plume::RenderTexture> t) {
  if (!t)
    return;
  s.perf.host_parked++;
  NoteHostRelease(t.get());
  s.texture_graveyard[s.recording_slot()].push_back(std::move(t));
}

void ParkView(VideoState &s, std::unique_ptr<plume::RenderTextureView> v) {
  if (v)
    s.view_graveyard[s.recording_slot()].push_back(std::move(v));
}

void ParkFramebuffer(VideoState &s, std::unique_ptr<plume::RenderFramebuffer> f) {
  if (f)
    s.framebuffer_graveyard[s.recording_slot()].push_back(std::move(f));
}

void ParkBuffer(VideoState &s, std::unique_ptr<plume::RenderBuffer> b) {
  if (!b)
    return;
  NoteHostRelease(b.get());
  s.buffer_graveyard[s.recording_slot()].push_back(std::move(b));
}

void ParkHostTexture(VideoState &s, HostTexture &host) {
  const u64 bytes = host.texture ? HostTextureBytes(host) : 0;
  if (host.texture && s.host_texture_pool.size() < kHostTexturePoolMax &&
      bytes <= kHostTexturePoolMaxBytes) {
    VideoState::PooledHostTexture entry;
    entry.host = std::move(host);
    entry.freedFrame = s.guest_frames;
    entry.bytes = bytes;
    s.host_texture_pool.push_back(std::move(entry));
    s.host_texture_pool_bytes += bytes;
    host = HostTexture{};
    TrimHostTexturePool(s);
    return;
  }
  DestroyHostTexture(s, host);
}

void DestroyHostTexture(VideoState &s, HostTexture &host) {
  ReleaseTextureSRVLocked(s, host);
  if (host.texture) {
    const auto *dead = host.texture.get();
    for (auto it = s.framebuffers.begin(); it != s.framebuffers.end();) {
      bool names_dead = false;
      for (u32 i = 0; i < it->second.attachmentCount; ++i)
        names_dead = names_dead || it->second.attachments[i] == dead;
      if (!names_dead) {
        ++it;
        continue;
      }
      if (s.bound_framebuffer == it->second.fb.get())
        s.bound_framebuffer = nullptr;
      ParkFramebuffer(s, std::move(it->second.fb));
      it = s.framebuffers.erase(it);
    }
  }
  for (auto &fb : host.mipFramebuffers)
    ParkFramebuffer(s, std::move(fb));
  host.mipFramebuffers.clear();
  for (auto &v : host.mipViews)
    ParkView(s, std::move(v));
  host.mipViews.clear();
  ParkView(s, std::move(host.depthView));
  ParkView(s, std::move(host.srv));
  ParkTexture(s, std::move(host.texture));
  host.layout = plume::RenderTextureLayout::UNKNOWN;
}

bool CreateOrRecycleHostTexture(VideoState &s, HostTexture &host,
                                const plume::RenderTextureDesc &desc, const char *tag,
                                plume::RenderPool *pool) {
  const bool renderable = (desc.flags & plume::RenderTextureFlag::RENDER_TARGET) ||
                          (desc.flags & plume::RenderTextureFlag::DEPTH_TARGET);
  const bool transient = pool != nullptr;
  for (size_t i = 0; i < s.host_texture_pool.size(); ++i) {
    auto &entry = s.host_texture_pool[i];
    if (entry.freedFrame + kNumFrames > s.guest_frames || entry.host.transientPool != transient ||
        !SameTextureShape(entry.host, host, desc))
      continue;
    HostTexture pooled = std::move(entry.host);
    s.host_texture_pool_bytes -= std::min(s.host_texture_pool_bytes, entry.bytes);
    s.host_texture_pool.erase(s.host_texture_pool.begin() + i);
    host.texture = std::move(pooled.texture);
    host.desc = pooled.desc;
    host.srv = std::move(pooled.srv);
    host.descriptorIndex = pooled.descriptorIndex;
    host.swizzledSrvs = std::move(pooled.swizzledSrvs);
    host.mipViews = std::move(pooled.mipViews);
    host.mipFramebuffers = std::move(pooled.mipFramebuffers);
    host.depthView = std::move(pooled.depthView);
    host.transientPool = pooled.transientPool;
    host.layout = pooled.layout;
    host.needsClear = renderable;
    s.perf.host_tex_recycled++;
    RetagHostAllocation(host.texture.get(), tag, desc);
    return true;
  }
  host.texture = CreateHostTexture(s.device.get(), desc, tag, pool);
  host.transientPool = transient && host.texture != nullptr;
  if (!host.texture && transient)
    host.texture = CreateHostTexture(s.device.get(), desc, tag);
  host.desc = desc;
  host.desc.optimizedClearValue = nullptr;
  host.layout = plume::RenderTextureLayout::UNKNOWN;
  host.needsClear = renderable && host.texture != nullptr;
  return host.texture != nullptr;
}

void EvictHostTexturePool(VideoState &s) {
  size_t i = 0;
  while (i < s.host_texture_pool.size()) {
    const bool idle = FrameAgeSeconds(s, s.host_texture_pool[i].freedFrame) > kHostTexturePoolSeconds;
    const bool over = s.host_texture_pool.size() > kHostTexturePoolMax ||
                      s.host_texture_pool_bytes > kHostTexturePoolMaxBytes;
    if (!idle && !over) {
      ++i;
      continue;
    }
    s.host_texture_pool_bytes -= std::min(s.host_texture_pool_bytes, s.host_texture_pool[i].bytes);
    DestroyHostTexture(s, s.host_texture_pool[i].host);
    s.host_texture_pool.erase(s.host_texture_pool.begin() + i);
  }
  s.perf.pool_size = static_cast<u32>(s.host_texture_pool.size());
}

void TransitionLocked(VideoState &s, HostTexture &host, plume::RenderTextureLayout layout) {
  if (!host.texture || host.layout == layout || !s.command_list_open)
    return;
  if (s.defer_shader_read_transitions && layout == plume::RenderTextureLayout::SHADER_READ &&
      s.pending_transition_count < kMaxPendingTransitions) {
    s.pending_transitions[s.pending_transition_count++] = {&host, layout};
    return;
  }
  const HostTextureTransition transition{&host, layout};
  TransitionManyLocked(s, &transition, 1);
}

void TransitionManyLocked(VideoState &s, const HostTextureTransition *transitions, u32 count) {
  if (!transitions || !count || !s.command_list_open)
    return;
  constexpr u32 kBatch = 8;
  for (u32 first = 0; first < count; first += kBatch) {
    plume::RenderTextureBarrier barriers[kBatch];
    u32 barrier_count = 0;
    plume::RenderBarrierStages stages = plume::RenderBarrierStage::NONE;
    const u32 end = std::min(first + kBatch, count);
    for (u32 i = first; i < end; ++i) {
      HostTexture *host = transitions[i].host;
      const plume::RenderTextureLayout layout = transitions[i].layout;
      if (!host || !host->texture || host->layout == layout)
        continue;
      barriers[barrier_count++] = plume::RenderTextureBarrier(host->texture.get(), layout);
      if (layout == plume::RenderTextureLayout::COPY_SOURCE ||
          layout == plume::RenderTextureLayout::COPY_DEST)
        stages |= plume::RenderBarrierStage::COPY;
      else
        stages |= plume::RenderBarrierStage::GRAPHICS;
      host->layout = layout;
    }
    if (!barrier_count)
      continue;
    s.command_list->barriers(stages, barriers, barrier_count);
    s.perf.texture_barrier_calls++;
    s.perf.texture_barrier_resources += barrier_count;
  }
}

}

namespace eot::gpu {

namespace {

constexpr u64 kChunkSize = 32ull * 1024 * 1024;

struct Chunk {
  std::unique_ptr<plume::RenderBuffer> buffer;
  u8 *cpu = nullptr;
  u64 capacity = 0;
  u64 used = 0;
  u64 gpuVa = 0;
};

struct Ring {
  std::vector<Chunk> chunks[kNumFrames];
  u64 frame_bytes[kNumFrames] = {};
};

Ring &ring() {
  static Ring r;
  return r;
}

bool MakeChunk(Chunk &chunk, u64 size) {
  auto &s = state();
  plume::RenderBufferDesc desc = plume::RenderBufferDesc::UploadBuffer(size);
  desc.flags = plume::RenderBufferFlag::VERTEX | plume::RenderBufferFlag::INDEX |
               plume::RenderBufferFlag::CONSTANT;
  chunk.buffer = CreateHostBuffer(s.device.get(), desc, "upload-ring");
  if (!chunk.buffer)
    return false;
  chunk.cpu = static_cast<u8 *>(chunk.buffer->map());
  if (!chunk.cpu) {
    EOT_ERROR("upload ring: map() failed for a {} byte chunk", size);
    chunk.buffer.reset();
    return false;
  }
  chunk.capacity = size;
  chunk.used = 0;
#if defined(EOT_D3D12)
  chunk.gpuVa = static_cast<plume::D3D12Buffer *>(chunk.buffer.get())->d3d->GetGPUVirtualAddress();
#else
  chunk.gpuVa = chunk.buffer->getDeviceAddress();
  if (!chunk.gpuVa) {
    EOT_ERROR("upload ring: the device gave the chunk no buffer device address (VK_KHR_buffer_device_address)");
    chunk.buffer->unmap();
    chunk.buffer.reset();
    return false;
  }
#endif
  return true;
}

}

bool UploadRingInit() {
  auto &r = ring();
  for (u32 i = 0; i < kNumFrames; ++i) {
    r.chunks[i].clear();
    Chunk c;
    if (!MakeChunk(c, kChunkSize))
      return false;
    r.chunks[i].push_back(std::move(c));
  }
  return true;
}

static std::mutex g_ring_mutex;
static std::atomic<u64> g_ring_epoch{0};
u64 UploadRingEpoch() { return g_ring_epoch.load(std::memory_order_acquire); }

void UploadRingResetFrame(u32 slot) {
  std::lock_guard lock(g_ring_mutex);
  auto &r = ring();
  g_ring_epoch.fetch_add(1, std::memory_order_acq_rel);
  auto &chunks = r.chunks[slot];
  for (size_t i = 0; i < chunks.size(); ++i)
    chunks[i].used = 0;
  while (chunks.size() > 4) {
    chunks.back().buffer->unmap();
    chunks.pop_back();
  }
  r.frame_bytes[slot] = 0;
}

bool UploadAllocate(u64 size, u64 alignment, UploadAlloc *out) {
  *out = UploadAlloc{};
  if (size == 0)
    return false;
  auto &s = state();
  auto &r = ring();
  if (alignment == 0)
    alignment = 1;
  std::lock_guard lock(g_ring_mutex);
  const u32 slot = s.recording_slot();
  auto &chunks = r.chunks[slot];
  for (u32 attempt = 0; attempt < 2; ++attempt) {
    for (auto &c : chunks) {
      const u64 start = (c.used + alignment - 1) / alignment * alignment;
      if (start + size <= c.capacity) {
        out->buffer = c.buffer.get();
        out->offset = start;
        out->cpu = c.cpu + start;
        out->size = size;
        out->gpuVa = c.gpuVa ? c.gpuVa + start : 0;
        c.used = start + size;
        r.frame_bytes[slot] += size;
        return true;
      }
    }
    Chunk c;
    const u64 want = size + alignment > kChunkSize ? size + alignment : kChunkSize;
    if (!MakeChunk(c, want))
      return false;
    chunks.push_back(std::move(c));
  }
  return false;
}

bool UploadBytes(const void *src, u64 size, u64 alignment, UploadAlloc *out) {
  if (!UploadAllocate(size, alignment, out))
    return false;
  std::memcpy(out->cpu, src, size);
  return true;
}

u64 UploadRingBytesThisFrame() {
  auto &s = state();
  return ring().frame_bytes[s.recording_slot()];
}

u64 UploadRingCapacityBytes(u32 *chunks) {
  std::lock_guard lock(g_ring_mutex);
  u64 bytes = 0;
  u32 n = 0;
  for (const auto &slot : ring().chunks) {
    for (const Chunk &c : slot) {
      bytes += c.capacity;
      n++;
    }
  }
  if (chunks)
    *chunks = n;
  return bytes;
}

}

namespace eot::gpu {

u32 AllocateDescriptorSlot(VideoState &s) {
  for (size_t i = kNullTextureDescriptorCount; i < s.descriptor_slot_used.size(); ++i) {
    if (!s.descriptor_slot_used[i]) {
      s.descriptor_slot_used[i] = true;
      return static_cast<u32>(i);
    }
  }
  return kInvalidDescriptorIndex;
}

void DrainDescriptorSlotsLocked(VideoState &s, u32 slot) {
  for (const auto &d : s.descriptor_graveyard[slot]) {
    if (s.texture_descriptor_set) {
      s.texture_descriptor_set->setTexture(d.slot, s.null_textures[d.null_index].get(),
                                           plume::RenderTextureLayout::SHADER_READ,
                                           s.null_texture_views[d.null_index].get());
    }
    if (d.slot >= kNullTextureDescriptorCount && d.slot < s.descriptor_slot_used.size())
      s.descriptor_slot_used[d.slot] = false;
  }
  s.descriptor_graveyard[slot].clear();
}

namespace {

u32 NullIndexFor(const HostTexture &host) {
  if (host.viewDimension == plume::RenderTextureViewDimension::TEXTURE_3D)
    return kNullTexture3DDescriptorIndex;
  if (host.viewDimension == plume::RenderTextureViewDimension::TEXTURE_CUBE)
    return kNullTextureCubeDescriptorIndex;
  return kNullTexture2DDescriptorIndex;
}

plume::RenderTextureViewDesc SamplingViewDesc(const HostTexture &host) {
  plume::RenderTextureViewDesc view_desc;
  view_desc.format = SampledViewFormat(host.format);
  view_desc.dimension = host.viewDimension;
  view_desc.mipLevels = host.mipLevels ? host.mipLevels : 1;
  return view_desc;
}

plume::RenderSwizzle ToHostSwizzle(u32 source) {
  switch (source & 7) {
  case 0:
    return plume::RenderSwizzle::R;
  case 1:
    return plume::RenderSwizzle::G;
  case 2:
    return plume::RenderSwizzle::B;
  case 3:
    return plume::RenderSwizzle::A;
  case 5:
    return plume::RenderSwizzle::ONE;
  default:
    return plume::RenderSwizzle::ZERO;
  }
}

u32 PublishView(VideoState &s, HostTexture &host, plume::RenderTextureView *view) {
  const u32 slot = AllocateDescriptorSlot(s);
  if (slot == kInvalidDescriptorIndex) {
    EOT_ERROR("bindless texture heap full at {} slots", kBindlessTextureCount);
    return kInvalidDescriptorIndex;
  }
  s.texture_descriptor_set->setTexture(slot, host.texture.get(),
                                       plume::RenderTextureLayout::SHADER_READ, view);
  return slot;
}

}

u32 BindStencilSRVLocked(VideoState &s, HostTexture &host) {
  if (!host.texture || !host.isDepth || !s.texture_descriptor_set)
    return kInvalidDescriptorIndex;
  if (host.stencilDescriptorIndex != kInvalidDescriptorIndex)
    return host.stencilDescriptorIndex;
#if defined(EOT_D3D12)
  if (host.format != plume::RenderFormat::D32_FLOAT_S8_UINT)
    return kInvalidDescriptorIndex;
  const u32 slot = AllocateDescriptorSlot(s);
  if (slot == kInvalidDescriptorIndex) {
    EOT_ERROR("bindless texture heap full at {} slots", kBindlessTextureCount);
    return kInvalidDescriptorIndex;
  }
  D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
  desc.Format = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
  desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  if (host.sampleCount > 1) {
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
  } else {
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipLevels = 1;
    desc.Texture2D.PlaneSlice = 1;
  }
  auto *set = static_cast<plume::D3D12DescriptorSet *>(s.texture_descriptor_set.get());
  auto *texture = static_cast<plume::D3D12Texture *>(host.texture.get());
  set->setSRV(slot, texture->d3d, &desc);
  s.perf.host_views++;
  host.stencilDescriptorIndex = slot;
  return slot;
#else
  return kInvalidDescriptorIndex;
#endif
}

void ReleaseTextureSRVLocked(VideoState &s, HostTexture &host) {
  const u32 null_index = NullIndexFor(host);
  s.texture_generation.fetch_add(1, std::memory_order_relaxed);
  if (host.descriptorIndex != kInvalidDescriptorIndex) {
    s.descriptor_graveyard[s.recording_slot()].push_back({host.descriptorIndex, null_index});
    host.descriptorIndex = kInvalidDescriptorIndex;
  }
  if (host.stencilDescriptorIndex != kInvalidDescriptorIndex) {
    s.descriptor_graveyard[s.recording_slot()].push_back({host.stencilDescriptorIndex, null_index});
    host.stencilDescriptorIndex = kInvalidDescriptorIndex;
  }
  for (auto &[swizzle, srv] : host.swizzledSrvs) {
    if (srv.descriptorIndex != kInvalidDescriptorIndex)
      s.descriptor_graveyard[s.recording_slot()].push_back({srv.descriptorIndex, null_index});
    if (srv.view)
      ParkView(s, std::move(srv.view));
  }
  host.swizzledSrvs.clear();
}

u32 BindTextureSRVLocked(VideoState &s, HostTexture &host) {
  if (!host.texture || !s.texture_descriptor_set)
    return kInvalidDescriptorIndex;
  if (host.descriptorIndex != kInvalidDescriptorIndex)
    return host.descriptorIndex;
  if (!host.srv) {
    s.perf.host_views++;
    host.srv = host.texture->createTextureView(SamplingViewDesc(host));
  }
  if (!host.srv)
    return kInvalidDescriptorIndex;
  host.descriptorIndex = PublishView(s, host, host.srv.get());
  return host.descriptorIndex;
}

u32 BindTextureSRVSwizzledLocked(VideoState &s, HostTexture &host, u32 swizzle) {
  swizzle &= 0xFFF;
  if (swizzle == kIdentityFetchSwizzle)
    return BindTextureSRVLocked(s, host);
  if (!host.texture || !s.texture_descriptor_set)
    return kInvalidDescriptorIndex;
  auto &entry = host.swizzledSrvs[swizzle];
  if (entry.descriptorIndex != kInvalidDescriptorIndex)
    return entry.descriptorIndex;
  if (!entry.view) {
    plume::RenderTextureViewDesc view_desc = SamplingViewDesc(host);
    view_desc.componentMapping = plume::RenderComponentMapping(
        ToHostSwizzle(swizzle), ToHostSwizzle(swizzle >> 3), ToHostSwizzle(swizzle >> 6),
        ToHostSwizzle(swizzle >> 9));
    s.perf.host_views++;
    entry.view = host.texture->createTextureView(view_desc);
  }
  if (!entry.view)
    return kInvalidDescriptorIndex;
  entry.descriptorIndex = PublishView(s, host, entry.view.get());
  return entry.descriptorIndex;
}

}
