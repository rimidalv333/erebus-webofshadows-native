// gpu/memory_report.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "gpu/memory_report.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <format>
#include <map>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include <plume_render_interface.h>
#if defined(EOT_D3D12)
#include <plume_d3d12.h>
#include <dxgi1_4.h>
#include <windows.h>
#include <psapi.h>
#endif

#include "core/logging.h"
#include "core/profiling.h"
#include "gpu/render_thread.h"
#include "gpu/device.h"
#include "gpu/draw.h"
#include "gpu/format.h"
#include "gpu/pipeline/pipeline_cache.h"
#include "gpu/settings.h"
#include "gpu/surfaces.h"
#include "gpu/taa.h"

namespace eot::gpu {
namespace {

constexpr uint64_t kMB = 1024ull * 1024ull;
constexpr u64 kBigAllocation = 16 * kMB;
constexpr auto kBudgetInterval = std::chrono::seconds(1);
constexpr auto kOverBudgetRepeat = std::chrono::seconds(30);
constexpr u64 kPlotFrames = 30;

const char *FormatName(plume::RenderFormat format) {
  switch (format) {
  case plume::RenderFormat::R32G32B32A32_FLOAT: return "RGBA32F";
  case plume::RenderFormat::R16G16B16A16_FLOAT: return "RGBA16F";
  case plume::RenderFormat::R16G16B16A16_UNORM: return "RGBA16";
  case plume::RenderFormat::R32G32_FLOAT: return "RG32F";
  case plume::RenderFormat::R8G8B8A8_UNORM: return "RGBA8";
  case plume::RenderFormat::B8G8R8A8_UNORM: return "BGRA8";
  case plume::RenderFormat::R16G16_FLOAT: return "RG16F";
  case plume::RenderFormat::R16G16_UNORM: return "RG16";
  case plume::RenderFormat::D32_FLOAT: return "D32";
  case plume::RenderFormat::R32_TYPELESS: return "D32";
  case plume::RenderFormat::D32_FLOAT_S8_UINT: return "D32S8";
  case plume::RenderFormat::R32_FLOAT: return "R32F";
  case plume::RenderFormat::R32_UINT: return "R32U";
  case plume::RenderFormat::R8G8_UNORM: return "RG8";
  case plume::RenderFormat::R16_FLOAT: return "R16F";
  case plume::RenderFormat::R16_UNORM: return "R16";
  case plume::RenderFormat::R8_UNORM: return "R8";
  case plume::RenderFormat::BC1_UNORM: return "BC1";
  case plume::RenderFormat::BC2_UNORM: return "BC2";
  case plume::RenderFormat::BC3_UNORM: return "BC3";
  case plume::RenderFormat::BC4_UNORM: return "BC4";
  case plume::RenderFormat::BC5_UNORM: return "BC5";
  default: return nullptr;
  }
}

std::string Shape(const plume::RenderTextureDesc &desc) {
  std::string shape = std::format("{}x{}", desc.width, desc.height);
  if (desc.depth > 1 || desc.arraySize > 1)
    shape += std::format("x{}", std::max<uint32_t>(desc.depth, desc.arraySize));
  const uint32_t samples = static_cast<uint32_t>(desc.multisampling.sampleCount);
  if (samples > 1)
    shape += std::format(" {}x", samples);
  if (desc.mipLevels > 1)
    shape += std::format(" m{}", desc.mipLevels);
  const char *format = FormatName(desc.format);
  shape += format ? std::format(" {}", format) : std::format(" f{}", static_cast<uint32_t>(desc.format));
  return shape;
}

u64 Mb(u64 bytes) { return (bytes + kMB / 2) / kMB; }
double MbF(u64 bytes) { return static_cast<double>(bytes) / static_cast<double>(kMB); }

std::string Narrow(const wchar_t *w) {
  std::string s;
  for (; w && *w; ++w)
    s.push_back(*w < 128 ? static_cast<char>(*w) : '?');
  return s;
}

std::string TagOf(const std::string &name) { return name.substr(0, name.find(' ')); }

#if defined(EOT_D3D12)
D3D12MA::Allocation *AllocationOf(const plume::RenderTexture *texture) {
  return texture ? static_cast<const plume::D3D12Texture *>(texture)->allocation : nullptr;
}
D3D12MA::Allocation *AllocationOf(const plume::RenderBuffer *buffer) {
  return buffer ? static_cast<const plume::D3D12Buffer *>(buffer)->allocation : nullptr;
}

void Tag(D3D12MA::Allocation *allocation, const std::string &tag) {
  if (!allocation || tag.empty())
    return;
  std::wstring wide;
  for (char c : tag)
    wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
  allocation->SetName(wide.c_str());
}
#endif

u64 EstimateBytes(plume::RenderFormat format, u32 width, u32 height, u32 layers, u32 mips, u32 samples) {
  const u32 block = std::max(1u, plume::RenderFormatBlockWidth(format));
  const u64 blocks = u64((width + block - 1) / block) * ((height + block - 1) / block);
  u64 bytes = blocks * plume::RenderFormatSize(format) * std::max(1u, layers) * std::max(1u, samples);
  if (mips > 1)
    bytes += bytes / 3;
  return bytes;
}

struct Tally {
  u64 bytes = 0;
  u32 count = 0;
};

struct Churn {
  std::mutex mutex;
  std::map<std::string, Tally> made, released;
  std::unordered_set<std::string> bigShapes;
};

Churn &churn() {
  static Churn c;
  return c;
}

void NoteMade(const std::string &name, u64 bytes, bool announce) {
  bool first = false;
  {
    Churn &c = churn();
    std::lock_guard lock(c.mutex);
    Tally &t = c.made[TagOf(name)];
    t.bytes += bytes;
    t.count++;
    if (announce && bytes >= kBigAllocation)
      first = c.bigShapes.insert(name).second;
  }
  if (!announce || bytes < kBigAllocation)
    return;
#if defined(EOT_PROFILING) && defined(REXGLUE_ENABLE_PROFILING)
  if (TracyIsStarted) {
    const std::string message = std::format("vram +{} MB {}", Mb(bytes), name);
    TracyMessage(message.c_str(), message.size());
  }
#endif
  if (first)
    EOT_DEBUG("[vram-alloc] +{} MB {} (the first of this shape; guest frame {})", Mb(bytes), name,
              state().guest_frames);
}

void NoteReleased(const std::string &name, u64 bytes) {
  Churn &c = churn();
  std::lock_guard lock(c.mutex);
  Tally &t = c.released[TagOf(name)];
  t.bytes += bytes;
  t.count++;
}

struct Bucket {
  u64 bytes = 0;
  u32 count = 0;
  void Add(u64 b) {
    bytes += b;
    count++;
  }
};

struct Census {
  Bucket frameMs, frameMsUndrawn, frameMsDeferred, frameTwin, frame1x, shadowTile, pipSurface, otherSurface;
  Bucket mirrorFrame, mirrorAtlas, mirrorPip, mirrorOther;
  Bucket textures, pool, velocity, taa;
  f64 poolOldestSeconds = 0.0;
  u64 transientHeapBytes = 0, transientUsedBytes = 0;
  u32 transientHeaps = 0;
  GeometryCacheSizes geometry;
  u64 uploadRing = 0, velocityHistory = 0;
  u32 uploadRingChunks = 0, velocityHistoryChunks = 0;
  u32 pipelines = 0, pipelinesUsed = 0;

  u64 Surfaces() const {
    return frameMs.bytes + frameTwin.bytes + frame1x.bytes + shadowTile.bytes + pipSurface.bytes +
           otherSurface.bytes;
  }
  u64 Mirrors() const { return mirrorFrame.bytes + mirrorAtlas.bytes + mirrorPip.bytes + mirrorOther.bytes; }
  u64 Images() const { return Surfaces() + Mirrors() + textures.bytes + pool.bytes + velocity.bytes + taa.bytes; }
  u64 VramBuffers() const { return geometry.indexVram + geometry.vertexVram; }
  u64 UploadBuffers() const {
    return geometry.indexUpload + geometry.vertexUpload + uploadRing + velocityHistory;
  }
};

bool FrameSized(const GuestSurface &surf) {
  return surf.width == kGuestRenderWidth && surf.height == kGuestRenderHeight;
}
bool ShadowTile(const GuestSurface &surf) { return IsShadowTile(surf); }

bool NearFrameFraction(u32 w, u32 h, u32 k) {
  const i32 fw = static_cast<i32>(kGuestRenderWidth >> k), fh = static_cast<i32>(kGuestRenderHeight >> k);
  return std::abs(static_cast<i32>(w) - fw) <= 2 && std::abs(static_cast<i32>(h) - fh) <= 2;
}

bool IsMirror(const GuestTexture &t) {
#if defined(EOT_D3D12)
  if (const D3D12MA::Allocation *a = AllocationOf(t.host.texture.get()))
    if (const wchar_t *name = a->GetName())
      return std::wcsncmp(name, L"resolve-mirror", 14) == 0;
#endif
  return t.resolveOwned || t.host.isDepth;
}

Census TakeCensus(VideoState &s) {
  EOT_CPU_ZONE("vram census");
  Census c;
  for (const auto &[key, slot] : s.surfaces) {
    if (!slot)
      continue;
    const GuestSurface &surf = *slot;
    const u64 host = HostTextureBytes(surf.host), single = HostTextureBytes(surf.single);
    if (ShadowTile(surf)) {
      c.shadowTile.Add(host + single);
    } else if (FrameSized(surf)) {
      if (surf.host.sampleCount > 1 && !surf.host.valid()) {
        c.frameMsDeferred.Add(EstimateBytes(surf.host.format, surf.host.width, surf.host.height, 1, 1,
                                            surf.host.sampleCount));
      } else if (surf.host.sampleCount > 1) {
        c.frameMs.Add(host);
        if (surf.hostDraws == 0)
          c.frameMsUndrawn.Add(host);
      } else {
        c.frame1x.Add(host);
      }
      if (single)
        c.frameTwin.Add(single);
    } else if (IsTextureCameraSurface(surf)) {
      c.pipSurface.Add(host + single);
    } else {
      c.otherSurface.Add(host + single);
    }
  }
  std::unordered_set<const plume::RenderTexture *> seen;
  seen.reserve(s.textures.size());
  for (const auto &[va, t] : s.textures) {
    if (!t || !t->host.texture || !seen.insert(t->host.texture.get()).second)
      continue;
    const u64 bytes = HostTextureBytes(t->host);
    if (!IsMirror(*t)) {
      c.textures.Add(bytes);
      continue;
    }
    if (t->host.isDepth && (t->width >= 2048 || t->height >= 2048))
      c.mirrorAtlas.Add(bytes);
    else if (NearFrameFraction(t->width, t->height, 0))
      c.mirrorFrame.Add(bytes);
    else if (NearFrameFraction(t->width, t->height, 1) || NearFrameFraction(t->width, t->height, 2) ||
             NearFrameFraction(t->width, t->height, 3) || t->width <= 256 || t->height <= 256)
      c.mirrorOther.Add(bytes);
    else
      c.mirrorPip.Add(bytes);
  }
  for (const auto &entry : s.host_texture_pool) {
    c.pool.Add(HostTextureBytes(entry.host));
    c.poolOldestSeconds = std::max(c.poolOldestSeconds, FrameAgeSeconds(s, entry.freedFrame));
  }
#if defined(EOT_D3D12)
  if (s.transient_mirror_pool) {
    if (D3D12MA::Pool *pool = static_cast<plume::D3D12Pool *>(s.transient_mirror_pool.get())->d3d) {
      D3D12MA::DetailedStatistics stats{};
      pool->CalculateStatistics(&stats);
      c.transientHeapBytes = stats.Stats.BlockBytes;
      c.transientUsedBytes = stats.Stats.AllocationBytes;
      c.transientHeaps = stats.Stats.BlockCount;
    }
  }
#endif
  velocity::ForEachImage([&](const HostTexture &h) { c.velocity.Add(HostTextureBytes(h)); });
  taa::ForEachImage([&](const HostTexture &h) { c.taa.Add(HostTextureBytes(h)); });
  GeometryCacheBytes(&c.geometry);
  c.uploadRing = UploadRingCapacityBytes(&c.uploadRingChunks);
  c.velocityHistory = velocity::HistoryArenaBytes(&c.velocityHistoryChunks);
  PipelineCacheCounts(&c.pipelines, &c.pipelinesUsed);
  return c;
}

std::string CensusLine(const Census &c, u32 samples) {
  std::string frame;
  if (c.frameMs.count || c.frameMsDeferred.count)
    frame = std::format("frame {}x {} MB ({}) of which {} MB ({}) never drawn, {} not made yet (about {} MB), "
                        "their twins {} MB ({})",
                        samples, Mb(c.frameMs.bytes), c.frameMs.count, Mb(c.frameMsUndrawn.bytes),
                        c.frameMsUndrawn.count, c.frameMsDeferred.count, Mb(c.frameMsDeferred.bytes),
                        Mb(c.frameTwin.bytes), c.frameTwin.count);
  if (c.frame1x.count || (!c.frameMs.count && !c.frameMsDeferred.count))
    frame += std::format("{}frame single-sample {} MB ({})", frame.empty() ? "" : ", ", Mb(c.frame1x.bytes),
                         c.frame1x.count);
  std::string line = std::format(
      "images {} MB: surfaces {} MB = {}, shadow tile {} MB, picture-in-picture {} MB ({}), other {} MB ({})",
      Mb(c.Images()), Mb(c.Surfaces()), frame, Mb(c.shadowTile.bytes), Mb(c.pipSurface.bytes), c.pipSurface.count,
      Mb(c.otherSurface.bytes), c.otherSurface.count);
  line += std::format(" | resolve mirrors {} MB = frame {} MB ({}), shadow atlas {} MB ({}), picture-in-picture "
                      "{} MB ({}), other {} MB ({})",
                      Mb(c.Mirrors()), Mb(c.mirrorFrame.bytes), c.mirrorFrame.count, Mb(c.mirrorAtlas.bytes),
                      c.mirrorAtlas.count, Mb(c.mirrorPip.bytes), c.mirrorPip.count, Mb(c.mirrorOther.bytes),
                      c.mirrorOther.count);
  line += std::format(" | textures {} MB ({}) | recycle list {} MB ({}, oldest {:.0f} s) | temporal history {} MB "
                      "({}), motion vectors {} MB ({})",
                      Mb(c.textures.bytes), c.textures.count, Mb(c.pool.bytes), c.pool.count, c.poolOldestSeconds,
                      Mb(c.taa.bytes), c.taa.count, Mb(c.velocity.bytes), c.velocity.count);
  line += std::format(" | picture-in-picture heaps {} MB in {}, {} MB of it in use", Mb(c.transientHeapBytes),
                      c.transientHeaps, Mb(c.transientUsedBytes));
  return line;
}

std::string BufferLine(const Census &c) {
  return std::format("buffers: index cache {} MB VRAM + {} MB upload heap ({} chunks), vertex mirrors {} MB VRAM + "
                     "{} MB upload heap ({} chunks), upload ring {} MB ({} chunks), motion-vector history {} MB "
                     "upload heap ({} chunks) | {} pipelines, {} of them drawn with",
                     Mb(c.geometry.indexVram), Mb(c.geometry.indexUpload), c.geometry.indexChunks,
                     Mb(c.geometry.vertexVram), Mb(c.geometry.vertexUpload), c.geometry.vertexChunks,
                     Mb(c.uploadRing), c.uploadRingChunks, Mb(c.velocityHistory), c.velocityHistoryChunks,
                     c.pipelines, c.pipelinesUsed);
}

std::string SurfaceName(const GuestSurface &surf) {
  return std::format("{}{}:t{}", surf.isDepth ? 'z' : 'c', surf.isDepth ? surf.depthFormat : surf.colorFormat,
                     surf.baseTile);
}

std::string FrameAgo(const VideoState &s, u64 frame) {
  if (s.guest_frames - std::min(s.guest_frames, frame) < kFrameClockRing)
    return std::format("{:.0f} s ago", FrameAgeSeconds(s, frame));
  return std::format("{} frames ago", s.guest_frames - frame);
}

std::string SurfaceLine(VideoState &s) {
  struct Row {
    const GuestSurface *surf;
    u64 host, single;
  };
  std::vector<Row> rows;
  for (const auto &[key, slot] : s.surfaces)
    if (slot)
      rows.push_back({slot.get(), HostTextureBytes(slot->host), HostTextureBytes(slot->single)});
  std::sort(rows.begin(), rows.end(),
            [](const Row &a, const Row &b) { return a.host + a.single > b.host + b.single; });
  std::string line;
  u32 listed = 0;
  u64 rest = 0;
  u32 rest_count = 0;
  for (const Row &r : rows) {
    const GuestSurface &surf = *r.surf;
    if (r.host + r.single < kMB || listed >= 40) {
      rest += r.host + r.single;
      rest_count++;
      continue;
    }
    listed++;
    const char *format = FormatName(surf.host.format);
    const bool deferred = surf.host.sampleCount > 1 && !surf.host.valid();
    std::string entry = std::format("{} {}x{} as {}x{} {}{} {}", SurfaceName(surf), surf.width, surf.height,
                                    surf.host.width, surf.host.height, format ? format : "?",
                                    surf.host.sampleCount > 1 ? std::format(" {}x", surf.host.sampleCount) : "",
                                    deferred ? std::string("not made") : std::format("{} MB", Mb(r.host)));
    if (r.single)
      entry += std::format(" + 1x {} MB", Mb(r.single));
    entry += std::format(", draws {}{}", surf.hostDraws,
                         surf.single.valid() ? std::format(" + {} on the 1x", surf.singleDraws) : "");
    if (surf.host.sampleCount > 1 && surf.hostDraws == 0 && !deferred)
      entry += std::format(" (the {}x image never drawn)", surf.host.sampleCount);
    entry += std::format(", made {}", FrameAgo(s, surf.createdFrame));
    line += (line.empty() ? "" : "; ") + entry;
  }
  if (rest_count)
    line += std::format("; {} more under 1 MB: {} MB", rest_count, Mb(rest));
  return std::format("{} surfaces: {}", rows.size(), line);
}

std::string ChurnLine() {
  Churn &c = churn();
  std::map<std::string, Tally> made, released;
  {
    std::lock_guard lock(c.mutex);
    made.swap(c.made);
    released.swap(c.released);
  }
  auto list = [](const std::map<std::string, Tally> &m) {
    std::vector<std::pair<std::string, Tally>> v(m.begin(), m.end());
    std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) { return a.second.bytes > b.second.bytes; });
    std::string out;
    for (const auto &[tag, t] : v)
      out += std::format("{}{} {} ({} MB)", out.empty() ? "" : ", ", tag, t.count, Mb(t.bytes));
    return out.empty() ? std::string("nothing") : out;
  };
  return std::format("since the last report: made {} | let go {}", list(made), list(released));
}

#if defined(EOT_D3D12)
struct Allocation {
  std::wstring heap;
  std::wstring type;
  std::wstring name;
  uint64_t size = 0;
  uint32_t usage = 0;
};

const wchar_t *ReadValue(const wchar_t *p, std::wstring &text, uint64_t &number) {
  while (*p == L' ' || *p == L':' || *p == L'\n' || *p == L'\r' || *p == L'\t')
    ++p;
  text.clear();
  number = 0;
  if (*p == L'"') {
    ++p;
    while (*p && *p != L'"') {
      if (*p == L'\\' && p[1])
        ++p;
      text.push_back(*p++);
    }
    return *p ? p + 1 : p;
  }
  while (*p >= L'0' && *p <= L'9')
    number = number * 10 + uint64_t(*p++ - L'0');
  return p;
}

std::vector<Allocation> ParseAllocations(const wchar_t *json) {
  static const wchar_t *const kHeaps[] = {L"\"DEFAULT\"", L"\"UPLOAD\"", L"\"READBACK\"", L"\"GPU_UPLOAD\"",
                                          L"\"CUSTOM\""};
  std::vector<Allocation> out;
  std::wstring heap = L"?";
  for (const wchar_t *p = json; *p; ++p) {
    if (*p != L'"')
      continue;
    for (const wchar_t *h : kHeaps) {
      const size_t n = std::wcslen(h);
      if (std::wcsncmp(p, h, n) == 0 && p[n] == L':') {
        heap.assign(h + 1, n - 2);
        break;
      }
    }
    if (std::wcsncmp(p, L"\"Type\"", 6) != 0)
      continue;
    Allocation a;
    a.heap = heap;
    const wchar_t *q = p;
    std::wstring key, text;
    uint64_t number = 0;
    while (*q && *q != L'}') {
      if (*q != L'"') {
        ++q;
        continue;
      }
      q = ReadValue(q, key, number);
      q = ReadValue(q, text, number);
      if (key == L"Type")
        a.type = text;
      else if (key == L"Size")
        a.size = number;
      else if (key == L"Usage")
        a.usage = static_cast<uint32_t>(number);
      else if (key == L"Name")
        a.name = text;
    }
    out.push_back(std::move(a));
    p = *q ? q : q - 1;
  }
  return out;
}

std::string Narrow(const std::wstring &w) { return Narrow(w.c_str()); }

struct OsView {
  DXGI_QUERY_VIDEO_MEMORY_INFO local{}, shared{};
  bool valid = false;
};

OsView QueryOs(plume::D3D12Device *device) {
  OsView v;
  IDXGIAdapter3 *adapter3 = nullptr;
  if (device && device->adapter && SUCCEEDED(device->adapter->QueryInterface(IID_PPV_ARGS(&adapter3)))) {
    v.valid = SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &v.local)) &&
              SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &v.shared));
    adapter3->Release();
  }
  return v;
}

u64 g_peak_local = 0;

void WriteCsv(VideoState &s, const std::vector<Allocation> &allocations) {
  const std::filesystem::path log_file = rex::LoggingConfig().log_file;
  const std::filesystem::path dir = log_file.empty() ? std::filesystem::path("logs") : log_file.parent_path();
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const std::string stem = (dir / std::format("vram_{}", s.guest_frames)).string();
  if (FILE *f = std::fopen((stem + "_allocations.csv").c_str(), "w")) {
    std::fprintf(f, "heap,type,tag,name,bytes,usage\n");
    for (const Allocation &a : allocations) {
      const std::string name = Narrow(a.name);
      std::fprintf(f, "%s,%s,%s,%s,%llu,%u\n", Narrow(a.heap).c_str(), Narrow(a.type).c_str(),
                   TagOf(name).c_str(), name.c_str(), static_cast<unsigned long long>(a.size), a.usage);
    }
    std::fclose(f);
  }
  if (FILE *f = std::fopen((stem + "_surfaces.csv").c_str(), "w")) {
    std::fprintf(f, "descriptor,guest_width,guest_height,guest_samples,host_width,host_height,host_format,"
                    "host_samples,host_bytes,single_bytes,host_draws,single_draws,age_frames,kind\n");
    for (const auto &[key, slot] : s.surfaces) {
      if (!slot)
        continue;
      const GuestSurface &surf = *slot;
      const char *format = FormatName(surf.host.format);
      const char *kind = ShadowTile(surf)                 ? "shadow-tile"
                         : FrameSized(surf)               ? "frame"
                         : IsTextureCameraSurface(surf)   ? "picture-in-picture"
                                                          : "other";
      std::fprintf(f, "%s,%u,%u,%u,%u,%u,%s,%u,%llu,%llu,%llu,%llu,%llu,%s\n", SurfaceName(surf).c_str(),
                   surf.width, surf.height, surf.msaaSamples, surf.host.width, surf.host.height,
                   format ? format : "?", surf.host.sampleCount,
                   static_cast<unsigned long long>(HostTextureBytes(surf.host)),
                   static_cast<unsigned long long>(HostTextureBytes(surf.single)),
                   static_cast<unsigned long long>(surf.hostDraws), static_cast<unsigned long long>(surf.singleDraws),
                   static_cast<unsigned long long>(s.guest_frames - std::min(s.guest_frames, surf.createdFrame)),
                   kind);
    }
    std::fclose(f);
  }
  EOT_INFO("[vram] wrote {}_allocations.csv and {}_surfaces.csv", stem, stem);
}

void LogModel(VideoState &s) {
  auto *device = static_cast<plume::D3D12Device *>(s.device.get());
  if (!device || !device->d3d)
    return;
  const u32 w = ScaleDim(kGuestRenderWidth), h = ScaleDim(kGuestRenderHeight);
  const u32 samples = std::max(1u, s.host_msaa_samples);
  auto size = [&](DXGI_FORMAT format, u32 width, u32 height, u32 count, bool depth) -> u64 {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = width;
    d.Height = height;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = format;
    d.SampleDesc.Count = count;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const D3D12_RESOURCE_ALLOCATION_INFO info = device->d3d->GetResourceAllocationInfo(0, 1, &d);
    return info.SizeInBytes == ~0ull ? 0 : info.SizeInBytes;
  };
  const DXGI_FORMAT f16 = DXGI_FORMAT_R16G16B16A16_FLOAT, u16 = DXGI_FORMAT_R16G16B16A16_UNORM,
                    u8 = DXGI_FORMAT_R8G8B8A8_UNORM, ds = DXGI_FORMAT_R32G8X24_TYPELESS,
                    rg16f = DXGI_FORMAT_R16G16_FLOAT;
  const u64 ms_f16 = size(f16, w, h, samples, false), ms_u16 = size(u16, w, h, samples, false),
            ms_u8 = size(u8, w, h, samples, false), ms_ds = size(ds, w, h, samples, true);
  const u64 s_f16 = size(f16, w, h, 1, false), s_u16 = size(u16, w, h, 1, false), s_u8 = size(u8, w, h, 1, false),
            s_ds = size(ds, w, h, 1, true);
  const u32 shadow_scale = static_cast<u32>(ShadowMapTargetScale());
  const u32 tile = 1024 * shadow_scale, atlas = 2048 * shadow_scale;
  const DXGI_FORMAT shadow_ds =
      FormatHasStencil(ShadowDepthFormat()) ? ds : DXGI_FORMAT_R32_TYPELESS;
  const u64 tile_bytes = size(shadow_ds, tile, tile, 1, true);
  const u64 atlas_bytes = atlas <= 16384 ? size(shadow_ds, atlas, atlas, 1, true) : 0;
  const u64 d32_one = size(DXGI_FORMAT_R32_TYPELESS, 1024, 1024, 1, true);
  const f64 ds_texel = static_cast<f64>(size(ds, 1024, 1024, 1, true)) / (1024.0 * 1024.0);
  const u64 set_ms = samples > 1 ? 2 * ms_f16 + ms_u16 + 3 * ms_u8 + 3 * ms_ds : 0;
  const u64 set_1x = 2 * s_f16 + s_u16 + 3 * s_u8 + 3 * s_ds;
  const u64 set_mirrors = 3 * s_f16 + 3 * s_u16 + 3 * s_u8 + s_ds;
  const u64 temporal = (Settings::Taa() ? 2 * s_f16 : 0) +
                       (Settings::MotionVectors() ? 2 * size(rg16f, w, h, samples, false) +
                                                        (samples > 1 ? 2 * size(rg16f, w, h, 1, false) : 0)
                                                  : 0);
  const u64 total = set_ms + set_1x + set_mirrors + tile_bytes + atlas_bytes + temporal;
  const OsView os = QueryOs(device);
  DXGI_ADAPTER_DESC1 ad{};
  if (device->adapter)
    device->adapter->GetDesc1(&ad);
  EOT_INFO("[vram-model] {}: {} MB of its own memory, {} MB of it offered to this process | frame {}x{} at {}x: "
           "one RGBA16F image {} MB, RGBA16 {} MB, RGBA8 {} MB, D32S8 {} MB; single-sample {} / {} / {} / {} MB | "
           "depth with stencil takes {:.1f} bytes a texel here (depth alone {:.1f}) | shadow tile {}^2 {} MB, "
           "atlas {}^2 {} MB",
           Narrow(ad.Description), Mb(ad.DedicatedVideoMemory), os.valid ? Mb(os.local.Budget) : 0, w, h, samples,
           Mb(ms_f16), Mb(ms_u16), Mb(ms_u8), Mb(ms_ds), Mb(s_f16), Mb(s_u16), Mb(s_u8), Mb(s_ds), ds_texel,
           static_cast<f64>(d32_one) / (1024.0 * 1024.0), tile, Mb(tile_bytes), atlas, Mb(atlas_bytes));
  EOT_INFO("[vram-model] the frame's usual set at these settings: multisampled {} MB, single-sample {} MB, "
           "resolve mirrors {} MB, shadows {} MB, temporal {} MB = about {} MB before textures, geometry, the "
           "picture-in-picture records and the driver's own{}",
           Mb(set_ms), Mb(set_1x), Mb(set_mirrors), Mb(tile_bytes + atlas_bytes), Mb(temporal), Mb(total),
           os.valid && os.local.Budget ? std::format(" ({}% of the budget)", total * 100 / os.local.Budget) : "");
  if (os.valid && os.local.Budget && total * 4 > os.local.Budget * 3)
    EOT_WARN("[vram-model] these settings alone ask for about {} MB of the {} MB this process may use: expect "
             "the OS to page video memory (stutter). Each multisample costs {} MB of the frame here, the shadow "
             "maps {} MB; lower wos_msaa, eot_shadow_map_size or eot_resolution",
             Mb(total), Mb(os.local.Budget), Mb(set_ms / samples), Mb(tile_bytes + atlas_bytes));
}
#endif

void Report(VideoState &s, bool csv) {
  EOT_CPU_ZONE("vram report");
  const Census c = TakeCensus(s);
#if defined(EOT_D3D12)
  auto *device = static_cast<plume::D3D12Device *>(s.device.get());
  if (!device || !device->allocator)
    return;
  const OsView os = QueryOs(device);
  if (os.valid)
    g_peak_local = std::max(g_peak_local, os.local.CurrentUsage);

  D3D12MA::TotalStatistics totals{};
  device->allocator->CalculateStatistics(&totals);
  WCHAR *json = nullptr;
  device->allocator->BuildStatsString(&json, TRUE);
  std::vector<Allocation> allocations = json ? ParseAllocations(json) : std::vector<Allocation>{};
  if (json)
    device->allocator->FreeStatsString(json);

  const auto &dflt = totals.HeapType[0].Stats; // D3D12_HEAP_TYPE_DEFAULT
  const u64 local = os.local.CurrentUsage;
  EOT_INFO("[vram] process {} MB of a {} MB budget in video memory, {} MB in shared memory | allocator: {} MB in "
           "{} allocations in video memory ({} MB of blocks), {} MB in all heaps | peak {} MB | the driver's own "
           "{} MB (the pipelines, the swap chain){}",
           Mb(local), Mb(os.local.Budget), Mb(os.shared.CurrentUsage), Mb(dflt.AllocationBytes),
           dflt.AllocationCount, Mb(dflt.BlockBytes), Mb(totals.Total.Stats.AllocationBytes), Mb(g_peak_local),
           local > dflt.BlockBytes ? Mb(local - dflt.BlockBytes) : 0,
           local > os.local.Budget ? " | OVER BUDGET: the OS is paging video memory" : "");
#endif
  EOT_INFO("[vram-census] {}", CensusLine(c, std::max(1u, s.host_msaa_samples)));
#if defined(EOT_D3D12)
  EOT_INFO("[vram-census] {} | the census holds {} MB of the allocator's {} MB in video memory", BufferLine(c),
           Mb(c.Images() + c.VramBuffers()), Mb(dflt.AllocationBytes));
#else
  EOT_INFO("[vram-census] {}", BufferLine(c));
#endif
  EOT_INFO("[vram-surfaces] {}", SurfaceLine(s));
  EOT_INFO("[vram-churn] {}", ChurnLine());

#if defined(EOT_D3D12)
  std::map<std::string, Tally> by_tag;
  std::map<std::string, Tally> by_shape;
  for (const Allocation &a : allocations) {
    const std::string name = a.name.empty() ? "(" + Narrow(a.type) + ")" : Narrow(a.name);
    Tally &sum = by_tag[Narrow(a.heap) + ":" + TagOf(name)];
    sum.bytes += a.size;
    ++sum.count;
    if (a.heap == L"DEFAULT" && a.type != L"FREE" && a.type != L"BUFFER") {
      Tally &shape = by_shape[name];
      shape.bytes += a.size;
      ++shape.count;
    }
  }
  std::vector<std::pair<std::string, Tally>> sorted(by_tag.begin(), by_tag.end());
  std::sort(sorted.begin(), sorted.end(), [](const auto &x, const auto &y) { return x.second.bytes > y.second.bytes; });
  std::string line;
  for (size_t i = 0; i < sorted.size() && i < 16; ++i) {
    if (!line.empty())
      line += ", ";
    line += std::format("{} {} MB ({})", sorted[i].first, sorted[i].second.bytes / kMB, sorted[i].second.count);
  }
  EOT_INFO("[vram-by-tag] {}", line);

  std::vector<std::pair<std::string, Tally>> shapes(by_shape.begin(), by_shape.end());
  std::sort(shapes.begin(), shapes.end(), [](const auto &x, const auto &y) { return x.second.bytes > y.second.bytes; });
  line.clear();
  for (size_t i = 0; i < shapes.size() && i < 40; ++i) {
    if (!line.empty())
      line += ", ";
    line += std::format("{} x{} = {} MB", shapes[i].first, shapes[i].second.count, shapes[i].second.bytes / kMB);
  }
  EOT_INFO("[vram-shapes] {}", line);

  if (csv)
    WriteCsv(s, allocations);

  allocations.erase(std::remove_if(allocations.begin(), allocations.end(),
                                   [](const Allocation &a) { return a.type == L"FREE"; }),
                    allocations.end());
  std::sort(allocations.begin(), allocations.end(), [](const auto &x, const auto &y) { return x.size > y.size; });
  line.clear();
  for (size_t i = 0; i < allocations.size() && i < 12; ++i) {
    if (!line.empty())
      line += ", ";
    const Allocation &a = allocations[i];
    line += std::format("{} {} MB", a.name.empty() ? Narrow(a.type) : Narrow(a.name), a.size / kMB);
  }
  EOT_INFO("[vram-largest] {}", line);

  PROCESS_MEMORY_COUNTERS_EX counters{};
  counters.cb = sizeof(counters);
  if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
                              sizeof(counters)))
    EOT_INFO("[ram] {} MB private, {} MB working set, {} MB peak working set", counters.PrivateUsage / kMB,
             counters.WorkingSetSize / kMB, counters.PeakWorkingSetSize / kMB);
#endif
}

void Plot(VideoState &s) {
#if defined(EOT_PROFILING) && defined(REXGLUE_ENABLE_PROFILING)
  const Census c = TakeCensus(s);
#if defined(EOT_D3D12)
  const OsView os = QueryOs(static_cast<plume::D3D12Device *>(s.device.get()));
  if (os.valid) {
    EOT_PLOT("vram process MB", MbF(os.local.CurrentUsage));
    EOT_PLOT("vram budget MB", MbF(os.local.Budget));
    EOT_PLOT("vram shared MB", MbF(os.shared.CurrentUsage));
  }
#endif
  EOT_PLOT("vram frame multisampled MB", MbF(c.frameMs.bytes));
  EOT_PLOT("vram frame multisampled never drawn MB", MbF(c.frameMsUndrawn.bytes));
  EOT_PLOT("vram frame single-sample MB", MbF(c.frameTwin.bytes + c.frame1x.bytes));
  EOT_PLOT("vram shadow tile + atlas MB", MbF(c.shadowTile.bytes + c.mirrorAtlas.bytes));
  EOT_PLOT("vram picture-in-picture MB", MbF(c.pipSurface.bytes + c.mirrorPip.bytes));
  EOT_PLOT("vram resolve mirrors MB", MbF(c.Mirrors()));
  EOT_PLOT("vram textures MB", MbF(c.textures.bytes));
  EOT_PLOT("vram recycle list MB", MbF(c.pool.bytes));
  EOT_PLOT("vram temporal MB", MbF(c.taa.bytes + c.velocity.bytes));
  EOT_PLOT("vram geometry caches MB", MbF(c.VramBuffers()));
  EOT_PLOT("upload heaps MB", MbF(c.UploadBuffers()));
  EOT_PLOT("pipelines", c.pipelines);
#else
  (void)s;
#endif
}

std::chrono::steady_clock::time_point g_next_report{};
std::chrono::steady_clock::time_point g_next_budget{};
std::chrono::steady_clock::time_point g_next_over_budget_warning{};
bool g_model_logged = false;
std::atomic<bool> g_report_requested{false};

void WatchBudget(VideoState &s, std::chrono::steady_clock::time_point now) {
#if defined(EOT_D3D12)
  if (now < g_next_budget)
    return;
  g_next_budget = now + kBudgetInterval;
  const OsView os = QueryOs(static_cast<plume::D3D12Device *>(s.device.get()));
  if (!os.valid)
    return;
  g_peak_local = std::max(g_peak_local, os.local.CurrentUsage);
  if (os.local.CurrentUsage <= os.local.Budget || now < g_next_over_budget_warning)
    return;
  g_next_over_budget_warning = now + kOverBudgetRepeat;
  EOT_WARN("[vram] over budget: the process holds {} MB of video memory against the {} MB the OS gives it ({} MB "
           "in shared memory): allocations are being moved to system memory, which is the stutter. What holds it:",
           Mb(os.local.CurrentUsage), Mb(os.local.Budget), Mb(os.shared.CurrentUsage));
  const Census c = TakeCensus(s);
  EOT_WARN("[vram] {}", CensusLine(c, std::max(1u, s.host_msaa_samples)));
#else
  (void)s;
  (void)now;
#endif
}

}

u64 HostTextureBytes(const HostTexture &host) {
  if (!host.texture)
    return 0;
#if defined(EOT_D3D12)
  if (const D3D12MA::Allocation *a = AllocationOf(host.texture.get()))
    return a->GetSize();
#endif
  return EstimateBytes(host.format, host.width, host.height, std::max(host.depth, host.arraySize), host.mipLevels,
                       host.sampleCount);
}

void TagHostAllocation(plume::RenderTexture *texture, const char *tag, const plume::RenderTextureDesc &desc) {
  if (!texture || !tag)
    return;
  const std::string name = std::string(tag) + " " + Shape(desc);
  u64 bytes = 0;
#if defined(EOT_D3D12)
  D3D12MA::Allocation *allocation = AllocationOf(texture);
  Tag(allocation, name);
  bytes = allocation ? allocation->GetSize() : 0;
#endif
  if (!bytes)
    bytes = EstimateBytes(desc.format, desc.width, desc.height, std::max(desc.depth, desc.arraySize), desc.mipLevels,
                          static_cast<u32>(desc.multisampling.sampleCount));
  NoteMade(name, bytes, true);
}

void RetagHostAllocation(plume::RenderTexture *texture, const char *tag, const plume::RenderTextureDesc &desc) {
#if defined(EOT_D3D12)
  if (texture && tag)
    Tag(AllocationOf(texture), std::string(tag) + " " + Shape(desc));
#else
  (void)texture;
  (void)tag;
  (void)desc;
#endif
}

void TagHostAllocation(plume::RenderBuffer *buffer, const char *tag) {
  if (!buffer || !tag)
    return;
#if defined(EOT_D3D12)
  D3D12MA::Allocation *allocation = AllocationOf(buffer);
  Tag(allocation, tag);
  NoteMade(tag, allocation ? allocation->GetSize() : 0, false);
#else
  NoteMade(tag, 0, false);
#endif
}

void NoteHostRelease(const plume::RenderTexture *texture) {
#if defined(EOT_D3D12)
  if (const D3D12MA::Allocation *a = AllocationOf(texture))
    NoteReleased(a->GetName() ? Narrow(a->GetName()) : std::string("(untagged)"), a->GetSize());
#else
  (void)texture;
#endif
}

void NoteHostRelease(const plume::RenderBuffer *buffer) {
#if defined(EOT_D3D12)
  if (const D3D12MA::Allocation *a = AllocationOf(buffer))
    NoteReleased(a->GetName() ? Narrow(a->GetName()) : std::string("(untagged)"), a->GetSize());
#else
  (void)buffer;
#endif
}

void MemoryReportTick(VideoState &s) {
  const auto now = std::chrono::steady_clock::now();
#if defined(EOT_D3D12)
  if (!g_model_logged && s.device) {
    g_model_logged = true;
    LogModel(s);
  }
#endif
  WatchBudget(s, now);
  if (EOT_PROFILER_CONNECTED() && s.presented_frames % kPlotFrames == 0)
    Plot(s);
  if (g_report_requested.exchange(false, std::memory_order_acq_rel)) {
    Report(s, true);
    return;
  }
  const i32 seconds = Settings::VramReportSeconds();
  if (seconds <= 0)
    return;
  const auto interval = std::chrono::seconds(seconds);
  if (g_next_report.time_since_epoch().count() == 0 || g_next_report > now + interval) {
    g_next_report = now + interval;
    return;
  }
  if (now < g_next_report)
    return;
  g_next_report = now + interval;
  Report(s, Settings::VramCsv());
}

void RequestMemoryReport() { g_report_requested.store(true, std::memory_order_release); }

}
