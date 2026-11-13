// WebOfShadowsRecomp for the PS5 - a developer tool to capture the game's
// Xbox 360 shaders for a native renderer experiment.
//
// Web of Shadows keeps its shaders inside Treyarch's "NCH" (.XEPACK) containers,
// which are compressed, so a static byte scan of the game files finds nothing.
// But once the game has loaded a level, it decompresses the shaders into guest
// RAM in their raw Xbox 360 container form. This walks the emulated guest's
// virtual memory for those containers (the same validation XenosRecomp uses),
// dedups them, and writes each one to /app0/wos-shaders.bin (readable over FTP).
// That file is then fed to XenosRecomp on a PC to translate the shaders to
// HLSL/SPIR-V. This is the memory-scan path of PS5_EdgeOfTimeTitle's
// gpu/shaders/shader_dump.cpp, aimed at the guest's address space.
//
// Off unless wos_shader_dump = true in wos_recomp.toml; a release build does
// nothing here.
//
// SPDX-License-Identifier: MIT

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <unordered_set>

#include <sys/stat.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

extern "C" void prospero_startup_log(const char *message);

// WebOfShadowsRecomp: capture the game's shaders to /app0/wos-shaders.bin for a
// PC-side XenosRecomp run. Developer tool, off by default - a scan touches all of
// guest memory, so it is not free.
REXCVAR_DEFINE_BOOL(wos_shader_dump, false, "WoS",
                    "Capture the game's Xbox 360 shaders to /app0/wos-shaders.bin (developer)");

namespace wos {
namespace shader_dump {

namespace {

constexpr const char *kOutPath = "/app0/wos-shaders.bin";

// XenosRecomp's shader container header: nine big-endian uint32s.
//   [0] flags  [1] virtualSize  [2] physicalSize  [3] fieldC
//   [4] constantTableOffset [5] definitionTableOffset [6] shaderOffset
//   [7] field1C  [8] field20
// A container is real when (flags & 0xFFFFFF00) == 0x102A1100, field1C and
// field20 are zero, and virtualSize+physicalSize is a sane size.
constexpr uint32_t kHeaderBytes = 9 * 4;  // 36
// A real shader is a few KiB; anything larger is pack data that happens to look
// like a header. EoT caps at 8 MiB; we do the same.
constexpr uint32_t kLargest = 8u << 20;

uint32_t ReadBE32(const uint8_t *p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

uint64_t Fnv1a(const uint8_t *p, size_t n) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < n; ++i)
    h = (h ^ p[i]) * 0x100000001B3ull;
  return h ^ n;
}

// Scan one contiguous host range [data, data+size) for shader containers,
// appending new ones to `out`. `seen` dedups across calls (so repeated scans of
// the same memory only write each shader once). Returns how many new ones.
size_t ScanRange(const uint8_t *data, size_t size, FILE *out,
                 std::unordered_set<uint64_t> &seen, uint64_t &bytes_written) {
  size_t found = 0;
  if (size <= kHeaderBytes)
    return 0;
  size_t i = 0;
  const size_t limit = size - kHeaderBytes - 1;
  while (i < limit) {
    const uint8_t *h = data + i;
    const uint32_t flags = ReadBE32(h);
    if ((flags & 0xFFFFFF00u) == 0x102A1100u) {
      const uint64_t dsz = uint64_t(ReadBE32(h + 4)) + ReadBE32(h + 8);
      if (dsz >= kHeaderBytes && dsz <= kLargest && dsz <= uint64_t(size - i) &&
          ReadBE32(h + 0x1C) == 0 && ReadBE32(h + 0x20) == 0) {
        // Validate the three internal offsets point within the container; this
        // rejects the rare random header that still passes the field tests.
        const uint32_t ctbl = ReadBE32(h + 0x10);
        const uint32_t dtbl = ReadBE32(h + 0x14);
        const uint32_t shof = ReadBE32(h + 0x18);
        if (ctbl < dsz && dtbl < dsz && shof < dsz) {
          const uint64_t fp = Fnv1a(h, size_t(dsz));
          if (seen.insert(fp).second) {
            static const uint8_t kPad[16] = {};
            std::fwrite(h, 1, size_t(dsz), out);
            std::fwrite(kPad, 1, sizeof(kPad), out);
            bytes_written += dsz + sizeof(kPad);
            ++found;
          }
          i += size_t(dsz);
          continue;
        }
      }
    }
    i += 4;
  }
  return found;
}

void Worker(rex::Runtime *runtime) {
  if (!runtime || !runtime->memory()) {
    REXLOG_WARN("[WoS-SHADERDUMP] no runtime memory; nothing to scan");
    return;
  }
  auto *memory = runtime->memory();
  uint8_t *base = memory->virtual_membase();
  if (!base) {
    REXLOG_WARN("[WoS-SHADERDUMP] no virtual_membase; nothing to scan");
    return;
  }

  FILE *out = std::fopen(kOutPath, "wb");
  if (!out) {
    REXLOG_ERROR("[WoS-SHADERDUMP] cannot write {}", kOutPath);
    return;
  }
  chmod(kOutPath, 0666);

  prospero_startup_log("shader dump: scanning guest memory for shaders");
  REXLOG_INFO("[WoS-SHADERDUMP] on: writing shaders to {} (membase {})", kOutPath,
              static_cast<void *>(base));

  std::unordered_set<uint64_t> seen;
  uint64_t bytes_written = 0;
  size_t total = 0;

  // IMPORTANT: this runtime's guest arena faults on an unmapped page rather than
  // reading zero, so we must never touch a page that is not committed and
  // readable. We walk the guest's virtual range region by region using the heap
  // API (the same checks wos_ps5_hooks.cpp's Readable() uses): LookupHeap finds
  // the heap for an address; QueryBaseAndSize gives the extent of the region
  // there; QueryRegionInfo says whether it is committed and readable. We scan
  // only those regions, and step over everything else without reading it.
  // base+guest_addr is the exact host pointer across [0, 0xE0000000) because
  // host_address_offset is zero there (only the 0xE0.. physical heap is shifted,
  // and shaders never live there). Scan periodically so shaders loaded at any
  // point (menu, then each level) are caught.
  constexpr uint32_t kScanBegin = 0x00010000u;
  constexpr uint32_t kScanEnd = 0xE0000000u;
  for (int pass = 1; pass <= 10; ++pass) {
    std::this_thread::sleep_for(std::chrono::seconds(20));
    const size_t before = total;
    uint64_t scanned_bytes = 0;
    uint32_t addr = kScanBegin;
    while (addr < kScanEnd) {
      auto *heap = memory->LookupHeap(addr);
      if (!heap) {
        // No heap covers this address; jump forward a page and keep looking.
        addr += 0x10000u;
        continue;
      }
      // The region of uniform attributes that contains addr.
      uint32_t region_addr = addr;
      uint32_t region_size = 0;
      if (!heap->QueryBaseAndSize(&region_addr, &region_size) || region_size == 0) {
        addr += 0x10000u;
        continue;
      }
      const uint32_t region_end = region_addr + region_size;
      rex::memory::HeapAllocationInfo info{};
      const bool ok = heap->QueryRegionInfo(region_addr, &info);
      // state must be committed (MemoryAllocationFlag); protect must allow read.
      const bool committed = ok && (info.state & rex::memory::kMemoryAllocationCommit) != 0;
      const bool readable = ok && (info.protect & rex::memory::kMemoryProtectRead) != 0;
      if (committed && readable) {
        // Clamp the region to our scan window and to the mapped heap, then scan
        // the host bytes directly - safe now that we know the pages are present.
        const uint32_t lo = std::max(region_addr, kScanBegin);
        const uint32_t hi = std::min(region_end, kScanEnd);
        if (hi > lo) {
          total += ScanRange(base + lo, size_t(hi - lo), out, seen, bytes_written);
          scanned_bytes += hi - lo;
        }
      }
      // Advance past this region (guard against no forward progress).
      addr = (region_end > addr) ? region_end : addr + 0x10000u;
    }
    std::fflush(out);
    REXLOG_INFO("[WoS-SHADERDUMP] pass {}: {} new, {} shaders total, {} KB written, "
                "{} MB of mapped memory scanned",
                pass, total - before, total, bytes_written >> 10, scanned_bytes >> 20);
    char line[160];
    std::snprintf(line, sizeof line,
                  "shader dump: pass %d, %zu shaders so far (%llu KB), %llu MB scanned",
                  pass, total, (unsigned long long)(bytes_written >> 10),
                  (unsigned long long)(scanned_bytes >> 20));
    prospero_startup_log(line);
  }

  std::fclose(out);
  REXLOG_INFO("[WoS-SHADERDUMP] DONE: {} distinct shaders, {} KB in {}", total,
              bytes_written >> 10, kOutPath);
  char line[128];
  std::snprintf(line, sizeof line, "shader dump: DONE, %zu shaders (%llu KB)", total,
                (unsigned long long)(bytes_written >> 10));
  prospero_startup_log(line);
}

}  // namespace

// Called from OnPostLaunchModule once the guest thread is running. Does nothing
// unless wos_shader_dump is on.
void Start(rex::Runtime *runtime) {
  if (!REXCVAR_GET(wos_shader_dump))
    return;
  std::thread([runtime] { Worker(runtime); }).detach();
}

}  // namespace shader_dump
}  // namespace wos
