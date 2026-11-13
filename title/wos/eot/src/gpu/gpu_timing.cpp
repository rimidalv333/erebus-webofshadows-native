// gpu/gpu_timing.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "gpu/gpu_timing.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <plume_render_interface.h>
#if defined(EOT_D3D12)
#include <plume_d3d12.h>
#else
#include <plume_vulkan.h>
#endif

#if defined(EOT_D3D12)
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#endif

#include "core/logging.h"
#include "gpu/backend.h"
#include "gpu/device.h"
#include "gpu/settings.h"

namespace eot::gpu {

namespace {

constexpr u32 kQueryCount = g_mvk ? 4096 : 8192;

struct SlotTiming {
  std::unique_ptr<plume::RenderQueryPool> pool;
  std::vector<u32> journal;
  std::vector<std::string> tags;
  bool diag = false;
  u32 used = 0;
  bool pending = false;
};

SlotTiming g_slots[kNumFrames];
u32 g_active_slot = 0;
u32 g_cat = kGpuCatOther;
bool g_supported = true;
bool g_open = false;

std::string g_diag_tag;

void WriteMark(SlotTiming &st, plume::RenderCommandList *cmd, u32 closing) {
  if (st.used >= kQueryCount)
    return;
  cmd->writeTimestamp(st.pool.get(), st.used++);
  st.journal.push_back(closing);
  if (st.diag)
    st.tags.push_back(g_diag_tag);
}

bool ReadbackIsMappable(plume::RenderQueryPool *pool) {
#if defined(EOT_D3D12)
  auto *d3d_pool = static_cast<plume::D3D12QueryPool *>(pool);
  auto *readback = d3d_pool->readbackBuffer.get();
  if (!readback || !readback->map())
    return false;
  readback->unmap();
#else
  (void)pool;
#endif
  return true;
}

const char *FormatName(u32 host_format) {
  switch (host_format) {
  case 10:
    return "16f";
  case 11:
    return "16u";
  case 20:
    return "8";
  case 24:
    return "10.10.10.2";
  case 50:
    return "11.11.10";
  default:
    return nullptr;
  }
}

}

u32 GpuTargetCategory(bool full_frame, bool has_depth, u32 color_count, u32 color0_host_format,
                      u32 samples, bool additive) {
  if (color_count == 0)
    return kGpuCatShadow;
  return 0x1000u | (full_frame && additive ? 0x2000u : 0u) | (full_frame ? 0x800u : 0u) |
         (has_depth ? 0x400u : 0u) | (samples > 1 ? 0x200u : 0u) | (color0_host_format & 0x1FFu);
}

std::string GpuCategoryName(u32 cat) {
  switch (cat) {
  case kGpuCatOther:
    return "other";
  case kGpuCatShadow:
    return "shadow";
  case kGpuCatResolve:
    return "resolve";
  case kGpuCatPresent:
    return "present";
  case kGpuCatUpload:
    return "upload";
  case kGpuCatResolveHw:
    return "resolve-hw";
  case kGpuCatResolveDepth:
    return "resolve-depth";
  case kGpuCatBroadcast:
    return "broadcast";
  case kGpuCatTaa:
    return "taa";
  default:
    break;
  }
  const bool full = cat & 0x800u, depth = cat & 0x400u, ms = cat & 0x200u, add = cat & 0x2000u;
  const u32 fmt = cat & 0x1FFu;
  const char *name = FormatName(fmt);
  return std::format("{} {}{}{}{}", full ? "full" : "small",
                     name ? std::string(name) : std::format("fmt{}", fmt), depth ? "+z" : "",
                     add ? "+add" : "", full && !ms ? "@1x" : "");
}

void GpuTimingFrameBegin(VideoState &s, plume::RenderCommandList *cmd, u32 slot) {
  if (!g_supported || !s.device || !cmd)
    return;
  if (!s.device->getCapabilities().queryPools) {
    g_supported = false;
    return;
  }
  auto &st = g_slots[slot];
  if (!st.pool) {
    st.pool = s.device->createQueryPool(kQueryCount);
    if (!st.pool) {
      g_supported = false;
      return;
    }
  }
  st.pending = false;
  st.used = 0;
  st.journal.clear();
  st.journal.push_back(kGpuCatOther);
  st.tags.clear();
  st.tags.push_back(std::string());
  st.diag = false;
  g_diag_tag.clear();
  cmd->resetQueryPool(st.pool.get(), 0, kQueryCount);
  cmd->writeTimestamp(st.pool.get(), st.used++);
  g_active_slot = slot;
  g_cat = kGpuCatOther;
  g_open = true;
}

void GpuTimingMark(VideoState &s, plume::RenderCommandList *cmd, u32 cat) {
  (void)s;
  if (!g_supported || !g_open || cat == g_cat || !cmd)
    return;
  WriteMark(g_slots[g_active_slot], cmd, g_cat);
  g_cat = cat;
}

void GpuTimingCountDraw(VideoState &s) {
  if (!g_supported || !g_open)
    return;
  static u32 *count = nullptr;
  static u32 count_cat = ~0u;
  static u64 count_reset = ~0ull;
  if (!count || count_cat != g_cat || count_reset != s.perf_resets) {
    count = &s.perf.gpu_cats[g_cat].second;
    count_cat = g_cat;
    count_reset = s.perf_resets;
  }
  ++*count;
}

bool DiagFrameNow(const VideoState &s) {
  return Settings::DiagFrame() > 0 && s.guest_frames + 1 == static_cast<u64>(Settings::DiagFrame());
}

bool GpuTimingDiagActive(const VideoState &s) {
  return g_supported && g_open && (g_slots[g_active_slot].diag || DiagFrameNow(s));
}

void GpuTimingDiagMark(VideoState &s, plume::RenderCommandList *cmd, std::string tag) {
  if (!GpuTimingDiagActive(s) || !cmd)
    return;
  auto &st = g_slots[g_active_slot];
  if (!st.diag) {
    st.diag = true;
    st.tags.assign(st.journal.size(), std::string());
  }
  WriteMark(st, cmd, g_cat);
  g_diag_tag = std::move(tag);
}

void GpuTimingFrameEnd(plume::RenderCommandList *cmd) {
  if (!g_supported || !g_open || !cmd)
    return;
  auto &st = g_slots[g_active_slot];
  WriteMark(st, cmd, g_cat);
  st.pending = st.journal.size() > 1;
  g_open = false;
}

void GpuTimingCollect(VideoState &s, u32 slot) {
  if (!g_supported)
    return;
  auto &st = g_slots[slot];
  if (!st.pending || !st.pool)
    return;
  st.pending = false;
  if (!ReadbackIsMappable(st.pool.get())) {
    EOT_ERROR("[gpu-timing] query readback could not be mapped (slot {}); timing disabled", slot);
    g_supported = false;
    return;
  }
#if defined(EOT_D3D12)
  st.pool->queryResults();
  const u64 *r = st.pool->getResults();
  const size_t n = std::min<size_t>(st.journal.size(), st.pool->getCount());
#else
  auto *vk_pool = static_cast<plume::VulkanQueryPool *>(st.pool.get());
  std::vector<u64> ns(st.used);
  const VkResult res =
      st.used ? vkGetQueryPoolResults(vk_pool->device->vk, vk_pool->vk, 0, st.used, sizeof(u64) * st.used,
                                      ns.data(), sizeof(u64), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT)
              : VK_SUCCESS;
  if (res != VK_SUCCESS) {
    EOT_WARN("[gpu-timing] vkGetQueryPoolResults failed ({:#x}) for slot {}; timing disabled", static_cast<u32>(res),
             slot);
    g_supported = false;
    return;
  }
  const double period = vk_pool->device->physicalDeviceProperties.limits.timestampPeriod;
  for (u64 &v : ns)
    v = static_cast<u64>(static_cast<double>(v) * period);
  const u64 *r = ns.data();
  const size_t n = std::min<size_t>(st.journal.size(), ns.size());
#endif
  if (n < 2 || r[n - 1] <= r[0])
    return;
  for (size_t i = 1; i < n; ++i) {
    if (r[i] <= r[i - 1])
      continue;
    s.perf.gpu_cats[st.journal[i]].first += (r[i] - r[i - 1]) * 1e-6;
  }
  s.perf.gpu_ms += (r[n - 1] - r[0]) * 1e-6;
  s.perf.gpu_frames++;
  {
    static f64 average = 0.0;
    const f64 total = (r[n - 1] - r[0]) * 1e-6;
    if (average > 0.0 && total > average * 1.4 && total > average + 2.0) {
      std::map<u32, f64> cats;
      for (size_t i = 1; i < n; ++i)
        if (r[i] > r[i - 1])
          cats[st.journal[i]] += (r[i] - r[i - 1]) * 1e-6;
      std::vector<std::pair<f64, u32>> order;
      for (const auto &[cat, ms] : cats)
        order.emplace_back(ms, cat);
      std::sort(order.begin(), order.end(), std::greater<>());
      std::string line;
      for (size_t i = 0; i < order.size() && i < 8; ++i)
        line += std::format(" {} {:.2f}", GpuCategoryName(order[i].second), order[i].first);
      EOT_WARN("[hitch-gpu] frame {} took {:.2f} ms on the GPU (average {:.2f}):{}{}", s.guest_frames,
               total, average, line, AdapterSensorsSummary());
      static u32 diag_armed = 0;
      static u64 diag_next = 0;
      const i32 from = Settings::DiagHitch();
      bool transfers_on_top = Settings::DiagHitchAny();
      for (size_t i = 0; i < order.size() && i < 2; ++i)
        transfers_on_top |= order[i].second == kGpuCatBroadcast || order[i].second == kGpuCatResolveDepth;
      if (from > 0 && diag_armed < 4 && transfers_on_top && s.guest_frames >= static_cast<u64>(from) &&
          s.guest_frames >= diag_next && Settings::DiagFrame() < static_cast<i32>(s.guest_frames)) {
        diag_armed++;
        diag_next = s.guest_frames + 300;
        Settings::ArmDiagFrame(static_cast<i32>(s.guest_frames + 2));
        EOT_INFO("[hitch-gpu] diagnostic frame armed for guest frame {} ({} of 4)",
                 s.guest_frames + 2, diag_armed);
      }
    }
    average = average > 0.0 ? average * 0.98 + total * 0.02 : total;
  }
  if (st.diag && Settings::Record()) {
    struct Sum {
      f64 us = 0.0;
      u32 count = 0;
      f64 max = 0.0;
    };
    std::map<std::string, Sum> sums;
    f64 tagged = 0;
    u32 items = 0;
    for (size_t i = 1; i < n && i < st.tags.size(); ++i) {
      if (st.tags[i].empty())
        continue;
      const f64 us = r[i] > r[i - 1] ? (r[i] - r[i - 1]) * 1e-3 : 0.0;
      tagged += us;
      items++;
      Sum &sum = sums[st.tags[i]];
      sum.us += us;
      sum.count++;
      sum.max = std::max(sum.max, us);
    }
    std::vector<std::pair<std::string, Sum>> order(sums.begin(), sums.end());
    std::sort(order.begin(), order.end(),
              [](const auto &a, const auto &b) { return a.second.us > b.second.us; });
    EOT_INFO("[record] frame {}: {:.3f} ms of items, {} items, {} tags{}", s.guest_frames,
             tagged * 1e-3, items, order.size(), st.used >= kQueryCount ? " (saturated)" : "");
    for (size_t i = 0; i < order.size() && i < 80; ++i)
      EOT_INFO("[record]   {:.1f} us x{} max {:.1f}: {}", order[i].second.us, order[i].second.count,
               order[i].second.max, order[i].first);
    st.diag = false;
    st.tags.clear();
  } else if (st.diag) {
    std::string untagged_note;
    f64 tagged = 0;
    u32 items = 0;
    for (size_t i = 1; i < n && i < st.tags.size(); ++i) {
      if (st.tags[i].empty())
        continue;
      const f64 us = r[i] > r[i - 1] ? (r[i] - r[i - 1]) * 1e-3 : 0.0;
      tagged += us;
      items++;
      EOT_INFO("[diag-gpu] {} {:.1f} us", st.tags[i], us);
    }
    EOT_INFO("[diag-gpu] frame {:.3f} ms, {} items {:.3f} ms, {} queries{}", (r[n - 1] - r[0]) * 1e-6,
             items, tagged * 1e-3, st.used, st.used >= kQueryCount ? " (saturated)" : "");
    st.diag = false;
    st.tags.clear();
  }
}

std::string GpuTimingSummary(const PerfCounters &p) {
  if (!g_supported)
    return "n/a";
  if (p.gpu_frames == 0)
    return "0";
  const f64 n = static_cast<f64>(p.gpu_frames);
  std::vector<std::pair<u32, std::pair<f64, u32>>> cats(p.gpu_cats.begin(), p.gpu_cats.end());
  std::sort(cats.begin(), cats.end(),
            [](const auto &a, const auto &b) { return a.second.first > b.second.first; });
  std::string out = std::format("{:.2f} ms/frame:", p.gpu_ms / n);
  u32 shown = 0;
  for (const auto &[cat, v] : cats) {
    if (v.first / n < 0.02 && shown >= 4)
      break;
    out += std::format(" {} {:.2f}", GpuCategoryName(cat), v.first / n);
    if (v.second)
      out += std::format(" ({} draws)", static_cast<u32>(v.second / std::max(1u, p.frames)));
    if (++shown >= 10)
      break;
  }
  return out + AdapterSensorsSummary();
}

}

#if EOT_GPU_PROFILING

namespace eot::gpu {

namespace {
TracyD3D12Ctx g_ctx = nullptr;
ID3D12GraphicsCommandList *g_cmd = nullptr;
}

void InitGPUProfiler(ID3D12Device *device, ID3D12CommandQueue *queue) {
  if (!TracyIsStarted || g_ctx)
    return;
  g_ctx = TracyD3D12Context(device, queue);
  TracyD3D12ContextName(g_ctx, "D3D12", 5);
}

TracyD3D12Ctx GpuProfilerCtx() { return g_ctx; }

void SetGPUProfilerCommandList(ID3D12GraphicsCommandList *cmd) { g_cmd = cmd; }

ID3D12GraphicsCommandList *GpuProfilerCommandList() { return g_cmd; }

}

#endif

namespace eot::gpu {

namespace {
std::mutex g_mutex;
LiveStats g_stats;
bool g_published = false;
}

void PublishLiveStats(const LiveStats &stats) {
  std::lock_guard lock(g_mutex);
  const u64 sequence = g_stats.sequence + 1;
  g_stats = stats;
  g_stats.sequence = sequence;
  g_published = true;
}

bool ReadLiveStats(LiveStats *out) {
  std::lock_guard lock(g_mutex);
  if (!g_published)
    return false;
  *out = g_stats;
  return true;
}

}

namespace eot::gpu {

#if defined(EOT_D3D12)
namespace {

using OpenAdapterFromLuidFn = NTSTATUS(APIENTRY *)(D3DKMT_OPENADAPTERFROMLUID *);
using QueryAdapterInfoFn = NTSTATUS(APIENTRY *)(const D3DKMT_QUERYADAPTERINFO *);

struct Sensors {
  std::once_flag open;
  QueryAdapterInfoFn query = nullptr;
  D3DKMT_HANDLE adapter = 0;
  bool reported_failure = false;
};
Sensors g_sensors;

void Open() {
  VideoState &s = state();
  auto *device = static_cast<plume::D3D12Device *>(s.device.get());
  if (!device || !device->d3d)
    return;
  HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
  if (!gdi)
    gdi = LoadLibraryW(L"gdi32.dll");
  if (!gdi)
    return;
  auto open = reinterpret_cast<OpenAdapterFromLuidFn>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid"));
  auto query = reinterpret_cast<QueryAdapterInfoFn>(GetProcAddress(gdi, "D3DKMTQueryAdapterInfo"));
  if (!open || !query)
    return;
  D3DKMT_OPENADAPTERFROMLUID request{};
  request.AdapterLuid = device->d3d->GetAdapterLuid();
  if (open(&request) != 0) {
    EOT_DEBUG("[gpu-sensors] the kernel would not open the adapter; no clock or temperature readings");
    return;
  }
  g_sensors.adapter = request.hAdapter;
  g_sensors.query = query;
}

template <typename T> bool Query(KMTQUERYADAPTERINFOTYPE type, T &data, NTSTATUS &status) {
  D3DKMT_QUERYADAPTERINFO info{};
  info.hAdapter = g_sensors.adapter;
  info.Type = type;
  info.pPrivateDriverData = &data;
  info.PrivateDriverDataSize = sizeof(data);
  status = g_sensors.query(&info);
  return status == 0;
}

}

std::string AdapterSensorsSummary() {
  std::call_once(g_sensors.open, Open);
  if (!g_sensors.query)
    return {};
  D3DKMT_NODE_PERFDATA node{};
  D3DKMT_ADAPTER_PERFDATA adapter{};
  NTSTATUS node_status = 0, adapter_status = 0;
  const bool have_node = Query(KMTQAITYPE_NODEPERFDATA, node, node_status);
  const bool have_adapter = Query(KMTQAITYPE_ADAPTERPERFDATA, adapter, adapter_status);
  if (!have_node && !have_adapter) {
    if (!g_sensors.reported_failure) {
      g_sensors.reported_failure = true;
      EOT_DEBUG("[gpu-sensors] the driver reports no performance data (status {:#x} / {:#x})",
                static_cast<u32>(node_status), static_cast<u32>(adapter_status));
    }
    return {};
  }
  auto mhz = [](u64 f) { return f >= 1000000 ? f / 1000000 : f; };
  std::string out;
  auto add = [&](const std::string &field) { out += (out.empty() ? " | " : ", ") + field; };
  if (have_node && node.Frequency)
    add(std::format("clock {}/{} MHz", mhz(node.Frequency),
                    mhz(node.MaxFrequency ? node.MaxFrequency : node.MaxFrequencyOC)));
  if (have_node && node.Voltage)
    add(std::format("{} mV", node.Voltage));
  if (have_adapter && adapter.MemoryFrequency)
    add(std::format("memory {} MHz", mhz(adapter.MemoryFrequency)));
  if (have_adapter && adapter.Temperature)
    add(std::format("{:.1f} C", adapter.Temperature / 10.0));
  if (have_adapter && adapter.Power)
    add(std::format("power {:.1f}%", adapter.Power / 10.0));
  return out;
}
#else
std::string AdapterSensorsSummary() { return {}; }
#endif

}

namespace eot::gpu {
namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kForgetAfter = std::chrono::seconds(120);

std::mutex g_lock;
std::unordered_map<u32, Clock::time_point> g_resident;
std::unordered_set<u64> g_seen;
std::vector<u32> g_released;
std::deque<u32> g_preload;
std::unordered_set<u32> g_preload_wanted;
std::unordered_map<u64, u32> g_unannounced;

}

void NoteTextureResident(u32 header_va) {
  if (!header_va)
    return;
  const auto now = Clock::now();
  std::lock_guard<std::mutex> guard(g_lock);
  g_resident[header_va] = now;
  if (g_preload_wanted.insert(header_va).second)
    g_preload.push_back(header_va);
  if (g_resident.size() > 16384)
    std::erase_if(g_resident, [&](const auto &kv) { return now - kv.second > kForgetAfter; });
}

bool TakeTextureResidentAge(u32 header_va, f64 &age_ms) {
  const auto now = Clock::now();
  std::lock_guard<std::mutex> guard(g_lock);
  const auto it = g_resident.find(header_va);
  if (it == g_resident.end())
    return false;
  const auto age = now - it->second;
  g_resident.erase(it);
  if (age > kForgetAfter)
    return false;
  age_ms = std::chrono::duration<f64, std::milli>(age).count();
  return true;
}

bool UploadSeenBefore(u64 storage_key) {
  std::lock_guard<std::mutex> guard(g_lock);
  return !g_seen.insert(storage_key).second;
}

void NoteTextureReleased(u32 header_va) {
  if (!header_va)
    return;
  std::lock_guard<std::mutex> guard(g_lock);
  g_resident.erase(header_va);
  g_preload_wanted.erase(header_va);
  if (g_released.size() < 65536)
    g_released.push_back(header_va);
}

bool TakeAnnouncedHeader(u32 &header_va) {
  std::lock_guard<std::mutex> guard(g_lock);
  while (!g_preload.empty()) {
    const u32 va = g_preload.front();
    g_preload.pop_front();
    if (g_preload_wanted.erase(va)) {
      header_va = va;
      return true;
    }
  }
  return false;
}

void TakeReleasedTextures(std::vector<u32> &out) {
  std::lock_guard<std::mutex> guard(g_lock);
  out.insert(out.end(), g_released.begin(), g_released.end());
  g_released.clear();
}

void NoteUnannouncedUpload(u32 width, u32 height, u32 guest_format, bool tiled) {
  const u64 key = (u64(width) << 40) | (u64(height) << 16) | (u64(guest_format) << 1) | (tiled ? 1 : 0);
  std::lock_guard<std::mutex> guard(g_lock);
  g_unannounced[key]++;
}

std::string TakeUnannouncedShapes() {
  std::vector<std::pair<u32, u64>> shapes;
  {
    std::lock_guard<std::mutex> guard(g_lock);
    for (const auto &[key, count] : g_unannounced)
      shapes.emplace_back(count, key);
    g_unannounced.clear();
  }
  std::sort(shapes.begin(), shapes.end(), std::greater<>());
  std::string out;
  for (size_t i = 0; i < shapes.size() && i < 6; ++i) {
    const u64 key = shapes[i].second;
    out += std::format("{}{}x{} f{}{} x{}", out.empty() ? "" : ", ", u32(key >> 40), u32((key >> 16) & 0xFFFFFF),
                       u32((key >> 1) & 0x7FFF), (key & 1) ? "" : " linear", shapes[i].first);
  }
  return out;
}

}
