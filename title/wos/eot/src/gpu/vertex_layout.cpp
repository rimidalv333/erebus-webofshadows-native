// gpu/vertex_layout.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "gpu/vertex_layout.h"

#include <format>
#include <memory>
#include <string>
#include <mutex>
#include <unordered_map>

#include <rex/memory/utils.h>
#include <xxhash.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/backend.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/format.h"
#include "gpu/shaders/guest_shaders.h"

namespace eot::gpu {

#include <cstring>

namespace {

struct LayoutCache {
  std::mutex mutex;
  std::unordered_map<u64, std::unique_ptr<InputLayout>> map;
};

LayoutCache &cache() {
  static LayoutCache c;
  return c;
}

struct DeclCacheEntry {
  u32 count = 0;
  u64 hash = 0;
  u8 raw[32 * sizeof(DeclElement)] = {};
};
std::unordered_map<u32, DeclCacheEntry> &decl_cache() {
  static std::unordered_map<u32, DeclCacheEntry> c;
  return c;
}

struct DecodedElement {
  u32 stream;
  u32 offset;
  u32 type;
  u32 usage;
  u32 usageIndex;
};

bool DecodeElements(const DeclElement *elements, u32 count, std::vector<DecodedElement> &out,
                    u64 *hash) {
  if (!elements || count == 0 || count > 32)
    return false;
  DeclElement canon[32];
  std::memcpy(canon, elements, count * sizeof(DeclElement));
  CanonicalizeDeclElements(canon, count);
  elements = canon;
  out.clear();
  out.reserve(count);
  for (u32 i = 0; i < count; ++i) {
    const DeclElement &e = elements[i];
    DecodedElement d;
    d.stream = e.stream;
    d.offset = e.offset;
    d.type = e.type;
    d.usage = e.usage;
    d.usageIndex = e.usageIndex;
    if (d.stream == 0xFF)
      break;
    out.push_back(d);
  }
  *hash = XXH3_64bits(elements, count * sizeof(DeclElement));
  return !out.empty();
}

bool ReadDeclaration(u32 decl_va, std::vector<DecodedElement> &out, u64 *hash) {
  const u32 count = mem::load<u32>(decl_va + obj::kDeclElementCount);
  if (count == 0 || count > 32)
    return false;
  return DecodeElements(mem::at<DeclElement>(decl_va + obj::kDeclElements), count, out, hash);
}

const InputLayout *FindLayout(u64 key) {
  auto &c = cache();
  std::lock_guard lock(c.mutex);
  auto it = c.map.find(key);
  return it == c.map.end() ? nullptr : it->second.get();
}

const InputLayout *BuildInputLayout(u64 vs_hash, const std::vector<VertexInput> &inputs,
                                    const std::vector<DecodedElement> &decl, u64 key,
                                    const DeclElement *raw, u32 count);

void SetSwapBit(InputLayout &l, u32 usage, u32 index) {
  const u32 bit = 1u << (index & 31);
  switch (static_cast<DeclUsage>(usage)) {
  case DeclUsage::TexCoord:
    l.swappedTexcoords |= bit;
    break;
  case DeclUsage::Normal:
    l.swappedNormals |= bit;
    break;
  case DeclUsage::Binormal:
    l.swappedBinormals |= bit;
    break;
  case DeclUsage::Tangent:
    l.swappedTangents |= bit;
    break;
  case DeclUsage::BlendWeight:
    l.swappedBlendWeights |= bit;
    break;
  case DeclUsage::Position:
    l.swappedPositions |= bit;
    break;
  default:
    break;
  }
}

// WoS port (fix55): layouts bound by vfetch get their own key, so one built earlier from a
// cache entry (PSO prediction, no fetch info) under the same decl/shader is never reused
// WoS port (fix102): the layout key also covers the shader's input list. Pipelines pre-built
// at boot take their inputs from the shader cache's vertex-layout table, draws from the guest
// shader itself; for 14 VS the table was wrong (overlapping records: 7855eaed listed POSITION0
// NORMAL0 POSITION0 NORMAL0 for POSITION0 NORMAL0 TANGENT0 TEXCOORD0), and under one shared
// key the boot-built layout was the one every draw used: the street's tangent and UVs read
// position/normal data - the black square that only came back with a warm pso folder.
extern "C" bool wos_layout_signature();
u64 InputsSignature(const std::vector<VertexInput> &inputs) {
  static const bool on = wos_layout_signature();  // fix107 switch
  if (!on)
    return 0;
  u64 h = 0x84222325CBF29CE4ull;
  for (const auto &in : inputs)
    h = (h ^ (u64(in.usage) | u64(in.usageIndex) << 8)) * 0x100000001B3ull;
  return h * 0xC2B2AE3D27D4EB4Full;
}

u64 FetchSalt(const std::vector<VertexInput> &inputs) {
  if (inputs.empty() || !inputs[0].hasFetch)
    return 0;
  u64 h = 0xF37C4B1D9A62E805ull;
  for (const auto &in : inputs) {
    const u64 v = u64(in.fetchFormat) | u64(in.fetchSigned) << 6 | u64(in.fetchInteger) << 7 |
                  u64(in.fetchStream) << 8 | u64(in.fetchOffset) << 12 | u64(in.fetchStride) << 36;
    h = (h ^ v) * 0x100000001B3ull;
    h ^= h >> 29;
  }
  return h;
}

}

static const InputLayout *GetInputLayoutSlow(VideoState &s, GuestShader &vs, u32 declaration_va,
                                             u32 raw_count, const DeclElement *raw_elements,
                                             bool raw_ok, bool *cacheable);

const InputLayout *GetInputLayout(VideoState &s, GuestShader &vs, u32 declaration_va) {
  if (!declaration_va || vs.isPixel)
    return nullptr;
  const u8 *decl_bytes = mem::at<u8>(declaration_va);
  const u32 raw_count =
      decl_bytes ? rex::memory::load_and_swap<u32>(decl_bytes + obj::kDeclElementCount) : 0;
  const auto *raw_elements =
      decl_bytes ? reinterpret_cast<const DeclElement *>(decl_bytes + obj::kDeclElements) : nullptr;
  const bool raw_ok = raw_count != 0 && raw_count <= 32 && raw_elements != nullptr;
  static_assert(sizeof(vs.lastDeclRaw) >= 32 * sizeof(DeclElement));
  if (raw_ok && vs.lastLayout && vs.lastDeclVa == declaration_va &&
      vs.lastDeclCount == raw_count &&
      std::memcmp(vs.lastDeclRaw, raw_elements, raw_count * sizeof(DeclElement)) == 0)
    return vs.lastLayout;
  bool cacheable = true;
  const InputLayout *layout =
      GetInputLayoutSlow(s, vs, declaration_va, raw_count, raw_elements, raw_ok, &cacheable);
  if (layout && raw_ok && cacheable) {
    vs.lastDeclVa = declaration_va;
    vs.lastDeclCount = raw_count;
    vs.lastLayout = layout;
    std::memcpy(vs.lastDeclRaw, raw_elements, raw_count * sizeof(DeclElement));
  }
  return layout;
}

static const InputLayout *GetInputLayoutSlow(VideoState &s, GuestShader &vs, u32 declaration_va,
                                             u32 raw_count, const DeclElement *raw_elements,
                                             bool raw_ok, bool *cacheable) {
  (void)s;
  // WoS port (fix56): bind by what the shader's vfetches read now (D3D patched them for this
  // declaration). Not patched yet -> name binding this time, and look again next draw.
  std::vector<VertexInput> live;
  // fix57: OFF. The live vfetches are re-patched by D3D for whichever declaration drew last
  // (the render thread reads them a frame later), so they described another draw: the menu
  // background vanished in fix56. D3D's own patch binds by usage name - what EoT always did.
  constexpr bool kBindByLiveFetch = false;
  const int fetch_state = kBindByLiveFetch ? LiveFetchInputs(vs, live) : 0;
  if (fetch_state < 0 && ++vs.unpatchedTries < 256)
    *cacheable = false;
  const std::vector<VertexInput> &use = fetch_state > 0 ? live : vs.inputs;
  const u64 salt = (fetch_state > 0 ? FetchSalt(live) : 0) ^ InputsSignature(use);
  if (raw_ok) {
    auto dit = decl_cache().find(declaration_va);
    if (dit != decl_cache().end() && dit->second.count == raw_count &&
        std::memcmp(dit->second.raw, raw_elements, raw_count * sizeof(DeclElement)) == 0) {
      if (const InputLayout *hit = FindLayout(dit->second.hash ^ (vs.hash * 0x9E3779B97F4A7C15ull) ^ salt))
        return hit;
    }
  }
  std::vector<DecodedElement> decl;
  u64 decl_hash = 0;
  if (!ReadDeclaration(declaration_va, decl, &decl_hash)) {
    u32 n;
    if (DiagShouldLog(0x5D00 ^ declaration_va, &n))
      EOT_WARN("[layout] declaration {:#x} unreadable", declaration_va);
    return nullptr;
  }
  if (raw_ok) {
    DeclCacheEntry &e = decl_cache()[declaration_va];
    e.count = raw_count;
    e.hash = decl_hash;
    std::memcpy(e.raw, raw_elements, raw_count * sizeof(DeclElement));
  }
  const u64 key = decl_hash ^ (vs.hash * 0x9E3779B97F4A7C15ull) ^ salt;
  if (const InputLayout *hit = FindLayout(key))
    return hit;
  return BuildInputLayout(vs.hash, use, decl, key, raw_elements, raw_count);
}

const InputLayout *GetInputLayoutFromRaw(u64 vs_hash, const std::vector<VertexInput> &inputs,
                                         const u8 *decl_raw, u32 decl_count) {
  std::vector<DecodedElement> decl;
  u64 decl_hash = 0;
  const auto *elements = reinterpret_cast<const DeclElement *>(decl_raw);
  if (!DecodeElements(elements, decl_count, decl, &decl_hash))
    return nullptr;
  const u64 key =
      decl_hash ^ (vs_hash * 0x9E3779B97F4A7C15ull) ^ FetchSalt(inputs) ^ InputsSignature(inputs);
  if (const InputLayout *hit = FindLayout(key))
    return hit;
  return BuildInputLayout(vs_hash, inputs, decl, key, elements, decl_count);
}

namespace {

const InputLayout *BuildInputLayout(u64 vs_hash, const std::vector<VertexInput> &inputs,
                                    const std::vector<DecodedElement> &decl, u64 key,
                                    const DeclElement *raw, u32 count) {
  auto layout = std::make_unique<InputLayout>();
  layout->key = key;
  u32 location = 0;
  bool ok = true;
  // WoS port (fix56): use the fetches only if each one reads a declared element (same stream,
  // offset and format) - a patch made for another declaration, or garbage, falls back to names
  bool use_fetch = !inputs.empty() && inputs[0].hasFetch;
  for (const VertexInput &in : inputs) {
    if (!use_fetch)
      break;
    bool found = false;
    for (const auto &d : decl)
      found |= d.stream == in.fetchStream && d.offset == in.fetchOffset &&
               (d.type & 0x3Fu) == in.fetchFormat;
    if (!found) {
      static u32 logged = 0;
      if (logged++ < 32)
        EOT_INFO("[layout] vfetch of vs {:016x} {}{} (s{} +{} fmt {}) matches no decl element: "
                 "name binding",
                 vs_hash, DeclUsageSemantic(in.usage), in.usageIndex, in.fetchStream,
                 in.fetchOffset, in.fetchFormat);
      use_fetch = false;
    }
  }
  layout->fetchBound = use_fetch;
  for (const VertexInput &in : inputs) {
    const DecodedElement *match = nullptr;
    for (const auto &d : decl) {
      if (d.usage == in.usage && d.usageIndex == in.usageIndex) {
        match = &d;
        break;
      }
    }
    const char *semantic = DeclUsageSemantic(in.usage);
    // WoS port (fix55): the GPU reads what the shader's vfetch says - its stream, byte offset,
    // format and sign - whatever the declaration names that slot. Web of Shadows' building
    // shaders fetch e.g. POSITION0 as half2 at +0 while the decl's POSITION0 is 16x4 at +8:
    // bound by name, every input read the wrong bytes and the buildings collapsed (black /
    // missing at medium range). Keep the decl's type word (it carries the D3DCOLOR swizzle)
    // when it describes the same format, else build one from the fetch.
    DecodedElement eff{};
    if (use_fetch) {
      const DecodedElement *same = nullptr;
      for (const auto &d : decl)
        if (d.stream == in.fetchStream && d.offset == in.fetchOffset &&
            (d.type & 0x3Fu) == in.fetchFormat) {
          same = &d;
          break;
        }
      u32 base = same ? same->type
                 : match && (match->type & 0x3Fu) == in.fetchFormat ? match->type
                                                                     : in.fetchFormat;
      base = (base & ~0x300u) | (in.fetchSigned ? 0x100u : 0u) | (in.fetchInteger ? 0x200u : 0u);
      eff.stream = in.fetchStream;
      eff.offset = in.fetchOffset;
      eff.type = base;
      eff.usage = in.usage;
      eff.usageIndex = in.usageIndex;
      if (!match || match->stream != eff.stream || match->offset != eff.offset ||
          match->type != eff.type) {
        static u32 logged = 0;
        if (logged < 200) {
          ++logged;
          EOT_INFO("[layout] vfetch remap vs {:016x} {}{}: decl {} -> fetch s{} +{} type {:#x} "
                   "(stride {})",
                   vs_hash, semantic, in.usageIndex,
                   match ? std::format("s{} +{} type {:#x}", match->stream, match->offset,
                                       match->type)
                         : std::string("none"),
                   eff.stream, eff.offset, eff.type, in.fetchStride);
        }
      }
      match = &eff;
    }
    if (!match) {
      layout->elements.emplace_back(semantic, in.usageIndex, location++,
                                    plume::RenderFormat::R32G32B32A32_FLOAT,
                                    kSyntheticVertexSlot, 0);
      layout->needsSyntheticSlot = true;
      continue;
    }
    DeclTypeInfo t = DecodeDeclType(match->type);
    if (t.format == plume::RenderFormat::UNKNOWN) {
      EOT_WARN("[layout] vs {:016x}: {}{} decl type {:#x} (xenos fmt {}) has no host format",
               vs_hash, semantic, in.usageIndex, match->type, t.xenosFormat);
      ok = false;
      break;
    }
    if (in.usage == static_cast<u32>(DeclUsage::BlendIndices)) {
      if (t.format == plume::RenderFormat::R8G8B8A8_UNORM ||
          t.format == plume::RenderFormat::B8G8R8A8_UNORM)
        t.format = plume::RenderFormat::R8G8B8A8_UINT;
    }
    if (in.usage == static_cast<u32>(DeclUsage::TexCoord) && t.integer && t.sixteenBit) {
      if (g_mvk) {
        t.format = t.byteSize == 4 ? plume::RenderFormat::R16G16_SINT
                                   : plume::RenderFormat::R16G16B16A16_SINT;
      } else {
        t.format = t.byteSize == 4 ? plume::RenderFormat::R16G16_UINT
                                   : plume::RenderFormat::R16G16B16A16_UINT;
        layout->sintTexcoords |= 1u << in.usageIndex;
      }
    }
    if (g_mvk && (t.packedDec3 || t.packed111110))
      t.format = plume::RenderFormat::R32_FLOAT;
    if (t.sixteenBit)
      SetSwapBit(*layout, in.usage, in.usageIndex);
    // WoS port (fix58): an 8/16-bit integer fetch (D3DDECLTYPE_UBYTE4, SHORT2/4...) feeding a
    // float shader input must arrive as the integer's value (the GPU converts: 7 -> 7.0). Bound
    // as *_UINT/_SINT the shader read the raw bits as a float (~0): the city's window/facade
    // LOD (vs 14a9ec0 & co., POSITION0 = 8888 integer) collapsed to a point - the black /
    // missing medium-distance buildings. Vulkan *_USCALED/_SSCALED do the conversion; the
    // flag rides in semanticIndex (bit 8), which only the patched plume attribute setup reads.
    // Not for BLENDINDICES (uint4 input), the sint-texcoord path or the packed formats
    // (those reinterpret the bits in the shader).
    u32 scaled = 0;
    if (t.integer && !t.packedDec3 && !t.packed111110 &&
        in.usage != static_cast<u32>(DeclUsage::BlendIndices) &&
        !(in.usage == static_cast<u32>(DeclUsage::TexCoord) && t.sixteenBit)) {
      switch (t.format) {
      case plume::RenderFormat::R8G8B8A8_UINT: case plume::RenderFormat::R8G8B8A8_SINT:
      case plume::RenderFormat::R8G8_UINT: case plume::RenderFormat::R8G8_SINT:
      case plume::RenderFormat::R8_UINT: case plume::RenderFormat::R8_SINT:
      case plume::RenderFormat::R16G16B16A16_UINT: case plume::RenderFormat::R16G16B16A16_SINT:
      case plume::RenderFormat::R16G16_UINT: case plume::RenderFormat::R16G16_SINT:
      case plume::RenderFormat::R16_UINT: case plume::RenderFormat::R16_SINT:
        scaled = 0x100;
        break;
      default:
        break;
      }
      if (scaled) {
        static u32 logged = 0;
        if (logged++ < 64)
          EOT_INFO("[layout] vs {:016x} {}{}: integer fetch (fmt {}) bound scaled (fix58)",
                   vs_hash, semantic, in.usageIndex, static_cast<u32>(t.format));
      }
    }
    if (t.packedDec3) {
      switch (static_cast<DeclUsage>(in.usage)) {
      case DeclUsage::Normal:
        layout->packedDec3 |= 1u << in.usageIndex;
        break;
      case DeclUsage::Tangent:
        layout->packedDec3 |= 1u << (8 + in.usageIndex);
        break;
      case DeclUsage::Binormal:
        layout->packedDec3 |= 1u << (16 + in.usageIndex);
        break;
      default:
        break;
      }
      layout->anyPacked111110 = true;
    }
    if (t.packed111110)
      layout->anyPacked111110 = true;
    if (match->stream >= 16) {
      EOT_WARN("[layout] vs {:016x}: stream {} out of range", vs_hash, match->stream);
      ok = false;
      break;
    }
    layout->elements.emplace_back(semantic, in.usageIndex | scaled, location++, t.format,
                                  match->stream, match->offset);
    layout->streamMask |= 1u << match->stream;
    if (in.usage == static_cast<u32>(DeclUsage::Position) && in.usageIndex == 1)
      layout->morphStreams |= 1u << match->stream;
    const u32 extent = match->offset + t.byteSize;
    if (extent > layout->streamExtent[match->stream])
      layout->streamExtent[match->stream] = extent;
  }
  if (!ok)
    return nullptr;
  {
    // WoS port (fix38): the first layouts built, so a wrong vertex format shows in the log
    static u32 logged = 0;
    if (logged < 96) {
      ++logged;
      std::string desc;
      for (const auto &e : layout->elements)
        desc += std::format(" [{}{} s{} +{} fmt {}]", e.semanticName ? e.semanticName : "?",
                            e.semanticIndex, e.slotIndex, e.alignedByteOffset,
                            static_cast<u32>(e.format));
      EOT_INFO("[layout] vs {:016x}:{} swapTex {:#x} sintTex {:#x} synthetic {}", vs_hash, desc,
               layout->swappedTexcoords, layout->sintTexcoords, layout->needsSyntheticSlot);
    }
  }
  if (layout->anyPacked111110)
    layout->spec |= kSpecR11G11B10Normal;
  if (layout->sintTexcoords)
    layout->spec |= kSpecSintTexcoord;
  if (raw && count) {
    layout->declRaw.assign(reinterpret_cast<const u8 *>(raw),
                           reinterpret_cast<const u8 *>(raw) + count * sizeof(DeclElement));
    CanonicalizeDeclElements(reinterpret_cast<DeclElement *>(layout->declRaw.data()), count);
  }
  auto &c = cache();
  std::lock_guard lock(c.mutex);
  auto it = c.map.find(key);
  if (it != c.map.end())
    return it->second.get();
  auto *result = layout.get();
  c.map.emplace(key, std::move(layout));
  return result;
}

}

void CanonicalizeDeclElements(DeclElement *elements, u32 count) {
  for (u32 i = 0; i < count; ++i)
    elements[i].pad = 0;
}

}
