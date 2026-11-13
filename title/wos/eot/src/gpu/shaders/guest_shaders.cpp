// gpu/shaders/guest_shaders.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "gpu/shaders/guest_shaders.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <xxhash.h>
#include <zstd.h>

#if defined(__PROSPERO__)
#include <rex/memory/utils.h>
#endif

#include "core/logging.h"
#include "core/profiling.h"

#include "core/memory_helpers.h"
#include "gpu/backend.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/shaders/shader_cache.h"

#if !defined(EOT_D3D12)
#include <smolv.h>
#endif

namespace eot::gpu {

namespace {

#if defined(__PROSPERO__)
// The PS5 build's shaders are recompiled from the containers the console
// dumps (the title's shader dump app reads the packs); a shader the game
// makes that is not in the cache, one in Default.xex for one, goes to
// /app0/shader-misses.bin the same way, once a run, its two parts as one
// container: what XenosRecomp scans for, and the bytes the hash covers
void RecordShaderMiss(u64 hash, const u8 *virtual_bytes, u32 virtual_size, const u8 *physical_bytes,
                      u32 physical_size) {
  static std::mutex mutex;
  static std::unordered_set<u64> recorded;
  static FILE *file = nullptr;
  std::lock_guard lock(mutex);
  if (!recorded.insert(hash).second)
    return;
  if (!file && !(file = std::fopen("/app0/shader-misses.bin", "ab")))
    return;
  static const u8 kPad[16] = {};
  std::fwrite(virtual_bytes, 1, virtual_size, file);
  if (physical_size)
    std::fwrite(physical_bytes, 1, physical_size, file);
  std::fwrite(kPad, 1, sizeof(kPad), file);
  std::fflush(file);
}
#endif

std::string GuestEntryPoint(const u8 *bytes, size_t size) {
#if defined(EOT_D3D12)
  (void)bytes;
  (void)size;
  return "main";
#else
  const auto *words = reinterpret_cast<const u32 *>(bytes);
  const size_t count = size / 4;
  constexpr u32 kOpEntryPoint = 15;
  for (size_t i = 5; i < count;) {
    const u32 opcode = words[i] & 0xFFFFu;
    const u32 length = words[i] >> 16;
    if (length == 0)
      break;
    if (opcode == kOpEntryPoint && i + 3 < count) {
      const char *name = reinterpret_cast<const char *>(&words[i + 3]);
      const size_t max = (std::min(i + length, count) - (i + 3)) * 4;
      return std::string(name, strnlen(name, max));
    }
    i += length;
  }
  return "shaderMain";
#endif
}

#if defined(EOT_MVK)
void StrictMathForMetal(std::vector<u8> &spirv) {
  constexpr u32 kOpExtension = 10;
  constexpr u32 kOpEntryPoint = 15;
  constexpr u32 kOpExecutionMode = 16;
  constexpr u32 kOpCapability = 17;
  constexpr u32 kOpDecorate = 71;
  constexpr u32 kDecorationNoContraction = 42;
  constexpr u32 kCapabilitySignedZeroInfNanPreserve = 4466;
  constexpr u32 kExecutionModeSignedZeroInfNanPreserve = 4461;
  constexpr u32 kSpirv14 = 0x00010400;
  static constexpr char kExtension[] = "SPV_KHR_float_controls";

  if (spirv.size() < 20 || spirv.size() % 4)
    return;
  std::vector<u32> in(spirv.size() / 4);
  std::memcpy(in.data(), spirv.data(), spirv.size());
  const auto is_no_contraction = [&](size_t i, u32 opcode, u32 length) {
    return opcode == kOpDecorate && length == 3 && in[i + 2] == kDecorationNoContraction;
  };

  bool no_contraction = false;
  bool has_capability = false;
  std::vector<u32> entries;
  for (size_t i = 5; i < in.size();) {
    const u32 opcode = in[i] & 0xFFFFu;
    const u32 length = in[i] >> 16;
    if (length == 0 || i + length > in.size())
      return;
    if (is_no_contraction(i, opcode, length))
      no_contraction = true;
    else if (opcode == kOpCapability && in[i + 1] == kCapabilitySignedZeroInfNanPreserve)
      has_capability = true;
    else if (opcode == kOpEntryPoint && length >= 3)
      entries.push_back(in[i + 2]);
    else if (opcode == kOpExecutionMode && length >= 3 && in[i + 2] == kExecutionModeSignedZeroInfNanPreserve)
      return;
    i += length;
  }
  if (!no_contraction || entries.empty())
    return;

  std::vector<u32> out(in.begin(), in.begin() + 5);
  out.reserve(in.size() + 16);
  bool past_capabilities = false;
  bool past_entries = false;
  bool seen_entry = false;
  for (size_t i = 5; i < in.size();) {
    const u32 opcode = in[i] & 0xFFFFu;
    const u32 length = in[i] >> 16;
    if (!past_capabilities && opcode != kOpCapability) {
      past_capabilities = true;
      if (!has_capability) {
        out.push_back((2u << 16) | kOpCapability);
        out.push_back(kCapabilitySignedZeroInfNanPreserve);
      }
      if (in[1] < kSpirv14) {
        u32 name[(sizeof(kExtension) + 3) / 4] = {};
        std::memcpy(name, kExtension, sizeof(kExtension));
        out.push_back((u32(1 + std::size(name)) << 16) | kOpExtension);
        out.insert(out.end(), std::begin(name), std::end(name));
      }
    }
    if (seen_entry && !past_entries && opcode != kOpEntryPoint) {
      past_entries = true;
      for (const u32 entry : entries) {
        out.push_back((4u << 16) | kOpExecutionMode);
        out.push_back(entry);
        out.push_back(kExecutionModeSignedZeroInfNanPreserve);
        out.push_back(32);
      }
    }
    if (opcode == kOpEntryPoint)
      seen_entry = true;
    if (!is_no_contraction(i, opcode, length))
      out.insert(out.end(), in.begin() + i, in.begin() + i + length);
    i += length;
  }
  spirv.resize(out.size() * 4);
  std::memcpy(spirv.data(), out.data(), spirv.size());
}
#endif

#if defined(__PROSPERO__)
// The decompressed cache, about 520 MiB of SPIR-V for the whole game, is more
// than the PS5 title's flexible memory (448 MiB, where malloc's memory comes
// from), so it lives in direct memory, as guest memory does
class CacheBlob {
 public:
  bool empty() const { return !size_; }
  const u8 *data() const { return data_; }
  u8 *data() { return data_; }
  size_t size() const { return size_; }
  void resize(size_t size) {
    clear();
    void *p = rex::memory::AllocFixed(nullptr, size, rex::memory::AllocationType::kReserveCommit,
                                      rex::memory::PageAccess::kReadWrite);
    if (!p) {
      EOT_ERROR("[shaders] no direct memory for the {} MB shader cache", size >> 20);
      return;
    }
    data_ = static_cast<u8 *>(p);
    size_ = size;
  }
  CacheBlob() = default;
  CacheBlob(const CacheBlob &) = delete;
  CacheBlob &operator=(const CacheBlob &) = delete;
  ~CacheBlob() { clear(); }
  void clear() {
    if (data_)
      rex::memory::DeallocFixed(data_, 0, rex::memory::DeallocationType::kRelease);
    data_ = nullptr;
    size_ = 0;
  }

 private:
  u8 *data_ = nullptr;
  size_t size_ = 0;
};

// The title can ship the compressed SPIR-V as a file beside eboot.bin
// (ps5/tools/split-shader-cache.py), so a build that only changes code is a
// small download; its cache source then defines this as 1. This weak 0 is for
// every other build: the console's tools refuse a title with a symbol left
// undefined, weak or not.
extern "C" __attribute__((weak)) const unsigned char reeot_spirv_cache_external = 0;

// Reads that file into direct memory; it must be the one made with this
// eboot.bin's entry table, which its size stands in for
bool ReadExternalSpirv(CacheBlob &out, size_t compressed_size) {
  constexpr const char *kPath = "/app0/shader-cache.zst";
  FILE *f = std::fopen(kPath, "rb");
  if (!f) {
    EOT_ERROR("[shaders] no {}: copy the shader-cache.zst that came with this eboot.bin next to it; "
              "no guest shader will run", kPath);
    return false;
  }
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  bool ok = size == static_cast<long>(compressed_size);
  if (!ok)
    EOT_ERROR("[shaders] {} is {} bytes where this eboot.bin's shaders are {}: copy the "
              "shader-cache.zst that came with it; no guest shader will run",
              kPath, size, compressed_size);
  if (ok) {
    out.resize(compressed_size);
    ok = out.size() == compressed_size &&
         std::fread(out.data(), 1, compressed_size, f) == compressed_size;
    if (!ok)
      EOT_ERROR("[shaders] could not read {}", kPath);
  }
  std::fclose(f);
  return ok;
}
#else
using CacheBlob = std::vector<u8>;
#endif

struct CacheState {
  CacheBlob blob;
  std::unordered_map<u64, const ShaderCacheEntry *> by_hash;
  std::unordered_map<u64, u64> canonical;
  std::mutex host_mutex;
  std::unordered_map<u64, std::unique_ptr<plume::RenderShader>> host;
  bool ready = false;
};

CacheState &cache() {
  static CacheState c;
  return c;
}

const u8 *EntryBytes(const ShaderCacheEntry &e, u32 *size,
                     VsVariant variant = VsVariant::Trimmed) {
  auto &c = cache();
  if (c.blob.empty()) {
    *size = 0;
    return nullptr;
  }
  u32 offset = 0;
#if defined(EOT_D3D12)
  offset = e.dxilOffset;
  *size = e.dxilSize;
  if (variant == VsVariant::Full && e.fullDxilSize) {
    offset = e.fullDxilOffset;
    *size = e.fullDxilSize;
  } else if (variant == VsVariant::PositionOnly && e.posDxilSize) {
    offset = e.posDxilOffset;
    *size = e.posDxilSize;
  } else if (variant == VsVariant::Velocity && e.velDxilSize) {
    offset = e.velDxilOffset;
    *size = e.velDxilSize;
  }
#else
  offset = e.spirvOffset;
  *size = e.spirvSize;
  if (variant == VsVariant::Full && e.fullSpirvSize) {
    offset = e.fullSpirvOffset;
    *size = e.fullSpirvSize;
  } else if (variant == VsVariant::PositionOnly && e.posSpirvSize) {
    offset = e.posSpirvOffset;
    *size = e.posSpirvSize;
  } else if (variant == VsVariant::Velocity && e.velSpirvSize) {
    offset = e.velSpirvOffset;
    *size = e.velSpirvSize;
  }
#endif
  return c.blob.data() + offset;
}

}

bool GuestShadersInit() {
  auto &c = cache();
  if (c.ready)
    return !c.blob.empty();
  c.ready = true;
#if defined(EOT_D3D12)
  const u8 *compressed = g_compressedDxilCache;
  const size_t compressed_size = g_dxilCacheCompressedSize;
  const size_t decompressed_size = g_dxilCacheDecompressedSize;
#else
  const u8 *compressed = g_compressedSpirvCache;
  const size_t compressed_size = g_spirvCacheCompressedSize;
  const size_t decompressed_size = g_spirvCacheDecompressedSize;
#endif
#if defined(__PROSPERO__)
  CacheBlob external;
  // Read as memory: the weak 0 above may not be the definition the link keeps
  if (*static_cast<const volatile unsigned char *>(&reeot_spirv_cache_external) && g_shaderCacheEntryCount &&
      decompressed_size) {
    if (!ReadExternalSpirv(external, compressed_size))
      return false;
    compressed = external.data();
  }
#endif
  if (g_shaderCacheEntryCount == 0 || decompressed_size == 0) {
    EOT_ERROR("[shaders] the embedded shader cache is empty: build "
              "reeot_shader_cache_regen and rebuild; no guest shader will run");
    return false;
  }
  c.blob.resize(decompressed_size);
  if (c.blob.size() != decompressed_size)
    return false;
  const size_t got =
      ZSTD_decompress(c.blob.data(), decompressed_size, compressed, compressed_size);
  if (ZSTD_isError(got) || got != decompressed_size) {
    EOT_ERROR("[shaders] shader cache decompression failed: {}",
              ZSTD_isError(got) ? ZSTD_getErrorName(got) : "short read");
    c.blob.clear();
    return false;
  }
  c.by_hash.reserve(g_shaderCacheEntryCount);
  std::unordered_map<u64, u64> by_bytes;
  std::string canon_rows;
  for (size_t i = 0; i < g_shaderCacheEntryCount; ++i) {
    const ShaderCacheEntry &e = g_shaderCacheEntries[i];
    c.by_hash.emplace(e.hash, &e);
    u32 size = 0;
    const u8 *bytes = EntryBytes(e, &size);
    const u64 bh = bytes && size ? XXH3_64bits(bytes, size) : e.hash;
    const u64 canon = by_bytes.emplace(bh, e.hash).first->second;
    c.canonical.emplace(e.hash, canon);
    canon_rows += std::format("{:016x},{:016x}\n", e.hash, canon);
  }
  EOT_DEBUG("[shaders] {} entries, {} unique DXIL: {} streamed duplicates share a host shader",
            g_shaderCacheEntryCount, by_bytes.size(), g_shaderCacheEntryCount - by_bytes.size());
  if (FILE *f = std::fopen("pso/shader_canon.csv", "wb")) {
    std::fputs("hash,canonical\n", f);
    std::fwrite(canon_rows.data(), 1, canon_rows.size(), f);
    std::fclose(f);
  }
  EOT_INFO("[shaders] cache ready: {} shaders, {} MB decompressed", g_shaderCacheEntryCount,
           decompressed_size / (1024 * 1024));
  return true;
}

u32 GuestShaderCacheCount() { return static_cast<u32>(g_shaderCacheEntryCount); }

GuestShader *FindGuestShader(VideoState &s, u32 object_va) {
  std::lock_guard lock(s.shaders_mutex);
  auto it = s.shaders.find(object_va);
  return it == s.shaders.end() ? nullptr : it->second.get();
}

void DrainShaderGraveyardLocked(VideoState &s) {
  std::vector<std::unique_ptr<GuestShader>> dead;
  {
    std::lock_guard lock(s.shaders_mutex);
    dead.swap(s.shader_graveyard);
  }
}

u32 FloatConstantRegisters(const u8 *bytes, u32 size, u32 table_off, bool is_pixel) {
  constexpr u32 kFull = 256;
  constexpr u32 kTableBytes = 4 + 28;
  constexpr u32 kInfoBytes = 20;      // D3DXSHADER_CONSTANTINFO
  if (!table_off || table_off + kTableBytes > size)
    return kFull;
  const u8 *table = bytes + table_off + 4;
  const u32 constants = rex::memory::load_and_swap<u32>(table + 12);
  const u32 info_off = rex::memory::load_and_swap<u32>(table + 16);
  if (!constants || constants > 1024)
    return kFull;
  const u64 info_end = u64(table_off) + 4 + info_off + u64(constants) * kInfoBytes;
  if (info_end > size)
    return kFull;
  u32 end = 0;
  for (u32 i = 0; i < constants; ++i) {
    const u8 *ci = table + info_off + i * kInfoBytes;
    const u32 set = rex::memory::load_and_swap<u16>(ci + 4);
    const u32 index = rex::memory::load_and_swap<u16>(ci + 6);
    const u32 count = rex::memory::load_and_swap<u16>(ci + 8);
    if (set != 2) // D3DXRS_FLOAT4
      continue;
    // WoS port (fix59): a matrix or array (count > 1) covers [index, index + count) - it no
    // longer forces the whole 256-register file. Nearly every WoS shader declares a matrix,
    // so every draw copied and compared 4 KiB per stage: ~24 MB a city frame on the game's
    // draw thread, ~2.7 MB with the declared ranges.
    const u32 regs = std::max(count, 1u);
    if (index + regs > kFull)
      return kFull;
    end = std::max(end, index + regs);
  }
  (void)is_pixel;
  return std::max(end, 16u);
}

u32 TextureFetchMask(const u8 *physical, u32 physical_size, const ShaderRecord *shader) {
  constexpr u32 kAllSlots = 0xFFFFu;
  constexpr u32 kInstructionBytes = 12;
  if (!physical || !shader)
    return kAllSlots;
  const u32 code_offset = shader->physicalOffset;
  const u32 code_size = shader->size;
  if (code_size < kInstructionBytes || code_offset > physical_size ||
      code_size > physical_size - code_offset)
    return kAllSlots;

  const u8 *code = physical + code_offset;
  u32 control_end = code_size;
  u32 mask = 0;
  bool found_clause = false;
  for (u32 control_offset = 0; control_offset + kInstructionBytes <= control_end;
       control_offset += kInstructionBytes) {
    const u32 w0 = rex::memory::load_and_swap<u32>(code + control_offset);
    const u32 w1 = rex::memory::load_and_swap<u32>(code + control_offset + 4);
    const u32 w2 = rex::memory::load_and_swap<u32>(code + control_offset + 8);
    const u64 controls[2] = {
        u64(w0) | (u64(w1 & 0xFFFFu) << 32),
        u64(w1 >> 16) | (u64(w2) << 16),
    };
    for (u64 control : controls) {
      const u32 opcode = static_cast<u32>((control >> 44) & 0xFu);
      const bool exec = (opcode >= 1 && opcode <= 6) || opcode == 13 || opcode == 14;
      if (!exec)
        continue;
      found_clause = true;
      const u32 address = static_cast<u32>(control & 0xFFFu);
      const u32 count = static_cast<u32>((control >> 12) & 7u);
      u32 sequence = static_cast<u32>((control >> 16) & 0xFFFu);
      if (address && address * kInstructionBytes < control_end)
        control_end = address * kInstructionBytes;
      if (address > code_size / kInstructionBytes ||
          count > code_size / kInstructionBytes - address)
        return kAllSlots;
      for (u32 i = 0; i < count; ++i, sequence >>= 2) {
        if (!(sequence & 1u))
          continue;
        const u32 fetch = rex::memory::load_and_swap<u32>(
            code + (address + i) * kInstructionBytes);
        const u32 fetch_opcode = fetch & 0x1Fu;
        if (fetch_opcode != 1u && fetch_opcode != 19u)
          continue;
        const u32 slot = (fetch >> 20) & 0x1Fu;
        if (slot >= 16)
          return kAllSlots;
        mask |= 1u << slot;
      }
    }
  }
  return found_clause && control_end < code_size ? mask : kAllSlots;
}

// WoS port (fix55): the vertex fetch instructions of a shader, by instruction address. Each
// vfetch carries its own offset, stride, format and sign; a mini fetch takes the stride and
// fetch constant of the full fetch before it (XenosRecomp's order: control-flow order).
struct DecodedVfetch {
  u32 format, offsetDwords, strideDwords, fetchIndex;
  bool isSigned, isInteger;
};
static std::unordered_map<u32, DecodedVfetch> DecodeVertexFetches(const u8 *code, u32 code_size) {
  std::unordered_map<u32, DecodedVfetch> out;
  constexpr u32 kInstructionBytes = 12;
  if (!code || code_size < kInstructionBytes)
    return out;
  u32 control_end = code_size;
  u32 last_stride = 0, last_index = 0;
  for (u32 control_offset = 0; control_offset + kInstructionBytes <= control_end;
       control_offset += kInstructionBytes) {
    const u32 w0 = rex::memory::load_and_swap<u32>(code + control_offset);
    const u32 w1 = rex::memory::load_and_swap<u32>(code + control_offset + 4);
    const u32 w2 = rex::memory::load_and_swap<u32>(code + control_offset + 8);
    const u64 controls[2] = {
        u64(w0) | (u64(w1 & 0xFFFFu) << 32),
        u64(w1 >> 16) | (u64(w2) << 16),
    };
    for (u64 control : controls) {
      const u32 opcode = static_cast<u32>((control >> 44) & 0xFu);
      const bool exec = (opcode >= 1 && opcode <= 6) || opcode == 13 || opcode == 14;
      if (!exec)
        continue;
      const u32 address = static_cast<u32>(control & 0xFFFu);
      const u32 count = static_cast<u32>((control >> 12) & 7u);
      u32 sequence = static_cast<u32>((control >> 16) & 0xFFFu);
      if (address && address * kInstructionBytes < control_end)
        control_end = address * kInstructionBytes;
      if (address > code_size / kInstructionBytes ||
          count > code_size / kInstructionBytes - address)
        return {};
      for (u32 i = 0; i < count; ++i, sequence >>= 2) {
        if (!(sequence & 1u))
          continue;
        const u8 *p = code + (address + i) * kInstructionBytes;
        const u32 f0 = rex::memory::load_and_swap<u32>(p);
        const u32 f1 = rex::memory::load_and_swap<u32>(p + 4);
        const u32 f2 = rex::memory::load_and_swap<u32>(p + 8);
        if ((f0 & 0x1Fu) != 0u) // vertex fetch opcode
          continue;
        const bool mini = ((f1 >> 30) & 1u) != 0;
        DecodedVfetch v;
        v.format = (f1 >> 16) & 0x3Fu;
        v.isInteger = ((f1 >> 13) & 1u) != 0;
        v.isSigned = ((f1 >> 12) & 1u) != 0; // format_comp_all (Xenia: is_signed)
        v.offsetDwords = (f2 >> 8) & 0x7FFFFFu;
        if (!mini) {
          last_stride = f2 & 0xFFu;
          last_index = ((f0 >> 20) & 0x1Fu) * 3 + ((f0 >> 25) & 3u);
        }
        v.strideDwords = last_stride;
        v.fetchIndex = last_index;
        out.emplace(address + i, v);
      }
    }
  }
  return out;
}

GuestShader *RegisterGuestShader(VideoState &s, u32 object_va, bool is_pixel) {
  if (!object_va)
    return nullptr;
  const u32 container_va =
      object_va + (is_pixel ? obj::kPixelShaderContainer : obj::kVertexShaderContainer);
  const u32 physical_va = mem::load<u32>(
      object_va + (is_pixel ? obj::kPixelShaderPhysical : obj::kVertexShaderPhysical));
  auto *header = mem::at<ShaderContainerHeader>(container_va);
  if (!header) {
    EOT_WARN("[shaders] register {:#x}: container unreadable", object_va);
    return nullptr;
  }
  const u32 flags = header->flags;
  if ((flags & 0xFFFFFF00u) != 0x102A1100u) {
    u32 n;
    if (DiagShouldLog(0x5B00, &n))
      EOT_WARN("[shaders] register {:#x}: bad container magic {:#x}", object_va, flags);
    return nullptr;
  }
  const u32 virtual_size = header->virtualSize;
  const u32 physical_size = header->physicalSize;
  const u8 *virtual_bytes = mem::at<u8>(container_va);
  const u8 *physical_bytes = physical_va ? mem::at<u8>(physical_va) : nullptr;
  if (!virtual_bytes || (physical_size && !physical_bytes) ||
      virtual_size < sizeof(*header)) {
    EOT_WARN("[shaders] register {:#x}: parts unreadable (virt {} phys {} @ {:#x})",
             object_va, virtual_size, physical_size, physical_va);
    return nullptr;
  }

  // WoS port: most of Web of Shadows' vertex shaders (flags 0x102A1121) carry offsets of a
  // second shader record in the header's fields 0x1C and 0x20. XenosRecomp only takes a
  // container with both zero, so the cache was recompiled from containers with them
  // zeroed - and the key is hashed the same way here.
  u8 zeroed_header[sizeof(ShaderContainerHeader)];
  std::memcpy(zeroed_header, virtual_bytes, sizeof(zeroed_header));
  std::memset(zeroed_header + 0x1C, 0, 8);
  XXH3_state_t *xs = XXH3_createState();
  XXH3_64bits_reset(xs);
  XXH3_64bits_update(xs, zeroed_header, sizeof(zeroed_header));
  XXH3_64bits_update(xs, virtual_bytes + sizeof(zeroed_header), virtual_size - sizeof(zeroed_header));
  if (physical_size)
    XXH3_64bits_update(xs, physical_bytes, physical_size);
  const u64 hash = XXH3_64bits_digest(xs);
  XXH3_freeState(xs);

  {
    std::lock_guard lock(s.shaders_mutex);
    auto it = s.shaders.find(object_va);
    if (it != s.shaders.end() && it->second && it->second->hash == hash)
      return it->second.get();
  }
  auto sh = std::make_unique<GuestShader>();
  sh->va = object_va;
  sh->hash = hash;
  sh->isPixel = is_pixel;
  auto &c = cache();
  auto it = c.by_hash.find(hash);
  sh->entry = it == c.by_hash.end() ? nullptr : it->second;
  if (sh->entry)
    sh->usesFloatConstants = sh->entry->usesFloatConstants != 0;
#if defined(__PROSPERO__)
  else
    RecordShaderMiss(hash, virtual_bytes, virtual_size, physical_bytes, physical_size);
#endif
  sh->floatConstantRegs =
      FloatConstantRegisters(virtual_bytes, virtual_size, header->constantTableOffset, is_pixel);
  const u32 shader_off = header->shaderOffset;
  const ShaderRecord *shader_record =
      shader_off <= virtual_size && sizeof(ShaderRecord) <= virtual_size - shader_off
          ? reinterpret_cast<const ShaderRecord *>(virtual_bytes + shader_off)
          : nullptr;
  sh->textureFetchMask = TextureFetchMask(physical_bytes, physical_size, shader_record);

  if (!is_pixel) {
    auto *rec = reinterpret_cast<const VertexShaderRecord *>(shader_record);
    if (rec) {
      const u32 first = rec->field18;
      const u32 count = rec->vertexElementCount;
      if (count <= 32) {
        for (u32 i = 0; i < count; ++i) {
          const u32 v = rec->vertexElementsAndInterpolators[first + i];
          VertexInput in;
          in.usage = static_cast<u8>((v >> 12) & 0xF);
          in.usageIndex = static_cast<u8>((v >> 16) & 0xF);
          in.address = static_cast<u16>(v & 0xFFF);
          sh->inputs.push_back(in);
        }
        if (physical_va && shader_record && shader_record->physicalOffset <= physical_size &&
            shader_record->size <= physical_size - shader_record->physicalOffset) {
          sh->codeVa = physical_va + shader_record->physicalOffset;
          sh->codeSize = shader_record->size;
        }
      }
    }
    if (sh->inputs.empty() && sh->entry) {
      for (u32 i = 0; i < sh->entry->vertexLayoutCount; ++i) {
        const u32 w0 = g_shaderVertexLayouts[sh->entry->vertexLayoutOffset + i * 2];
        VertexInput in;
        in.usage = static_cast<u8>(w0 & 0xF);
        in.usageIndex = static_cast<u8>((w0 >> 4) & 0xF);
        sh->inputs.push_back(in);
      }
    }
  }

  EOT_DEBUG("[shaders] {} {:#x} hash {:016x} {} inputs={} spec={:#x} regs={} texmask={:#06x}",
            is_pixel ? "ps" : "vs", object_va, hash, sh->entry ? "hit" : "MISS",
            sh->inputs.size(), sh->entry ? sh->entry->specConstantsMask : 0,
            sh->floatConstantRegs, sh->textureFetchMask);
  std::lock_guard lock(s.shaders_mutex);
  auto &slot = s.shaders[object_va];
  if (slot && slot->hash == hash)
    return slot.get();
  if (slot)
    s.shader_graveyard.push_back(std::move(slot));
  slot = std::move(sh);
  s.shader_generation.fetch_add(1, std::memory_order_release);
  return slot.get();
}

u64 CanonicalShaderHash(u64 hash) {
  auto &c = cache();
  auto it = c.canonical.find(hash);
  return it == c.canonical.end() ? hash : it->second;
}

const ShaderCacheEntry *FindShaderCacheEntry(u64 hash) {
  auto &c = cache();
  auto it = c.by_hash.find(hash);
  return it == c.by_hash.end() ? nullptr : it->second;
}

int LiveFetchInputs(const GuestShader &vs, std::vector<VertexInput> &out) {
  out = vs.inputs;
  if (!vs.codeVa || out.empty())
    return 0;
  const u8 *code = mem::at<u8>(vs.codeVa);
  const auto fetches = DecodeVertexFetches(code, vs.codeSize);
  for (auto &in : out) {
    auto f = fetches.find(in.address);
    if (f == fetches.end() || f->second.fetchIndex > 95 || 95 - f->second.fetchIndex >= 16)
      return 0;
    if (f->second.format == 0 || f->second.strideDwords == 0)
      return -1; // not patched (yet)
    in.hasFetch = true;
    in.fetchFormat = static_cast<u8>(f->second.format);
    in.fetchSigned = f->second.isSigned;
    in.fetchInteger = f->second.isInteger;
    in.fetchStream = static_cast<u8>(95 - f->second.fetchIndex);
    in.fetchOffset = f->second.offsetDwords * 4;
    in.fetchStride = f->second.strideDwords * 4;
  }
  return 1;
}

void VertexInputsFromEntry(const ShaderCacheEntry &e, std::vector<VertexInput> &out) {
  out.clear();
  for (u32 i = 0; i < e.vertexLayoutCount; ++i) {
    const u32 w0 = g_shaderVertexLayouts[e.vertexLayoutOffset + i * 2];
    VertexInput in;
    in.usage = static_cast<u8>(w0 & 0xF);
    in.usageIndex = static_cast<u8>((w0 >> 4) & 0xF);
    out.push_back(in);
  }
}

bool EntryHasVelocity(const ShaderCacheEntry *e) {
  if (!e)
    return false;
#if defined(EOT_D3D12)
  return e->velDxilSize != 0;
#else
  return e->velSpirvSize != 0;
#endif
}

VsVariant VsVariantFor(const ShaderCacheEntry *vs, const ShaderCacheEntry *ps, bool null_ps,
                       bool velocity) {
  if (null_ps)
    return VsVariant::PositionOnly;
  if (!vs || !ps)
    return VsVariant::Trimmed;
  if (velocity && EntryHasVelocity(vs) && EntryHasVelocity(ps) &&
      !(ps->interpolantMask & ~vs->interpolantMask))
    return VsVariant::Velocity;
  return (ps->interpolantMask & ~vs->interpolantMask) ? VsVariant::Full : VsVariant::Trimmed;
}

plume::RenderShader *GetHostShaderByHash(VideoState &s, u64 hash, u32 spec_mask, bool is_pixel,
                                         bool worker, VsVariant variant) {
  auto &c = cache();
  const ShaderCacheEntry *entry = FindShaderCacheEntry(hash);
  if (!entry || !s.device)
    return nullptr;
  if (is_pixel && variant != VsVariant::Velocity)
    variant = VsVariant::Trimmed;
  const u32 effective = 0;
  (void)spec_mask;
  const u64 salt = (u64(effective) << 3) | (u64(variant) << 1) | 1u;
  const u64 key = hash ^ (salt * 0x9E3779B97F4A7C15ull) ^ (is_pixel ? 1u : 0u);
  {
    std::lock_guard lock(c.host_mutex);
    auto it = c.host.find(key);
    if (it != c.host.end())
      return it->second.get();
  }
  u32 size = 0;
  const u8 *bytes = EntryBytes(*entry, &size, variant);
#if !defined(EOT_D3D12)
  std::vector<u8> decoded;
  if (bytes && size) {
    decoded.resize(smolv::GetDecodedBufferSize(bytes, size));
    if (decoded.empty() || !smolv::Decode(bytes, size, decoded.data(), decoded.size())) {
      EOT_ERROR("[shaders] SMOL-V decode failed for {:016x} ({} bytes)", hash, size);
      decoded.clear();
    }
#if defined(EOT_MVK)
    StrictMathForMetal(decoded);
#endif
    bytes = decoded.data();
    size = static_cast<u32>(decoded.size());
  }
#endif
  std::unique_ptr<plume::RenderShader> host;
  if (bytes && size) {
    (void)worker;
    host = s.device->createShader(bytes, size, GuestEntryPoint(bytes, size).c_str(), kHostShaderFormat);
    if (host)
      host->setName(std::format("{:016x}", hash));
    if (!host)
      EOT_ERROR("[shaders] createShader failed for {:016x}", hash);
  }
  std::lock_guard lock(c.host_mutex);
  auto [it, inserted] = c.host.emplace(key, std::move(host));
  return it->second.get();
}

plume::RenderShader *ResolveHostShader(VideoState &s, GuestShader &shader, u32 spec_mask,
                                       VsVariant variant) {
  if (!shader.entry) {
    if (!shader.cacheMissLogged) {
      shader.cacheMissLogged = true;
      EOT_WARN("[shaders] {} {:#x} hash {:016x} is not in the shader cache",
               shader.isPixel ? "ps" : "vs", shader.va, shader.hash);
    }
    return nullptr;
  }
  if (shader.isPixel && variant != VsVariant::Velocity)
    variant = VsVariant::Trimmed;
  if (shader.lastHost && shader.lastSpecMask == spec_mask && static_cast<VsVariant>(shader.lastVariant) == variant)
    return shader.lastHost;
  plume::RenderShader *host =
      GetHostShaderByHash(s, shader.hash, spec_mask, shader.isPixel, false, variant);
  if (host) {
    shader.lastSpecMask = spec_mask;
    shader.lastVariant = static_cast<u8>(variant);
    shader.lastHost = host;
  }
  return host;
}

}
