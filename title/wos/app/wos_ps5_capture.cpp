// wos/app/wos_ps5_capture.cpp - one frame of the game's D3D work, written whole.
//
// wos_capture_frame = N (0 = off): between the game's swap N-1 and swap N, every D3D
// call the native renderer hooks is recorded with its arguments, and every draw with the
// device's entire state (the 0x6080-byte D3D device: constant/fetch/register shadows,
// bound objects - as dword deltas against the previous draw), plus, the first time each
// is seen in the frame: the bound textures' first 16 KB, the vertex streams' and index
// buffer's first 16 KB, and both shaders' reassembled containers. Written at swap N to
// /app0/userdata/wos_recomp/dump/capture_N.bin for wos/tools/wos_capture.py.
//
// Game-derived data: it stays on the console and with the developer, never in the repo.
//
// Record: u32 tag, u32 payload bytes, payload. Tags:
//   'CALL' name[32] thread r3..r10 f1 | 64 bytes at r5 | 64 bytes at r7
//   'DEVF' device va, 0x6080 bytes (first draw of a device)
//   'DEVD' device va, u32 n, n x (u16 dword index, u16 pad, u32 value) (later draws)
//   'BLOB' kind (1 texture 2 vertex 3 index 4 vs 5 ps 6 indirect buffer 7 mesh object
//          8 command buffer object 9 GPU register mirror at a draw (fix34: key = draw serial;
//          fetch registers 0x4800-0x48BF, then the overlay's state registers, then the IM_LOAD
//          vs/ps addresses) 10 raw stream span parsed (key = serial)), key, u32 bytes, data
//   'SWAP' frame number
//
// fix34: the capture starts at wos_capture_frame whatever the frame draws, and again each
// time L3+R3 are pressed together (at most 8 a session, 5 s apart): the next frame is
// captured to capture_padN_fF.bin, eot_diag_dump's dumps are taken of it (f<F>_*.ppm,
// [stats]) with Edge of Time's detailed one-frame log, and the register mirror logs every
// draw's overlay for two frames.
//
// SPDX-License-Identifier: MIT

#include <cstdlib>
#include <pthread.h>
#include <sys/stat.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/runtime.h>

#include "core/memory_helpers.h"
#include "gpu/d3d.h"

REXCVAR_DEFINE_INT32(wos_capture_frame, 0, "WoS",
                     "Record frame N's D3D calls and draw state to dump/capture_N.bin (0 = off)");

// WoS port (fix109): raw guest memory ranges saved into every capture (both renderers), as
// "addr:bytes[,addr:bytes]" - default: the 4096x1024 DXT5 panorama at 0x1c589000 that the
// park water reflects (native shows clouds only, emulated shows trees: who wrote what)
REXCVAR_DEFINE_STRING(wos_capture_mem, "0x1c589000:0x400000", "WoS",
                      "fix109: guest memory ranges (addr:bytes, comma separated) saved in each capture");

REXCVAR_DEFINE_BOOL(wos_capture_pad, true, "WoS",
                    "L3+R3 together: capture the next frame and dump it (eot_diag_dump)");

namespace eot::gpu {
void WosArmDiagnosticFrame();  // gpu/present.cpp: dumps + detailed log of the next frame
}
namespace wos::native_renderer {
bool Enabled();
void VerboseOverlays(uint64_t n);
}

namespace wos::harvest {
bool ReassembleShader(uint8_t *base, uint32_t obj, bool pixel, std::vector<uint8_t> &out);
}

namespace wos::capture {
namespace {
constexpr uint32_t kDeviceBytes = 0x6080;
std::mutex g_lock;
std::atomic<uint64_t> g_swaps{0};
std::atomic<bool> g_active{false};
std::vector<uint8_t> g_out;
std::unordered_map<uint32_t, std::vector<uint8_t>> g_last_device;
std::unordered_set<uint64_t> g_blobs;

void Put32(uint32_t v) {
  const size_t n = g_out.size();
  g_out.resize(n + 4);
  std::memcpy(g_out.data() + n, &v, 4);
}
void PutBytes(const void *p, size_t len) {
  const size_t n = g_out.size();
  g_out.resize(n + len);
  if (p)
    std::memcpy(g_out.data() + n, p, len);
  else
    std::memset(g_out.data() + n, 0, len);
}
void Tag(const char *tag, uint32_t bytes) {
  uint32_t t;
  std::memcpy(&t, tag, 4);
  Put32(t);
  Put32(bytes);
}

uint32_t Be32(const uint8_t *p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}

void Blob(uint32_t kind, uint32_t key, const uint8_t *data, uint32_t bytes) {
  if (!data || !bytes || !g_blobs.insert((uint64_t(kind) << 32) | key).second)
    return;
  Tag("BLOB", 12 + bytes);
  Put32(kind);
  Put32(key);
  Put32(bytes);
  PutBytes(data, bytes);
}

// Guest memory a GPU address (physical, or a physical view) points at, if readable.
const uint8_t *Gpu(uint32_t address, uint32_t bytes) {
  return eot::mem::gpu_readable(address, bytes) ? eot::mem::gpu<const uint8_t>(address) : nullptr;
}

void Device(uint32_t device) {
  const uint8_t *dev = eot::mem::at<const uint8_t>(device);
  if (!dev)
    return;
  auto &last = g_last_device[device];
  if (last.empty()) {
    last.assign(dev, dev + kDeviceBytes);
    Tag("DEVF", 4 + kDeviceBytes);
    Put32(device);
    PutBytes(dev, kDeviceBytes);
    return;
  }
  std::vector<std::pair<uint32_t, uint32_t>> changed;
  for (uint32_t i = 0; i < kDeviceBytes / 4; ++i) {
    uint32_t a, b;
    std::memcpy(&a, dev + 4 * i, 4);
    std::memcpy(&b, last.data() + 4 * i, 4);
    if (a != b)
      changed.emplace_back(i, a);
  }
  std::memcpy(last.data(), dev, kDeviceBytes);
  Tag("DEVD", 8 + 8 * uint32_t(changed.size()));
  Put32(device);
  Put32(uint32_t(changed.size()));
  for (auto &[i, v] : changed) {
    Put32(i);
    Put32(v);
  }
}

void Resources(uint8_t *base, uint32_t device) {
  const uint8_t *dev = eot::mem::at<const uint8_t>(device);
  if (!dev)
    return;
  // textures: 32 fetch slots of 6 dwords; type 2 is a texture
  for (uint32_t t = 0; t < 32; ++t) {
    const uint8_t *fc = dev + eot::gpu::dev::kFetchConstants + 24 * t;
    if ((Be32(fc) & 3) != 2)
      continue;
    const uint32_t addr = Be32(fc + 4) & 0xFFFFF000u;
    if (const uint8_t *p = addr ? Gpu(addr, 16384) : nullptr)
      Blob(1, addr, p, 16384);
  }
  // vertex streams 0..3
  for (uint32_t st = 0; st < 4; ++st) {
    const uint32_t slot = eot::gpu::dev::StreamFetchSlotOffset(st);
    const uint32_t d0 = Be32(dev + slot);
    if ((d0 & 3) != 3)
      continue;
    const uint32_t addr = d0 & ~3u;
    if (const uint8_t *p = Gpu(addr, 16384))
      Blob(2, addr, p, 16384);
  }
  // index buffer (object at device + kIndexBuffer; data address at +0x18)
  if (const uint32_t ib = Be32(dev + eot::gpu::dev::kIndexBuffer)) {
    if (const uint8_t *o = eot::mem::at<const uint8_t>(ib)) {
      const uint32_t addr = Be32(o + eot::gpu::obj::kBufferFetch0);
      if (const uint8_t *p = addr ? Gpu(addr, 16384) : nullptr)
        Blob(3, addr, p, 16384);
    }
  }
  // shaders
  for (int pixel = 0; pixel < 2; ++pixel) {
    const uint32_t obj =
        Be32(dev + (pixel ? eot::gpu::dev::kPixelShader : eot::gpu::dev::kVertexShader));
    if (!obj || g_blobs.count((uint64_t(4 + pixel) << 32) | obj))
      continue;
    std::vector<uint8_t> c;
    if (wos::harvest::ReassembleShader(base, obj, pixel != 0, c))
      Blob(4 + pixel, obj, c.data(), uint32_t(c.size()));
    else
      g_blobs.insert((uint64_t(4 + pixel) << 32) | obj);
  }
}
}  // namespace

bool Active() { return g_active.load(std::memory_order_relaxed); }

// World mesh draws this frame: the capture waits for a frame with some (gameplay).
std::atomic<uint32_t> g_mesh_draws{0};
void NoteMeshDraw() { g_mesh_draws.fetch_add(1, std::memory_order_relaxed); }

// Raw data for the capture (kinds 6+: 6 indirect buffer dwords, 7 mesh object, 8 command
// buffer object), once per (kind, key) in the captured frame.
void NoteBlob(uint32_t kind, uint32_t key, const uint8_t *data, uint32_t bytes) {
  if (!Active() || !data || !bytes)
    return;
  std::lock_guard lock(g_lock);
  if (!g_active.load(std::memory_order_relaxed))
    return;
  Blob(kind, key, data, bytes);
}

// Every hooked D3D call, before it runs. draw = record the device and resources too.
void Call(const char *name, const PPCContext &ctx, uint8_t *base, bool draw) {
  if (!Active())
    return;
  std::lock_guard lock(g_lock);
  if (!g_active.load(std::memory_order_relaxed))
    return;
  Tag("CALL", 32 + 4 + 8 * 4 + 4 + 64 + 64);
  char n[32] = {};
  std::strncpy(n, name, 31);
  PutBytes(n, 32);
  Put32(uint32_t(reinterpret_cast<uintptr_t>(pthread_self())));
  const uint32_t regs[8] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                            ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32};
  PutBytes(regs, sizeof regs);
  const float f1 = float(ctx.f1.f64);
  PutBytes(&f1, 4);
  for (uint32_t va : {ctx.r5.u32, ctx.r7.u32}) {
    const uint8_t *p = (va >= 0x10000 && eot::mem::readable(va, 64)) ? eot::mem::at<const uint8_t>(va) : nullptr;
    PutBytes(p, 64);
  }
  if (draw) {
    Device(ctx.r3.u32);
    Resources(base, ctx.r3.u32);
  }
}

// L3+R3 held together (edge, 5 s apart, at most 8): a diagnostic frame on demand.
namespace {
std::string g_capture_name;
bool PadTrigger(uint64_t swap) {
  if (!REXCVAR_GET(wos_capture_pad))
    return false;
  static bool held = false;
  static uint64_t last = 0;
  static uint32_t count = 0;
  rex::Runtime *rt = rex::Runtime::instance();
  if (!rt || !rt->input_system())
    return false;
  auto *input = static_cast<rex::input::InputSystem *>(rt->input_system());
  rex::input::X_INPUT_STATE state{};
  if (input->GetStateForUI(0, &state) != 0)
    return false;
  const uint16_t buttons = state.gamepad.buttons;
  const uint16_t combo = rex::input::X_INPUT_GAMEPAD_LEFT_THUMB | rex::input::X_INPUT_GAMEPAD_RIGHT_THUMB;
  const bool now = (buttons & combo) == combo;
  const bool pressed = now && !held;
  held = now;
  if (!pressed || count >= 8 || (last && swap < last + 300))
    return false;
  last = swap;
  ++count;
  REXLOG_INFO("[WoS-TRIGGER] L3+R3 #{}: capturing and dumping the next frame (swap {})", count, swap + 1);
  return true;
}
}  // namespace

// At every swap: start, or finish and write, the captured frame.
void OnSwap() {
  const int32_t target = REXCVAR_GET(wos_capture_frame);
  const uint64_t n = g_swaps.fetch_add(1, std::memory_order_relaxed) + 1;
  g_mesh_draws.store(0, std::memory_order_relaxed);
  const bool pad = PadTrigger(n);
  if (pad && wos::native_renderer::Enabled()) {  // fix52: EoT's dumps only when it renders
    eot::gpu::WosArmDiagnosticFrame();
    wos::native_renderer::VerboseOverlays(6000);
  }
  if (target <= 0 && !pad && !g_active.load(std::memory_order_relaxed))
    return;
  std::lock_guard lock(g_lock);
  static bool done = false;
  static uint32_t pads = 0;
  const bool auto_start = !done && target > 0 && n + 1 >= uint64_t(target);
  if (!g_active.load(std::memory_order_relaxed) && (auto_start || pad)) {
    if (auto_start)
      done = true;
    g_capture_name = pad ? std::format("capture_pad{}_f{}.bin", ++pads, n + 1)
                         : std::format("capture_{}.bin", target);
    g_out.clear();
    g_out.reserve(64u << 20);
    g_last_device.clear();
    g_blobs.clear();
    g_active.store(true, std::memory_order_relaxed);
    REXLOG_INFO("[WoS-CAPTURE] recording frame {} -> {}", n + 1, g_capture_name);
    // fix109: raw memory ranges (blob kind 21, key = address)
    {
      const std::string spec = REXCVAR_GET(wos_capture_mem);
      size_t i = 0;
      while (i < spec.size()) {
        size_t j = spec.find(',', i);
        if (j == std::string::npos)
          j = spec.size();
        const std::string item = spec.substr(i, j - i);
        const size_t colon = item.find(':');
        if (colon != std::string::npos) {
          const uint32_t addr = static_cast<uint32_t>(std::strtoul(item.c_str(), nullptr, 0));
          const uint32_t bytes =
              static_cast<uint32_t>(std::strtoul(item.c_str() + colon + 1, nullptr, 0));
          if (bytes && bytes <= (32u << 20)) {
            if (const uint8_t *p = Gpu(addr, bytes)) {
              Blob(21, addr, p, bytes);
              REXLOG_INFO("[WoS-CAPTURE] memory {:#x}+{:#x} saved", addr, bytes);
            } else {
              REXLOG_INFO("[WoS-CAPTURE] memory {:#x}+{:#x} not readable", addr, bytes);
            }
          }
        }
        i = j + 1;
      }
    }
  } else if (g_active.load(std::memory_order_relaxed)) {
    g_active.store(false, std::memory_order_relaxed);
    Tag("SWAP", 4);
    Put32(uint32_t(n));
    ::mkdir("/app0/userdata/wos_recomp/dump", 0777);
    const std::string path = "/app0/userdata/wos_recomp/dump/" + g_capture_name;
    if (FILE *f = std::fopen(path.c_str(), "wb")) {
      std::fwrite(g_out.data(), 1, g_out.size(), f);
      std::fclose(f);
      REXLOG_INFO("[WoS-CAPTURE] wrote {} ({} MB)", path, g_out.size() >> 20);
    } else {
      REXLOG_ERROR("[WoS-CAPTURE] could not write {}", path);
    }
    std::vector<uint8_t>().swap(g_out);
  }
}
}  // namespace wos::capture
