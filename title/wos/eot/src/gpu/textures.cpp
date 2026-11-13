// gpu/textures.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "gpu/textures.h"
#include "gpu/render_thread.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <cstdio>
#include <format>
#include <string>
#include <dirent.h>
#include <sys/stat.h>
#include <xxhash.h>

#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <functional>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/d3d.h"
#include "core/profiling.h"
#include "gpu/device.h"
#include "gpu/gpu_timing.h"

#include "gpu/settings.h"
#include "gpu/surfaces.h"
#include "gpu/format.h"

extern "C" const char *prospero_state_root();

extern "C" int wos_debug_cube_lod_bias();  // wos_ps5_native_renderer.cpp (fix71)
extern "C" bool wos_cube_tail_clamp();
extern "C" bool wos_debug_cube_faces();

namespace eot::gpu {

namespace {

namespace xe = rex::graphics::xenos;
namespace tu = rex::graphics::texture_util;
namespace tc = rex::graphics::texture_conversion;
using rex::graphics::FormatInfo;
using rex::graphics::TextureInfo;

struct UnlockTable {
  std::mutex mutex;
  std::unordered_map<u32, u64> seq;
  std::atomic<u64> global{0};
};

UnlockTable &unlocks() {
  static UnlockTable t;
  return t;
}

std::unordered_map<u32, TextureInfo> &infos() {
  static std::unordered_map<u32, TextureInfo> m;
  return m;
}

// WoS port: one storage, two numbers. The game's resolve-target headers carry the
// texture's guest virtual address (0xBCAF1000 in the A0-BF view, 0xFD820000 in the
// E0 view, which sits 0x1000 above its physical page), while the headers it samples
// through carry the physical one (0x1CAF1000, 0x1D821000). Fetch constants are kept
// physical so a resolve and the reads of what it wrote meet on one texture (and a
// header flipping between the two forms is not "changed", throwing away its contents).
u32 PhysicalGpuAddress(u32 a) {
  if (a < 0x20000000u)
    return a;
  return (a & 0x1FFFFFFFu) + (a >= 0xE0000000u ? 0x1000u : 0u);
}

void NormalizeFetchAddresses(u32 fetch[6]) {
  if (const u32 base = fetch[1] & 0xFFFFF000u)
    fetch[1] = PhysicalGpuAddress(base) | (fetch[1] & 0xFFFu);
  if (const u32 mip = fetch[5] & 0xFFFFF000u)
    fetch[5] = PhysicalGpuAddress(mip) | (fetch[5] & 0xFFFu);
}

// WoS port: bumped whenever a texture first becomes a resolve destination; a slot
// sampling resolve memory re-checks for an owner only when this moved.
std::atomic<u64> g_owner_epoch{1};

bool ReadFetch(u32 header_va, u32 out[6]) {
  auto *p = mem::at<be_u32>(header_va + obj::kTextureFetch);
  if (!p)
    return false;
  bool any = false;
  for (u32 i = 0; i < 6; ++i) {
    out[i] = p[i];
    any |= out[i] != 0;
  }
  return any;
}

u32 InfoWidth(const TextureInfo &info) { return info.width + 1; }
u32 InfoHeight(const TextureInfo &info) { return info.height + 1; }
u32 InfoDepth(const TextureInfo &info) { return info.depth + 1; }

bool SynthesizesMips(const TextureInfo &info, const TextureFormatMapping &m) {
  if (info.dimension != xe::DataDimension::k2DOrStacked || InfoDepth(info) > 1 ||
      info.mip_min_level != 0 || info.mip_max_level != 0)
    return false;
  const u32 w = InfoWidth(info), h = InfoHeight(info);
  if (w < 128 || h < 128 || (w & (w - 1)) || (h & (h - 1)))
    return false;
  return m.format == plume::RenderFormat::R8_UNORM || m.format == plume::RenderFormat::R8G8_UNORM ||
         m.format == plume::RenderFormat::R8G8B8A8_UNORM;
}

inline u32 TiledRowOffset(u32 y, u32 width, u32 log2_bpp) {
  const u32 macro = ((y / 32) * (width / 32)) << (log2_bpp + 7);
  const u32 micro = ((y & 6) << 2) << log2_bpp;
  return macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 8) << (3 + log2_bpp)) + ((y & 1) << 4);
}

inline u32 TiledColumnOffset(u32 x, u32 y, u32 log2_bpp, u32 base_offset) {
  const u32 macro = (x / 32) << (log2_bpp + 7);
  const u32 micro = (x & 7) << log2_bpp;
  const u32 offset = base_offset + (macro + ((micro & ~0xFu) << 1) + (micro & 0xF));
  return ((offset & ~0x1FFu) << 3) + ((offset & 0x1C0) << 2) + (offset & 0x3F) + ((y & 16) << 7) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
}

template <u32 kBpb, xe::Endian kEndian> inline void CopySwapBlockFixed(u8 *out, const u8 *in) {
  if constexpr (kEndian == xe::Endian::k8in16) {
    for (u32 i = 0; i < kBpb; i += 2) {
      u16 v;
      std::memcpy(&v, in + i, 2);
      v = static_cast<u16>((v >> 8) | (v << 8));
      std::memcpy(out + i, &v, 2);
    }
  } else if constexpr (kEndian == xe::Endian::k8in32) {
    for (u32 i = 0; i < kBpb; i += 4) {
      u32 v;
      std::memcpy(&v, in + i, 4);
      v = __builtin_bswap32(v);
      std::memcpy(out + i, &v, 4);
    }
  } else if constexpr (kEndian == xe::Endian::k16in32) {
    for (u32 i = 0; i < kBpb; i += 4) {
      u32 v;
      std::memcpy(&v, in + i, 4);
      v = (v >> 16) | (v << 16);
      std::memcpy(out + i, &v, 4);
    }
  } else {
    std::memcpy(out, in, kBpb);
  }
}

template <u32 kBpb, xe::Endian kEndian>
void UntileFixed(u8 *out, const u8 *in, const tc::UntileInfo &ui) {
  constexpr u32 kLog2Bpp = (kBpb / 4) + ((kBpb / 2) >> (kBpb / 4));
  const u64 out_pitch = u64(ui.output_pitch) * kBpb;
  for (u32 y = 0; y < ui.height; ++y) {
    const u32 gy = ui.offset_y + y;
    const u32 row = TiledRowOffset(gy, ui.input_pitch, kLog2Bpp);
    u8 *dst = out + y * out_pitch;
    for (u32 x = 0; x < ui.width; ++x, dst += kBpb) {
      const u32 block = TiledColumnOffset(ui.offset_x + x, gy, kLog2Bpp, row) >> kLog2Bpp;
      CopySwapBlockFixed<kBpb, kEndian>(dst, in + u64(block) * kBpb);
    }
  }
}

template <u32 kBpb> bool UntileForBpb(u8 *out, const u8 *in, const tc::UntileInfo &ui, xe::Endian endian) {
  switch (endian) {
  case xe::Endian::k8in16:
    if constexpr (kBpb % 2 == 0) {
      UntileFixed<kBpb, xe::Endian::k8in16>(out, in, ui);
      return true;
    }
    return false;
  case xe::Endian::k8in32:
    if constexpr (kBpb % 4 == 0) {
      UntileFixed<kBpb, xe::Endian::k8in32>(out, in, ui);
      return true;
    }
    return false;
  case xe::Endian::k16in32:
    if constexpr (kBpb % 4 == 0) {
      UntileFixed<kBpb, xe::Endian::k16in32>(out, in, ui);
      return true;
    }
    return false;
  default:
    UntileFixed<kBpb, xe::Endian::kNone>(out, in, ui);
    return true;
  }
}

bool UntileFast(u8 *out, const u8 *in, const tc::UntileInfo &ui, u32 bpb, xe::Endian endian) {
  switch (bpb) {
  case 1: return UntileForBpb<1>(out, in, ui, endian);
  case 2: return UntileForBpb<2>(out, in, ui, endian);
  case 4: return UntileForBpb<4>(out, in, ui, endian);
  case 8: return UntileForBpb<8>(out, in, ui, endian);
  case 16: return UntileForBpb<16>(out, in, ui, endian);
  default: return false;
  }
}

void UntileLevel(u8 *out, const u8 *in, const tc::UntileInfo &ui, u32 bpb, xe::Endian endian) {
  static u8 verdict[17][4] = {};
  const u32 e = static_cast<u32>(endian) & 3;
  u8 &state = verdict[std::min(bpb, 16u)][e];
  if (state != 2 && UntileFast(out, in, ui, bpb, endian)) {
    if (state == 1)
      return;
    const u64 pitch = u64(ui.output_pitch) * bpb;
    std::vector<u8> reference(pitch * ui.height + 64 * bpb, 0);
    tc::UntileInfo sdk = ui;
    sdk.copy_callback = [endian](void *o, const void *i, size_t n) { tc::CopySwapBlock(endian, o, i, n); };
    tc::Untile(reference.data(), in, &sdk);
    bool same = true;
    for (u32 y = 0; y < ui.height && same; ++y)
      same = std::memcmp(out + y * pitch, reference.data() + y * pitch, u64(ui.width) * bpb) == 0;
    state = same ? 1 : 2;
    if (same) {
      EOT_DEBUG("[textures] fast untile checked against the SDK's for {}-byte blocks, byte order {}", bpb, e);
    } else {
      EOT_ERROR("[textures] fast untile differs from the SDK's for {}-byte blocks, byte order {}; "
                "using the SDK's",
                bpb, e);
      std::memcpy(out, reference.data(), pitch * ui.height);
    }
    return;
  }
  tc::UntileInfo sdk = ui;
  sdk.copy_callback = [endian](void *o, const void *i, size_t n) { tc::CopySwapBlock(endian, o, i, n); };
  tc::Untile(out, in, &sdk);
}

void SynthesizeMipChain(VideoState &s, HostTexture &host, plume::RenderFormat format,
                        std::vector<u8> &level0, u32 w, u32 h, u64 pitch, u32 bpb) {
  std::vector<u8> prev = std::move(level0), cur;
  for (u32 level = 1; level < host.mipLevels; ++level) {
    const u32 lw = std::max(1u, w >> 1), lh = std::max(1u, h >> 1);
    const u64 lpitch = (u64(lw) * bpb + kTextureRowPitchAlignment - 1) /
                       kTextureRowPitchAlignment * kTextureRowPitchAlignment;
    const u64 bytes = (lpitch * lh + kTexturePlacementAlignment - 1) /
                      kTexturePlacementAlignment * kTexturePlacementAlignment;
    cur.assign(bytes, 0);
    for (u32 y = 0; y < lh; ++y) {
      const u8 *r0 = prev.data() + u64(std::min(2 * y, h - 1)) * pitch;
      const u8 *r1 = prev.data() + u64(std::min(2 * y + 1, h - 1)) * pitch;
      u8 *dst = cur.data() + y * lpitch;
      for (u32 x = 0; x < lw; ++x) {
        const u64 x0 = u64(std::min(2 * x, w - 1)) * bpb, x1 = u64(std::min(2 * x + 1, w - 1)) * bpb;
        for (u32 c = 0; c < bpb; ++c)
          dst[x * bpb + c] = static_cast<u8>(
              (u32(r0[x0 + c]) + r0[x1 + c] + r1[x0 + c] + r1[x1 + c] + 2) >> 2);
      }
    }
    UploadAlloc staging;
    if (!UploadAllocate(bytes, kTexturePlacementAlignment, &staging))
      return;
    std::memcpy(staging.cpu, cur.data(), bytes);
    GpuTimingMark(s, s.command_list, kGpuCatUpload);
    s.command_list->copyTextureRegion(
        plume::RenderTextureCopyLocation::Subresource(host.texture.get(), level, 0),
        plume::RenderTextureCopyLocation::PlacedFootprint(
            staging.buffer, format, lw, lh, 1, static_cast<u32>(lpitch / bpb), staging.offset),
        0, 0, 0);
    prev.swap(cur);
    w = lw;
    h = lh;
    pitch = lpitch;
  }
  host.needsClear = false;
}

bool CreateHostImage(VideoState &s, GuestTexture &t, const TextureInfo &info) {
  HostTexture &host = t.host;
  const TextureFormatMapping m = MapTextureFormat(info.format);
  if (!m.supported) {
    if (!t.uploadFailed) {
      t.uploadFailed = true;
      EOT_WARN("[textures] {:#x}: unsupported guest format {} ({}x{}), sampling null", t.va,
               static_cast<u32>(info.format), InfoWidth(info), InfoHeight(info));
    }
    return false;
  }
  const bool depth = info.format == xe::TextureFormat::k_24_8 ||
                     info.format == xe::TextureFormat::k_24_8_FLOAT;

  plume::RenderTextureDesc desc;
  desc.width = InfoWidth(info);
  desc.height = InfoHeight(info);
  if (m.blockCompressed) {
    desc.width = (desc.width + 3) & ~3u;
    desc.height = (desc.height + 3) & ~3u;
  }
  desc.depth = 1;
  desc.arraySize = 1;
  desc.committed = false;
  switch (info.dimension) {
  case xe::DataDimension::k3D:
    desc.dimension = plume::RenderTextureDimension::TEXTURE_3D;
    desc.depth = InfoDepth(info);
    host.viewDimension = plume::RenderTextureViewDimension::TEXTURE_3D;
    break;
  case xe::DataDimension::kCube:
    desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
    desc.arraySize = 6;
    desc.flags = plume::RenderTextureFlag::CUBE;
    host.viewDimension = plume::RenderTextureViewDimension::TEXTURE_CUBE;
    break;
  case xe::DataDimension::k1D:
  case xe::DataDimension::k2DOrStacked:
  default:
    desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
    host.viewDimension = plume::RenderTextureViewDimension::TEXTURE_2D;
    if (info.is_stacked && InfoDepth(info) > 1)
      desc.arraySize = InfoDepth(info);
    break;
  }
  u32 max_levels = 1;
  for (u32 w = desc.width, h = desc.height; (w > 1 || h > 1) && max_levels < 14; ++max_levels) {
    w = std::max(1u, w >> 1);
    h = std::max(1u, h >> 1);
  }
  t.synthMips = !depth && SynthesizesMips(info, m);
  desc.mipLevels = t.synthMips ? max_levels : std::min(info.mip_max_level + 1u, max_levels);
  if (t.synthMips)
    EOT_DEBUG("[textures] {:#x}: {}x{} fmt {} ships one level; the host carries {}", t.va,
              desc.width, desc.height, static_cast<u32>(info.format), desc.mipLevels);

  if (depth) {
    desc.format = plume::RenderFormat::D32_FLOAT_S8_UINT;
    desc.flags = desc.flags | plume::RenderTextureFlag::DEPTH_TARGET;
    host.isDepth = true;
  } else {
    desc.format = t.gammaSigned && m.srgbFormat != plume::RenderFormat::UNKNOWN ? m.srgbFormat : m.format;
    if (!m.blockCompressed && desc.dimension != plume::RenderTextureDimension::TEXTURE_3D)
      desc.flags = desc.flags | plume::RenderTextureFlag::RENDER_TARGET;
    host.isDepth = false;
  }
  host.format = desc.format;
  host.width = desc.width;
  host.height = desc.height;
  host.depth = desc.depth;
  host.mipLevels = desc.mipLevels;
  host.arraySize = desc.arraySize;
  host.renderable = (desc.flags & plume::RenderTextureFlag::RENDER_TARGET) ||
                    (desc.flags & plume::RenderTextureFlag::DEPTH_TARGET);
  CreateOrRecycleHostTexture(s, host, desc, "guest-texture");
  if (!host.texture) {
    t.uploadFailed = true;
    return false;
  }
  return true;
}

// WoS port: CPU-written textures. Bink writes each video frame straight into its
// planes' memory (no Lock/Unlock the renderer sees), so the first upload - whatever the
// memory held, noise - stayed on screen. A sparse hash of the base level (256 dwords
// spread over it, the first and last pages included) is taken at upload and compared
// once a frame when the texture is sampled; a change re-uploads it.
bool ContentHashEligible(const GuestTexture &t) {
  if (t.resolveOwned || t.host.isDepth || !t.baseAddress || t.width < 16 || t.height < 16)
    return false;
  const TextureFormatMapping m = MapTextureFormat(t.format);
  // WoS port (fix43): block-compressed too. Web of Shadows streams the city's DXT textures in
  // without an Unlock: a texture first uploaded before its data arrived stayed black (the
  // mid-distance buildings drawn as black silhouettes, fix41/42 frames) - EoT only watched
  // uncompressed ones (Bink video). 256 sampled words, once a frame per texture used.
  return m.supported;
}

u64 ContentHash(const GuestTexture &t, const TextureInfo &info) {
  const u32 bytes = std::max<u32>(info.memory.base_size, 4u) & ~3u;
  if (!mem::gpu_readable(t.baseAddress, bytes))
    return 0;
  const u8 *p = mem::gpu<const u8>(t.baseAddress);
  if (!p)
    return 0;
  constexpr u32 kSamples = 256;
  const u32 words = bytes / 4;
  u64 h = 0xcbf29ce484222325ull ^ bytes;
  for (u32 i = 0; i < kSamples; ++i) {
    const u32 w = static_cast<u32>((static_cast<u64>(i) * (words - 1)) / (kSamples - 1));
    u32 v;
    std::memcpy(&v, p + 4 * w, 4);
    h = (h ^ v) * 0x100000001b3ull;
  }
  // WoS port (fix45): the mips too - the city's atlases (1024x2048 DXT, 11 levels) drew black
  // at medium distance: mips uploaded before they were filled were never refreshed when only
  // the base level was watched
  const u32 mip_bytes = info.memory.mip_size & ~3u;
  if (info.memory.mip_address && mip_bytes >= 4 && mem::gpu_readable(info.memory.mip_address, mip_bytes))
    if (const u8 *m = mem::gpu<const u8>(info.memory.mip_address)) {
      const u32 mwords = mip_bytes / 4;
      constexpr u32 kMipSamples = 128;
      for (u32 i = 0; i < kMipSamples; ++i) {
        const u32 w = static_cast<u32>((static_cast<u64>(i) * (mwords - 1)) / (kMipSamples - 1));
        u32 v;
        std::memcpy(&v, m + 4 * w, 4);
        h = (h ^ v) * 0x100000001b3ull;
      }
    }
  return h | 1;  // never 0: 0 means "no hash"
}


// WoS port (fix39): texture mods, as the emulated-GPU builds had them (rexglue patch 0008):
// <state root>/mods/textures/<fingerprint>.bin replaces the texture whose guest data -
// the base level plus the mips when they follow it in the same allocation, tiled and
// big-endian as the console keeps them - hashes (XXH3-64) to <fingerprint>, when the file
// is exactly that size (the PlayStation button prompts). The guest memory is left alone:
// the uploader reads the replacement's bytes instead.
struct TextureMods {
  std::once_flag scanned;
  std::string dir;
  std::unordered_map<u64, std::shared_ptr<const std::vector<u8>>> loaded;
  std::unordered_map<u64, u32> sizes_by_hash;
  std::vector<u32> sizes;
  std::mutex lock;
  u32 near_misses = 0, hits = 0;
};
TextureMods &texture_mods() {
  static TextureMods m;
  std::call_once(m.scanned, [] {
    m.dir = std::string(prospero_state_root()) + "/mods/textures";
    if (DIR *d = opendir(m.dir.c_str())) {
      while (dirent *e = readdir(d)) {
        unsigned long long hash = 0;
        char tail[8] = {};
        if (std::sscanf(e->d_name, "%16llx%7s", &hash, tail) != 2 || std::string(tail) != ".bin")
          continue;
        struct stat st;
        if (stat((m.dir + "/" + e->d_name).c_str(), &st) != 0 || st.st_size <= 0)
          continue;
        m.sizes_by_hash[hash] = static_cast<u32>(st.st_size);
        if (std::find(m.sizes.begin(), m.sizes.end(), u32(st.st_size)) == m.sizes.end())
          m.sizes.push_back(static_cast<u32>(st.st_size));
      }
      closedir(d);
    }
    EOT_INFO("[texmods] {} replacement(s) in {}", m.sizes_by_hash.size(), m.dir);
  });
  return m;
}
// The replacement for a texture about to be uploaded, or null. *bytes = its size.
std::shared_ptr<const std::vector<u8>> TextureModFor(u32 base, u32 mip_address, u32 base_size,
                                                     u32 mips_size, const TextureInfo &info,
                                                     u32 *bytes) {
  TextureMods &m = texture_mods();
  if (m.sizes.empty() || !base || !base_size)
    return nullptr;
  u32 size = base_size;
  if (mip_address && mip_address == base + base_size)
    size += mips_size;
  if (std::find(m.sizes.begin(), m.sizes.end(), size) == m.sizes.end())
    return nullptr;
  if (!mem::gpu_readable(base, size))
    return nullptr;
  const u8 *data = mem::gpu<u8>(base);
  if (!data)
    return nullptr;
  const u64 hash = XXH3_64bits(data, size);
  std::lock_guard lock(m.lock);
  auto it = m.sizes_by_hash.find(hash);
  if (it == m.sizes_by_hash.end() || it->second != size) {
    if (m.near_misses < 48) {
      ++m.near_misses;
      EOT_INFO("[texmods] {:016x}: {}x{} fmt {} {} at {:#x} ({} bytes, mips {:#x}) - the size of a "
               "replacement, no file for this fingerprint",
               hash, InfoWidth(info), InfoHeight(info), static_cast<u32>(info.format),
               info.is_tiled ? "tiled" : "linear", base, size, mip_address);
    }
    return nullptr;
  }
  auto &slot = m.loaded[hash];
  if (!slot) {
    const std::string path = std::format("{}/{:016x}.bin", m.dir, hash);
    auto buf = std::make_shared<std::vector<u8>>(size);
    FILE *f = std::fopen(path.c_str(), "rb");
    const size_t got = f ? std::fread(buf->data(), 1, size, f) : 0;
    if (f)
      std::fclose(f);
    if (got != size) {
      EOT_WARN("[texmods] {}: read {} of {} bytes; not used", path, got, size);
      m.sizes_by_hash.erase(hash);
      return nullptr;
    }
    slot = std::move(buf);
  }
  if (m.hits < 64) {
    ++m.hits;
    EOT_INFO("[texmods] replaced {:016x} ({}x{} fmt {}, {} bytes at {:#x})", hash, InfoWidth(info),
             InfoHeight(info), static_cast<u32>(info.format), size, base);
  }
  *bytes = size;
  return slot;
}

extern "C" bool wos_texture_slice_check();  // (fix83) wos_recomp.toml

// WoS port (fix83): XXH3 of slice k (of 32) of the base level and of the mip block
static u64 SliceHash(const GuestTexture &t, const TextureInfo &info, u32 k) {
  u64 h = 0x9E3779B97F4A7C15ull * (k + 1);
  auto part = [&](u32 address, u32 size) {
    size &= ~3u;
    if (!address || size < 128 || !mem::gpu_readable(address, size))
      return;
    const u8 *p = mem::gpu<const u8>(address);
    if (!p)
      return;
    const u32 a = static_cast<u32>((u64(size) * k / 32) & ~3ull);
    const u32 b = static_cast<u32>((u64(size) * (k + 1) / 32) & ~3ull);
    if (b > a)
      h ^= XXH3_64bits_withSeed(p + a, b - a, h);
  };
  part(t.baseAddress, std::max<u32>(info.memory.base_size, 4u));
  part(info.memory.mip_address, info.memory.mip_size);
  return h;
}

void UploadFromGuest(VideoState &s, GuestTexture &t, const TextureInfo &info) {
  if (ContentHashEligible(t)) {
    t.contentHash = ContentHash(t, info);
    t.contentCheckFrame = s.guest_frames;
    static const bool slices = wos_texture_slice_check();
    t.slicesValid = slices;
    if (slices)
      for (u32 k = 0; k < 32; ++k)
        t.sliceHash[k] = SliceHash(t, info, k);
  }
  t.storeSwapRB = false;
  TextureReleaseBorrower(s, t);
  FlushAliasDependents(s, t);
  t.aliasPending = false;
  t.contentSerial++;
  EOT_CPU_ZONE("texture upload");
  HostTexture &host = t.host;
  if (!host.texture || host.isDepth)
    return;
  PerfScope perf_scope(s.perf.upload_ms);
  s.perf.uploads++;
  if (t.uploaded) {
    s.perf.uploads_refresh++;
  } else if (UploadSeenBefore((u64(info.memory.base_address) << 32) ^ (u64(t.fetch[1]) << 16) ^ t.fetch[2] ^
                              (u64(info.memory.mip_address) << 7))) {
    s.perf.uploads_again++;
    f64 age_ms = 0;
    if (TakeTextureResidentAge(t.va, age_ms))
      s.perf.uploads_again_reloaded++;
  } else {
    s.perf.uploads_new++;
    f64 age_ms = 0;
    const u32 bucket = !TakeTextureResidentAge(t.va, age_ms) ? 4
                       : age_ms < 50.0                       ? 0
                       : age_ms < 250.0                      ? 1
                       : age_ms < 1000.0                     ? 2
                                                             : 3;
    s.perf.upload_lead[bucket]++;
    if (bucket == 4)
      NoteUnannouncedUpload(InfoWidth(info), InfoHeight(info), static_cast<u32>(info.format), info.is_tiled);
  }
  const TextureFormatMapping m = MapTextureFormat(info.format);
  if (m.convert) {
    if (!t.uploadFailed) {
      t.uploadFailed = true;
      EOT_WARN("[textures] {:#x}: guest format {} needs a texel conversion the uploader "
               "lacks yet; sampling whatever is there",
               t.va, static_cast<u32>(info.format));
    }
    return;
  }
  xe::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, t.fetch, sizeof(fetch));
  const FormatInfo *fi = info.format_info();
  const u32 bpb = fi->bytes_per_block();
  const u32 slices = host.arraySize > 1 ? host.arraySize : host.depth;
  const bool is_3d = info.dimension == xe::DataDimension::k3D;
  const bool has_base = info.memory.base_address != 0;
  const u32 width = InfoWidth(info), height = InfoHeight(info), depth = InfoDepth(info);
  // WoS port (fix35): GetGuestTextureLayout's mip arrays hold 14 levels (see GetGuestTexture)
  if (std::max({width, height, depth}) > 8192 || info.mip_max_level > 13)
    return;
  const tu::TextureGuestLayout layout = tu::GetGuestTextureLayout(
      info.dimension, fetch.pitch, width, height, is_3d ? depth : slices, info.is_tiled,
      info.format, info.has_packed_mips, has_base, info.mip_max_level);

  u32 mod_bytes = 0;
  std::shared_ptr<const std::vector<u8>> mod;
  if (!is_3d && slices == 1 && has_base && !t.resolveOwned)
    mod = TextureModFor(info.memory.base_address, info.memory.mip_address,
                        layout.base.level_data_extent_bytes, layout.mips_total_extent_bytes, info,
                        &mod_bytes);

  TransitionLocked(s, host, plume::RenderTextureLayout::COPY_DEST);
  for (u32 level = info.mip_min_level; level <= info.mip_max_level && level < host.mipLevels;
       ++level) {
    u32 address = 0;
    const tu::TextureGuestLayout::Level *lvl = nullptr;
    u32 x_blocks = 0, y_blocks = 0, z_blocks = 0;
    const bool packed = layout.packed_level != UINT32_MAX && level >= layout.packed_level;
    if (level == 0 && has_base) {
      address = info.memory.base_address;
      lvl = &layout.base;
    } else if (!info.memory.mip_address && layout.packed_level == 0 && has_base) {
      address = info.memory.base_address;
      lvl = &layout.base;
    } else {
      if (!info.memory.mip_address)
        continue;
      const u32 stored = packed ? layout.packed_level : level;
      address = info.memory.mip_address + layout.mip_offsets_bytes[stored];
      lvl = &layout.mips[stored];
    }
    if (packed) {
      tu::GetPackedMipOffset(width, height, is_3d ? depth : 1, info.format, level, x_blocks,
                             y_blocks, z_blocks);
    }
    u32 w = 0, h = 0;
    info.GetMipSize(level, &w, &h);
    if (!address || !w || !h || !lvl->row_pitch_bytes)
      continue;
    if (m.blockCompressed) {
      w = std::max(4u, (w + 3) & ~3u);
      h = std::max(4u, (h + 3) & ~3u);
    }
    if (w > std::max(1u, host.width >> level) || h > std::max(1u, host.height >> level)) {
      w = std::max(1u, host.width >> level);
      h = std::max(1u, host.height >> level);
    }
    if (!mem::gpu_readable(address, lvl->level_data_extent_bytes)) {  // WoS port: physical
      u32 n;
      if (DiagShouldLog(0x5EB0 ^ t.va, &n))
        EOT_WARN("[textures] {:#x}: level {} at {:#x} ({} bytes) is not mapped guest memory; "
                 "skipping the upload (x{})",
                 t.va, level, address, lvl->level_data_extent_bytes, n + 1);
      continue;
    }
    const u8 *src_base = mem::gpu<u8>(address);
    // WoS port (fix45): levels uploaded from memory that is still all zero (black)
    if (src_base && lvl->level_data_extent_bytes >= 64) {
      static u32 zero_logged = 0;
      if (zero_logged < 60) {
        const u32 words = lvl->level_data_extent_bytes / 4;
        bool all_zero = true;
        for (u32 i = 0; i < 32 && all_zero; ++i) {
          u32 v;
          std::memcpy(&v, src_base + 4 * ((u64(i) * (words - 1)) / 31), 4);
          all_zero = v == 0;
        }
        if (all_zero) {
          ++zero_logged;
          EOT_WARN("[zero-level] {:#x} {}x{} fmt {} level {} at {:#x} ({} bytes{}): uploading zeros",
                   t.va, width, height, static_cast<u32>(info.format), level, address,
                   lvl->level_data_extent_bytes, packed ? ", packed tail" : "");
        }
      }
    }
    if (mod && address >= info.memory.base_address &&
        u64(address - info.memory.base_address) + lvl->level_data_extent_bytes <= mod_bytes)
      src_base = mod->data() + (address - info.memory.base_address);  // fix39: texture mod
    if (!src_base)
      continue;
    const u32 bw = (w + fi->block_width - 1) / fi->block_width;
    const u32 bh = (h + fi->block_height - 1) / fi->block_height;
    const u32 guest_pitch_blocks = lvl->row_pitch_bytes / bpb;
    const u64 host_pitch = (u64(bw) * bpb + kTextureRowPitchAlignment - 1) /
                           kTextureRowPitchAlignment * kTextureRowPitchAlignment;
    const u64 host_slice_bytes = (host_pitch * bh + kTexturePlacementAlignment - 1) /
                                 kTexturePlacementAlignment * kTexturePlacementAlignment;
    const u64 guest_slice_bytes =
        is_3d ? u64(lvl->z_slice_stride_block_rows) * lvl->row_pitch_bytes
              : lvl->array_slice_stride_bytes;
    UploadAlloc staging;
    if (!UploadAllocate(host_slice_bytes * slices, kTexturePlacementAlignment, &staging))
      return;
    std::vector<u8> scratch;
    if (t.synthMips && level == 0 && slices == 1 && host.mipLevels > 1)
      scratch.assign(host_slice_bytes, 0);
    for (u32 slice = 0; slice < slices; ++slice) {
      u8 *dst = scratch.empty() ? staging.cpu + slice * host_slice_bytes : scratch.data();
      const u8 *src = src_base + slice * guest_slice_bytes +
                      (is_3d ? u64(z_blocks) * lvl->row_pitch_bytes * lvl->z_slice_stride_block_rows
                             : 0);
      if (info.is_tiled && is_3d) {
        u32 bpb_log2 = 0;
        while ((1u << bpb_log2) < bpb)
          ++bpb_log2;
        for (u32 y = 0; y < bh; ++y) {
          u8 *dst_row = dst + y * host_pitch;
          for (u32 x = 0; x < bw; ++x) {
            const i32 off = tu::GetTiledOffset3D(
                static_cast<i32>(x + x_blocks), static_cast<i32>(y + y_blocks),
                static_cast<i32>(slice + z_blocks), guest_pitch_blocks,
                lvl->z_slice_stride_block_rows, bpb_log2);
            tc::CopySwapBlock(info.endianness, dst_row + x * bpb, src_base + off, bpb);
          }
        }
      } else if (info.is_tiled) {
        tc::UntileInfo ui{};
        ui.offset_x = x_blocks;
        ui.offset_y = y_blocks;
        ui.width = bw;
        ui.height = bh;
        ui.input_pitch = guest_pitch_blocks;
        ui.output_pitch = static_cast<u32>(host_pitch / bpb);
        ui.input_format_info = fi;
        ui.output_format_info = fi;
        UntileLevel(dst, src, ui, bpb, info.endianness);
      } else {
        const u8 *row = src + u64(y_blocks) * lvl->row_pitch_bytes + u64(x_blocks) * bpb;
        for (u32 y = 0; y < bh; ++y) {
          tc::CopySwapBlock(info.endianness, dst + y * host_pitch, row + y * lvl->row_pitch_bytes,
                            u64(bw) * bpb);
        }
      }
      // WoS port (fix72): diagnostic - wos_debug_cube_faces paints every face of every
      // block-compressed cube map one solid colour (+X red, -X cyan, +Y green, -Y magenta,
      // +Z blue, -Z yellow): which way the facade glass looks up its reflection
      {
        static const bool faces = wos_debug_cube_faces();
        if (faces && info.dimension == xe::DataDimension::kCube && slices == 6 && slice < 6 &&
            m.blockCompressed && (bpb == 8 || bpb == 16)) {
          static const u16 kColor[6] = {0xF800, 0x07FF, 0x07E0, 0xF81F, 0x001F, 0xFFE0};
          u8 block[16] = {};
          u8 *c = block;
          if (bpb == 16) {  // BC2/BC3: alpha half opaque
            block[0] = 0xFF;
            block[1] = 0xFF;
            if (static_cast<u32>(info.format) == 19)  // DXT2/3: explicit alpha nibbles
              std::memset(block, 0xFF, 8);
            c = block + 8;
          }
          c[0] = static_cast<u8>(kColor[slice] & 0xFF);
          c[1] = static_cast<u8>(kColor[slice] >> 8);
          c[2] = c[0];
          c[3] = c[1];
          for (u32 y = 0; y < bh; ++y)
            for (u32 x = 0; x < bw; ++x)
              std::memcpy(dst + y * host_pitch + x * bpb, block, bpb);
        }
      }
      if (!scratch.empty())
        std::memcpy(staging.cpu, scratch.data(), host_slice_bytes);
      const u32 row_width_texels = static_cast<u32>(host_pitch / bpb) * fi->block_width;
      GpuTimingMark(s, s.command_list, kGpuCatUpload);
      s.command_list->copyTextureRegion(
          plume::RenderTextureCopyLocation::Subresource(host.texture.get(), level,
                                                        host.arraySize > 1 ? slice : 0),
          plume::RenderTextureCopyLocation::PlacedFootprint(
              staging.buffer, m.format, w, h, 1, row_width_texels,
              staging.offset + slice * host_slice_bytes),
          0, 0, is_3d ? slice : 0);
    }
    s.perf.upload_blocks += u64(bw) * bh * slices;
    if (!scratch.empty()) {
      PerfScope synth_scope(s.perf.upload_synth_ms);
      SynthesizeMipChain(s, host, m.format, scratch, w, h, host_pitch, bpb);
    }
  }
  t.uploaded = true;
  if (info.mip_min_level == 0 && info.mip_max_level + 1 >= host.mipLevels)
    host.needsClear = false;
}

}

constexpr f64 kTextureIdleSeconds = 30.0;
constexpr f64 kUploadedTextureIdleSeconds = 300.0;
constexpr size_t kTextureCap = 4096;

static bool ReadyToRetire(VideoState &s, const std::shared_ptr<GuestTexture> &slot) {
  if (slot.use_count() > 1)
    SurfacesReleaseMirror(s, *slot);
  if (slot.use_count() > 1 || (!slot->borrower && slot->aliasDependents.empty()))
    return true;
  BeginCommandList(s);
  return s.command_list_open;
}

// WoS port (fix34): textures replaced or evicted stay alive for a few frames. Raw pointers
// to them outlive the map's reference within a frame - the deferred SHADER_READ transitions
// (&texture->host), draw_bound_textures, the slot cache - and the fix32 run died in
// VulkanCommandList::barriers on one (fault.log RIP elf 0x36df7c2) while its 2,000+ texture
// uploads a frame recreated textures constantly.
namespace {
constexpr u32 kGraveFrames = 4;
std::vector<std::shared_ptr<GuestTexture>> g_texture_grave[kGraveFrames];
u32 g_grave_index = 0;
u64 g_grave_total = 0;
}
void BuryGuestTexture(const std::shared_ptr<GuestTexture> &t) {
  if (t) {
    g_texture_grave[g_grave_index].push_back(t);
    ++g_grave_total;
  }
}

void EvictStaleGuestTextures(VideoState &s) {
  EOT_CPU_ZONE("evict guest textures");
  // the oldest frame's graveyard goes now; this frame's evictions join the newest
  g_grave_index = (g_grave_index + 1) % kGraveFrames;
  g_texture_grave[g_grave_index].clear();
  bool evicted = false;
  {
    static std::vector<u32> released;
    released.clear();
    TakeReleasedTextures(released);
    for (u32 va : released) {
      const auto it = s.textures.find(va);
      if (it == s.textures.end() || !it->second || it->second->resolveOwned || !ReadyToRetire(s, it->second))
        continue;
      if (it->second.use_count() == 1) {
        TextureReleaseBorrower(s, *it->second);
        FlushAliasDependents(s, *it->second);
        ParkHostTexture(s, it->second->host);
      }
      BuryGuestTexture(it->second);
      infos().erase(it->first);
      s.textures.erase(it);
      s.perf.textures_evicted++;
      s.perf.textures_released++;
      evicted = true;
    }
  }
  u64 oldest_kept_frame = 0;
  if (s.textures.size() > kTextureCap) {
    std::vector<u64> frames;
    frames.reserve(s.textures.size());
    for (const auto &kv : s.textures)
      frames.push_back(kv.second ? kv.second->lastUseFrame : 0);
    std::nth_element(frames.begin(), frames.end() - kTextureCap, frames.end());
    oldest_kept_frame = frames[frames.size() - kTextureCap];
  }
  for (auto it = s.textures.begin(); it != s.textures.end();) {
    const std::shared_ptr<GuestTexture> &slot = it->second;
    if (!slot) {
      infos().erase(it->first);
      it = s.textures.erase(it);
      s.perf.textures_evicted++;
      evicted = true;
      continue;
    }
    const f64 idle_seconds = slot->resolveOwned ? kTextureIdleSeconds : kUploadedTextureIdleSeconds;
    if ((FrameAgeSeconds(s, slot->lastUseFrame) < idle_seconds && slot->lastUseFrame >= oldest_kept_frame) ||
        !ReadyToRetire(s, slot)) {
      ++it;
      continue;
    }
    if (slot.use_count() == 1) {
      TextureReleaseBorrower(s, *slot);
      FlushAliasDependents(s, *slot);
      ParkHostTexture(s, slot->host);
    }
    BuryGuestTexture(slot);
    infos().erase(it->first);
    it = s.textures.erase(it);
    s.perf.textures_evicted++;
    evicted = true;
  }
  if (evicted)
    s.texture_generation.fetch_add(1, std::memory_order_relaxed);
}

void PreloadAnnouncedTextures(VideoState &s, f64 budget_ms) {
  if (!s.command_list_open || budget_ms <= 0.0)
    return;
  const u64 start = PerfNow();
  const f64 ms_per_tick = PerfMsPerTick();
  u32 header = 0;
  while (static_cast<f64>(PerfNow() - start) * ms_per_tick < budget_ms && TakeAnnouncedHeader(header)) {
    if (s.textures.count(header))
      continue;
    GuestTexture *t = GetGuestTexture(s, header);
    if (!t || t->resolveOwned || !t->host.texture || t->host.isDepth)
      continue;
    const u32 before = s.perf.uploads;
    PrepareTextureForSampling(s, *t);
    s.perf.uploads_preloaded += s.perf.uploads - before;
  }
}

void NotifyResourceUnlocked(u32 resource_va) {
  auto &u = unlocks();
  {
    std::lock_guard lock(u.mutex);
    const u64 next = u.global.load(std::memory_order_relaxed) + 1;
    u.seq[resource_va] = next;
    u.global.store(next, std::memory_order_release);
  }
  auto &s = state();
  if (RenderThreadActive()) {
    RenderThreadUnlock(resource_va);
    return;
  }
  std::unique_lock lock(s.mutex, std::try_to_lock);
  if (lock.owns_lock()) {
    NotifyResourceUnlockedLocked(s, resource_va);
  } else {
    s.texture_generation.fetch_add(1, std::memory_order_relaxed);
  }
}

void NotifyResourceUnlockedLocked(VideoState &s, u32 resource_va) {
  const auto texture = s.textures.find(resource_va);
  if (texture != s.textures.end() && texture->second)
    texture->second->bindingGeneration++;
}

u64 ResourceUnlockSeq(u32 resource_va) {
  auto &u = unlocks();
  struct Cached {
    u32 va = 0;
    u64 seq = 0;
    u64 global = ~0ull;
  };
  static thread_local Cached cache[8];
  const u64 global = u.global.load(std::memory_order_acquire);
  Cached &c = cache[(resource_va >> 5) & 7];
  if (c.va == resource_va && c.global == global)
    return c.seq;
  u64 seq = 0;
  {
    std::lock_guard lock(u.mutex);
    auto it = u.seq.find(resource_va);
    seq = it == u.seq.end() ? 0 : it->second;
  }
  c = {resource_va, seq, global};
  return seq;
}

namespace {
bool LooksLikeTextureFetch(const u32 *fetch) {
  xe::xe_gpu_texture_fetch_t f;
  std::memcpy(&f, fetch, sizeof(f));
  const auto *fi = FormatInfo::Get(static_cast<u32>(f.format));
  return fi && fi->block_width && fi->bytes_per_block() && fi->block_height &&
         fi->bits_per_pixel &&
         static_cast<u32>(f.dimension) <= static_cast<u32>(xe::DataDimension::kCube);
}
}  // namespace

static bool InResolveMemory(const VideoState &s, const GuestTexture &t);

GuestTexture *GetGuestTexture(VideoState &s, u32 header_va, bool create_host_image,
                              const u32 *fallback_fetch) {
  if (!header_va)
    return nullptr;
  u32 fetch[6];
  const bool have = ReadFetch(header_va, fetch);
  if (have)
    NormalizeFetchAddresses(fetch);
  u32 fallback_norm[6];
  if (fallback_fetch) {
    std::memcpy(fallback_norm, fallback_fetch, sizeof(fallback_norm));
    NormalizeFetchAddresses(fallback_norm);
    fallback_fetch = fallback_norm;
  }
  // The draw's fetch constant is what the GPU samples: it wins whenever it is a texture's.
  // (A header on the stack is gone or stale by replay - and stale stack bytes can still
  // look like a texture: the sixth native run's 0x701ffa60 kept changing size.)
  if (fallback_fetch && LooksLikeTextureFetch(fallback_fetch) &&
      (!have || std::memcmp(fetch, fallback_fetch, sizeof(fetch)) != 0)) {
    std::memcpy(fetch, fallback_fetch, sizeof(fetch));
    u32 n;
    if (DiagShouldLog(0x5E20 ^ header_va, &n))
      EOT_DEBUG("[textures] {:#x}: header gone; using the draw's fetch constant", header_va);
  } else if (!have) {
    return nullptr;
  }
  auto &slot = s.textures[header_va];
  auto same_storage = [](const u32 *a, const u32 *b) {
    auto fmt = [](u32 d1) {
      const u32 f = d1 & 0x3F;
      return (d1 & ~0x3Fu) | (f == 50 ? 6u : f);
    };
    return (a[0] & ~0x7FFFCu) == (b[0] & ~0x7FFFCu) && fmt(a[1]) == fmt(b[1]) &&
           a[2] == b[2] && (a[5] & 0xFFFFFE00u) == (b[5] & 0xFFFFFE00u);
    // (WoS port: dword 5's low bits are sampler state - border colour, tri clamp,
    // aniso bias - not storage; only dimension, packed mips and the mip address count)
  };
  // WoS port: the texture whose storage this is, as a resolve destination: a real
  // (non-provisional) one first, then the most recently resolved.
  auto find_owner = [&](const GuestTexture *self) -> std::shared_ptr<GuestTexture> {
    std::shared_ptr<GuestTexture> best;
    for (auto &[other_va, other] : s.textures) {
      if (!other || other.get() == self || other_va == header_va ||
          !(other->resolveOwned || other->host.isDepth) || !same_storage(other->fetch, fetch))
        continue;
      if (!best || (best->resolveProvisional && !other->resolveProvisional) ||
          (best->resolveProvisional == other->resolveProvisional &&
           other->lastResolvedFrame > best->lastResolvedFrame))
        best = other;
    }
    return best;
  };
  if (slot && same_storage(slot->fetch, fetch)) {
    if (std::memcmp(slot->fetch, fetch, sizeof(fetch)) != 0)
      std::memcpy(slot->fetch, fetch, sizeof(fetch));
    // WoS port: a header first seen before anything was resolved into its storage got a
    // texture of its own (uploaded from guest memory, or taken over and cleared); once a
    // resolve destination with the same storage exists, this header reads that instead.
    const u64 epoch = g_owner_epoch.load(std::memory_order_relaxed);
    if ((!slot->resolveOwned || slot->resolveProvisional) && !slot->host.isDepth &&
        slot->ownerScanEpoch != epoch && InResolveMemory(s, *slot)) {
      slot->ownerScanEpoch = epoch;
      if (std::shared_ptr<GuestTexture> owner = find_owner(slot.get());
          owner && !owner->resolveProvisional) {
        u32 n;
        if (DiagShouldLog(0x5EC0 ^ header_va, &n) && n < 4)
          EOT_INFO("[textures] {:#x}: now reads resolve destination {:#x} ({}x{} fmt {} base {:#x})",
                   header_va, owner->va, owner->width, owner->height,
                   static_cast<u32>(owner->format), owner->baseAddress);
        if (slot.use_count() > 1)
          SurfacesReleaseMirror(s, *slot);
        if (slot.use_count() == 1) {
          TextureReleaseBorrower(s, *slot);
          FlushAliasDependents(s, *slot);
          ParkHostTexture(s, slot->host);
        }
        s.texture_generation.fetch_add(1, std::memory_order_relaxed);
        slot = owner;
        return slot.get();
      }
    }
    if (create_host_image && !slot->host.texture) {
      const auto info = infos().find(header_va);
      if (info != infos().end() && CreateHostImage(s, *slot, info->second))
        slot->bindingGeneration++;
    }
    return slot.get();
  }

  xe::xe_gpu_texture_fetch_t f;
  std::memcpy(&f, fetch, sizeof(f));
  TextureInfo info{};
  // WoS port: TextureInfo::Prepare divides by the format's block size and trusts the
  // dimension (and by its bytes per block, 0 for the 1-bit formats: an all-zero fetch
  // constant is k_1_REVERSE); a fetch constant that is not a texture's (a garbage or stale header -
  // the first native run died in Prepare with SIGFPE on the render thread) is refused
  // here instead.
  {
    // (type is not checked: Web of Shadows samples some textures through fetch constants
    // marked invalid, which the emulated GPU allows - gpu_allow_invalid_fetch_constants)
    const bool valid = LooksLikeTextureFetch(fetch);
    if (!valid) {
      u32 n;
      if (DiagShouldLog(0x5E10 ^ header_va, &n))
        EOT_WARN("[textures] {:#x}: not a texture fetch constant (type {} format {}); skipped",
                 header_va, static_cast<u32>(f.type), static_cast<u32>(f.format));
      // s.textures[header_va] above made an empty slot; every walk of s.textures
      // dereferences its entries (the second native run died in LogPerfLocked's)
      if (!slot)
        s.textures.erase(header_va);
      return nullptr;
    }
  }
  if (!TextureInfo::Prepare(f, &info)) {
    u32 n;
    if (DiagShouldLog(0x5E00 ^ header_va, &n))
      EOT_WARN("[textures] {:#x}: TextureInfo::Prepare rejected the fetch constant", header_va);
    if (!slot)
      s.textures.erase(header_va);  // WoS port: no empty slot left behind
    return nullptr;
  }
  // WoS port (fix35): a texture larger than 8192 on a side (or 1024 deep) is not one the
  // Xenos samples - a garbage fetch constant (a 1D one has a 24-bit width). rexglue's
  // GetGuestTextureLayout overruns its 14-entry mip arrays for those: the fix34 run died
  // memsetting past the render thread's stack top (backtrace: UploadFromGuest ->
  // GetGuestTextureLayout).
  if (InfoWidth(info) > 8192 || InfoHeight(info) > 8192 || InfoDepth(info) > 1024 ||
      info.mip_max_level > 13 || info.mip_min_level > info.mip_max_level) {
    u32 n;
    if (DiagShouldLog(0x5E30 ^ header_va, &n))
      EOT_WARN("[textures] {:#x}: refusing a {}x{}x{} dim {} fmt {} texture (mips {}..{}) - not "
               "a real texture's fetch constant [{:08x} {:08x} {:08x} {:08x} {:08x} {:08x}]",
               header_va, InfoWidth(info), InfoHeight(info), InfoDepth(info),
               static_cast<u32>(info.dimension), static_cast<u32>(info.format), info.mip_min_level,
               info.mip_max_level, fetch[0], fetch[1], fetch[2], fetch[3], fetch[4], fetch[5]);
    if (!slot)
      s.textures.erase(header_va);
    return nullptr;
  }
  if (slot) {
    u32 n;
    if (DiagShouldLog(0x5EA0 ^ header_va, &n))
      EOT_DEBUG("[textures] {:#x}: header changed ({}x{} fmt {} base {:#x} -> {}x{} fmt {} base "
               "{:#x}), recreating (x{})",
               header_va, slot->width, slot->height, static_cast<u32>(slot->format),
               slot->baseAddress, InfoWidth(info), InfoHeight(info),
               static_cast<u32>(info.format), info.memory.base_address, n + 1);
    if (slot.use_count() > 1)
      SurfacesReleaseMirror(s, *slot);
    if (slot.use_count() == 1) {
      TextureReleaseBorrower(s, *slot);
      FlushAliasDependents(s, *slot);
      ParkHostTexture(s, slot->host);
    }
    s.texture_generation.fetch_add(1, std::memory_order_relaxed);
    BuryGuestTexture(slot);  // WoS port (fix34): raw pointers to it may live this frame
    slot.reset();
    static u64 recreated = 0;
    if (++recreated % 5000 == 1)
      EOT_INFO("[textures] {} textures recreated over their storage so far ({} kept alive "
               "for the frame)", recreated, g_grave_total);
  }
  if (std::shared_ptr<GuestTexture> other = find_owner(nullptr)) {
    u32 n;
    if (DiagShouldLog(0x5EB0 ^ header_va, &n))
      EOT_DEBUG("[textures] {:#x}: aliases {:#x} ({}x{} fmt {} base {:#x})", header_va, other->va,
               other->width, other->height, static_cast<u32>(other->format), other->baseAddress);
    infos()[header_va] = info;
    slot = other;
    return slot.get();
  }
  auto t = std::make_shared<GuestTexture>();
  t->va = header_va;
  std::memcpy(t->fetch, fetch, sizeof(fetch));
  t->format = info.format;
  t->dimension = info.dimension;
  t->width = InfoWidth(info);
  t->height = InfoHeight(info);
  t->depth = InfoDepth(info);
  t->mipLevels = info.mip_levels();
  t->tiled = info.is_tiled;
  t->baseAddress = info.memory.base_address;
  t->mipAddress = info.memory.mip_address;
  t->gammaSigned = f.sign_x == xe::TextureSign::kGamma;
  infos()[header_va] = info;
  if (create_host_image)
    CreateHostImage(s, *t, info);
  EOT_DEBUG("[textures] {:#x}: {}x{}x{} mips {}..{} fmt {} {} {} base {:#x} mip {:#x} -> host fmt {}",
            header_va, InfoWidth(info), InfoHeight(info), InfoDepth(info), info.mip_min_level,
            info.mip_max_level, static_cast<u32>(info.format), info.is_tiled ? "tiled" : "linear",
            t->gammaSigned ? "gamma" : "linear-sign", t->baseAddress, t->mipAddress,
            static_cast<u32>(t->host.format));
  slot = std::move(t);
  return slot.get();
}

u32 SamplingSwizzle(const GuestTexture &t, u32 fetch_swizzle) {
  fetch_swizzle &= 0xFFF;
  if (!t.storeSwapRB)
    return fetch_swizzle;
  u32 out = 0;
  for (u32 c = 0; c < 4; ++c) {
    u32 sel = (fetch_swizzle >> (3 * c)) & 7;
    if (sel == 0)
      sel = 2;
    else if (sel == 2)
      sel = 0;
    out |= sel << (3 * c);
  }
  return out;
}

static std::pair<u32, u32> BaseRange(const GuestTexture &t) {
  const auto it = infos().find(t.va);
  if (it == infos().end() || !t.baseAddress)
    return {0, 0};
  return {t.baseAddress, t.baseAddress + std::max<u32>(it->second.memory.base_size, 1)};
}

static bool InResolveMemory(const VideoState &s, const GuestTexture &t) {
  const auto [start, end] = BaseRange(t);
  if (start >= end || s.resolve_ranges.empty())
    return false;
  auto it = s.resolve_ranges.upper_bound(start);
  if (it != s.resolve_ranges.begin() && std::prev(it)->second > start)
    return true;
  return it != s.resolve_ranges.end() && it->first < end;
}

void NoteResolveDestination(VideoState &s, const GuestTexture &t) {
  g_owner_epoch.fetch_add(1, std::memory_order_relaxed);  // WoS port: see GetGuestTexture
  auto [start, end] = BaseRange(t);
  if (start >= end)
    return;
  auto &ranges = s.resolve_ranges;
  auto it = ranges.upper_bound(start);
  if (it != ranges.begin() && std::prev(it)->second >= start)
    --it;
  while (it != ranges.end() && it->first <= end) {
    start = std::min(start, it->first);
    end = std::max(end, it->second);
    it = ranges.erase(it);
  }
  ranges.emplace(start, end);
}

static bool TakeOverAsResolveTarget(VideoState &s, GuestTexture &t) {
  if (t.host.isDepth || !s.command_list_open)
    return false;
  if (t.host.texture && !t.host.renderable) {
    ParkHostTexture(s, t.host);
    t.host = HostTexture{};
    t.bindingGeneration++;
  }
  if (!EnsureResolveMirror(s, t, false) || !t.host.renderable)
    return false;
  for (u32 m = 0; m < t.host.mipLevels; ++m) {
    plume::RenderFramebuffer *fb = GetMipFramebuffer(s, t, m);
    if (!fb)
      continue;
    TransitionLocked(s, t.host, plume::RenderTextureLayout::COLOR_WRITE);
    s.command_list->setFramebuffer(fb);
    s.command_list->clearColor(0, plume::RenderColor(0, 0, 0, 0), nullptr, 0);
  }
  s.bound_framebuffer = nullptr;
  s.bound_pipeline = nullptr;
  s.bound_draw_targets_valid = false;
  t.host.needsClear = false;
  t.resolveOwned = true;
  t.resolveProvisional = true;
  t.uploaded = true;
  s.perf.uploads_skipped++;
  return true;
}

extern "C" bool wos_fix_reused_resolve_memory();  // (fix82) wos_recomp.toml

// WoS port (fix82): a sparse look at a texture's base level - any non-zero word?
static bool GuestBaseHasData(const GuestTexture &t, const TextureInfo &info) {
  const u32 bytes = std::max<u32>(info.memory.base_size, 4u) & ~3u;
  if (!t.baseAddress || !mem::gpu_readable(t.baseAddress, bytes))
    return false;
  const u8 *p = mem::gpu<const u8>(t.baseAddress);
  if (!p)
    return false;
  const u32 words = bytes / 4;
  for (u32 i = 0; i < 64; ++i) {
    u32 v;
    std::memcpy(&v, p + 4 * static_cast<u32>((static_cast<u64>(i) * (words - 1)) / 63), 4);
    if (v)
      return true;
  }
  return false;
}

u32 PrepareTextureForSampling(VideoState &s, GuestTexture &t, u32 swizzle) {
  if (!t.host.texture)
    return kInvalidDescriptorIndex;
  swizzle = SamplingSwizzle(t, swizzle);
  const u64 seq = ResourceUnlockSeq(t.va);
  // WoS port (fix82): the black blocks on the roads. Memory the game once resolved into stays
  // "resolve memory" for good (s.resolve_ranges only grows); a texture the game later streams
  // into such memory was taken over as a provisional resolve target - a host image cleared to
  // black that is never uploaded and never content-checked. A provisional texture that no
  // resolve has written: when its guest bytes change after the takeover, it is the game's
  // texture after all - drop the resolve mirror and upload it from guest memory.
  static const bool fix_reuse = wos_fix_reused_resolve_memory();
  if (fix_reuse && t.resolveOwned && t.resolveProvisional && t.contentCheckFrame != s.guest_frames) {
    t.contentCheckFrame = s.guest_frames;
    if (auto it = infos().find(t.va); it != infos().end()) {
      const u64 h = ContentHash(t, it->second);
      if (h && h != t.provisionalHash && GuestBaseHasData(t, it->second)) {
        static u32 logged = 0;
        if (logged++ < 24)
          EOT_INFO("[textures] {:#x} ({}x{} fmt {} base {:#x}): provisional resolve target filled "
                   "by the game - uploading it from guest memory (fix82)",
                   t.va, t.width, t.height, static_cast<u32>(t.format), t.baseAddress);
        ParkHostTexture(s, t.host);
        t.host = HostTexture{};
        t.bindingGeneration++;
        t.resolveOwned = false;
        t.resolveProvisional = false;
        t.uploaded = false;
        t.provisionalHash = 0;
        if (!CreateHostImage(s, t, it->second))
          return kInvalidDescriptorIndex;
        UploadFromGuest(s, t, it->second);
        t.uploadedUnlockSeq = seq;
        t.lastUseFrame = s.guest_frames;
        TransitionLocked(s, t.host, plume::RenderTextureLayout::SHADER_READ);
        return BindTextureSRVSwizzledLocked(s, t.host, swizzle);
      }
    }
  }
  bool stale = !t.uploaded || seq != t.uploadedUnlockSeq;
  // WoS port: bytes changed under an uploaded texture without an Unlock (Bink video)
  if (!stale && t.contentHash && t.contentCheckFrame != s.guest_frames && ContentHashEligible(t)) {
    t.contentCheckFrame = s.guest_frames;
    const auto it = infos().find(t.va);
    // fix83: plus one full slice a frame - a part of the texture rewritten between the sparse
    // hash's sample points (the black blocks on the roads: lightmap regions) is found too
    bool slice_changed = false;
    if (it != infos().end() && t.slicesValid) {
      const u32 k = static_cast<u32>(s.guest_frames & 31);
      slice_changed = SliceHash(t, it->second, k) != t.sliceHash[k];
      if (slice_changed) {
        static u32 logged = 0;
        if (logged++ < 24)
          EOT_INFO("[textures] {:#x} ({}x{} fmt {} base {:#x}): slice {} rewritten under an unchanged "
                   "sparse hash - re-uploading (fix83)",
                   t.va, t.width, t.height, static_cast<u32>(t.format), t.baseAddress, k);
      }
    }
    if (it != infos().end() && (slice_changed || ContentHash(t, it->second) != t.contentHash)) {
      stale = true;
      if (t.contentChanges++ < 3) {
        u32 n;
        if (DiagShouldLog(0x5ED0 ^ t.va, &n) && n == 0)
          EOT_INFO("[textures] {:#x} ({}x{} fmt {} base {:#x}): bytes changed without an Unlock; re-uploading",
                   t.va, t.width, t.height, static_cast<u32>(t.format), t.baseAddress);
      }
    }
  }
  if (stale && t.resolveOwned) {
    u32 n;
    if (seq > t.uploadedUnlockSeq && DiagShouldLog(0x5E80 ^ t.va, &n))
      EOT_DEBUG("[textures] {:#x}: Unlock on a resolve-owned mirror ignored (seq {} -> {})", t.va,
               t.uploadedUnlockSeq, seq);
    t.uploadedUnlockSeq = seq;
  } else if (stale && !t.uploaded && InResolveMemory(s, t) &&
             !(fix_reuse && infos().count(t.va) && GuestBaseHasData(t, infos().find(t.va)->second)) &&
             TakeOverAsResolveTarget(s, t)) {
    // (fix82: not when the game has already put data there - then it is its texture)
    t.uploadedUnlockSeq = seq;
    if (fix_reuse)
      if (auto it = infos().find(t.va); it != infos().end())
        t.provisionalHash = ContentHash(t, it->second);
  } else if (stale) {
    auto it = infos().find(t.va);
    if (it != infos().end()) {
      UploadFromGuest(s, t, it->second);
      t.uploadedUnlockSeq = seq;
    }
  }
  t.lastUseFrame = s.guest_frames;
  TransitionLocked(s, t.host, plume::RenderTextureLayout::SHADER_READ);
  return BindTextureSRVSwizzledLocked(s, t.host, swizzle);
}

bool EnsureResolveMirror(VideoState &s, GuestTexture &t, bool depth_source, float scale,
                         plume::RenderFormat depth_format) {
  const float k = scale > 0.0f ? scale : RenderScaleFactor();
  const auto info_it = infos().find(t.va);
  if (info_it == infos().end())
    return false;
  const TextureInfo &info = info_it->second;

  const TextureFormatMapping mapping = MapTextureFormat(t.format);
  const bool texture_is_depth = t.format == xe::TextureFormat::k_24_8 ||
                                t.format == xe::TextureFormat::k_24_8_FLOAT;
  if (!t.host.texture &&
      (t.dimension != xe::DataDimension::k2DOrStacked ||
       (info.is_stacked && InfoDepth(info) > 1) || !mapping.supported ||
       mapping.blockCompressed || depth_source != texture_is_depth)) {
    if (!CreateHostImage(s, t, info))
      return false;
    t.bindingGeneration++;
  }
  if ((!t.resolveOwned || t.resolveProvisional) &&
      (!t.host.texture || (depth_source == t.host.isDepth && t.host.renderable))) {
    const plume::RenderFormat want = depth_source && depth_format != plume::RenderFormat::UNKNOWN
                                         ? depth_format
                                         : ResolveDestinationFormat(t.format, depth_source);
    if (want == plume::RenderFormat::UNKNOWN)
      return false;
    const u32 want_w = ScaleDimBy(InfoWidth(info), k), want_h = ScaleDimBy(InfoHeight(info), k);
    if (k != RenderScaleFactor() && (t.host.width != want_w || t.host.height != want_h))
      EOT_DEBUG("[textures] {:#x}: resolve mirror at x{:.2f}: {}x{}", t.va, k, want_w, want_h);
    if (!t.host.texture || t.host.format != want || t.host.width != want_w ||
        t.host.height != want_h) {
      EOT_DEBUG("[textures] {:#x}: switching mirror to resolve format {} {}x{} (was {} {}x{})",
                t.va, static_cast<u32>(want), want_w, want_h, static_cast<u32>(t.host.format),
                t.host.width, t.host.height);
      const bool replacing_host = t.host.texture != nullptr;
      TextureReleaseBorrower(s, t);
      FlushAliasDependents(s, t);
      ParkHostTexture(s, t.host);
      t.aliasPending = false;
      t.bindingGeneration++;
      s.mirror_generation++;
      const bool is_depth = depth_source;
      t.host = HostTexture{};
      t.synthMips = false;
      plume::RenderTextureDesc desc;
      desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
      desc.width = want_w;
      desc.height = want_h;
      desc.depth = 1;
      desc.arraySize = 1;
      u32 max_levels = 1;
      for (u32 w = desc.width, h = desc.height; (w > 1 || h > 1) && max_levels < 14;
           ++max_levels) {
        w = std::max(1u, w >> 1);
        h = std::max(1u, h >> 1);
      }
      desc.mipLevels = is_depth && replacing_host
                           ? 1u
                           : std::min(info.mip_max_level + 1u, max_levels);
      desc.format = ImageResourceFormat(want);
      desc.flags = is_depth ? plume::RenderTextureFlag::DEPTH_TARGET
                            : plume::RenderTextureFlag::RENDER_TARGET;
      const u32 gw = InfoWidth(info), gh = InfoHeight(info);
      bool frame_shape = gw >= 2048 && gh >= 2048;
      bool near_frame = false;
      for (u32 k = 0; k < 4; ++k) {
        const i32 fw = static_cast<i32>(kGuestRenderWidth >> k), fh = static_cast<i32>(kGuestRenderHeight >> k);
        frame_shape |= gw == static_cast<u32>(fw) && gh == static_cast<u32>(fh);
        near_frame |= std::abs(static_cast<i32>(gw) - fw) <= 2 && std::abs(static_cast<i32>(gh) - fh) <= 2;
      }
      desc.committed = frame_shape && u64(desc.width) * desc.height >= 256ull * 256ull;
      plume::RenderPool *pool = !frame_shape && !near_frame && gw > 256 && gh > 256
                                    ? s.transient_mirror_pool.get()
                                    : nullptr;
      t.host.format = want;
      t.host.viewDimension = plume::RenderTextureViewDimension::TEXTURE_2D;
      t.host.width = desc.width;
      t.host.height = desc.height;
      t.host.depth = 1;
      t.host.mipLevels = desc.mipLevels;
      t.host.arraySize = 1;
      t.host.isDepth = is_depth;
      CreateOrRecycleHostTexture(s, t.host, desc, "resolve-mirror", pool);
      t.host.renderable = t.host.texture != nullptr;
      t.uploaded = false;
      if (!t.host.texture)
        return false;
    }
  }
  if (!t.host.renderable) {
    u32 n;
    if (DiagShouldLog(0x5F00 ^ t.va, &n))
      EOT_WARN("[textures] {:#x}: resolve into a non-renderable mirror (fmt {})", t.va,
               static_cast<u32>(t.host.format));
    return false;
  }
  if (depth_source != t.host.isDepth) {
    u32 n;
    if (DiagShouldLog(0x5F80 ^ t.va, &n))
      EOT_WARN("[textures] {:#x}: {} resolve into a {} mirror", t.va,
               depth_source ? "depth" : "colour", t.host.isDepth ? "depth" : "colour");
    return false;
  }
  return true;
}

plume::RenderFramebuffer *GetMipFramebuffer(VideoState &s, GuestTexture &t, u32 mip) {
  HostTexture &host = t.host;
  if (!host.texture || mip >= host.mipLevels)
    return nullptr;
  // WoS port (fix34): only a renderable, single-layer 2D image can be a framebuffer here
  // (the view below is TEXTURE_2D, one layer); a compressed or cube/3D/array texture
  // reaching this path crashed in VulkanFramebuffer's constructor (fix29-fix32)
  if (!host.renderable || host.arraySize > 1 || host.depth > 1 ||
      (host.viewDimension != plume::RenderTextureViewDimension::TEXTURE_2D &&
       host.viewDimension != plume::RenderTextureViewDimension::UNKNOWN)) {
    u32 n;
    if (DiagShouldLog(0x5F40 ^ t.va, &n))
      EOT_WARN("[textures] {:#x}: no framebuffer for a {} texture ({}x{}x{} layers {} fmt {})", t.va,
               host.renderable ? "non-2D" : "non-renderable", host.width, host.height, host.depth,
               host.arraySize, static_cast<u32>(host.format));
    return nullptr;
  }
  if (host.mipFramebuffers.size() < host.mipLevels) {
    host.mipFramebuffers.resize(host.mipLevels);
    host.mipViews.resize(host.mipLevels);
  }
  if (host.mipFramebuffers[mip])
    return host.mipFramebuffers[mip].get();
  plume::RenderTextureViewDesc vd;
  vd.format = host.format;
  vd.dimension = plume::RenderTextureViewDimension::TEXTURE_2D;
  vd.mipSlice = mip;
  vd.mipLevels = 1;
  vd.arrayIndex = 0;
  vd.arraySize = 1;
  s.perf.host_views++;
  host.mipViews[mip] = host.texture->createTextureView(vd);
  if (!host.mipViews[mip])
    return nullptr;
  plume::RenderFramebufferDesc fd;
  const plume::RenderTexture *color[1] = {host.texture.get()};
  const plume::RenderTextureView *views[1] = {host.mipViews[mip].get()};
  if (host.isDepth) {
    fd.depthAttachment = host.texture.get();
    fd.depthAttachmentView = host.mipViews[mip].get();
  } else {
    fd.colorAttachments = color;
    fd.colorAttachmentViews = views;
    fd.colorAttachmentsCount = 1;
  }
  host.mipFramebuffers[mip] = s.device->createFramebuffer(fd);
  return host.mipFramebuffers[mip].get();
}

}

namespace eot::gpu {

namespace {

struct DescKey {
  plume::RenderTextureAddressMode u, v, w;
  plume::RenderFilter min, mag;
  plume::RenderMipmapMode mip;
  plume::RenderBorderColor border;
  u32 maxAniso;
  bool anisoEnabled;
  float lodBias;
  float minLod;
  float maxLod;

  bool operator==(const DescKey &o) const noexcept {
    return u == o.u && v == o.v && w == o.w && min == o.min && mag == o.mag && mip == o.mip &&
           border == o.border && maxAniso == o.maxAniso && anisoEnabled == o.anisoEnabled &&
           lodBias == o.lodBias && minLod == o.minLod && maxLod == o.maxLod;
  }
};

struct DescKeyHash {
  size_t operator()(const DescKey &k) const noexcept {
    u64 bits = 0;
    bits |= static_cast<u64>(k.u);
    bits |= static_cast<u64>(k.v) << 4;
    bits |= static_cast<u64>(k.w) << 8;
    bits |= static_cast<u64>(k.min) << 12;
    bits |= static_cast<u64>(k.mag) << 16;
    bits |= static_cast<u64>(k.mip) << 20;
    bits |= static_cast<u64>(k.border) << 24;
    bits |= static_cast<u64>(k.maxAniso) << 28;
    bits |= static_cast<u64>(k.anisoEnabled) << 36;
    u32 lb, mn, mx;
    std::memcpy(&lb, &k.lodBias, 4);
    std::memcpy(&mn, &k.minLod, 4);
    std::memcpy(&mx, &k.maxLod, 4);
    return std::hash<u64>{}(bits ^ (u64(lb) << 13) ^ (u64(mn) << 29) ^ (u64(mx) << 41));
  }
};

struct Cache {
  std::unordered_map<DescKey, std::pair<std::unique_ptr<plume::RenderSampler>, u32>, DescKeyHash>
      map;
};

Cache &cache() {
  static Cache c;
  return c;
}

plume::RenderTextureAddressMode ConvertClamp(xe::ClampMode mode) {
  using A = plume::RenderTextureAddressMode;
  switch (mode) {
  case xe::ClampMode::kRepeat:
    return A::WRAP;
  case xe::ClampMode::kMirroredRepeat:
    return A::MIRROR;
  case xe::ClampMode::kClampToEdge:
  case xe::ClampMode::kClampToHalfway:
    return A::CLAMP;
  case xe::ClampMode::kMirrorClampToEdge:
  case xe::ClampMode::kMirrorClampToHalfway:
  case xe::ClampMode::kMirrorClampToBorder:
    return A::MIRROR_ONCE;
  case xe::ClampMode::kClampToBorder:
    return A::BORDER;
  }
  return A::CLAMP;
}

}

plume::RenderSamplerDesc DecodeSamplerFromFetch(const u32 fc[6], bool mipmapped_upload,
                                                bool synthesized_mips) {
  xe::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, fc, sizeof(fetch));
  const bool base_map = fetch.mip_filter == xe::TextureFilter::kBaseMap && !synthesized_mips;

  plume::RenderSamplerDesc d;
  d.addressU = ConvertClamp(fetch.clamp_x);
  d.addressV = ConvertClamp(fetch.clamp_y);
  d.addressW = ConvertClamp(fetch.clamp_z);
  const bool mag_point = fetch.mag_filter == xe::TextureFilter::kPoint;
  const bool min_point = fetch.min_filter == xe::TextureFilter::kPoint;
  const bool mip_point = fetch.mip_filter == xe::TextureFilter::kPoint || base_map;
  d.magFilter = mag_point ? plume::RenderFilter::NEAREST : plume::RenderFilter::LINEAR;
  d.minFilter = min_point ? plume::RenderFilter::NEAREST : plume::RenderFilter::LINEAR;
  d.mipmapMode = mip_point ? plume::RenderMipmapMode::NEAREST : plume::RenderMipmapMode::LINEAR;
  if (base_map) {
    d.maxLOD = 0.0f;
  }
  u32 aniso = 0;
  switch (fetch.aniso_filter) {
  case xe::AnisoFilter::kMax_2_1:
    aniso = 2;
    break;
  case xe::AnisoFilter::kMax_4_1:
    aniso = 4;
    break;
  case xe::AnisoFilter::kMax_8_1:
    aniso = 8;
    break;
  case xe::AnisoFilter::kMax_16_1:
    aniso = 16;
    break;
  default:
    break;
  }
  const i32 forced = Settings::Anisotropy();
  if (forced == 1)
    aniso = 0;
  else if (forced > 1 && !mag_point && !min_point && mipmapped_upload)
    aniso = std::max<u32>(aniso, std::min<u32>(static_cast<u32>(forced), 16u));
  const bool aniso_on = aniso > 1 && !mag_point && !min_point;
  d.anisotropyEnabled = aniso_on;
  d.maxAnisotropy = aniso_on ? aniso : 1u;
  if (aniso_on && !base_map)
    d.mipmapMode = plume::RenderMipmapMode::LINEAR;
  d.borderColor = fetch.border_color == xe::BorderColor::k_ABGR_White
                      ? plume::RenderBorderColor::OPAQUE_WHITE
                      : plume::RenderBorderColor::TRANSPARENT_BLACK;
  d.mipLODBias = static_cast<float>(fetch.lod_bias) / 32.0f;
  d.minLOD = static_cast<float>(fetch.mip_min_level);
  if (d.maxLOD != 0.0f)
    d.maxLOD = synthesized_mips ? 15.0f : static_cast<float>(fetch.mip_max_level);
  // WoS port (fix70): the packed mip tail (levels <= 16 texels) of cube maps comes out scrambled
  // (dumped host levels of the facade's reflection cube bccf000: levels 0-2 right, 3-7 noise;
  // 2D tails are fine). Sampled at distance it lit/darkened whole facades in patches that moved
  // with the view - the glass "shimmer" (gone with the cube slots unbound, fix69). Until the
  // cube tail layout is fixed, cube maps stop at the last level above the tail.
  // fix71: the clamp did not stop the shimmer (fix70 run) and sharpens distant reflections -
  // off unless wos_cube_tail_clamp; wos_debug_cube_lod_bias adds whole mip levels to every
  // cube map's sampler (does the shimmer follow the reflection's level of detail?)
  static const int cube_bias = wos_debug_cube_lod_bias();
  static const bool tail_clamp = wos_cube_tail_clamp();
  if (fetch.dimension == xe::DataDimension::kCube && cube_bias != 0) {
    d.mipLODBias += static_cast<float>(cube_bias);
    static bool logged = false;
    if (!logged) {
      logged = true;
      EOT_INFO("[textures] cube maps sampled with {} extra mip levels of bias (fix71 diagnostic)",
               cube_bias);
    }
  }
  if (tail_clamp && fetch.dimension == xe::DataDimension::kCube && fetch.packed_mips &&
      d.maxLOD > 0.0f) {
    const u32 w = fetch.size_2d.width + 1, h = fetch.size_2d.height + 1;
    u32 log2_min = 0;
    while ((1u << log2_min) < std::min(w, h))
      ++log2_min;
    const u32 packed_level = log2_min <= 4 ? 0 : log2_min - 4;
    if (packed_level >= 1 && d.maxLOD > static_cast<float>(packed_level - 1)) {
      d.maxLOD = static_cast<float>(packed_level - 1);
      if (d.minLOD > d.maxLOD)
        d.minLOD = d.maxLOD;
      static bool logged = false;
      if (!logged) {
        logged = true;
        EOT_INFO("[textures] cube maps with packed mips sampled down to level {} at most "
                 "({}x{}; fix70)",
                 packed_level - 1, w, h);
      }
    }
  }
  return d;
}

u32 ResolveSamplerSlotLocked(const plume::RenderSamplerDesc &desc) {
  auto &s = state();
  if (!s.ready || !s.device || !s.sampler_descriptor_set)
    return kSamplerLinearClamp;

  const DescKey key{desc.addressU,    desc.addressV,      desc.addressW,
                    desc.minFilter,   desc.magFilter,     desc.mipmapMode,
                    desc.borderColor, desc.maxAnisotropy, desc.anisotropyEnabled,
                    desc.mipLODBias,  desc.minLOD,        desc.maxLOD};
  auto &c = cache();
  auto it = c.map.find(key);
  if (it != c.map.end())
    return it->second.second;

  u32 slot = kInvalidDescriptorIndex;
  for (size_t i = kReservedSamplerCount; i < s.sampler_descriptor_used.size(); ++i) {
    if (!s.sampler_descriptor_used[i]) {
      s.sampler_descriptor_used[i] = true;
      slot = static_cast<u32>(i);
      break;
    }
  }
  if (slot == kInvalidDescriptorIndex) {
    u32 n;
    if (DiagShouldLog(0x5A01, &n))
      EOT_WARN("[sampler-cache] bindless sampler heap full, falling back to slot 0");
    return kSamplerLinearClamp;
  }
  auto sampler = s.device->createSampler(desc);
  s.sampler_descriptor_set->setSampler(slot, sampler.get());
  c.map.emplace(key, std::make_pair(std::move(sampler), slot));
  static u32 logged = 0;
  if (logged++ < 48)
    EOT_DEBUG("[sampler] #{} slot {}: min {} mag {} mip {} aniso {}x{} lod bias {:g} lod {:g}..{:g} "
             "address {}/{}/{}",
             logged, slot, static_cast<int>(desc.minFilter), static_cast<int>(desc.magFilter),
             static_cast<int>(desc.mipmapMode), desc.anisotropyEnabled ? 1 : 0,
             desc.maxAnisotropy, desc.mipLODBias, desc.minLOD, desc.maxLOD,
             static_cast<int>(desc.addressU), static_cast<int>(desc.addressV),
             static_cast<int>(desc.addressW));
  return slot;
}

}
