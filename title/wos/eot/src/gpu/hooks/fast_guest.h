// gpu/hooks/fast_guest.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <cstring>
#include <format>
#include <string>

#include <rex/types.h>
#include <rex/memory/utils.h>
#include <rex/system/xmemory.h>

#include "core/logging.h"
#include "gpu/d3d.h"

namespace eot::gpu::fastguest {

inline u8 *Guest(u8 *base, u32 va) { return base + va + rex::memory::detail::PhysicalHostOffset(va); }
inline const u8 *Guest(const u8 *base, u32 va) {
  return base + va + rex::memory::detail::PhysicalHostOffset(va);
}

inline u16 Ld16(const u8 *p) { return rex::memory::load_and_swap<u16>(p); }
inline u32 Ld32(const u8 *p) { return rex::memory::load_and_swap<u32>(p); }
inline u64 Ld64(const u8 *p) { return rex::memory::load_and_swap<u64>(p); }
inline void St32(u8 *p, u32 v) {
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}
inline void St64(u8 *p, u64 v) {
  v = __builtin_bswap64(v);
  std::memcpy(p, &v, 8);
}

// WoS port (fix105): Web of Shadows' D3D device offsets (EoT: 11036 / 11040)
constexpr u32 kDevReleaseStamp = 10908;
constexpr u32 kDevReleaseMask = 10912;
constexpr u32 kObjReleaseStamp = 8;

inline bool NeedsRing(const u8 *base, const u8 *dev, u32 old) {
  if (!old || Ld32(dev + kDevReleaseStamp) != 0)
    return false;
  return (Ld32(dev + kDevReleaseMask) & Ld32(Guest(base, old))) != 0;
}
inline void StampReplaced(u8 *base, const u8 *dev, u32 old) {
  if (!old)
    return;
  if (const u32 stamp = Ld32(dev + kDevReleaseStamp))
    St32(Guest(base, old) + kObjReleaseStamp, stamp);
}

struct DeviceCompare {
  struct Range {
    u32 from, to;
  };
  static constexpr u32 kBytes = dev::kDeviceSize;
  u8 *before, *after_fast, *after_orig;
  DeviceCompare() {
    static thread_local u8 a[kBytes], b[kBytes], c[kBytes];
    before = a;
    after_fast = b;
    after_orig = c;
  }
  void Snapshot(const u8 *dev, u8 *out) const { std::memcpy(out, dev, kBytes); }
  void Restore(u8 *dev) const { std::memcpy(dev, before, kBytes); }
  void Report(const char *what, const Range *skip, u32 skip_count) const {
    static u32 reports = 0;
    if (reports >= 40)
      return;
    std::string diffs;
    u32 count = 0;
    for (u32 off = 0; off < kBytes; off += 4) {
      bool skipped = false;
      for (u32 i = 0; i < skip_count; ++i)
        if (off >= skip[i].from && off < skip[i].to)
          skipped = true;
      if (skipped || std::memcmp(after_fast + off, after_orig + off, 4) == 0)
        continue;
      if (count < 12)
        diffs += std::format(" +{:#x}: hook {:08x} xdk {:08x}", off, Ld32(after_fast + off),
                             Ld32(after_orig + off));
      count++;
    }
    if (count) {
      EOT_WARN("[fast-setters] {} differs in {} words:{}", what, count, diffs);
      reports++;
    }
  }
};

}
