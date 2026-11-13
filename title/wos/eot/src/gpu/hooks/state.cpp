// gpu/hooks/state.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include <atomic>
#include <cstring>

#include <rex/hook.h>
#include <rex/memory/utils.h>
#include <cmath>
#include <cstdint>
#include <mutex>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/wos_tiling.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/draw.h"
#include "gpu/hooks/fast_guest.h"
#include "gpu/settings.h"
#include "gpu/trace.h"

using namespace eot;
using namespace eot::gpu;

namespace {

using namespace eot::gpu::fastguest;

// WoS port (fix105): Web of Shadows' D3D offsets, read off its recompiled setters
// (sub_82ABD328 / 82AB41A8 / 82AB4460 / 82AB8420 / 82AB8540). EoT: 12332, 12358, 12240, 11070.
constexpr u32 kDevSamplerMinMip = 11950;
constexpr u32 kDevSamplerMaxMip = 11976;
constexpr u32 kDevDeclStride = 11832;
constexpr u32 kDevShaderFlags = 10942;   // 0x2ABE: bit 7 cleared by SetVertexShader
constexpr u32 kDevPendingGroup0 = 0;
constexpr u32 kDevPendingGroup1 = 8;
constexpr u32 kDevPendingGroup2 = 16;
constexpr u32 kDevPendingGroup3 = 24;
constexpr u32 kDevPendingGroup4 = 32;
constexpr u32 kDevConstantArea = 1152;
constexpr u32 kTexFetch = 28;
constexpr u32 kVbAddress = 24, kVbSize = 28;
constexpr u32 kPsLiteralTable = 60;
constexpr u32 kVsRecord = 872;

// WoS port: Edge of Time's draw hooks skip its D3D's draw; Web of Shadows' draw always runs (its
// PM4 feeds the register mirror). fix105: the five fast setters below run with Web of Shadows'
// offsets (wos_fast_setters; eot_fast_setters_verify runs the original too and compares).
constexpr bool kWosOriginalSetters = true;  // the draw (DrawFlush) only
bool FastSettersOff() {
  static const bool off = !Settings::FastSetters();
  return off;
}

void ApplyLiteralTable(u8 *dev, const u8 *table, u32 pending_group) {
  St64(dev + pending_group, Ld64(dev + pending_group) & ~Ld64(table));
  if (Ld64(table + 8) != 0)
    St64(dev + kDevPendingGroup4, Ld64(dev + kDevPendingGroup4) | (1ull << 56));
  const u32 size = Ld32(table + 16);
  const u8 *p = table + 20;
  const u8 *end = p + size;
  while (p < end) {
    const u16 count = Ld16(p + 2);
    p += 4;
    if (count == 0)
      break;
    p += 4;
  }
  if (p >= end)
    return;
  while (p < end) {
    const u16 off = Ld16(p), count = Ld16(p + 2);
    p += 4;
    if (count == 0)
      break;
    std::memcpy(dev + kDevConstantArea + off, p, count * 4u);
    p += count * 4u;
  }
  while (p < end) {
    const u16 off = Ld16(p);
    u32 count = Ld16(p + 2);
    p += 4;
    if (count == 0)
      return;
    u8 *dst = dev + kDevConstantArea + off;
    do {
      const u32 mask = Ld32(p), value = Ld32(p + 4);
      St32(dst, (Ld32(dst) & mask) | value);
      dst += 4;
      p += 8;
      count = (count + 65536u - 2u) & 0xFFFFu;
    } while (count != 0);
  }
}

bool FastSetPixelShader(u8 *base, u32 device, u32 shader) {
  if (FastSettersOff())  // WoS port (fix105): wos_fast_setters
    return false;
  u8 *dev = Guest(base, device);
  const u32 old = Ld32(dev + dev::kPixelShader);
  if (NeedsRing(base, dev, old))
    return false;
  StampReplaced(base, dev, old);
  St32(dev + dev::kPixelShader, shader);
  St64(dev + kDevPendingGroup2, Ld64(dev + kDevPendingGroup2) | 0x120000ull);
  if (!shader)
    return true;
  const u8 *obj = Guest(base, shader);
  const u32 table = Ld32(obj + kPsLiteralTable);
  if (table)
    ApplyLiteralTable(dev, obj + 40 + table, kDevPendingGroup1);
  return true;
}

bool FastSetVertexShader(u8 *base, u32 device, u32 shader) {
  if (FastSettersOff())  // WoS port (fix105): wos_fast_setters
    return false;
  u8 *dev = Guest(base, device);
  const u32 old = Ld32(dev + dev::kVertexShader);
  if (NeedsRing(base, dev, old))
    return false;
  if (shader)
    St64(dev + kDevPendingGroup2, Ld64(dev + kDevPendingGroup2) | 0x80000ull);
  StampReplaced(base, dev, old);
  dev[kDevShaderFlags] &= 0x7F;
  St32(dev + dev::kVertexShader, shader);
  if (!shader || shader + kVsRecord == 0)
    return true;
  const u8 *rec = Guest(base, shader + kVsRecord);
  const u32 table = Ld32(rec + 20);
  if (table)
    ApplyLiteralTable(dev, rec + table, kDevPendingGroup0);
  return true;
}

bool FastSetTexture(u8 *base, u32 device, u32 sampler, u32 texture, u64 mask) {
  if (FastSettersOff())  // WoS port (fix105): wos_fast_setters
    return false;
  u8 *dev = Guest(base, device);
  const u32 old = Ld32(dev + dev::kTextureObject0 + 4 * sampler);
  if (NeedsRing(base, dev, old))
    return false;
  u8 *slot = dev + dev::kFetchConstants + 24 * sampler;
  if (texture) {
    const u8 *tex = Guest(base, texture) + kTexFetch;
    const u32 t0 = Ld32(tex), t1 = Ld32(tex + 4), t2 = Ld32(tex + 8), t3 = Ld32(tex + 12),
              t4 = Ld32(tex + 16), t5 = Ld32(tex + 20);
    const u32 d0 = Ld32(slot), d1 = Ld32(slot + 4), d3 = Ld32(slot + 12), d4 = Ld32(slot + 16),
              d5 = Ld32(slot + 20);
    const u32 base_addr = ((((t1 >> 20) & 0xFFFu) + 512u) & 0x1000u) + (t1 & 0x1FFFFFFFu);
    const u32 mip_addr = ((((t5 >> 20) & 0xFFFu) + 512u) & 0x1000u) + (t5 & 0x1FFFFE00u);
    u32 n4 = (d4 & ~0x3FCu) | (t4 & 0x3FCu);
    u32 lo = dev[kDevSamplerMinMip + sampler];
    if (const u32 tlo = (t4 >> 2) & 0xFu; tlo > lo)
      lo = tlo;
    n4 = (n4 & ~0x3Cu) | ((lo << 2) & 0x3Cu);
    u32 hi = dev[kDevSamplerMaxMip + sampler];
    if (const u32 thi = (t4 >> 6) & 0xFu; thi < hi)
      hi = thi;
    n4 = (n4 & ~0x3C0u) | ((hi << 6) & 0x3C0u);
    St32(slot, (d0 & 0x3FFC00u) | (t0 & ~0x3FFC00u));
    St32(slot + 4, (d1 & 0x800u) | (base_addr & ~0x800u));
    St32(slot + 8, t2);
    St32(slot + 12, (d3 & 0x7FF80000u) | (t3 & ~0x7FF80000u));
    St32(slot + 16, n4);
    St32(slot + 20, (d5 & 0x1FFu) | (mip_addr & ~0x1FFu));
    St64(dev + kDevPendingGroup3, Ld64(dev + kDevPendingGroup3) | mask);
  }
  // WoS port (fix105): Web of Shadows' SetTexture(NULL) leaves the fetch slot as it is (EoT's
  // clears its type bits)
  St32(dev + dev::kTextureObject0 + 4 * sampler, texture);
  StampReplaced(base, dev, old);
  return true;
}

bool FastSetStreamSource(u8 *base, u32 device, u32 stream, u32 vb, u32 offset, u32 stride,
                         u64 mask) {
  if (FastSettersOff())  // WoS port (fix105): wos_fast_setters
    return false;
  u8 *dev = Guest(base, device);
  const u32 old = Ld32(dev + dev::kStreamObject0 + 4 * stream);
  if (NeedsRing(base, dev, old))
    return false;
  if (vb) {
    const u8 *obj = Guest(base, vb);
    const u32 addr = Ld32(obj + kVbAddress) + offset;
    const u32 size = Ld32(obj + kVbSize);
    u8 *fetch = dev + dev::StreamFetchSlotOffset(stream);
    St32(fetch, ((((addr >> 20) & 0xFFFu) + 512u) & 0x1000u) + (addr & 0x1FFFFFFFu));
    St32(fetch + 4, size - offset);
    St64(dev + kDevPendingGroup3, Ld64(dev + kDevPendingGroup3) | mask);
  }
  StampReplaced(base, dev, old);
  St32(dev + dev::kStreamObject0 + 4 * stream, vb);
  const u32 stride4 = stride >> 2;
  dev[dev::kStreamStride0 + stream] = static_cast<u8>(stride4);
  const u32 s = stride4 & 0x3FFFFFFFu;
  if (s != 0 && s != dev[kDevDeclStride + stream])
    St64(dev + kDevPendingGroup2, Ld64(dev + kDevPendingGroup2) | 0x80000u);
  return true;
}

bool FastSetIndices(u8 *base, u32 device, u32 ib) {
  if (FastSettersOff())  // WoS port (fix105): wos_fast_setters
    return false;
  u8 *dev = Guest(base, device);
  const u32 old = Ld32(dev + dev::kIndexBuffer);
  if (NeedsRing(base, dev, old))
    return false;
  StampReplaced(base, dev, old);
  St32(dev + dev::kIndexBuffer, ib);
  return true;
}

struct VerifyRegions {
  struct Region {
    u8 *p;
    u32 n;
  };
  Region regions[7];
  u32 count = 0;
  u8 before[128];
  u8 after_fast[128];
  u32 total = 0;
  void add(u8 *p, u32 n) {
    regions[count++] = {p, n};
    total += n;
  }
  void snapshot(u8 *out) const {
    u32 off = 0;
    for (u32 i = 0; i < count; ++i) {
      std::memcpy(out + off, regions[i].p, regions[i].n);
      off += regions[i].n;
    }
  }
  void restore(const u8 *in) const {
    u32 off = 0;
    for (u32 i = 0; i < count; ++i) {
      std::memcpy(regions[i].p, in + off, regions[i].n);
      off += regions[i].n;
    }
  }
};

void ReportVerify(const char *what, const VerifyRegions &v, const u8 *after_orig) {
  static u32 reports = 0;
  if (reports >= 40)
    return;
  u32 off = 0;
  for (u32 i = 0; i < v.count; ++i) {
    if (std::memcmp(v.after_fast + off, after_orig + off, v.regions[i].n) != 0) {
      std::string fast, orig;
      for (u32 b = 0; b < v.regions[i].n; ++b) {
        fast += std::format("{:02x}", v.after_fast[off + b]);
        orig += std::format("{:02x}", after_orig[off + b]);
      }
      EOT_WARN("[fast-setters] {} region {} differs: hook {} xdk {}", what, i, fast, orig);
      reports++;
    }
    off += v.regions[i].n;
  }
}

}

REX_EXTERN(__imp__D3DDevice_SetRenderTarget);
REX_EXTERN(__imp__D3DDevice_SetDepthStencilSurface);
REX_EXTERN(__imp__D3DDevice_SetViewport);
REX_EXTERN(__imp__D3DDevice_SetTexture);
REX_EXTERN(__imp__D3DDevice_SetVertexShader);
REX_EXTERN(__imp__D3DDevice_SetPixelShader);
REX_EXTERN(__imp__D3DDevice_SetStreamSource);
REX_EXTERN(__imp__D3DDevice_SetIndices);

extern "C" REX_FUNC(D3DDevice_SetRenderTarget) {
  FlushPendingUpDraw();
  const u32 index = ctx.r4.u32, surface = ctx.r5.u32;
  {
    PerfScopeSampled guest_scope(state().perf.guest_d3d_ms, state().perf.guest_d3d_calls);
    __imp__D3DDevice_SetRenderTarget(ctx, base);
  }
  if (trace::Enabled()) {
    const u32 info = surface ? mem::load<u32>(surface + 0x1C) : 0;
    const u32 size = surface ? mem::load<u32>(surface + 0x24) : 0;
    EOT_TRACE_CALL("SetRenderTarget {} surf={:#x} {}x{} fmt={} tile={}", index, surface,
                   surface ? (size >> 18) + 1 : 0, surface ? ((size >> 3) & 0x7FFF) + 1 : 0,
                   (info >> 16) & 0xF, info & 0xFFF);
  }
  trace::Bump(trace::Counter::SetRenderTarget);
}

extern "C" REX_FUNC(D3DDevice_SetDepthStencilSurface) {
  FlushPendingUpDraw();
  const u32 surface = ctx.r4.u32;
  {
    PerfScopeSampled guest_scope(state().perf.guest_d3d_ms, state().perf.guest_d3d_calls);
    __imp__D3DDevice_SetDepthStencilSurface(ctx, base);
  }
  if (trace::Enabled()) {
    const u32 size = surface ? mem::load<u32>(surface + 0x24) : 0;
    EOT_TRACE_CALL("SetDepthStencilSurface surf={:#x} {}x{}", surface,
                   surface ? (size >> 18) + 1 : 0, surface ? ((size >> 3) & 0x7FFF) + 1 : 0);
  }
  trace::Bump(trace::Counter::SetDepth);
}

extern "C" REX_FUNC(D3DDevice_SetViewport) {
  FlushPendingUpDraw();
  const u32 vp = ctx.r4.u32;
  {
    PerfScopeSampled guest_scope(state().perf.guest_d3d_ms, state().perf.guest_d3d_calls);
    __imp__D3DDevice_SetViewport(ctx, base);
  }
  if (trace::Enabled() && vp) {
    EOT_TRACE_CALL("SetViewport x={} y={} w={} h={} minZ={} maxZ={}", mem::u32at(vp),
                   mem::u32at(vp + 4), mem::u32at(vp + 8), mem::u32at(vp + 12),
                   mem::f32at(vp + 16), mem::f32at(vp + 20));
  }
  trace::Bump(trace::Counter::SetViewport);
}

extern "C" REX_FUNC(D3DDevice_SetTexture) {
  FlushPendingUpDraw();
  const u32 device = ctx.r3.u32, sampler = ctx.r4.u32, texture = ctx.r5.u32;
  const u64 mask = ctx.r6.u64;
  static const bool verify = Settings::FastSettersVerify();
  bool done = false;
  if (sampler < 26 && device) {
    if (verify) {
      u8 *dev = Guest(base, device);
      VerifyRegions v;
      v.add(dev + dev::kFetchConstants + 24 * sampler, 24);
      v.add(dev + dev::kTextureObject0 + 4 * sampler, 4);
      v.add(dev + kDevPendingGroup3, 8);
      if (const u32 old = Ld32(dev + dev::kTextureObject0 + 4 * sampler))
        v.add(Guest(base, old) + kObjReleaseStamp, 4);
      v.snapshot(v.before);
      if (FastSetTexture(base, device, sampler, texture, mask)) {
        v.snapshot(v.after_fast);
        v.restore(v.before);
        __imp__D3DDevice_SetTexture(ctx, base);
        u8 after_orig[128];
        v.snapshot(after_orig);
        ReportVerify("SetTexture", v, after_orig);
        done = true;
      }
    } else {
      done = FastSetTexture(base, device, sampler, texture, mask);
    }
  }
  if (!done) {
    PerfScopeSampled guest_scope(state().perf.guest_d3d_ms, state().perf.guest_d3d_calls);
    __imp__D3DDevice_SetTexture(ctx, base);
  }
  if (trace::Enabled()) {
    const u32 d1 = texture ? mem::load<u32>(texture + 0x1C + 4) : 0;
    const u32 d2 = texture ? mem::load<u32>(texture + 0x1C + 8) : 0;
    EOT_TRACE_CALL("SetTexture {} tex={:#x} fmt={} {}x{}", sampler, texture, d1 & 0x3F,
                   (d2 & 0x1FFF) + 1, ((d2 >> 13) & 0x1FFF) + 1);
  }
  trace::Bump(trace::Counter::SetTexture);
}

extern "C" REX_FUNC(D3DDevice_SetVertexShader) {
  FlushPendingUpDraw();
  const u32 device = ctx.r3.u32, shader = ctx.r4.u32;
  static const bool verify = Settings::FastSettersVerify();
  bool done = false;
  if (device) {
    if (verify) {
      u8 *dev = Guest(base, device);
      DeviceCompare cmp;
      cmp.Snapshot(dev, cmp.before);
      if (FastSetVertexShader(base, device, shader)) {
        cmp.Snapshot(dev, cmp.after_fast);
        cmp.Restore(dev);
        __imp__D3DDevice_SetVertexShader(ctx, base);
        cmp.Snapshot(dev, cmp.after_orig);
        cmp.Report("SetVertexShader", nullptr, 0);
        done = true;
      }
    } else {
      done = FastSetVertexShader(base, device, shader);
    }
  }
  if (!done) {
    PerfScopeSampled guest_scope(state().perf.guest_d3d_ms, state().perf.guest_d3d_calls);
    __imp__D3DDevice_SetVertexShader(ctx, base);
  }
  EOT_TRACE_CALL("SetVertexShader {:#x}", shader);
  trace::Bump(trace::Counter::SetVertexShader);
}

extern "C" REX_FUNC(D3DDevice_SetPixelShader) {
  FlushPendingUpDraw();
  const u32 device = ctx.r3.u32, shader = ctx.r4.u32;
  static const bool verify = Settings::FastSettersVerify();
  bool done = false;
  if (device) {
    if (verify) {
      u8 *dev = Guest(base, device);
      DeviceCompare cmp;
      cmp.Snapshot(dev, cmp.before);
      if (FastSetPixelShader(base, device, shader)) {
        cmp.Snapshot(dev, cmp.after_fast);
        cmp.Restore(dev);
        __imp__D3DDevice_SetPixelShader(ctx, base);
        cmp.Snapshot(dev, cmp.after_orig);
        cmp.Report("SetPixelShader", nullptr, 0);
        done = true;
      }
    } else {
      done = FastSetPixelShader(base, device, shader);
    }
  }
  if (!done) {
    PerfScopeSampled guest_scope(state().perf.guest_d3d_ms, state().perf.guest_d3d_calls);
    __imp__D3DDevice_SetPixelShader(ctx, base);
  }
  EOT_TRACE_CALL("SetPixelShader {:#x}", shader);
  trace::Bump(trace::Counter::SetPixelShader);
}

extern "C" REX_FUNC(D3DDevice_SetStreamSource) {
  FlushPendingUpDraw();
  const u32 device = ctx.r3.u32, stream = ctx.r4.u32, vb = ctx.r5.u32, offset = ctx.r6.u32,
            stride = ctx.r7.u32;
  const u64 mask = ctx.r8.u64;
  static const bool verify = Settings::FastSettersVerify();
  bool done = false;
  if (stream < 16 && device) {
    if (verify) {
      u8 *dev = Guest(base, device);
      VerifyRegions v;
      v.add(dev + dev::StreamFetchSlotOffset(stream), 8);
      v.add(dev + dev::kStreamObject0 + 4 * stream, 4);
      v.add(dev + dev::kStreamStride0 + stream, 1);
      v.add(dev + kDevPendingGroup2, 8);
      v.add(dev + kDevPendingGroup3, 8);
      if (const u32 old = Ld32(dev + dev::kStreamObject0 + 4 * stream))
        v.add(Guest(base, old) + kObjReleaseStamp, 4);
      v.snapshot(v.before);
      if (FastSetStreamSource(base, device, stream, vb, offset, stride, mask)) {
        v.snapshot(v.after_fast);
        v.restore(v.before);
        __imp__D3DDevice_SetStreamSource(ctx, base);
        u8 after_orig[128];
        v.snapshot(after_orig);
        ReportVerify("SetStreamSource", v, after_orig);
        done = true;
      }
    } else {
      done = FastSetStreamSource(base, device, stream, vb, offset, stride, mask);
    }
  }
  if (!done) {
    PerfScopeSampled guest_scope(state().perf.guest_d3d_ms, state().perf.guest_d3d_calls);
    __imp__D3DDevice_SetStreamSource(ctx, base);
  }
  EOT_TRACE_CALL("SetStreamSource {} vb={:#x} offset={} stride={}", stream, vb, offset, stride);
  trace::Bump(trace::Counter::SetStreamSource);
}

extern "C" REX_FUNC(D3DDevice_SetIndices) {
  FlushPendingUpDraw();
  const u32 device = ctx.r3.u32, ib = ctx.r4.u32;
  static const bool verify = Settings::FastSettersVerify();
  bool done = false;
  if (device) {
    if (verify) {
      u8 *dev = Guest(base, device);
      VerifyRegions v;
      v.add(dev + dev::kIndexBuffer, 4);
      if (const u32 old = Ld32(dev + dev::kIndexBuffer))
        v.add(Guest(base, old) + kObjReleaseStamp, 4);
      v.snapshot(v.before);
      if (FastSetIndices(base, device, ib)) {
        v.snapshot(v.after_fast);
        v.restore(v.before);
        __imp__D3DDevice_SetIndices(ctx, base);
        u8 after_orig[128];
        v.snapshot(after_orig);
        ReportVerify("SetIndices", v, after_orig);
        done = true;
      }
    } else {
      done = FastSetIndices(base, device, ib);
    }
  }
  if (!done) {
    PerfScopeSampled guest_scope(state().perf.guest_d3d_ms, state().perf.guest_d3d_calls);
    __imp__D3DDevice_SetIndices(ctx, base);
  }
  EOT_TRACE_CALL("SetIndices {:#x}", ib);
  trace::Bump(trace::Counter::SetIndices);
}

using namespace eot;
using namespace eot::gpu;

REX_EXTERN(__imp__D3DDevice_SetGammaRamp);
REX_EXTERN(__imp__D3DDevice_SetPWLGamma);

extern "C" REX_FUNC(D3DDevice_Swap) {
  FlushPendingUpDraw();
  const u32 front = ctx.r4.u32;
  EOT_TRACE_CALL("Swap front={:#x}", front);
  Video::Present(front);
  ctx.r3.u64 = 0;
}

// WoS port: Web of Shadows' SetGammaRamp (sub_82AB8658) and SetPWLGamma (sub_82AB8720)
// take (device, flags, ramp) and keep the converted ramp at device + 15004 (0x3A9C), in
// Edge of Time's layout (3 channels x 512 bytes).
constexpr u32 kDeviceGammaShadow = 0x3A9C;

extern "C" REX_FUNC(D3DDevice_SetGammaRamp) {
  FlushPendingUpDraw();
  const u32 device = ctx.r3.u32;
  const u32 ramp = ctx.r5.u32;  // WoS: r4 is the flags
#if defined(__PROSPERO__)
  // The PS5's first boots reached the render thread's gamma ramp with no device
  // (run 9, 2026-10-12): the game's own code writes the ramp into the device
  // (kDeviceGammaShadow), so it is skipped rather than written at address 0
  if (!device) {
    static std::atomic<int> skipped{0};
    if (skipped.fetch_add(1) < 4)
      EOT_WARN("[gamma] D3DDevice_SetGammaRamp with no device (ramp {:#x}); skipped", ramp);
    return;
  }
#endif
  __imp__D3DDevice_SetGammaRamp(ctx, base);
  EOT_TRACE_CALL("SetGammaRamp ramp={:#x}", ramp);
  if (!ramp || !device)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  for (u32 c = 0; c < 3; ++c)
    for (u32 i = 0; i < 256; ++i)
      s.gamma_table[c][i] =
          mem::load<uint16_t>(device + kDeviceGammaShadow + (c * 256 + i) * 2);
  s.gamma_mode = VideoState::GammaMode::Table;
  s.gamma_lut_dirty = true;
  EOT_DEBUG("[gamma] 256-entry ramp: r[0]={} r[32]={} r[64]={} r[128]={} r[255]={}",
           s.gamma_table[0][0], s.gamma_table[0][32], s.gamma_table[0][64], s.gamma_table[0][128],
           s.gamma_table[0][255]);
}

extern "C" REX_FUNC(D3DDevice_SetPWLGamma) {
  FlushPendingUpDraw();
  const u32 device = ctx.r3.u32;
  const u32 ramp = ctx.r5.u32;  // WoS: r4 is the flags
#if defined(__PROSPERO__)
  // The PS5's first boots reached the render thread's gamma ramp with no device
  // (run 9, 2026-10-12): the game's own code writes the ramp into the device
  // (kDeviceGammaShadow), so it is skipped rather than written at address 0
  if (!device) {
    static std::atomic<int> skipped{0};
    if (skipped.fetch_add(1) < 4)
      EOT_WARN("[gamma] D3DDevice_SetPWLGamma with no device (ramp {:#x}); skipped", ramp);
    return;
  }
#endif
  __imp__D3DDevice_SetPWLGamma(ctx, base);
  EOT_TRACE_CALL("SetPWLGamma ramp={:#x}", ramp);
  if (!ramp || !device)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  for (u32 c = 0; c < 3; ++c) {
    for (u32 i = 0; i < 128; ++i) {
      const u32 entry = device + kDeviceGammaShadow + (c * 128 + i) * 4;
      s.gamma_pwl[c][i][0] = mem::load<uint16_t>(entry);
      s.gamma_pwl[c][i][1] = mem::load<uint16_t>(entry + 2);
    }
  }
  s.gamma_mode = VideoState::GammaMode::Pwl;
  s.gamma_lut_dirty = true;
  EOT_DEBUG("[gamma] PWL ramp: r[0]={}+{} r[16]={}+{} r[32]={}+{} r[64]={}+{} r[127]={}+{}",
           s.gamma_pwl[0][0][0], s.gamma_pwl[0][0][1], s.gamma_pwl[0][16][0],
           s.gamma_pwl[0][16][1], s.gamma_pwl[0][32][0], s.gamma_pwl[0][32][1],
           s.gamma_pwl[0][64][0], s.gamma_pwl[0][64][1], s.gamma_pwl[0][127][0],
           s.gamma_pwl[0][127][1]);
}

using namespace eot::gpu;

REX_EXTERN(__imp__D3DDevice_DrawVertices);
REX_EXTERN(__imp__D3DDevice_DrawIndexedVertices);
REX_EXTERN(__imp__D3DDevice_ClearF);
REX_EXTERN(__imp__D3DDevice_Resolve);
REX_EXTERN(__imp__D3DDevice_BeginVertices);

namespace {

using namespace eot::gpu::fastguest;

// WoS port: Edge of Time skips its D3D's draw (it only clears the pending masks, calling
// two internals of its XDK for some state). Web of Shadows runs its own draw: the PM4 it
// writes goes nowhere (no GPU), but the D3D's bookkeeping stays exactly as the game's.
[[maybe_unused]] void FastDrawFlush(PPCContext &ctx, u8 *base, u32 device) {
  u8 *dev = Guest(base, device);
  const u64 p0 = Ld64(dev), p1 = Ld64(dev + 8), p2 = Ld64(dev + 16), p3 = Ld64(dev + 24),
            p4 = Ld64(dev + 32);
  if (p0)
    St64(dev, 0);
  if (p1)
    St64(dev + 8, 0);
  if (p2) {

    St64(dev + 16, 0);
  }
  if (p3)
    St64(dev + 24, 0);
  if (p4) {

    St64(dev + 32, 0);
  }
}

constexpr DeviceCompare::Range kDrawSkip[] = {{40, 64}, {11064, 11072}, {13600, 13624}};

template <typename Original>
void DrawFlush(PPCContext &ctx, u8 *base, u32 device, const char *what, Original original) {
  static const bool verify = Settings::FastSettersVerify();
  if (device && !kWosOriginalSetters) {
    if (verify) {
      u8 *dev = Guest(base, device);
      DeviceCompare cmp;
      cmp.Snapshot(dev, cmp.before);
      const PPCContext saved = ctx;
      FastDrawFlush(ctx, base, device);
      cmp.Snapshot(dev, cmp.after_fast);
      cmp.Restore(dev);
      ctx = saved;
      original();
      cmp.Snapshot(dev, cmp.after_orig);
      cmp.Report(what, kDrawSkip, 3);
    } else {
      FastDrawFlush(ctx, base, device);
    }
    return;
  }
  PerfScopeSampled guest_scope(state().perf.guest_d3d_ms, state().perf.guest_d3d_calls);
  original();
}

u64 ReverseBits(u64 v) {
  v = ((v >> 1) & 0x5555555555555555ull) | ((v & 0x5555555555555555ull) << 1);
  v = ((v >> 2) & 0x3333333333333333ull) | ((v & 0x3333333333333333ull) << 2);
  v = ((v >> 4) & 0x0F0F0F0F0F0F0F0Full) | ((v & 0x0F0F0F0F0F0F0F0Full) << 4);
  return __builtin_bswap64(v);
}

FloatConstantDirty PendingFloatConstants(u32 device_va) {
  if (!device_va)
    return {};
  const u8 *pending = eot::mem::at<u8>(device_va + eot::gpu::dev::kPendingMask);
  if (!pending)
    return {};
  return {ReverseBits(Ld64(pending)), ReverseBits(Ld64(pending + 8))};
}

}

extern "C" REX_FUNC(D3DDevice_DrawVertices) {
  FlushPendingUpDraw();
  const u32 device = ctx.r3.u32, prim = ctx.r4.u32, start = ctx.r5.u32, count = ctx.r6.u32;
  const FloatConstantDirty constants = PendingFloatConstants(device);
  DrawFlush(ctx, base, device, "DrawVertices",
            [&] { __imp__D3DDevice_DrawVertices(ctx, base); });
  EOT_TRACE_CALL("DrawVertices prim={} start={} count={}", prim, start, count);
  trace::Bump(trace::Counter::DrawVertices);
  DrawGuestPrimitives(device, prim, start, count, constants);
}

// fix118 (wos_strip_merge): a window-strip list as one draw. ctx holds DrawVertices'
// arguments for the list's first range: the game's own D3D draw runs for that one (it flushes
// the device state); the renderer draws every range. ranges = {first, count} pairs.
extern "C" void wos_eot_DrawVertexRanges(PPCContext &ctx, uint8_t *base, const u32 *ranges,
                                         u32 range_count) {
  FlushPendingUpDraw();
  const u32 device = ctx.r3.u32, prim = ctx.r4.u32;
  const FloatConstantDirty constants = PendingFloatConstants(device);
  DrawFlush(ctx, base, device, "DrawVertices",
            [&] { __imp__D3DDevice_DrawVertices(ctx, base); });
  trace::Bump(trace::Counter::DrawVertices);
  DrawGuestPrimitiveRanges(device, prim, ranges, range_count, constants);
}

extern "C" REX_FUNC(D3DDevice_DrawIndexedVertices) {
  FlushPendingUpDraw();
  const u32 device = ctx.r3.u32, prim = ctx.r4.u32;
  const i32 base_vertex = ctx.r5.s32;
  const u32 start_index = ctx.r6.u32, count = ctx.r7.u32;
  const FloatConstantDirty constants = PendingFloatConstants(device);
  PrefetchIndexProbes(device, start_index, count);
  DrawFlush(ctx, base, device, "DrawIndexedVertices",
            [&] { __imp__D3DDevice_DrawIndexedVertices(ctx, base); });
  EOT_TRACE_CALL("DrawIndexedVertices prim={} base={} start={} count={}", prim, base_vertex,
                 start_index, count);
  trace::Bump(trace::Counter::DrawIndexed);
  DrawGuestIndexedPrimitives(device, prim, base_vertex, start_index, count, constants);
}

extern "C" REX_FUNC(D3DDevice_ClearF) {
  FlushPendingUpDraw();
  // WoS port: BeginTiling's clear covers one EDRAM tile; the tiled surface here is whole
  const u32 rect = wos_tiling::ClearWhole() ? 0u : ctx.r5.u32;
  const u32 device = ctx.r3.u32, flags = ctx.r4.u32, color = ctx.r6.u32;
  const float z = static_cast<float>(ctx.f1.f64);
  const u32 stencil = ctx.r8.u32;
  {
    PerfScopeSampled guest_scope(state().perf.guest_d3d_ms, state().perf.guest_d3d_calls);
    __imp__D3DDevice_ClearF(ctx, base);
  }
  EOT_TRACE_CALL("ClearF flags={:#x} rect={:#x} color={:#x} z={} stencil={}", flags, rect, color,
                 z, stencil);
  trace::Bump(trace::Counter::Clear);
  ClearGuestTargets(device, flags, rect, color, z, stencil);
}

extern "C" REX_FUNC(D3DDevice_Resolve) {
  FlushPendingUpDraw();
  const u32 device = ctx.r3.u32, flags = ctx.r4.u32, src_rect = ctx.r5.u32, dest = ctx.r6.u32;
  const u32 dest_point = ctx.r7.u32, dest_level = ctx.r8.u32, clear_color = ctx.r9.u32;
  const u32 r10 = ctx.r10.u32;
  const float clear_z = static_cast<float>(ctx.f1.f64);
  {
    PerfScopeSampled guest_scope(state().perf.guest_d3d_ms, state().perf.guest_d3d_calls);
    __imp__D3DDevice_Resolve(ctx, base);
  }
  EOT_TRACE_CALL("Resolve flags={:#x} src={} rect={:#x} dest={:#x} point={:#x} level={} "
                 "r9={:#x} r10={:#x} z={}",
                 flags, flags & 7, src_rect, dest, dest_point, dest_level, clear_color, r10,
                 clear_z);
  trace::Bump(trace::Counter::Resolve);
  ResolveGuest(device, flags, src_rect, dest, dest_point, dest_level, clear_color, clear_z);
}

extern "C" REX_FUNC(D3DDevice_BeginVertices) {
  FlushPendingUpDraw();
  const u32 device = ctx.r3.u32, prim = ctx.r4.u32, count = ctx.r5.u32, stride = ctx.r6.u32;
  const FloatConstantDirty constants = PendingFloatConstants(device);
  {
    PerfScopeSampled guest_scope(state().perf.guest_d3d_ms, state().perf.guest_d3d_calls);
    __imp__D3DDevice_BeginVertices(ctx, base);
  }
  const u32 data = ctx.r3.u32;
  EOT_TRACE_CALL("BeginVertices prim={} count={} stride={} data={:#x}", prim, count, stride, data);
  trace::Bump(trace::Counter::BeginVertices);
  QueueGuestUpDraw(device, prim, count, stride, data, constants);
}

// WoS port: Web of Shadows' D3D has no BeginIndexedVertices (no function but the draws,
// BeginVertices and two command-buffer replays commits shaders), so it is not hooked.
#if 0
extern "C" REX_FUNC(D3DDevice_BeginIndexedVertices) {
  FlushPendingUpDraw();
  const u32 device = ctx.r3.u32;
  const u32 prim = ctx.r4.u32;
  const FloatConstantDirty constants = PendingFloatConstants(device);
  {
    PerfScopeSampled guest_scope(state().perf.guest_d3d_ms, state().perf.guest_d3d_calls);
    __imp__D3DDevice_BeginIndexedVertices(ctx, base);
  }
  if (constants.vs || constants.ps) {
    auto &s = state();
    std::lock_guard lock(s.guest_mutex);
    s.vs_float_constants_stale |= constants.vs;
    s.ps_float_constants_stale |= constants.ps;
  }
  EOT_TRACE_CALL("BeginIndexedVertices prim={} (unmodelled)", prim);
  trace::Bump(trace::Counter::BeginVertices);
}
#endif
