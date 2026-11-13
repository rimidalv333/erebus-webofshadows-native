// core/memory_helpers.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once
#include <chrono>

#include <atomic>

#include <rex/system/kernel_state.h>
#include <rex/types.h>

namespace eot {

template <typename T> using be = rex::be<T>;

namespace mem {

struct Bases {
  u8 *virtual_base = nullptr;
  u8 *physical_base = nullptr;
  u32 e0_offset = 0;
};
inline Bases g_bases;
inline std::atomic<u8 *> g_virtual_base{nullptr};

inline const Bases *bases() {
  if (g_virtual_base.load(std::memory_order_acquire))
    return &g_bases;
  auto *kernel = REX_KERNEL_STATE();
  auto *memory = kernel ? kernel->memory() : nullptr;
  u8 *virtual_base = memory ? memory->virtual_membase() : nullptr;
  if (!virtual_base)
    return nullptr;
  g_bases.physical_base = memory->template TranslatePhysical<u8 *>(0);
  g_bases.e0_offset = static_cast<u32>(memory->template TranslateVirtual<u8 *>(0xE0000000u) -
                                       (virtual_base + 0xE0000000u));
  g_bases.virtual_base = virtual_base;
  g_virtual_base.store(virtual_base, std::memory_order_release);
  return &g_bases;
}

template <typename T> inline T *at(u32 va) {
  if (va < 0x1000)
    return nullptr;
  const Bases *b = bases();
  if (!b)
    return nullptr;
  return reinterpret_cast<T *>(b->virtual_base + va + (va >= 0xE0000000u ? b->e0_offset : 0u));
}

template <typename T> inline T *phys(u32 gpu_address) {
  const Bases *b = bases();
  if (!b)
    return nullptr;
  return reinterpret_cast<T *>(b->physical_base + (gpu_address & 0x1FFFFFFFu));
}

// WoS port: GPU-side addresses (texture fetch bases, index buffer and vertex data
// addresses) are physical (below 0x20000000) and live in the physical memory, not in
// the guest virtual heap at the same number; anything at or above 0x20000000 is taken
// as a guest virtual address (the A0/C0/E0 views and the rest).
template <typename T> inline T *gpu(u32 address) {
  return address < 0x20000000u ? phys<T>(address) : at<T>(address);
}
inline bool gpu_readable(u32 address, u32 bytes);

template <typename T> inline T load(u32 va, T fallback = T{}) {
  auto *p = at<be<T>>(va);
  return p ? static_cast<T>(*p) : fallback;
}

template <typename T> inline bool store(u32 va, T value) {
  auto *p = at<be<T>>(va);
  if (!p)
    return false;
  *p = value;
  return true;
}

inline u32 u32at(u32 va) { return load<u32>(va); }
inline f32 f32at(u32 va) { return load<f32>(va); }

// WoS port (fix42): QueryRegionInfo takes the heap's lock and walks the page table across
// the whole region; the GPU register mirror checked the command stream with it twice a draw
// (plus twice for the shader objects): [WoS-COST] put the mirror at ~50% of the game's D3D
// thread. Committed readable regions are remembered per thread for 250 ms.
struct ReadableRegion {
  u32 base = 0, end = 0;
  u64 expires = 0;
};
inline u64 readable_now_ms() {
  return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count());
}
inline bool readable(u32 va, u32 bytes) {
  if (va < 0x1000 || bytes == 0 || va + bytes < va)
    return false;
  static thread_local ReadableRegion cache[8];
  static thread_local u32 next_slot = 0;
  const u32 end = va + bytes;
  for (const auto &r : cache)
    if (va >= r.base && end <= r.end && r.end) {
      if (readable_now_ms() < r.expires)
        return true;
      break;
    }
  auto *kernel = REX_KERNEL_STATE();
  auto *memory = kernel ? kernel->memory() : nullptr;
  if (!memory)
    return false;
  u32 cursor = va;
  while (cursor < end) {
    auto *heap = memory->LookupHeap(cursor);
    rex::memory::HeapAllocationInfo info{};
    if (!heap || !heap->QueryRegionInfo(cursor, &info))
      return false;
    if (!(info.state & rex::memory::kMemoryAllocationCommit) ||
        info.protect == rex::memory::kMemoryProtectNoAccess)
      return false;
    const u32 region_end = info.base_address + info.region_size;
    if (region_end <= cursor)
      return false;
    if (info.region_size >= 0x10000) {
      ReadableRegion &slot = cache[next_slot++ & 7];
      slot.base = info.base_address;
      slot.end = region_end;
      slot.expires = readable_now_ms() + 250;
    }
    cursor = region_end;
  }
  return true;
}

inline bool gpu_readable(u32 address, u32 bytes) {
  if (address >= 0x20000000u)
    return readable(address, bytes);
  // the whole physical range is committed at startup (rexglue's Memory::Initialize)
  return address >= 0x1000 && bytes && u64(address) + bytes <= 0x20000000ull;
}

inline const char *str(u32 va) {
  auto *p = at<const char>(va);
  return p ? p : "";
}

}
}
