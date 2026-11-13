// WebOfShadowsRecomp for the PS5 - native renderer P0: harvest the game's shaders WHOLE.
//
// The native renderer (a port of Edge of Time's) draws with shaders recompiled ahead of
// time by XenosRecomp from the game's shader containers. A container is a virtual part
// (header, constant and definition tables) followed by a physical part (the microcode
// and its literal constants). The game's packs are compressed, and once a shader is
// created the 360's D3D moves the physical part into its own GPU allocation - so a scan
// of memory or of the packs does not find whole containers (both earlier dumps had
// garbage where the code should be).
//
// The live shader OBJECT has both parts: the virtual part inline in the object and a
// pointer to the physical part. Edge of Time's renderer reassembles them the same way
// (its RegisterGuestShader) and keys its cache by XXH3(virtual || physical), which is
// byte-for-byte XenosRecomp's key for the contiguous container in the game's files.
//
// Object layout (Edge of Time's d3d.h; confirmed for WoS's vertex shaders by the
// draw-time commit sub_82ACC888 reading 0x368+4/+8 and +0x20):
//   pixel shader:  container inline at +0x28,  physical pointer at +0x18
//   vertex shader: container inline at +0x368, physical pointer at +0x20
//
// v2: the physical pointer is a GPU address, not a CPU one. D3D's commit (sub_82ACC888)
// turns it into the IM_LOAD address as (p & 0x1FFFFFFE) + (p >= 0xE0000000 ? 0x1000 : 0),
// and the GPU (and rexglue) read it from PHYSICAL memory. v1 read it as a CPU virtual
// address and its "microcode" was not microcode (it never matched rexglue's IM_LOADed
// code). v2 reads it exactly the way the GPU does and logs the raw pointer to confirm.
// Every harvested container is validated against the magic, its pixel/vertex flag and
// sane sizes, so a wrong layout shows up as rejections in the log, not as bad data.
//
// Hooks D3DDevice_SetPixelShader (sub_82AB41A8) and D3DDevice_SetVertexShader
// (sub_82AB4460), both (device = r3, shader = r4), identified by their stores to the
// device's shader slots 12692 and 12696. An object is hashed on first sight, before
// its first draw; D3D's bind-time microcode patcher (sub_82ACBC50) rewrites vertex
// fetch instructions at draw time, so later sets of the same object are re-hashed
// occasionally and any change is counted: that tells us whether the patch is in place.
//
// Output: /app0/wos-shader-harvest-v2.bin, in Edge of Time's shader-misses.bin format -
// each container (virtual || physical) followed by 16 zero bytes - which XenosRecomp's
// directory mode reads as is. Game-derived data: it stays on the console and in the
// build, never in the repository.
//
// Off unless wos_shader_harvest is set. Costs one hash-map lookup per shader set when
// on, and nothing when off.
//
// SPDX-License-Identifier: MIT
// Portions adapted from EdgeOfTimeRecomp (BSD-3-Clause, (c) 2026 goliath-tilers):
// src/gpu/shaders/guest_shaders.cpp RegisterGuestShader / RecordShaderMiss.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(wos_shader_harvest, false, "WoS",
                    "Native renderer P0: write every shader the game sets, whole, to "
                    "wos-shader-harvest.bin for XenosRecomp");

namespace wos {
namespace harvest {
namespace {

constexpr uint32_t kPsContainer = 0x28;
constexpr uint32_t kPsPhysical = 0x18;
constexpr uint32_t kVsContainer = 0x368;
constexpr uint32_t kVsPhysical = 0x20;
constexpr uint32_t kMaxPart = 256u << 10;  // a real shader part is a few KiB
constexpr const char *kOutPath = "/app0/wos-shader-harvest-v2.bin";

std::atomic<int> g_state{-1};  // -1 unknown, 0 off, 1 on
std::mutex g_mutex;
std::unordered_map<uint32_t, uint64_t> g_object_hash;  // object va -> hash at first sight
std::unordered_map<uint32_t, uint32_t> g_object_sets;  // object va -> sets seen
std::unordered_set<uint64_t> g_written;
FILE *g_file = nullptr;

struct Stats {
  uint64_t sets = 0, objects = 0, written_ps = 0, written_vs = 0, bytes = 0;
  uint64_t bad_magic = 0, wrong_kind = 0, bad_size = 0, unreadable = 0, field_nonzero = 0;
  uint64_t rehashed = 0, changed = 0;
} g_stats;

bool Enabled() {
  int s = g_state.load(std::memory_order_relaxed);
  if (s < 0) {
    s = REXCVAR_GET(wos_shader_harvest) ? 1 : 0;
    g_state.store(s, std::memory_order_relaxed);
    if (s) {
      REXLOG_INFO("[WoS-HARVEST] on: writing whole shader containers to {}", kOutPath);
    }
  }
  return s == 1;
}

// Mapped (committed) in a guest heap. Physical shader allocations sit in the 0xA0000000+
// physical views, which are heaps rexglue knows; the read-protection bit is not required.
bool Mapped(uint32_t va, uint32_t size) {
  if (!size) return true;
  if (va < 0x00010000u || uint64_t(va) + size > 0xFFFF0000ull) return false;
  auto *ks = rex::system::kernel_state();
  if (!ks || !ks->memory()) return false;
  auto *memory = ks->memory();
  for (uint32_t probe : {va, va + size - 1}) {
    auto *heap = memory->LookupHeap(probe);
    rex::memory::HeapAllocationInfo info{};
    if (!heap || !heap->QueryRegionInfo(probe, &info) ||
        (info.state & rex::memory::kMemoryAllocationCommit) == 0) {
      return false;
    }
  }
  return true;
}

uint32_t BE32(const uint8_t *p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// Reassemble one object's container. Returns false (and counts why) if anything about
// it does not look like a real shader of the expected kind.
// The GPU address D3D's commit puts in IM_LOAD for a physical pointer.
uint32_t GpuPhysical(uint32_t p) {
  return (p & 0x1FFFFFFEu) + ((((p >> 20) + 0x200u) & 0x1000u));
}

// rexglue's shader key: XXH3 of the microcode as the guest's big-endian bytes (checked
// against rexglue's dump names). Logged so a harvested container can be matched by name.
uint64_t UcodeHash(const std::vector<uint8_t> &c) {
  const uint32_t vsz = BE32(c.data() + 4), so = BE32(c.data() + 24);
  if (uint64_t(so) + 8 > vsz) return 0;
  const uint32_t po = BE32(c.data() + so), sz = BE32(c.data() + so + 4);
  if (uint64_t(vsz) + po + sz > c.size() || (sz & 3)) return 0;
  return XXH3_64bits(c.data() + vsz + po, sz);
}

bool Reassemble(uint8_t *base, uint32_t obj, bool pixel, std::vector<uint8_t> &out,
                uint32_t *pva_out = nullptr) {
  const uint32_t cva = obj + (pixel ? kPsContainer : kVsContainer);
  const uint32_t pptr_va = obj + (pixel ? kPsPhysical : kVsPhysical);
  if (!Mapped(cva, 36) || !Mapped(pptr_va, 4)) {
    ++g_stats.unreadable;
    return false;
  }
  const uint8_t *h = base + cva;
  const uint32_t flags = BE32(h);
  if ((flags & 0xFFFFFF00u) != 0x102A1100u) {
    ++g_stats.bad_magic;
    return false;
  }
  // XenosRecomp: (flags & 1) == 0 is a pixel shader.
  if (((flags & 1u) == 0) != pixel) {
    ++g_stats.wrong_kind;
    return false;
  }
  const uint32_t vsz = BE32(h + 4), psz = BE32(h + 8);
  if (vsz < 36 || vsz > kMaxPart || psz > kMaxPart) {
    ++g_stats.bad_size;
    return false;
  }
  // XenosRecomp's directory scan only accepts containers whose fields 0x1C and 0x20 are
  // zero; count any that are not, since those would be skipped offline.
  if (BE32(h + 0x1C) != 0 || BE32(h + 0x20) != 0) {
    ++g_stats.field_nonzero;
  }
  const uint32_t pva = BE32(base + pptr_va);
  const uint32_t phys = GpuPhysical(pva);
  if (!Mapped(cva, vsz) || (psz && (!pva || uint64_t(phys) + psz > 0x20000000ull))) {
    ++g_stats.unreadable;
    return false;
  }
  out.resize(size_t(vsz) + psz);
  std::memcpy(out.data(), base + cva, vsz);
  if (psz) {
    std::memcpy(out.data() + vsz,
                rex::system::kernel_state()->memory()->TranslatePhysical(phys), psz);
  }
  if (pva_out) *pva_out = pva;
  return true;
}

void Report() {
  const Stats &s = g_stats;
  REXLOG_INFO(
      "[WoS-HARVEST] sets {} objects {} | written PS {} VS {} ({} KiB) | rejected: magic {} "
      "kind {} size {} unreadable {} | f1C/f20 nonzero {} | rehashed {} CHANGED {}",
      s.sets, s.objects, s.written_ps, s.written_vs, s.bytes >> 10, s.bad_magic, s.wrong_kind,
      s.bad_size, s.unreadable, s.field_nonzero, s.rehashed, s.changed);
}

}  // namespace

// Append one container if its hash is new. Caller holds g_mutex.
void Write(const std::vector<uint8_t> &container, uint64_t hash, uint32_t obj, uint32_t pva,
           bool pixel, const char *why) {
  if (!g_written.insert(hash).second) return;
  if (!g_file && !(g_file = std::fopen(kOutPath, "ab"))) {
    REXLOG_INFO("[WoS-HARVEST] cannot open {} for writing", kOutPath);
    g_state.store(0, std::memory_order_relaxed);
    return;
  }
  static const uint8_t kPad[16] = {};
  std::fwrite(container.data(), 1, container.size(), g_file);
  std::fwrite(kPad, 1, sizeof(kPad), g_file);
  std::fflush(g_file);
  (pixel ? g_stats.written_ps : g_stats.written_vs)++;
  g_stats.bytes += container.size() + sizeof(kPad);
  const uint64_t n = g_stats.written_ps + g_stats.written_vs;
  if (n <= 12) {
    REXLOG_INFO("[WoS-HARVEST] {} {} {:016X} obj {:08X} phys-ptr {:08X} -> {:08X}: virtual {} + "
                "physical {} bytes, ucode {:016X} first dwords {:08X} {:08X} {:08X}",
                why, pixel ? "PS" : "VS", hash, obj, pva, GpuPhysical(pva),
                BE32(container.data() + 4), BE32(container.data() + 8), UcodeHash(container),
                container.size() >= BE32(container.data() + 4) + 12
                    ? BE32(container.data() + BE32(container.data() + 4)) : 0,
                container.size() >= BE32(container.data() + 4) + 12
                    ? BE32(container.data() + BE32(container.data() + 4) + 4) : 0,
                container.size() >= BE32(container.data() + 4) + 12
                    ? BE32(container.data() + BE32(container.data() + 4) + 8) : 0);
  }
  if (n <= 12 || (n % 50) == 0) Report();
}

void OnSetShader(uint8_t *base, uint32_t obj, bool pixel) {
  if (!obj || !Enabled()) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  ++g_stats.sets;
  uint32_t &sets = g_object_sets[obj];
  ++sets;
  auto it = g_object_hash.find(obj);
  if (it != g_object_hash.end()) {
    // Seen before. Re-hash on the 2nd set (after the first draw) and every 64th: a
    // changed hash means D3D patched the microcode in place or the object address was
    // reused for another shader. Every new version is written too, so nothing is lost.
    if (sets != 2 && (sets & 63u) != 0) return;
    std::vector<uint8_t> again;
    uint32_t pva = 0;
    if (Reassemble(base, obj, pixel, again, &pva)) {
      ++g_stats.rehashed;
      const uint64_t h = XXH3_64bits(again.data(), again.size());
      if (h != it->second) {
        if (++g_stats.changed <= 8) {
          REXLOG_INFO("[WoS-HARVEST] object {:08X} ({}) hash CHANGED {:016X} -> {:016X} "
                      "after {} sets",
                      obj, pixel ? "PS" : "VS", it->second, h, sets);
        }
        it->second = h;
        Write(again, h, obj, pva, pixel, "changed");
      }
    }
    return;
  }
  std::vector<uint8_t> container;
  uint32_t pva = 0;
  if (!Reassemble(base, obj, pixel, container, &pva)) {
    g_object_hash.emplace(obj, 0);  // do not retry a bad object every set
    return;
  }
  ++g_stats.objects;
  const uint64_t hash = XXH3_64bits(container.data(), container.size());
  g_object_hash.emplace(obj, hash);
  Write(container, hash, obj, pva, pixel, "new");
}

// For the frame capture (wos_ps5_capture.cpp): one object's container, the patched-in-
// place fields 0x1C/0x20 left as they are.
bool ReassembleShader(uint8_t *base, uint32_t obj, bool pixel, std::vector<uint8_t> &out) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return Reassemble(base, obj, pixel, out);
}

}  // namespace harvest
}  // namespace wos

// --- The hooks: strong overrides of the recompiler's weak guest symbols ----------------

extern "C" void __imp__sub_82AB41A8(PPCContext &ctx, uint8_t *base);  // SetPixelShader
extern "C" void __imp__sub_82AB4460(PPCContext &ctx, uint8_t *base);  // SetVertexShader

extern "C" void wos_eot_SetPixelShader(PPCContext &ctx, uint8_t *base);   // the native
extern "C" void wos_eot_SetVertexShader(PPCContext &ctx, uint8_t *base);  // renderer's
namespace wos::native_renderer {
bool Enabled();  // wos_ps5_native_renderer.cpp
}

// D3DDevice_SetPixelShader(device = r3, shader = r4)
extern "C" void sub_82AB41A8(PPCContext &ctx, uint8_t *base) {
  wos::harvest::OnSetShader(base, ctx.r4.u32, true);
  if (wos::native_renderer::Enabled()) {
    wos_eot_SetPixelShader(ctx, base);
  } else {
    __imp__sub_82AB41A8(ctx, base);
  }
}

// D3DDevice_SetVertexShader(device = r3, shader = r4)
extern "C" void sub_82AB4460(PPCContext &ctx, uint8_t *base) {
  wos::harvest::OnSetShader(base, ctx.r4.u32, false);
  if (wos::native_renderer::Enabled()) {
    wos_eot_SetVertexShader(ctx, base);
  } else {
    __imp__sub_82AB4460(ctx, base);
  }
}
