// gpu/device.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "gpu/device.h"
#include "gpu/gpu_timing.h"
#include "gpu/memory_report.h"
#include "gpu/pipeline/pipeline_cache.h"
#include "gpu/pipeline/pso_precache.h"

#include <cmath>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#if !defined(__PROSPERO__)
#include <SDL3/SDL.h>
#endif
#include <plume_render_interface.h>
#include <rex/cvar.h>
#include <plume_render_interface_builders.h>
#if defined(EOT_D3D12)
#include <plume_d3d12.h>
#else
#include <plume_vulkan.h>
#endif

#if !defined(_WIN32)
#include <dlfcn.h>
#include <sched.h>
#endif
#if defined(__APPLE__)
#include <mach/mach_time.h>
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <sys/sysctl.h>
#endif
#if !defined(_WIN32)

#include <fstream>
#endif
#if defined(__PROSPERO__)
// WoS port (fix46): thread pinning on the console (see PinThreadToPhysicalCore)
#include <pthread.h>
#include <pthread_np.h>
#include <sys/cpuset.h>
extern "C" int wos_cpu_layout_render_cpu();  // wos/app/wos_ps5_threads.cpp
extern "C" int ps5_pthread_setaffinity_np(pthread_t thread, size_t size, const void *set);
#endif
#include <rex/ui/window.h>

#include <renderdoc_app.h>

#include "core/logging.h"
#include "gpu/backend.h"
#include "gpu/render_thread.h"
#include "gpu/format.h"
#include "gpu/settings.h"
#include "platform/display.h"
#if defined(__APPLE__)
#include "platform/moltenvk.h"
#endif

#if defined(EOT_D3D12)
#include "shaders/blit_ps.hlsl.dxil.h"
#include "shaders/copy_depth_ps.hlsl.dxil.h"
#include "shaders/copy_vs.hlsl.dxil.h"
#include "shaders/resolve_msaa_color_2x_ps.hlsl.dxil.h"
#include "shaders/resolve_msaa_color_4x_ps.hlsl.dxil.h"
#include "shaders/resolve_msaa_color_8x_ps.hlsl.dxil.h"
#include "shaders/resolve_msaa_depth_2x_ps.hlsl.dxil.h"
#include "shaders/resolve_msaa_depth_4x_ps.hlsl.dxil.h"
#include "shaders/resolve_msaa_depth_8x_ps.hlsl.dxil.h"
#include "shaders/derive_depth_stencil_2x_ps.hlsl.dxil.h"
#include "shaders/derive_depth_stencil_4x_ps.hlsl.dxil.h"
#include "shaders/derive_depth_stencil_8x_ps.hlsl.dxil.h"
#else
#include "shaders/blit_ps.hlsl.spirv.h"
#include "shaders/copy_depth_ps.hlsl.spirv.h"
#include "shaders/copy_depth_f24_ps.hlsl.spirv.h"
#include "shaders/copy_vs.hlsl.spirv.h"
#include "shaders/resolve_msaa_color_2x_ps.hlsl.spirv.h"
#include "shaders/resolve_msaa_color_4x_ps.hlsl.spirv.h"
#include "shaders/resolve_msaa_color_8x_ps.hlsl.spirv.h"
#include "shaders/resolve_msaa_depth_2x_ps.hlsl.spirv.h"
#include "shaders/resolve_msaa_depth_4x_ps.hlsl.spirv.h"
#include "shaders/resolve_msaa_depth_8x_ps.hlsl.spirv.h"
#endif

namespace plume {
#if defined(EOT_D3D12)
extern std::unique_ptr<RenderInterface> CreateD3D12Interface();
#else
extern std::unique_ptr<RenderInterface> CreateVulkanInterface();
#endif
}

namespace eot::gpu {

VideoState &state() {
  static VideoState s;
  return s;
}

namespace {

float ChannelFromArgb(u32 argb, int shift) {
  return static_cast<float>((argb >> shift) & 0xFF) / 255.0f;
}

bool BuildNullTextureDescriptors(VideoState &s) {
  for (u32 i = 0; i < kNullTextureDescriptorCount; ++i) {
    plume::RenderTextureDesc desc;
    desc.width = 1;
    desc.height = 1;
    desc.depth = 1;
    desc.mipLevels = 1;
    desc.arraySize = 1;
    desc.format = plume::RenderFormat::R8G8B8A8_UNORM;
    desc.flags = plume::RenderTextureFlag::NONE;
    desc.committed = true;

    plume::RenderTextureViewDesc view_desc;
    view_desc.format = desc.format;
    view_desc.mipLevels = 1;
    view_desc.componentMapping = plume::RenderComponentMapping(
        plume::RenderSwizzle::ZERO, plume::RenderSwizzle::ZERO,
        plume::RenderSwizzle::ZERO, plume::RenderSwizzle::ONE);

    switch (i) {
    case kNullTexture2DDescriptorIndex:
      desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
      view_desc.dimension = plume::RenderTextureViewDimension::TEXTURE_2D;
      break;
    case kNullTexture3DDescriptorIndex:
      desc.dimension = plume::RenderTextureDimension::TEXTURE_3D;
      view_desc.dimension = plume::RenderTextureViewDimension::TEXTURE_3D;
      break;
    case kNullTextureCubeDescriptorIndex:
      desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
      desc.arraySize = 6;
      desc.flags = plume::RenderTextureFlag::CUBE;
      view_desc.dimension = plume::RenderTextureViewDimension::TEXTURE_CUBE;
      break;
    default:
      return false;
    }

    auto texture = CreateHostTexture(s.device.get(), desc, "null-descriptor");
    if (!texture) {
      EOT_ERROR("Create null texture descriptor {} failed", i);
      return false;
    }
    auto view = texture->createTextureView(view_desc);
    if (!view) {
      EOT_ERROR("Create null texture view {} failed", i);
      return false;
    }
    s.texture_descriptor_set->setTexture(i, texture.get(),
                                         plume::RenderTextureLayout::SHADER_READ,
                                         view.get());
    s.null_textures[i] = std::move(texture);
    s.null_texture_views[i] = std::move(view);
  }
  return true;
}

std::string DescribeBackend(plume::RenderDevice *device) {
#if defined(EOT_D3D12)
  auto *dev = static_cast<plume::D3D12Device *>(device);
  static const D3D_FEATURE_LEVEL kLevels[] = {
      D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
      D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  D3D12_FEATURE_DATA_FEATURE_LEVELS levels = {};
  levels.NumFeatureLevels = static_cast<u32>(std::size(kLevels));
  levels.pFeatureLevelsRequested = kLevels;
  if (dev && dev->d3d &&
      dev->d3d->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &levels,
                                    sizeof(levels)) >= 0) {
    const u32 fl = static_cast<u32>(levels.MaxSupportedFeatureLevel);
    return std::format("D3D12 {}_{}", (fl >> 12) & 0xF, (fl >> 8) & 0xF);
  }
  return "D3D12";
#else
  (void)device;
  return "Vulkan";
#endif
}

}

namespace {
RENDERDOC_API_1_6_0 *g_rdoc = nullptr;
bool g_rdoc_capturing = false;
}

void RenderDocInit() {
  if (g_rdoc || Settings::RenderDocFrame() <= 0)
    return;
  const std::string dll = Settings::RenderDocDll();
#if defined(_WIN32)
  HMODULE mod = LoadLibraryA(dll.c_str());
  if (!mod) {
    EOT_ERROR("[rdc] LoadLibrary({}) failed ({})", dll, GetLastError());
    return;
  }
  auto get_api = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(mod, "RENDERDOC_GetAPI"));
#else
  const std::string so = dll.ends_with(".dll") ? "librenderdoc.so" : dll;
  void *mod = ::dlopen(so.c_str(), RTLD_NOW | RTLD_NOLOAD);
  if (!mod)
    mod = ::dlopen(so.c_str(), RTLD_NOW);
  if (!mod) {
    EOT_ERROR("[rdc] dlopen({}) failed ({})", so, ::dlerror());
    return;
  }
  auto get_api = reinterpret_cast<pRENDERDOC_GetAPI>(::dlsym(mod, "RENDERDOC_GetAPI"));
#endif
  if (!get_api || !get_api(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void **>(&g_rdoc)) ||
      !g_rdoc) {
    EOT_ERROR("[rdc] RENDERDOC_GetAPI failed");
    g_rdoc = nullptr;
    return;
  }
  const std::string path = Settings::RenderDocPath();
  g_rdoc->SetCaptureFilePathTemplate(path.c_str());
  g_rdoc->SetCaptureOptionU32(eRENDERDOC_Option_CaptureCallstacks, 0);
  g_rdoc->SetCaptureOptionU32(eRENDERDOC_Option_RefAllResources, 1);
  int major = 0, minor = 0, patch = 0;
  g_rdoc->GetAPIVersion(&major, &minor, &patch);
  EOT_INFO("[rdc] RenderDoc {}.{}.{} loaded from {}; capturing guest frame {} to {}", major,
           minor, patch, dll, Settings::RenderDocFrame(), path);
}

void RenderDocFrameBoundary(u64 guest_frame_just_presented) {
  if (!g_rdoc)
    return;
  const u64 target = static_cast<u64>(Settings::RenderDocFrame());
  if (!g_rdoc_capturing && guest_frame_just_presented + 1 == target) {
    g_rdoc->StartFrameCapture(nullptr, nullptr);
    g_rdoc_capturing = true;
    EOT_INFO("[rdc] StartFrameCapture before guest frame {}", target);
  } else if (g_rdoc_capturing && guest_frame_just_presented == target) {
    const u32 ok = g_rdoc->EndFrameCapture(nullptr, nullptr);
    g_rdoc_capturing = false;
    u32 count = g_rdoc->GetNumCaptures();
    char name[512] = {};
    u32 len = sizeof(name);
    if (count)
      g_rdoc->GetCapture(count - 1, name, &len, nullptr);
    EOT_INFO("[rdc] EndFrameCapture after guest frame {} -> ok={} captures={} last={}", target,
             ok, count, name);
  }
}

static float HalfToFloat(u16 h) {
  const u32 sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, man = h & 0x3FF;
  u32 bits;
  if (exp == 0) {
    if (!man) {
      bits = sign << 31;
    } else {
      u32 e = 127 - 15 + 1, m = man;
      while (!(m & 0x400)) { m <<= 1; --e; }
      bits = (sign << 31) | (e << 23) | ((m & 0x3FF) << 13);
    }
  } else if (exp == 31) {
    bits = (sign << 31) | 0x7F800000u | (man << 13);
  } else {
    bits = (sign << 31) | ((exp - 15 + 127) << 23) | (man << 13);
  }
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

bool LogHostTextureStatsLocked(VideoState &s, HostTexture &host, const char *label) {
#if defined(EOT_D3D12)
  (void)s; (void)host; (void)label;
  return false;
#else
  if (!s.command_list_open || !host.texture || host.isDepth || host.sampleCount > 1)
    return false;
  const bool half = host.format == plume::RenderFormat::R16G16B16A16_FLOAT;
  const bool unorm8 = host.format == plume::RenderFormat::R8G8B8A8_UNORM;
  if (!half && !unorm8) {
    EOT_INFO("[stats] {}: host format {} not read back", label, static_cast<u32>(host.format));
    return false;
  }
  const u32 w = host.width, h = host.height, bpp = half ? 8u : 4u;
  auto readback = CreateHostBuffer(s.device.get(),
                                   plume::RenderBufferDesc::ReadbackBuffer(u64(w) * h * bpp), "stats-readback");
  if (!readback)
    return false;
  auto *cmd = s.command_list;
  TransitionLocked(s, host, plume::RenderTextureLayout::COPY_SOURCE);
  VkBufferImageCopy region{};
  region.bufferRowLength = w;
  region.bufferImageHeight = h;
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {w, h, 1};
  vkCmdCopyImageToBuffer(static_cast<plume::VulkanCommandList *>(cmd)->vk,
                         static_cast<plume::VulkanTexture *>(host.texture.get())->vk,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         static_cast<plume::VulkanBuffer *>(readback.get())->vk, 1, &region);
  s.bound_pipeline = nullptr;
  s.bound_framebuffer = nullptr;
  const u32 cur = s.recording_slot();
  SubmitOpenListLocked(s);
  s.queue->waitForCommandFence(s.fences[cur].get());
  s.command_list_submitted[cur] = false;
  const auto *src = static_cast<const u8 *>(readback->map());
  if (src) {
    double mn[4], mx[4], sum[4] = {};
    u64 nan[4] = {}, inf[4] = {};
    for (int c = 0; c < 4; ++c) { mn[c] = 1e30; mx[c] = -1e30; }
    auto texel = [&](u32 x, u32 y, float out[4]) {
      const u8 *p = src + (u64(y) * w + x) * bpp;
      for (int c = 0; c < 4; ++c) {
        if (half) {
          u16 v;
          std::memcpy(&v, p + 2 * c, 2);
          out[c] = HalfToFloat(v);
        } else {
          out[c] = p[c] / 255.0f;
        }
      }
    };
    for (u32 y = 0; y < h; ++y)
      for (u32 x = 0; x < w; ++x) {
        float v[4];
        texel(x, y, v);
        for (int c = 0; c < 4; ++c) {
          if (std::isnan(v[c])) { ++nan[c]; continue; }
          if (std::isinf(v[c])) { ++inf[c]; continue; }
          mn[c] = std::min<double>(mn[c], v[c]);
          mx[c] = std::max<double>(mx[c], v[c]);
          sum[c] += v[c];
        }
      }
    const double n = double(w) * h;
    float a[4], b[4], c4[4];
    texel(0, 0, a);
    texel(w / 2, h / 2, b);
    texel(w - 1, h - 1, c4);
    EOT_INFO("[stats] {} {}x{}: mean ({:.4g} {:.4g} {:.4g} {:.4g}) min ({:.4g} {:.4g} {:.4g} {:.4g}) max ({:.4g} "
             "{:.4g} {:.4g} {:.4g}) nan {}/{}/{}/{} inf {}/{}/{}/{} | (0,0) ({:.4g} {:.4g} {:.4g} {:.4g}) mid ({:.4g} "
             "{:.4g} {:.4g} {:.4g}) last ({:.4g} {:.4g} {:.4g} {:.4g})",
             label, w, h, sum[0] / n, sum[1] / n, sum[2] / n, sum[3] / n, mn[0], mn[1], mn[2], mn[3], mx[0], mx[1],
             mx[2], mx[3], nan[0], nan[1], nan[2], nan[3], inf[0], inf[1], inf[2], inf[3], a[0], a[1], a[2], a[3],
             b[0], b[1], b[2], b[3], c4[0], c4[1], c4[2], c4[3]);
    readback->unmap();
  }
  ParkBuffer(s, std::move(readback));
  BeginCommandList(s);
  return src != nullptr;
#endif
}

bool DumpHostTextureLocked(VideoState &s, HostTexture &host, const char *path, float scale,
                           u32 lut_index, u32 swizzle, u32 shrink) {
  if (!s.command_list_open || !host.texture || !path)
    return false;
  auto *cmd = s.command_list;
  // WoS port (fix107): shrink > 1 renders straight into a w/shrink x h/shrink target, and the
  // target, its framebuffer and the readback buffer are kept and reused (we wait for the GPU
  // before reading, so reuse is safe). Before, every per-draw dump made a full-size 8 MB
  // readback buffer that was only parked, and a 2000-draw capture ran the GPU heap dry.
  const u32 k = std::max(1u, shrink);
  const u32 w = std::max(1u, host.width / k), h = std::max(1u, host.height / k);
  const u32 row = ((w * 4 + 255) / 256) * 256;
  struct DumpTarget {
    HostTexture tex;
    std::unique_ptr<plume::RenderFramebuffer> fb;
  };
  static DumpTarget targets[4];
  static u32 next_victim = 0;
  static std::unique_ptr<plume::RenderBuffer> readback;
  static u64 readback_size = 0;
  DumpTarget *slot = nullptr;
  for (auto &t : targets)
    if (t.tex.texture && t.fb && t.tex.width == w && t.tex.height == h)
      slot = &t;
  if (!slot) {
    slot = &targets[next_victim++ % 4];
    if (slot->fb)
      ParkFramebuffer(s, std::move(slot->fb));
    ParkHostTexture(s, slot->tex);
    plume::RenderTextureDesc td;
    td.dimension = plume::RenderTextureDimension::TEXTURE_2D;
    td.width = w;
    td.height = h;
    td.depth = 1;
    td.mipLevels = 1;
    td.arraySize = 1;
    td.format = plume::RenderFormat::R8G8B8A8_UNORM;
    td.flags = plume::RenderTextureFlag::RENDER_TARGET;
    td.committed = true;
    slot->tex.texture = CreateHostTexture(s.device.get(), td, "dump-target");
    slot->tex.format = td.format;
    slot->tex.width = w;
    slot->tex.height = h;
    slot->tex.mipLevels = 1;
    slot->tex.layout = plume::RenderTextureLayout::UNKNOWN;
    slot->tex.renderable = true;
    if (!slot->tex.texture)
      return false;
    const plume::RenderTexture *colors[1] = {slot->tex.texture.get()};
    plume::RenderFramebufferDesc fd(colors, 1);
    slot->fb = s.device->createFramebuffer(fd);
  }
  HostTexture &target = slot->tex;
  auto &target_fb = slot->fb;
  if (!target_fb)
    return false;
  const u64 need = u64(row) * h;
  if (!readback || readback_size < need) {
    if (readback)
      ParkBuffer(s, std::move(readback));
    readback = CreateHostBuffer(s.device.get(), plume::RenderBufferDesc::ReadbackBuffer(need),
                                "dump-readback");
    readback_size = readback ? need : 0;
  }
  if (!readback)
    return false;
  auto *fb = target_fb.get();

  TransitionLocked(s, host, plume::RenderTextureLayout::SHADER_READ);
  const u32 src_index = swizzle == kIdentityFetchSwizzle
                            ? BindTextureSRVLocked(s, host)
                            : BindTextureSRVSwizzledLocked(s, host, swizzle);
  if (src_index == kInvalidDescriptorIndex)
    return false;
  TransitionLocked(s, target, plume::RenderTextureLayout::COLOR_WRITE);
  cmd->setFramebuffer(fb);
  plume::RenderViewport vp(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f);
  plume::RenderRect sc(0, 0, static_cast<i32>(w), static_cast<i32>(h));
  cmd->setViewports(&vp, 1);
  cmd->setScissors(&sc, 1);
  cmd->setPipeline(GetBlitPipeline(s, plume::RenderFormat::R8G8B8A8_UNORM));
  CopyPushConstants pc;
  pc.resourceDescriptorIndex = src_index;
  pc.resourceDescriptorIndex2 = lut_index != kInvalidDescriptorIndex ? lut_index : 0u;
  pc.param0 = scale;
  pc.param1 = lut_index != kInvalidDescriptorIndex ? 2.0f : 1.0f;
  cmd->setGraphicsPushConstants(kCopyPushConstantRangeIndex, &pc, kCopyPushConstantByteOffset,
                                sizeof(pc));
  cmd->drawInstanced(3, 1, 0, 0);
  TransitionLocked(s, target, plume::RenderTextureLayout::COPY_SOURCE);
#if defined(EOT_D3D12)
  {
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = static_cast<plume::D3D12Buffer *>(readback.get())->d3d;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = w;
    dst.PlacedFootprint.Footprint.Height = h;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = row;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = static_cast<plume::D3D12Texture *>(target.texture.get())->d3d;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    static_cast<plume::D3D12CommandList *>(cmd)->d3d->CopyTextureRegion(&dst, 0, 0, 0, &src,
                                                                        nullptr);
  }
#else
  {
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = row / 4;
    region.bufferImageHeight = h;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(static_cast<plume::VulkanCommandList *>(cmd)->vk,
                           static_cast<plume::VulkanTexture *>(target.texture.get())->vk,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, static_cast<plume::VulkanBuffer *>(readback.get())->vk,
                           1, &region);
  }
#endif
  s.bound_pipeline = nullptr;
  s.bound_framebuffer = nullptr;

  const u32 cur = s.recording_slot();
  SubmitOpenListLocked(s);
  s.queue->waitForCommandFence(s.fences[cur].get());
  s.command_list_submitted[cur] = false;
  const auto *src = static_cast<const u8 *>(readback->map());
  bool ok = false;
  if (src) {
    if (FILE *f = std::fopen(path, "wb")) {
      std::fprintf(f, "P6\n%u %u\n255\n", w, h);
      std::vector<u8> line(u64(w) * 3);
      for (u32 y = 0; y < h; ++y) {
        const u8 *p = src + u64(y) * row;
        for (u32 x = 0; x < w; ++x) {
          line[x * 3 + 0] = p[x * 4 + 0];
          line[x * 3 + 1] = p[x * 4 + 1];
          line[x * 3 + 2] = p[x * 4 + 2];
        }
        std::fwrite(line.data(), 1, line.size(), f);
      }
      std::fclose(f);
      ok = true;
    }
    readback->unmap();
  }
  BeginCommandList(s);
  EOT_INFO("[dump] {} {}x{} scale {} -> {}", ok ? "wrote" : "FAILED", w, h, scale, path);
  return ok;
}

// WoS port (fix45): one mip level / array layer of a host texture, raw (block-compressed
// stays compressed), for offline decoding - whether the city's lower mips and the IBL cube's
// faces hold what the guest has. Vulkan only. bytes_per_block 8/16 for BC1/BC2-3, 4 = RGBA8.
bool DumpHostTextureLevelRawLocked(VideoState &s, HostTexture &host, u32 level, u32 layer,
                                   u32 bytes_per_block, bool block_compressed, const char *path) {
#if defined(EOT_D3D12)
  (void)s; (void)host; (void)level; (void)layer; (void)bytes_per_block; (void)block_compressed;
  (void)path;
  return false;
#else
  if (!s.command_list_open || !host.texture || !path || level >= host.mipLevels ||
      layer >= std::max(1u, host.arraySize))
    return false;
  const u32 w = std::max(1u, host.width >> level), h = std::max(1u, host.height >> level);
  const u32 bw = block_compressed ? (w + 3) / 4 : w, bh = block_compressed ? (h + 3) / 4 : h;
  const u64 bytes = u64(bw) * bh * bytes_per_block;
  if (!bytes || bytes > (8ull << 20))
    return false;
  auto readback =
      CreateHostBuffer(s.device.get(), plume::RenderBufferDesc::ReadbackBuffer(bytes), "mip-readback");
  if (!readback)
    return false;
  auto *cmd = s.command_list;
  const plume::RenderTextureLayout old_layout = host.layout;
  TransitionLocked(s, host, plume::RenderTextureLayout::COPY_SOURCE);
  VkBufferImageCopy region{};
  region.bufferOffset = 0;
  region.bufferRowLength = 0;
  region.bufferImageHeight = 0;
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1};
  region.imageExtent = {w, h, 1};
  vkCmdCopyImageToBuffer(static_cast<plume::VulkanCommandList *>(cmd)->vk,
                         static_cast<plume::VulkanTexture *>(host.texture.get())->vk,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         static_cast<plume::VulkanBuffer *>(readback.get())->vk, 1, &region);
  if (old_layout != plume::RenderTextureLayout::UNKNOWN)
    TransitionLocked(s, host, old_layout);
  s.bound_pipeline = nullptr;
  s.bound_framebuffer = nullptr;
  const u32 cur = s.recording_slot();
  SubmitOpenListLocked(s);
  s.queue->waitForCommandFence(s.fences[cur].get());
  s.command_list_submitted[cur] = false;
  bool ok = false;
  if (const auto *src = static_cast<const u8 *>(readback->map())) {
    if (FILE *f = std::fopen(path, "wb")) {
      std::fwrite(src, 1, bytes, f);
      std::fclose(f);
      ok = true;
    }
    readback->unmap();
  }
  ParkBuffer(s, std::move(readback));
  BeginCommandList(s);
  return ok;
#endif
}

namespace {

#if defined(_WIN32)
struct PhysicalCore {
  u32 efficiency;
  KAFFINITY mask;
  u32 group;
};

std::vector<PhysicalCore> EnumeratePhysicalCores() {
  std::vector<PhysicalCore> cores;
  DWORD length = 0;
  GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
  if (!length)
    return cores;
  std::vector<u8> buffer(length);
  if (!GetLogicalProcessorInformationEx(
          RelationProcessorCore,
          reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(buffer.data()), &length))
    return cores;
  for (DWORD off = 0; off < length;) {
    auto *e = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(buffer.data() + off);
    if (e->Relationship == RelationProcessorCore && e->Processor.GroupCount >= 1)
      cores.push_back({e->Processor.EfficiencyClass, e->Processor.GroupMask[0].Mask,
                       e->Processor.GroupMask[0].Group});
    off += e->Size;
  }
  std::stable_sort(cores.begin(), cores.end(), [](const PhysicalCore &a, const PhysicalCore &b) {
    return a.efficiency > b.efficiency;
  });
  return cores;
}
#elif defined(__linux__)
std::vector<std::vector<int>> EnumeratePhysicalCores() {
  std::vector<std::vector<int>> cores;
  for (int cpu = 0; cpu < 1024; ++cpu) {
    const std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/";
    std::ifstream in(base + "core_cpus_list");
    if (!in)
      in.open(base + "thread_siblings_list");
    if (!in)
      break;
    std::string list;
    std::getline(in, list);
    std::vector<int> set;
    size_t pos = 0;
    while (pos < list.size()) {
      size_t end = list.find(',', pos);
      if (end == std::string::npos)
        end = list.size();
      const std::string item = list.substr(pos, end - pos);
      const size_t dash = item.find('-');
      const int lo = std::atoi(item.c_str());
      const int hi = dash == std::string::npos ? lo : std::atoi(item.c_str() + dash + 1);
      for (int c = lo; c <= hi && c < 1024; ++c)
        set.push_back(c);
      pos = end + 1;
    }
    if (set.empty() || set.front() != cpu)
      continue;
    cores.push_back(std::move(set));
  }
  return cores;
}
#endif

}

u32 PhysicalCoreCount() {
  static const u32 count = [] {
    u32 n = 0;
#if defined(_WIN32) || defined(__linux__)
    n = static_cast<u32>(EnumeratePhysicalCores().size());
#elif defined(__APPLE__)
    int cores = 0;
    size_t len = sizeof(cores);
    if (::sysctlbyname("hw.physicalcpu", &cores, &len, nullptr, 0) == 0 && cores > 0)
      n = static_cast<u32>(cores);
#endif
    if (n == 0) {
      const u32 hw = std::max(1u, std::thread::hardware_concurrency());
      n = std::max(1u, (hw + 1) / 2);
    }
    return n;
  }();
  return count;
}

thread_local bool g_thread_sleeps_precisely = false;

bool PinThreadToPhysicalCore(u32 core, const char *what) {
#if defined(_WIN32)
  const std::vector<PhysicalCore> cores = EnumeratePhysicalCores();
  if (cores.size() < 8)
    return false;
  if (core >= cores.size() || cores[core].group != 0 || !cores[core].mask)
    return false;
  if (!SetThreadAffinityMask(GetCurrentThread(), cores[core].mask))
    return false;
  EOT_INFO("[gpu] {} pinned to physical core {} (logical mask {:#x} of {} cores)", what, core,
           static_cast<u64>(cores[core].mask), cores.size());
  return true;
#elif defined(__linux__)
  const std::vector<std::vector<int>> cores = EnumeratePhysicalCores();
  if (cores.size() < 8 || core >= cores.size())
    return false;
  cpu_set_t mask;
  CPU_ZERO(&mask);
  for (int c : cores[core])
    CPU_SET(c, &mask);
  if (::sched_setaffinity(0, sizeof(mask), &mask) != 0)
    return false;
  EOT_INFO("[gpu] {} pinned to physical core {} ({} logical CPUs of {} cores)", what, core, cores[core].size(),
           cores.size());
  return true;
#elif defined(__APPLE__)
  (void)core;
  mach_timebase_info_data_t tb{};
  mach_timebase_info(&tb);
  const f64 ticks_per_ns = static_cast<f64>(tb.denom) / static_cast<f64>(tb.numer);
  auto ticks = [&](f64 ms) { return static_cast<u32>(ms * 1e6 * ticks_per_ns); };
  thread_time_constraint_policy_data_t policy{};
  policy.period = ticks(1000.0 / 60.0);
  policy.computation = ticks(5.0);
  policy.constraint = ticks(12.0);
  policy.preemptible = 1;
  const kern_return_t kr =
      thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                        reinterpret_cast<thread_policy_t>(&policy), THREAD_TIME_CONSTRAINT_POLICY_COUNT);
  if (kr != KERN_SUCCESS) {
    EOT_WARN("[gpu] {}: the realtime scheduling class was refused ({}); its sleeps will run late", what,
             static_cast<int>(kr));
    return false;
  }
  g_thread_sleeps_precisely = true;
  EOT_INFO("[gpu] {} takes the realtime scheduling class (5 ms of every 16.7, pre-emptible)", what);
  return true;
#elif defined(__PROSPERO__)
  // WoS port (fix46): the console is FreeBSD - none of the branches above applied, so
  // nothing was ever pinned. The render thread (core 2 here) takes the CPU of the title's
  // layout (wos_ps5_threads.cpp, wos_cpu_layout); the game's own threads are placed
  // there by name, so the guest rendering thread (core 1) is left to it.
  if (core != 2)
    return false;
  const int cpu = wos_cpu_layout_render_cpu();
  if (cpu < 0)
    return false;
  cpuset_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  if (ps5_pthread_setaffinity_np(pthread_self(), sizeof set, &set) != 0) {
    EOT_WARN("[gpu] {}: pinning to CPU {} was refused", what, cpu);
    return false;
  }
  EOT_INFO("[gpu] {} pinned to CPU {} (its twin CPU {} left free)", what, cpu, cpu ^ 1);
  return true;
#else
  (void)core;
  (void)what;
  return false;
#endif
}

bool ThreadSleepsPrecisely() { return g_thread_sleeps_precisely; }

f64 PerfMsPerTickSlow() {
  static const f64 ms_per_tick = [] {
    const auto s0 = std::chrono::steady_clock::now();
    const u64 t0 = PerfNow();
    auto s1 = s0;
    do {
      s1 = std::chrono::steady_clock::now();
    } while (std::chrono::duration<f64, std::milli>(s1 - s0).count() < 2.0);
    const u64 t1 = PerfNow();
    return std::chrono::duration<f64, std::milli>(s1 - s0).count() /
           static_cast<f64>(std::max<u64>(1, t1 - t0));
  }();
  g_perf_ms_per_tick = ms_per_tick;
  return ms_per_tick;
}

void DrainHostDebugMessages(VideoState &s, const char *phase) {
#if defined(EOT_D3D12)
  auto *dev = static_cast<plume::D3D12Device *>(s.device.get());
  if (!dev || !dev->d3d)
    return;
  static ID3D12InfoQueue *queue = nullptr;
  static bool probed = false;
  if (!probed) {
    probed = true;
    if (FAILED(dev->d3d->QueryInterface(IID_PPV_ARGS(&queue))))
      queue = nullptr;
  }
  if (!queue)
    return;
  const UINT64 count = queue->GetNumStoredMessages();
  static std::unordered_map<int, u32> per_id;
  for (UINT64 i = 0; i < count; ++i) {
    SIZE_T len = 0;
    if (FAILED(queue->GetMessage(i, nullptr, &len)) || len == 0)
      continue;
    std::vector<char> buf(len);
    auto *msg = reinterpret_cast<D3D12_MESSAGE *>(buf.data());
    if (FAILED(queue->GetMessage(i, msg, &len)))
      continue;
    if (msg->Severity == D3D12_MESSAGE_SEVERITY_INFO ||
        msg->Severity == D3D12_MESSAGE_SEVERITY_MESSAGE)
      continue;
    const u32 n = per_id[static_cast<int>(msg->ID)]++;
    if (n < 4 || (n & (n - 1)) == 0) {
      EOT_WARN("[d3d12-debug] ({}) sev={} id={} x{}: {}", phase ? phase : "?",
               static_cast<int>(msg->Severity), static_cast<int>(msg->ID), n + 1,
               msg->pDescription ? msg->pDescription : "");
    }
  }
  queue->ClearStoredMessages();
  const HRESULT removed = dev->d3d->GetDeviceRemovedReason();
  static bool removed_logged = false;
  if (FAILED(removed) && !removed_logged) {
    removed_logged = true;
    EOT_ERROR("[d3d12] DEVICE REMOVED during {}: reason {:#x} (0x887a0006=hung, "
              "0x887a0005=removed, 0x887a0007=reset, 0x887a0020=internal error, "
              "0x887a0001=invalid call) at guest frame {}",
              phase ? phase : "?", static_cast<u32>(removed), s.guest_frames);
  }
#else
  (void)s;
  (void)phase;
#endif
}

plume::RenderColor ArgbToRenderColor(u32 argb) {
  return plume::RenderColor(ChannelFromArgb(argb, 16), ChannelFromArgb(argb, 8),
                            ChannelFromArgb(argb, 0), ChannelFromArgb(argb, 24));
}

bool DiagShouldLog(u64 site, u32 *n_out) {
  *n_out = 0;
  if (Settings::DiagVerbosity() < 1)
    return false;
  using Clock = std::chrono::steady_clock;
  struct DiagState {
    u64 site = 0;
    u32 count = 0;
    Clock::time_point last_log{};
  };
  static std::mutex m;
  static DiagState states[4096];
  std::lock_guard lock(m);
  u64 h = site * 0x9E3779B97F4A7C15ull;
  h ^= h >> 29;
  DiagState &st = states[h & 4095];
  if (st.site != site) {
    st.site = site;
    st.count = 0;
    st.last_log = Clock::time_point{};
  }
  const u32 n = st.count++;
  *n_out = n;
  const auto now = Clock::now();
  if (n == 0 || now - st.last_log >= std::chrono::seconds(1)) {
    st.last_log = now;
    return true;
  }
  return false;
}

static void ReportCreationFailure(plume::RenderDevice *device) {
#if defined(EOT_D3D12)
  static bool reported = false;
  if (reported || !device)
    return;
  auto *d3d = static_cast<plume::D3D12Device *>(device)->d3d;
  if (!d3d)
    return;
  const HRESULT reason = d3d->GetDeviceRemovedReason();
  reported = true;
  if (FAILED(reason))
    EOT_ERROR("[gpu] the device was removed: reason {:#010x} (0x887A0005 reset, 0x887A0006 hung, "
              "0x887A0007 internal error, 0x887A0020 driver); every creation fails from here",
              static_cast<u32>(reason));
  else
    EOT_ERROR("[gpu] creation failed with the device alive: memory exhaustion (check commit "
              "charge and event 2004)");
#else
  (void)device;
#endif
}

std::unique_ptr<plume::RenderBuffer>
CreateHostBuffer(plume::RenderDevice *device, const plume::RenderBufferDesc &desc,
                 const char *tag) {
  if (!device)
    return nullptr;
  auto buffer = device->createBuffer(desc);
  if (!buffer
#if defined(EOT_D3D12)
      || static_cast<plume::D3D12Buffer *>(buffer.get())->d3d == nullptr
#else
      || static_cast<plume::VulkanBuffer *>(buffer.get())->vk == VK_NULL_HANDLE
#endif
  ) {
    u32 n;
    if (DiagShouldLog(0x4100 ^ desc.size, &n))
      EOT_ERROR("CreateHostBuffer({}) failed: backend resource null (size={} bytes) x{}",
                tag ? tag : "?", desc.size, n + 1);
    ReportCreationFailure(device);
    return nullptr;
  }
  TagHostAllocation(buffer.get(), tag);
  return buffer;
}

std::unique_ptr<plume::RenderTexture>
CreateHostTexture(plume::RenderDevice *device, const plume::RenderTextureDesc &desc,
                  const char *tag, plume::RenderPool *pool) {
  if (!device)
    return nullptr;
  EOT_CPU_ZONE_DYN(tag ? tag : "host texture");
  state().perf.host_textures++;
  if (tag) {
    if (tag[0] == 's')
      state().perf.host_tex_surface++;
    else if (tag[0] == 'r')
      state().perf.host_tex_mirror++;
    else if (tag[0] == 'g')
      state().perf.host_tex_guest++;
  }
  auto texture = pool ? pool->createTexture(desc) : device->createTexture(desc);
  if (!texture
#if defined(EOT_D3D12)
      || static_cast<plume::D3D12Texture *>(texture.get())->d3d == nullptr
#else
      || static_cast<plume::VulkanTexture *>(texture.get())->vk == VK_NULL_HANDLE
#endif
  ) {
    u32 n;
    if (DiagShouldLog(0x4000 ^ (u64(desc.width) << 16) ^ desc.height ^ (u64(desc.format) << 40), &n))
      EOT_ERROR("CreateHostTexture({}) failed: backend resource null ({}x{} fmt={} flags={:#x}) x{}",
                tag ? tag : "?", desc.width, desc.height, static_cast<u32>(desc.format),
                static_cast<u32>(desc.flags), n + 1);
    ReportCreationFailure(device);
    return nullptr;
  }
  TagHostAllocation(texture.get(), tag, desc);
  return texture;
}

std::unique_ptr<plume::RenderPipeline>
CreateHostGraphicsPipeline(plume::RenderDevice *device,
                           const plume::RenderGraphicsPipelineDesc &desc, const char *tag) {
  if (!device)
    return nullptr;
  auto pipeline = device->createGraphicsPipeline(desc);
  if (!pipeline
#if defined(EOT_D3D12)
      || static_cast<plume::D3D12GraphicsPipeline *>(pipeline.get())->d3d == nullptr
#else
      || static_cast<plume::VulkanGraphicsPipeline *>(pipeline.get())->vk == VK_NULL_HANDLE
#endif
  ) {
    u32 n;
    if (DiagShouldLog(0x4200, &n))
      EOT_ERROR("CreateHostGraphicsPipeline({}) failed: backend pipeline null x{}",
                tag ? tag : "?", n + 1);
    ReportCreationFailure(device);
    return nullptr;
  }
  return pipeline;
}

bool BuildSwapFramebuffers(VideoState &s) {
  s.swap_framebuffers.clear();
  s.render_semaphores.clear();
  const u32 count = s.swap_chain->getTextureCount();
  s.swap_framebuffers.reserve(count);
  for (u32 i = 0; i < count; ++i) {
    plume::RenderTexture *tex = s.swap_chain->getTexture(i);
    const plume::RenderTexture *color_attachments[1] = {tex};
    plume::RenderFramebufferDesc desc(color_attachments, 1);
    auto fb = s.device->createFramebuffer(desc);
    if (!fb) {
      EOT_ERROR("createFramebuffer failed for back buffer {}", i);
      s.swap_framebuffers.clear();
      return false;
    }
    s.swap_framebuffers.push_back(std::move(fb));
    auto sem = s.device->createCommandSemaphore();
    if (!sem) {
      EOT_ERROR("createCommandSemaphore failed for present semaphore {}", i);
      s.swap_framebuffers.clear();
      return false;
    }
    s.render_semaphores.push_back(std::move(sem));
  }
  return true;
}

bool BuildPipelineLayout(VideoState &s) {
  plume::RenderPipelineLayoutBuilder layout_builder;
  layout_builder.begin(false, true);

  plume::RenderDescriptorSetBuilder tex_set_builder;
  tex_set_builder.begin();
  tex_set_builder.addTexture(0, kBindlessTextureCount);
  tex_set_builder.end(true, kBindlessTextureCount);

  s.texture_descriptor_set = tex_set_builder.create(s.device.get());
  if (!s.texture_descriptor_set) {
    EOT_ERROR("createDescriptorSet for the bindless texture heap failed");
    return false;
  }
  s.descriptor_slot_used.assign(kBindlessTextureCount, false);
  for (u32 i = 0; i < kNullTextureDescriptorCount; ++i)
    s.descriptor_slot_used[i] = true;
  if (!BuildNullTextureDescriptors(s))
    return false;
  // WoS port (fix101): opaque white, sampled in place of a resolve target whose memory a
  // later resolve of another format has since overwritten (see BindTexturesAndSamplers)
  {
    plume::RenderTextureViewDesc white_desc;
    white_desc.format = plume::RenderFormat::R8G8B8A8_UNORM;
    white_desc.mipLevels = 1;
    white_desc.dimension = plume::RenderTextureViewDimension::TEXTURE_2D;
    white_desc.componentMapping = plume::RenderComponentMapping(
        plume::RenderSwizzle::ONE, plume::RenderSwizzle::ONE, plume::RenderSwizzle::ONE,
        plume::RenderSwizzle::ONE);
    s.white_texture_view = s.null_textures[kNullTexture2DDescriptorIndex]->createTextureView(white_desc);
    const u32 slot = s.white_texture_view ? AllocateDescriptorSlot(s) : kInvalidDescriptorIndex;
    if (slot != kInvalidDescriptorIndex) {
      s.texture_descriptor_set->setTexture(slot, s.null_textures[kNullTexture2DDescriptorIndex].get(),
                                           plume::RenderTextureLayout::SHADER_READ,
                                           s.white_texture_view.get());
      s.white_texture_index = slot;
    }
  }

  layout_builder.addDescriptorSet(tex_set_builder);
  layout_builder.addDescriptorSet(tex_set_builder);
  layout_builder.addDescriptorSet(tex_set_builder);

  plume::RenderDescriptorSetBuilder sampler_set_builder;
  sampler_set_builder.begin();
  sampler_set_builder.addSampler(0, kBindlessSamplerCount);
  sampler_set_builder.end(true, kBindlessSamplerCount);
  s.sampler_descriptor_set = sampler_set_builder.create(s.device.get());
  if (!s.sampler_descriptor_set) {
    EOT_ERROR("createDescriptorSet for the bindless sampler heap failed");
    return false;
  }
  s.sampler_descriptor_used.assign(kBindlessSamplerCount, false);

  plume::RenderSamplerDesc linear;
  linear.minFilter = plume::RenderFilter::LINEAR;
  linear.magFilter = plume::RenderFilter::LINEAR;
  linear.mipmapMode = plume::RenderMipmapMode::LINEAR;
  linear.addressU = plume::RenderTextureAddressMode::CLAMP;
  linear.addressV = plume::RenderTextureAddressMode::CLAMP;
  linear.addressW = plume::RenderTextureAddressMode::CLAMP;
  s.linear_sampler = s.device->createSampler(linear);
  plume::RenderSamplerDesc point = linear;
  point.minFilter = plume::RenderFilter::NEAREST;
  point.magFilter = plume::RenderFilter::NEAREST;
  point.mipmapMode = plume::RenderMipmapMode::NEAREST;
  s.point_sampler = s.device->createSampler(point);
  if (!s.linear_sampler || !s.point_sampler) {
    EOT_ERROR("createSampler for the reserved samplers failed");
    return false;
  }
  s.sampler_descriptor_set->setSampler(kSamplerLinearClamp, s.linear_sampler.get());
  s.sampler_descriptor_set->setSampler(kSamplerPointClamp, s.point_sampler.get());
  for (u32 i = 0; i < kReservedSamplerCount; ++i)
    s.sampler_descriptor_used[i] = true;

  layout_builder.addDescriptorSet(sampler_set_builder);
  layout_builder.addDescriptorSet(tex_set_builder);

#if defined(EOT_D3D12)
  layout_builder.addRootDescriptor(0, 4, plume::RenderRootDescriptorType::CONSTANT_BUFFER);
  layout_builder.addRootDescriptor(1, 4, plume::RenderRootDescriptorType::CONSTANT_BUFFER);
  layout_builder.addRootDescriptor(2, 4, plume::RenderRootDescriptorType::CONSTANT_BUFFER);
  layout_builder.addPushConstant(3, 4, sizeof(CopyPushConstants),
                                 plume::RenderShaderStageFlag::PIXEL);
  layout_builder.addPushConstant(4, 4, sizeof(u32),
                                 plume::RenderShaderStageFlag::VERTEX |
                                     plume::RenderShaderStageFlag::PIXEL);
#else
  layout_builder.addPushConstant(0, 4, kCopyPushConstantByteOffset + sizeof(CopyPushConstants),
                                 plume::RenderShaderStageFlag::VERTEX |
                                     plume::RenderShaderStageFlag::PIXEL);
#endif

  layout_builder.end();
  s.pipeline_layout = layout_builder.create(s.device.get());
  if (!s.pipeline_layout) {
    EOT_ERROR("createPipelineLayout failed");
    return false;
  }
#if defined(EOT_D3D12)
  {
    const auto *layout = static_cast<const plume::D3D12PipelineLayout *>(s.pipeline_layout.get());
    for (u32 r = 0; r < 3 && r < layout->rootDescriptorRootIndicesAndTypes.size(); ++r) {
      const auto &[index, type] = layout->rootDescriptorRootIndicesAndTypes[r];
      if (type == plume::RenderRootDescriptorType::CONSTANT_BUFFER)
        s.root_cbv_index[r] = index;
    }
  }
#endif
  return true;
}

bool BuildHelperPipelines(VideoState &s) {
  s.copy_vs = s.device->createShader(EOT_SHADER_BLOB(copy_vs), "main", kHostShaderFormat);
  s.blit_ps = s.device->createShader(EOT_SHADER_BLOB(blit_ps), "main", kHostShaderFormat);
  s.copy_depth_ps =
      s.device->createShader(EOT_SHADER_BLOB(copy_depth_ps), "main", kHostShaderFormat);
#if !defined(EOT_D3D12)
  s.copy_depth_f24_ps =
      s.device->createShader(EOT_SHADER_BLOB(copy_depth_f24_ps), "main", kHostShaderFormat);
#endif
  if (!s.copy_vs || !s.blit_ps || !s.copy_depth_ps) {
    EOT_ERROR("createShader for the host helper passes failed");
    return false;
  }
  s.resolve_msaa_color_ps[0] =
      s.device->createShader(EOT_SHADER_BLOB(resolve_msaa_color_2x_ps), "main", kHostShaderFormat);
  s.resolve_msaa_color_ps[1] =
      s.device->createShader(EOT_SHADER_BLOB(resolve_msaa_color_4x_ps), "main", kHostShaderFormat);
  s.resolve_msaa_color_ps[2] =
      s.device->createShader(EOT_SHADER_BLOB(resolve_msaa_color_8x_ps), "main", kHostShaderFormat);
  s.resolve_msaa_depth_ps[0] =
      s.device->createShader(EOT_SHADER_BLOB(resolve_msaa_depth_2x_ps), "main", kHostShaderFormat);
  s.resolve_msaa_depth_ps[1] =
      s.device->createShader(EOT_SHADER_BLOB(resolve_msaa_depth_4x_ps), "main", kHostShaderFormat);
  s.resolve_msaa_depth_ps[2] =
      s.device->createShader(EOT_SHADER_BLOB(resolve_msaa_depth_8x_ps), "main", kHostShaderFormat);
  for (u32 i = 0; i < 3; ++i) {
    if (!s.resolve_msaa_color_ps[i] || !s.resolve_msaa_depth_ps[i]) {
      EOT_ERROR("createShader for the multisample resolve passes failed");
      return false;
    }
  }
#if defined(EOT_D3D12)
  {
    D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
    auto *device = static_cast<plume::D3D12Device *>(s.device.get())->d3d;
    s.stencil_ref_supported =
        SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))) &&
        options.PSSpecifiedStencilRefSupported;
    if (s.stencil_ref_supported) {
      s.derive_depth_stencil_ps[0] = s.device->createShader(EOT_SHADER_BLOB(derive_depth_stencil_2x_ps),
                                                            "main", kHostShaderFormat);
      s.derive_depth_stencil_ps[1] = s.device->createShader(EOT_SHADER_BLOB(derive_depth_stencil_4x_ps),
                                                            "main", kHostShaderFormat);
      s.derive_depth_stencil_ps[2] = s.device->createShader(EOT_SHADER_BLOB(derive_depth_stencil_8x_ps),
                                                            "main", kHostShaderFormat);
      for (u32 i = 0; i < 3; ++i)
        s.stencil_ref_supported = s.stencil_ref_supported && s.derive_depth_stencil_ps[i] != nullptr;
    }
    EOT_INFO("[gpu] pixel shader stencil reference: {}", s.stencil_ref_supported ? "supported" : "not supported");
  }
#endif
  return GetBlitPipeline(s, plume::RenderFormat::B8G8R8A8_UNORM) != nullptr;
}

plume::RenderPipeline *GetDeriveDepthStencilPipeline(VideoState &s, plume::RenderFormat ds_format,
                                                     u32 src_samples) {
  const int tier = src_samples == 2 ? 0 : src_samples == 4 ? 1 : src_samples == 8 ? 2 : -1;
  if (tier < 0 || !s.stencil_ref_supported || !s.derive_depth_stencil_ps[tier])
    return nullptr;
  const u64 key = (static_cast<u64>(ds_format) << 8) | static_cast<u64>(tier);
  auto it = s.derive_depth_stencil_pipelines.find(key);
  if (it != s.derive_depth_stencil_pipelines.end())
    return it->second.get();
  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = s.pipeline_layout.get();
  desc.vertexShader = s.copy_vs.get();
  desc.pixelShader = s.derive_depth_stencil_ps[tier].get();
  desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  desc.cullMode = plume::RenderCullMode::NONE;
  desc.depthFunction = plume::RenderComparisonFunction::ALWAYS;
  desc.depthEnabled = true;
  desc.depthWriteEnabled = true;
  desc.stencilEnabled = true;
  desc.stencilReadMask = 0xFF;
  desc.stencilWriteMask = 0xFF;
  desc.stencilReference = 0;
  desc.stencilFrontFace.compareFunction = plume::RenderComparisonFunction::ALWAYS;
  desc.stencilFrontFace.passOp = plume::RenderStencilOp::REPLACE;
  desc.stencilFrontFace.failOp = plume::RenderStencilOp::REPLACE;
  desc.stencilFrontFace.depthFailOp = plume::RenderStencilOp::REPLACE;
  desc.stencilBackFace = desc.stencilFrontFace;
  desc.renderTargetCount = 0;
  desc.depthTargetFormat = ds_format;
  auto pso = CreateHostGraphicsPipeline(s.device.get(), desc, "derive-depth-stencil");
  if (!pso)
    return nullptr;
  auto *raw = pso.get();
  s.derive_depth_stencil_pipelines.emplace(key, std::move(pso));
  return raw;
}

plume::RenderPipeline *GetResolveMsaaPipeline(VideoState &s, plume::RenderFormat dst_format,
                                              u32 src_samples, bool depth) {
  const int tier = src_samples == 2 ? 0 : src_samples == 4 ? 1 : src_samples == 8 ? 2 : -1;
  if (tier < 0)
    return nullptr;
  const u64 key = (static_cast<u64>(dst_format) << 8) | (static_cast<u64>(tier) << 1) |
                  (depth ? 1u : 0u);
  auto it = s.resolve_msaa_pipelines.find(key);
  if (it != s.resolve_msaa_pipelines.end())
    return it->second.get();
  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = s.pipeline_layout.get();
  desc.vertexShader = s.copy_vs.get();
  desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  desc.cullMode = plume::RenderCullMode::NONE;
  desc.depthFunction = plume::RenderComparisonFunction::ALWAYS;
  if (depth) {
    desc.pixelShader = s.resolve_msaa_depth_ps[tier].get();
    desc.depthEnabled = true;
    desc.depthWriteEnabled = true;
    desc.renderTargetCount = 0;
    desc.depthTargetFormat = dst_format;
  } else {
    desc.pixelShader = s.resolve_msaa_color_ps[tier].get();
    desc.depthEnabled = false;
    desc.depthWriteEnabled = false;
    desc.renderTargetCount = 1;
    desc.renderTargetFormat[0] = dst_format;
    desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
    desc.depthTargetFormat = plume::RenderFormat::UNKNOWN;
  }
  auto pso = CreateHostGraphicsPipeline(s.device.get(), desc, "resolve-msaa");
  if (!pso)
    return nullptr;
  auto *raw = pso.get();
  s.resolve_msaa_pipelines.emplace(key, std::move(pso));
  return raw;
}

plume::RenderPipeline *GetBlitPipeline(VideoState &s, plume::RenderFormat rt_format, u32 samples) {
  samples = std::max(samples, 1u);
  const u64 key = static_cast<u64>(rt_format) | (static_cast<u64>(samples) << 32);
  auto it = s.blit_pipelines.find(key);
  if (it != s.blit_pipelines.end())
    return it->second.get();
  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = s.pipeline_layout.get();
  desc.vertexShader = s.copy_vs.get();
  desc.pixelShader = s.blit_ps.get();
  desc.depthFunction = plume::RenderComparisonFunction::ALWAYS;
  desc.depthEnabled = false;
  desc.depthWriteEnabled = false;
  desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  desc.cullMode = plume::RenderCullMode::NONE;
  desc.renderTargetCount = 1;
  desc.renderTargetFormat[0] = rt_format;
  desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
  desc.depthTargetFormat = plume::RenderFormat::UNKNOWN;
  desc.multisampling.sampleCount = static_cast<plume::RenderSampleCounts>(samples);
  auto pso = CreateHostGraphicsPipeline(s.device.get(), desc, samples > 1 ? "blit-broadcast" : "blit");
  if (!pso)
    return nullptr;
  auto *raw = pso.get();
  s.blit_pipelines.emplace(key, std::move(pso));
  return raw;
}

plume::RenderPipeline *GetDepthCopyPipeline(VideoState &s, plume::RenderFormat ds_format,
                                            u32 samples, bool float24) {
  samples = std::max(samples, 1u);
  if (!s.copy_depth_f24_ps)
    float24 = false;
  const u64 key = static_cast<u64>(ds_format) | (static_cast<u64>(samples) << 32) |
                  (float24 ? 1ull << 63 : 0ull);
  auto it = s.depth_copy_pipelines.find(key);
  if (it != s.depth_copy_pipelines.end())
    return it->second.get();
  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = s.pipeline_layout.get();
  desc.vertexShader = s.copy_vs.get();
  desc.pixelShader = float24 ? s.copy_depth_f24_ps.get() : s.copy_depth_ps.get();
  desc.depthFunction = plume::RenderComparisonFunction::ALWAYS;
  desc.depthEnabled = true;
  desc.depthWriteEnabled = true;
  desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  desc.cullMode = plume::RenderCullMode::NONE;
  desc.renderTargetCount = 0;
  desc.depthTargetFormat = ds_format;
  desc.multisampling.sampleCount = static_cast<plume::RenderSampleCounts>(samples);
  auto pso = CreateHostGraphicsPipeline(s.device.get(), desc,
                                        samples > 1 ? "copy-depth-broadcast" : "copy-depth");
  if (!pso)
    return nullptr;
  auto *raw = pso.get();
  s.depth_copy_pipelines.emplace(key, std::move(pso));
  return raw;
}

namespace {

std::string PanelMode() {
#if defined(__PROSPERO__)
  // One display, its mode chosen by plume when it made the swap chain
  if (const auto &chain = state().swap_chain)
    return fmt::format("{}x{} PS5 display", chain->getWidth(), chain->getHeight());
  return "PS5 display";
#else
  int count = 0;
  SDL_Window **windows = SDL_GetWindows(&count);
  SDL_Window *win = (windows && count > 0) ? windows[0] : nullptr;
  SDL_free(windows);
  if (!win)
    return "unknown";
  const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(win));
  if (!mode)
    return "unknown";
  return fmt::format("{}x{} @ {:.0f} Hz", mode->w, mode->h, mode->refresh_rate);
#endif
}

}

namespace {

// Keeps the guest's video mode when exclusive fullscreen sets the resolution cvar.
void PinGuestVideoMode() {
  for (auto &entry : rex::cvar::GetRegistry())
    if ((entry.name == "video_mode_width" || entry.name == "video_mode_height") && entry.default_value != "0")
      entry.default_value = "0";
}

}

bool Video::CreateHostDevice(rex::ui::Window *window) {
  if (!window) {
    EOT_ERROR("Video::CreateHostDevice: null window");
    return false;
  }
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (s.ready)
    return true;
  RenderDocInit();

  plume::RenderWindow render_window{};
#if defined(_WIN32)
  render_window = static_cast<plume::RenderWindow>(window->GetNativeWindowHandle());
  if (!render_window) {
    EOT_ERROR("Window has no native HWND yet");
    return false;
  }
#elif defined(__PROSPERO__)
  // The display: plume makes its surface with VK_KHR_display at this size
  // (ps5/patches/plume-ps5.patch)
  render_window = {window->GetActualPhysicalWidth(), window->GetActualPhysicalHeight()};
#elif defined(PLUME_SDL_VULKAN_ENABLED)
  {
    int count = 0;
    SDL_Window **windows = SDL_GetWindows(&count);
    render_window = (windows && count > 0) ? windows[0] : nullptr;
    SDL_free(windows);
  }
  if (!render_window) {
    EOT_ERROR("No SDL window exists yet");
    return false;
  }
#elif defined(__APPLE__)
  if (!eot::platform::PrepareMoltenVK() || !eot::platform::GetMetalRenderWindow(render_window))
    return false;
#else
  EOT_ERROR("Native window handles are not wired up for this platform");
  return false;
#endif

#if defined(EOT_D3D12)
#if defined(EOT_D3D12)
  if (Settings::D3D12Debug()) {
    ID3D12Debug *debug = nullptr;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
      debug->EnableDebugLayer();
      debug->Release();
      EOT_INFO("[gpu] D3D12 debug layer enabled (eot_d3d12_debug)");
    }
  }
#endif
  s.render_iface = plume::CreateD3D12Interface();
#else
  s.render_iface = plume::CreateVulkanInterface();
#endif
  if (!s.render_iface) {
    EOT_ERROR("plume CreateInterface failed");
    return false;
  }
  s.device = s.render_iface->createDevice();
  if (!s.device) {
    EOT_ERROR("plume createDevice failed");
    return false;
  }
  s.backend_info = DescribeBackend(s.device.get());

  s.queue = s.device->createCommandQueue(plume::RenderCommandListType::DIRECT);
  for (u32 i = 0; i < kNumFrames; ++i) {
    s.command_lists[i] = s.queue->createCommandList();
    s.fences[i] = s.device->createCommandFence();
    s.acquire_semaphores[i] = s.device->createCommandSemaphore();
  }
  s.command_list = s.command_lists[0].get();

  constexpr uint32_t kMaxFrameLatency = 2;
  {
    const eot::platform::Display display = eot::platform::DisplayFor(window->GetNativeWindowHandle());
    SetAutoRenderHeight(eot::platform::AutoRenderHeight(display));
    SetDisplayHeight(display.height);
    if (display.refresh_hz)
      s.display_refresh_hz = display.refresh_hz;
    EOT_INFO("[gpu] display {}x{} at {} Hz, {} (the display suggests {}p)", display.width, display.height,
             display.refresh_hz, Settings::Fullscreen() ? "fullscreen" : "windowed",
             eot::platform::AutoRenderHeight(display));
    if (Settings::Fullscreen() && Settings::FullscreenMode() == "exclusive") {
      const u32 want_h = InternalRenderHeight();
      const u32 want_w = display.width && display.height
                             ? static_cast<u32>(std::lround(static_cast<f64>(want_h) * display.width /
                                                            display.height))
                             : InternalRenderWidth();
      PinGuestVideoMode();
      rex::cvar::SetFlagByName("resolution", std::format("{}x{}", want_w, want_h));
      rex::cvar::SetFlagByName("fullscreen_exclusive", "true");
      EOT_INFO("[gpu] exclusive fullscreen at the display mode nearest {}x{} for the {}x{} internal render",
               want_w, want_h, InternalRenderWidth(), want_h);
    } else if (rex::cvar::GetFlagByName("fullscreen_exclusive") == "true" ||
               !rex::cvar::GetFlagByName("resolution").empty()) {
      rex::cvar::SetFlagByName("fullscreen_exclusive", "false");
      rex::cvar::SetFlagByName("resolution", "");
    }
  }
  s.present_wait = s.device->getCapabilities().presentWait;
  if (!s.present_wait)
    EOT_INFO("[gpu] no present wait on this driver; the frame's fence paces the presents");
  plume::RenderSwapChainDesc desc(render_window, plume::RenderFormat::B8G8R8A8_UNORM, kNumFrames + 1,
                                  s.present_wait, kMaxFrameLatency);
  s.swap_chain = s.queue->createSwapChain(desc);
  if (s.swap_chain) {
    s.swap_chain->setVsyncEnabled(Settings::Vsync());
#if !defined(EOT_D3D12)
    s.swap_chain->resize();
#endif
  }
  if (!s.swap_chain || s.swap_chain->isEmpty()) {
    EOT_ERROR("plume createSwapChain failed");
    return false;
  }
  if (!BuildSwapFramebuffers(s) || !BuildPipelineLayout(s) || !BuildHelperPipelines(s))
    return false;
  if (!UploadRingInit()) {
    EOT_ERROR("upload ring init failed");
    return false;
  }

#if EOT_GPU_PROFILING
  InitGPUProfiler(static_cast<plume::D3D12Device *>(s.device.get())->d3d,
                  static_cast<plume::D3D12CommandQueue *>(s.queue.get())->d3d);
#endif
  ApplyQualityPresetAtBoot();
  {
    const i32 asked = Settings::Msaa();
    u32 samples = asked == 2 || asked == 4 || asked == 8 ? static_cast<u32>(asked) : 1u;
    if (samples > 1) {
      const plume::RenderFormat formats[] = {
          plume::RenderFormat::R16G16B16A16_FLOAT, plume::RenderFormat::R8G8B8A8_UNORM,
          plume::RenderFormat::R16G16B16A16_UNORM, DepthRenderTargetFormat()};
      plume::RenderSampleCounts mask = ~0u;
      for (const plume::RenderFormat f : formats)
        mask &= s.device->getSampleCountsSupported(f);
      while (samples > 1 && !(mask & samples))
        samples >>= 1;
      if (samples != static_cast<u32>(asked))
        EOT_WARN("[gpu] wos_msaa {} is not supported for every scene format (mask {:#x}); using {}x",
                 asked, mask, samples);
    }
    s.host_msaa_samples = samples;
  }
#if defined(EOT_D3D12)
  if (auto *d3d_device = static_cast<plume::D3D12Device *>(s.device.get());
      d3d_device->allocator &&
      d3d_device->allocator->GetD3D12Options().ResourceHeapTier >= D3D12_RESOURCE_HEAP_TIER_2) {
    plume::RenderPoolDesc pool_desc;
    pool_desc.heapType = plume::RenderHeapType::DEFAULT;
    auto pool = s.device->createPool(pool_desc);
    if (pool && static_cast<plume::D3D12Pool *>(pool.get())->d3d)
      s.transient_mirror_pool = std::move(pool);
    else
      EOT_WARN("[gpu] no allocator pool for the picture-in-picture mirrors; they share the heaps");
  }
#endif
  s.ready = true;
  RenderThreadStart();
  EOT_INFO("[gpu] {} on {} ready: swapchain {}x{}, {} bindless texture slots, msaa {}x on the "
           "full-frame surfaces",
           s.backend_info, s.device->getDescription().name, s.swap_chain->getWidth(),
           s.swap_chain->getHeight(), kBindlessTextureCount, s.host_msaa_samples);
  EOT_INFO("[gpu] internal render {}x{} ({} = {:.3f}x the guest's {}x{}), fitted to the window at "
           "present; vsync {}, fps cap {}",
           InternalRenderWidth(), InternalRenderHeight(), Settings::Resolution(),
           RenderScaleFactor(), kGuestRenderWidth, kGuestRenderHeight,
           Settings::Vsync() ? "on" : "off",
           Settings::FpsLimit() > 0 ? std::to_string(Settings::FpsLimit()) : std::string("none"));
#if defined(EOT_D3D12)
  EOT_INFO("[gpu] swap chain: {} buffers, {} frames in flight, tearing {}; panel {}", kNumFrames + 1,
           kMaxFrameLatency,
           static_cast<plume::D3D12Interface *>(s.render_iface.get())->allowTearing ? "allowed"
                                                                                  : "not supported here",
           PanelMode());
#endif
  return true;
}

void Video::RequestShutdown() { state().shutting_down.store(true, std::memory_order_release); }

bool Video::IsShuttingDown() { return state().shutting_down.load(std::memory_order_acquire); }

void Video::BeginShutdown() {
  auto &s = state();
  s.shutting_down.store(true, std::memory_order_release);
  RenderThreadStop();
  PsoPrecacheStop();
  PsoCacheFlushIfDirty(true);
  if (s.quiesced)
    return;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
  while (!s.mutex.try_lock()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      EOT_WARN("[gpu] shutdown: a thread is still in the renderer; exiting without the drain");
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  s.ready = false;
  if (s.command_list_open && s.command_list) {
    s.command_list->end();
    s.command_list_open = false;
  }
  if (s.queue) {
    for (u32 i = 0; i < kNumFrames; ++i) {
      if (s.command_list_submitted[i] && s.fences[i]) {
        s.queue->waitForCommandFence(s.fences[i].get());
        s.command_list_submitted[i] = false;
      }
    }
  }
  s.quiesced = true;
}

void Video::Shutdown() {
  auto &s = state();
  std::unique_lock lock(s.mutex, std::try_to_lock);
  if (!lock.owns_lock() && !s.quiesced) {
    EOT_WARN("Shutdown: renderer busy, skipping GPU drain");
    return;
  }
  s.ready = false;
  if (s.command_list_open && s.command_list) {
    s.command_list->end();
    s.command_list_open = false;
  }
  if (s.queue) {
    for (u32 i = 0; i < kNumFrames; ++i) {
      if (s.command_list_submitted[i] && s.fences[i]) {
        s.queue->waitForCommandFence(s.fences[i].get());
        s.command_list_submitted[i] = false;
      }
    }
  }
  s.swap_framebuffers.clear();
  s.render_semaphores.clear();
  s.swap_chain.reset();
}

plume::RenderDevice *Video::HostDevice() { return state().device.get(); }

u32 Video::OutputWidth() {
  auto &s = state();
  return s.swap_chain ? s.swap_chain->getWidth() : 0;
}

u32 Video::OutputHeight() {
  auto &s = state();
  return s.swap_chain ? s.swap_chain->getHeight() : 0;
}

void Video::RequestResize() {
  state().resize_requested.store(true, std::memory_order_release);
}

}
