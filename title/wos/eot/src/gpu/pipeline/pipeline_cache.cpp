// gpu/pipeline/pipeline_cache.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "gpu/pipeline/pipeline_cache.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <xxhash.h>

#include "core/logging.h"
#include "core/profiling.h"

#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/pipeline/pso_precache.h"
#include "gpu/pipeline/pso_predictor.h"
#include "gpu/pipeline/pso_records.h"
#include "gpu/settings.h"
#include "gpu/shaders/guest_shaders.h"
#include "gpu/vertex_layout.h"

namespace eot::gpu {

namespace {

struct Entry {
  std::unique_ptr<plume::RenderPipeline> pipeline;
  PsoSource source = PsoSource::Draw;
  u16 templateIndex = kNoTemplate;
  bool used = false;
};

struct Cache {
  std::shared_mutex mutex;
  std::unordered_map<u64, Entry> map;
  u32 failures = 0;
  bool capture = false;
  u32 gapBuilds = 0, raceBuilds = 0;
  u64 lastSummaryFrame = 0;
};

Cache &cache() {
  static Cache c;
  return c;
}

struct PackageHold {
  TokenPtr token;
  std::chrono::steady_clock::time_point start;
  bool held = false;
};

struct Loading {
  std::mutex mutex;
  std::unordered_map<u16, std::vector<PsoRecord>> sets;
  std::unordered_map<u32, PackageHold> holds;
  std::atomic<bool> screen{false};
  std::atomic<u32> currentPackage{0};
  std::atomic<u32> levelPackage{0};
  std::atomic<bool> levelKnown{false};
  u64 screenSinceFrame = 0;
  u32 screens = 0, holdsCount = 0, setsQueued = 0, drawnCaptured = 0;
  f64 holdMs = 0;
};

Loading &loading() {
  static Loading l;
  return l;
}

const char *SourceName(PsoSource s) {
  switch (s) {
  case PsoSource::CompiledIn:
    return "compiled-in";
  case PsoSource::LocalCsv:
    return "local csv";
  case PsoSource::Predicted:
    return "predicted";
  default:
    return "draw";
  }
}

void CaptureLocked(VideoState &s, const PipelineState &st) {
  if (!st.layout)
    return;
  const InputLayout &l = *st.layout;
  if (l.declRaw.empty() || l.declRaw.size() > sizeof(PsoRecord::declRaw))
    return;
  PsoRecord r{};
  r.state = st;
  r.state.vs = nullptr;
  r.state.ps = nullptr;
  r.state.layout = nullptr;
  r.state.sampleCount = 1;
  r.declCount = static_cast<u32>(l.declRaw.size() / sizeof(DeclElement));
  std::memcpy(r.declRaw, l.declRaw.data(), l.declRaw.size());
  r.frame = s.guest_frames;
  auto &ld = loading();
  const u32 level = ld.levelPackage.load(std::memory_order_relaxed);
  const u32 last = ld.currentPackage.load(std::memory_order_relaxed);
  if (level)
    r.packages[r.packageCount++] = static_cast<u16>(level);
  if (last && last != level)
    r.packages[r.packageCount++] = static_cast<u16>(last);
  PsoCaptureAdd(r);
}

void RouteOne(const PsoRecord &r, PsoSource source, size_t *queued, size_t *per_package) {
  if (r.packageCount == 0) {
    const bool known = source == PsoSource::CompiledIn || source == PsoSource::LocalCsv;
    *queued += PsoPrecacheEnqueue(r, source, known ? PsoLane::Recorded : PsoLane::Predicted)
                   ? 1
                   : 0;
    return;
  }
  {
    auto &l = loading();
    std::lock_guard lock(l.mutex);
    for (u32 i = 0; i < r.packageCount; ++i)
      l.sets[r.packages[i]].push_back(r);
  }
  ++*per_package;
  PsoPrecacheEnqueue(r, source, PsoLane::Background);
}

void RouteRecord(const PsoRecord &r, PsoSource source, size_t *queued, size_t *per_package) {
  const u32 msaa = state().host_msaa_samples;
  if (msaa > 1 && r.state.sampleCount == 1) {
    PsoRecord twin = r;
    twin.state.sampleCount = msaa;
    RouteOne(twin, source, queued, per_package);
  }
  RouteOne(r, source, queued, per_package);
}

}

void ZeroPipelineState(PipelineState &state) { std::memset(&state, 0, sizeof(state)); }

u64 HashPipelineState(const PipelineState &state) {
  return XXH3_64bits(reinterpret_cast<const u8 *>(&state) + kPipelineKeyOffset,
                     sizeof(state) - kPipelineKeyOffset);
}

void CanonicalizePipelineState(PipelineState &st, u32 spec_mask, u32 stream_mask) {
#if defined(EOT_D3D12)
  (void)spec_mask;
  st.spec = 0;
#else
  st.spec &= spec_mask;
#endif
  if (st.sampleCount == 0)
    st.sampleCount = 1;
  if (!st.depthEnable) {
    st.depthWrite = false;
    st.depthFunc = plume::RenderComparisonFunction::ALWAYS;
    st.depthBias = 0;
    st.slopeScaledDepthBias = 0.0f;
  }
  if (st.depthBias == 0 && st.slopeScaledDepthBias == 0.0f) {
    st.slopeScaledDepthBias = 0.0f;
    st.targetScale = 1.0f;
  }
  if (!st.stencilEnable) {
    st.stencilReadMask = st.stencilWriteMask = st.stencilRef = 0;
    std::memset(&st.stencilFront, 0, sizeof(st.stencilFront));
    std::memset(&st.stencilBack, 0, sizeof(st.stencilBack));
    st.stencilFront.compareFunction = plume::RenderComparisonFunction::ALWAYS;
    st.stencilBack.compareFunction = plume::RenderComparisonFunction::ALWAYS;
    for (plume::RenderStencilFaceDesc *face : {&st.stencilFront, &st.stencilBack}) {
      face->failOp = plume::RenderStencilOp::KEEP;
      face->depthFailOp = plume::RenderStencilOp::KEEP;
      face->passOp = plume::RenderStencilOp::KEEP;
    }
  }
  for (u32 i = 0; i < 4; ++i) {
    plume::RenderBlendDesc &b = st.blend[i];
    if (i >= st.rtCount) {
      std::memset(&b, 0, sizeof(b));
      st.rtFormats[i] = plume::RenderFormat::UNKNOWN;
      continue;
    }
    if (b.renderTargetWriteMask == 0)
      b.blendEnabled = false;
    if (!b.blendEnabled) {
      b.srcBlend = plume::RenderBlend::ONE;
      b.dstBlend = plume::RenderBlend::ZERO;
      b.blendOp = plume::RenderBlendOperation::ADD;
      b.srcBlendAlpha = plume::RenderBlend::ONE;
      b.dstBlendAlpha = plume::RenderBlend::ZERO;
      b.blendOpAlpha = plume::RenderBlendOperation::ADD;
    }
  }
  if (st.rtCount == 0)
    st.alphaToCoverage = false;
  if (stream_mask != 0xFFFFu) {
    for (u32 S = 0; S < 16; ++S)
      if (!(stream_mask & (1u << S)))
        st.strides[S] = 0;
  }
}

plume::RenderPipeline *GetOrCreatePipeline(VideoState &s, const PipelineState &st, bool worker,
                                           PsoSource source, u16 template_index) {
  auto &c = cache();
  const u64 key = HashPipelineState(st);
  struct HotPipeline {
    u64 key = 0;
    plume::RenderPipeline *pipeline = nullptr;
  };
  static thread_local HotPipeline hot[256];
  HotPipeline *hot_entry = nullptr;
  if (!worker) {
    hot_entry = &hot[key & 255];
    if (hot_entry->pipeline && hot_entry->key == key) {
      s.perf.pipeline_hot_hits++;
      return hot_entry->pipeline;
    }
  }
  {
    std::shared_lock lock(c.mutex);
    auto it = c.map.find(key);
    if (it != c.map.end()) {
      plume::RenderPipeline *pipeline = it->second.pipeline.get();
      if (!worker && !it->second.used) {
        it->second.used = true;
        if (pipeline && c.capture) {
          CaptureLocked(s, st);
          loading().drawnCaptured++;
        }
      }
      if (hot_entry && pipeline)
        *hot_entry = {key, pipeline};
      return pipeline;
    }
  }
  std::unique_ptr<PerfScope> perf_scope;
  if (!worker) {
    perf_scope = std::make_unique<PerfScope>(s.perf.pso_ms);
    s.perf.psos++;
  }

  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = s.pipeline_layout.get();
  desc.vertexShader = st.vs;
  desc.pixelShader = st.ps;

  std::vector<plume::RenderInputSlot> slots;
  if (st.layout) {
    for (u32 slot = 0; slot < 16; ++slot) {
      if (st.layout->streamMask & (1u << slot))
        slots.emplace_back(slot, st.strides[slot]);
    }
    if (st.layout->needsSyntheticSlot)
      slots.emplace_back(kSyntheticVertexSlot, 0u);
    desc.inputSlots = slots.data();
    desc.inputSlotsCount = static_cast<u32>(slots.size());
    desc.inputElements = st.layout->elements.data();
    desc.inputElementsCount = static_cast<u32>(st.layout->elements.size());
  }

  desc.primitiveTopology = st.topology;
  desc.cullMode = st.cull;
  desc.frontFace = st.frontFace;
  desc.depthClipEnabled = st.depthClip;
  desc.depthBias = st.depthBias;
  desc.slopeScaledDepthBias = st.slopeScaledDepthBias * st.targetScale;
  desc.depthBiasClamp = 0.0f;
  desc.depthEnabled = st.depthEnable;
  desc.depthWriteEnabled = st.depthWrite;
  desc.depthFunction = st.depthFunc;
  desc.stencilEnabled = st.stencilEnable;
  desc.stencilReadMask = st.stencilReadMask;
  desc.stencilWriteMask = st.stencilWriteMask;
  desc.stencilReference = st.stencilRef;
  desc.stencilFrontFace = st.stencilFront;
  desc.stencilBackFace = st.stencilBack;
  desc.multisampling.sampleCount = st.sampleCount > 1 ? st.sampleCount : 1;
  desc.alphaToCoverageEnabled = st.alphaToCoverage;
  desc.renderTargetCount = st.rtCount;
  for (u32 i = 0; i < st.rtCount && i < 4; ++i) {
    desc.renderTargetFormat[i] = st.rtFormats[i];
    desc.renderTargetBlend[i] = st.blend[i];
  }
  desc.depthTargetFormat = st.dsFormat;

#if !defined(EOT_D3D12)
  const plume::RenderSpecConstant spec_constant(0, st.spec);
  if (st.spec != 0) {
    desc.specConstants = &spec_constant;
    desc.specConstantsCount = 1;
  }
#endif

  EOT_CPU_ZONE("pipeline build");
  auto pso = CreateHostGraphicsPipeline(s.device.get(), desc, "guest-draw");
  std::unique_lock lock(c.mutex);
  if (!pso) {
    if (c.failures++ < 32) {
      EOT_ERROR("[pso] creation failed ({}): vs={:016x} ps={:016x} elements={} rt={} ds={} topo={}",
                SourceName(source), st.vsHash, st.psHash,
                st.layout ? st.layout->elements.size() : 0, st.rtCount,
                static_cast<u32>(st.dsFormat), static_cast<u32>(st.topology));
    }
    c.map.emplace(key, Entry{nullptr, source, template_index, false});
    return nullptr;
  }
  auto it = c.map.find(key);
  if (it != c.map.end()) {
    plume::RenderPipeline *pipeline = it->second.pipeline.get();
    if (hot_entry && pipeline)
      *hot_entry = {key, pipeline};
    return pipeline;
  }
  auto *raw = pso.get();
  if (!worker) {
    PsoSource known;
    const bool race = PsoPrecacheKnown(key, &known);
    (race ? c.raceBuilds : c.gapBuilds)++;
    static u32 created = 0;
    if (created++ < 400 || !race) {
      const u32 vs_va = s.current_vs_va;
      EOT_DEBUG("[pso] #{} render-thread build ({}) key={:016x} vs={:016x} ps={:016x} spec={:#x} "
               "depth={}{} func{} stencil={} cull={} rt0fmt={} ds={} topo={} vsVa={:#x} psVa={:#x} "
               "nodes={}/{} origin={} package={:#x}{}",
               created, race ? std::string("race with ") + SourceName(known) : "GAP, captured",
               key, st.vsHash, st.psHash, st.spec, st.depthEnable ? "on" : "off",
               st.depthWrite ? "w" : "", static_cast<u32>(st.depthFunc), st.stencilEnable,
               static_cast<u32>(st.cull), static_cast<u32>(st.rtFormats[0]),
               static_cast<u32>(st.dsFormat), static_cast<u32>(st.topology), vs_va,
               s.current_ps_va, PsoPredictorDescribeObject(vs_va),
               s.current_ps_va ? PsoPredictorDescribeObject(s.current_ps_va) : std::string("-"),
               s.current_origin, loading().currentPackage.load(std::memory_order_relaxed),
               loading().screen.load(std::memory_order_relaxed) ? " (loading screen)" : "");
    }
    if (c.capture) {
      CaptureLocked(s, st);
      loading().drawnCaptured++;
    }
  }
  c.map.emplace(key, Entry{std::move(pso), source, template_index, !worker});
  if (hot_entry)
    *hot_entry = {key, raw};
  return raw;
}

PsoBuildResult BuildPipelineFromRecord(VideoState &s, const PsoRecord &r, PsoSource source) {
  if (!s.ready || !s.device)
    return PsoBuildResult::Skipped;
  {
    auto &c = cache();
    std::shared_lock lock(c.mutex);
    if (c.map.count(HashPipelineState(r.state)))
      return PsoBuildResult::Existing;
  }
  const ShaderCacheEntry *vs_entry = FindShaderCacheEntry(r.state.vsHash);
  if (!vs_entry || r.declCount == 0 || r.declCount > 32)
    return PsoBuildResult::Skipped;
  PipelineState st = r.state;
  const ShaderCacheEntry *ps_entry = r.state.psHash ? FindShaderCacheEntry(r.state.psHash) : nullptr;
  const VsVariant variant = VsVariantFor(vs_entry, ps_entry, r.state.psHash == 0, r.state.velocity);
  if (r.state.velocity && (variant != VsVariant::Velocity || !Settings::MotionVectors()))
    return PsoBuildResult::Skipped;
  st.vs = GetHostShaderByHash(s, r.state.vsHash, r.state.spec, false, true, variant);
  st.ps = r.state.psHash
              ? GetHostShaderByHash(s, r.state.psHash, r.state.spec, true, true,
                                    r.state.velocity ? VsVariant::Velocity : VsVariant::Trimmed)
              : nullptr;
  if (!st.vs || (r.state.psHash && !st.ps))
    return PsoBuildResult::Skipped;
  std::vector<VertexInput> inputs;
  VertexInputsFromEntry(*vs_entry, inputs);
  st.layout = GetInputLayoutFromRaw(r.state.vsHash, inputs, r.declRaw, r.declCount);
  if (!st.layout)
    return PsoBuildResult::Skipped;
  st.layoutKey = st.layout->key;
  u32 spec_mask = vs_entry->specConstantsMask;
  if (r.state.psHash) {
    if (const ShaderCacheEntry *e = FindShaderCacheEntry(r.state.psHash))
      spec_mask |= e->specConstantsMask;
  }
  CanonicalizePipelineState(st, spec_mask, st.layout->streamMask);
  return GetOrCreatePipeline(s, st, true, source, r.templateIndex)
             ? PsoBuildResult::Built
             : PsoBuildResult::Failed;
}

void PsoCachePrecache() {
  auto &c = cache();
  auto &s = state();
  if (!s.ready || !s.device)
    return;
  PsoCaptureConfigure();
  {
    std::unique_lock lock(c.mutex);
    c.capture = true;
  }
  PsoPrecacheStart();

  size_t compiled_in = 0, local = 0, queued = 0, per_package = 0;
  std::vector<const PsoRecord *> ordered;
  ordered.reserve(CompiledInPipelines().size());
  for (const PsoRecord &r : CompiledInPipelines())
    ordered.push_back(&r);
  std::stable_sort(ordered.begin(), ordered.end(),
                   [](const PsoRecord *a, const PsoRecord *b) { return a->frame < b->frame; });
  for (const PsoRecord *r : ordered) {
    ++compiled_in;
    RouteRecord(*r, PsoSource::CompiledIn, &queued, &per_package);
  }
  std::vector<PsoRecord> rows;
  local = LoadPsoCsvDir(PsoDir(), rows);
  for (const PsoRecord &r : rows)
    RouteRecord(r, PsoSource::LocalCsv, &queued, &per_package);
  size_t packages;
  {
    auto &l = loading();
    std::lock_guard lock(l.mutex);
    packages = l.sets.size();
  }
  const size_t routed = (compiled_in + local) * (state().host_msaa_samples > 1 ? 2u : 1u);
  EOT_INFO("[pso] boot: {} compiled-in + {} local rows -> {} queued on the recorded lane, {} "
           "held for {} level package(s) ({} duplicate); templates: {}; capturing every pipeline "
           "drawn to {}/ as '{}'",
           compiled_in, local, queued, per_package, packages,
           routed > queued + per_package ? routed - queued - per_package : 0u,
           CompiledInTemplates().size(), PsoDir(), PsoSessionTag());
}

void PsoCacheSetLoadingScreen(bool on) {
  auto &l = loading();
  auto &s = state();
  const bool was = l.screen.exchange(on, std::memory_order_acq_rel);
  if (was == on)
    return;
  PsoPrecacheSetLoading(on);
  const PsoPrecacheStats ps = PsoPrecacheGetStats();
  if (on) {
    l.screenSinceFrame = s.guest_frames;
    std::lock_guard lock(l.mutex);
    l.screens++;
    EOT_INFO("[pso] loading screen up at frame {} (pool pending recorded {} prio {} bg {})",
             s.guest_frames, ps.recordedPending, ps.priorityPending, ps.backgroundPending);
    return;
  }
  u32 pending = 0;
  {
    std::lock_guard lock(l.mutex);
    for (auto &[id, h] : l.holds)
      pending += h.token ? h.token->Pending() : 0;
    l.holds.clear();
  }
  EOT_INFO("[pso] loading screen down at frame {} after {} frames (pool pending recorded {} prio "
           "{} bg {}, {} package pipelines still building)",
           s.guest_frames, s.guest_frames - l.screenSinceFrame, ps.recordedPending,
           ps.priorityPending, ps.backgroundPending, pending);
}

bool PsoCacheInLoadingScreen() { return loading().screen.load(std::memory_order_acquire); }

bool PsoCacheWaitsAllowed() {
  return loading().screen.load(std::memory_order_acquire);
}

bool PsoCacheLevelKnown() { return loading().levelKnown.load(std::memory_order_relaxed); }

void PsoCacheOnPackageLoad(u32 id, bool level) {
  auto &l = loading();
  if (id == 0 || id >= 0x1000)
    return;
  l.currentPackage.store(id, std::memory_order_relaxed);
  std::vector<PsoRecord> rows;
  {
    std::lock_guard lock(l.mutex);
    auto it = l.sets.find(static_cast<u16>(id));
    if (it != l.sets.end())
      rows = it->second;
  }
  if (level) {
    l.levelPackage.store(id, std::memory_order_relaxed);
    l.levelKnown.store(rows.size() >= kPsoKnownPackageRows, std::memory_order_relaxed);
  }
  TokenPtr token = std::make_shared<CompileToken>();
  u32 queued = 0;
  for (const PsoRecord &r : rows)
    queued += PsoPrecacheEnqueue(r, PsoSource::CompiledIn, PsoLane::Recorded, token) ? 1 : 0;
  {
    std::lock_guard lock(l.mutex);
    if (!rows.empty())
      l.setsQueued++;
    l.holds[id] = PackageHold{token, std::chrono::steady_clock::now(), false};
  }
  if (!rows.empty() || level) {
    EOT_DEBUG("[pso] package {:#x}{}: {} of {} recorded pipelines queued on the recorded lane{}",
              id, level ? " (level)" : "", queued, rows.size(),
              l.screen.load(std::memory_order_relaxed) ? " (loading screen)" : "");
  }
}

bool PsoCacheHoldPackage(u32 id) {
  auto &l = loading();
  if (!PsoCacheWaitsAllowed())
    return false;
  std::lock_guard lock(l.mutex);
  auto it = l.holds.find(id);
  if (it == l.holds.end())
    return false;
  PackageHold &h = it->second;
  const f64 ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - h.start)
                     .count();
  const TokenPtr screen = PsoPrecacheScreenToken();
  const u32 own = h.token ? h.token->Pending() : 0;
  const u32 pending = own + (screen ? screen->Pending() : 0);
  if (pending == 0 || ms >= kPsoHoldMaxMs) {
    if (h.held) {
      l.holdMs += ms;
      EOT_INFO("[pso] package {:#x} released after {:.0f} ms{}", id, ms,
               pending ? std::format(" (bounded, {} still building)", pending) : "");
    }
    l.holds.erase(it);
    return false;
  }
  if (!h.held) {
    h.held = true;
    l.holdsCount++;
    EOT_INFO("[pso] package {:#x} reported loading: holding the screen for {} pipelines ({} its "
             "own)",
             id, pending, own);
  }
  return true;
}

void PipelineCacheCounts(u32 *alive, u32 *used) {
  auto &c = cache();
  u32 a = 0, u = 0;
  {
    std::shared_lock lock(c.mutex);
    for (const auto &[key, e] : c.map) {
      if (!e.pipeline)
        continue;
      a++;
      u += e.used ? 1 : 0;
    }
  }
  *alive = a;
  *used = u;
}

void PsoCacheFlushIfDirty(bool force) {
  auto &s = state();
  PsoCaptureFlush(force, s.guest_frames);
  auto &c = cache();
  if (!force && s.guest_frames < c.lastSummaryFrame + 600)
    return;
  u32 by_source[4] = {}, used_by_source[4] = {}, total = 0;
  u32 gaps, races;
  const auto &templates = CompiledInTemplates();
  std::vector<std::pair<u32, u32>> per_template;
  if (force)
    per_template.assign(templates.size(), {0, 0});
  {
    std::unique_lock lock(c.mutex);
    c.lastSummaryFrame = s.guest_frames;
    for (const auto &[key, e] : c.map) {
      if (!e.pipeline)
        continue;
      const u32 i = static_cast<u32>(e.source) & 3;
      by_source[i]++;
      used_by_source[i] += e.used ? 1 : 0;
      total++;
      if (force && e.templateIndex < per_template.size()) {
        per_template[e.templateIndex].first++;
        per_template[e.templateIndex].second += e.used ? 1 : 0;
      }
    }
    gaps = c.gapBuilds;
    races = c.raceBuilds;
    c.gapBuilds = c.raceBuilds = 0;
  }
  const PsoPrecacheStats ps = PsoPrecacheGetStats();
  const PsoPredictorStats pr = PsoPredictorGetStats();
  if (total == 0 && ps.queued == 0)
    return;
  u32 screens, holds, sets_queued, drawn;
  f64 hold_ms;
  {
    auto &l = loading();
    std::lock_guard lock(l.mutex);
    screens = l.screens;
    holds = l.holdsCount;
    sets_queued = l.setsQueued;
    hold_ms = l.holdMs;
    drawn = l.drawnCaptured;
  }
  EOT_DEBUG("[pso] {} pipelines: draw {} | compiled-in {} ({} used) | local {} ({} used) | "
            "predicted {} ({} used) | render-thread builds since last: {} gaps, {} races | pool: "
            "{} queued, {} built, {} existing, {} skipped, {} failed, pending recorded {} prio {} bg "
            "{} | predictor: {} models, {} materials, {} slots, {} queued, {} without template, {} "
            "shadow biases | loading: {} screens, {} package sets, {} holds {:.0f} ms | {} drawn "
            "captured",
            total, by_source[0], by_source[1], used_by_source[1], by_source[2],
            used_by_source[2], by_source[3], used_by_source[3], gaps, races, ps.queued, ps.built,
            ps.existing, ps.skipped, ps.failed, ps.recordedPending, ps.priorityPending,
            ps.backgroundPending, pr.models, pr.materials, pr.slots, pr.queued, pr.noTemplate,
            pr.shadowBiases, screens, sets_queued, holds, hold_ms, drawn);
  if (!force || templates.empty())
    return;
  std::vector<std::string> rows;
  rows.reserve(templates.size());
  u32 used_templates = 0;
  for (size_t i = 0; i < templates.size(); ++i) {
    const PsoTemplate &t = templates[i];
    PsoRecord r{};
    r.state = t.state;
    rows.push_back(std::format("{},{},{:x},{},{},{},{}", t.technique, t.pass, t.materialClass,
                               static_cast<u32>(t.biasKind), per_template[i].first,
                               per_template[i].second, PsoRecordToCsv(r, "template")));
    used_templates += per_template[i].second ? 1 : 0;
  }
  const std::string header = std::format("# eot-pso-templates-used v{}\n"
                                         "technique,pass,class,biasKind,predicted,used,{}",
                                         kPsoCsvVersion, PsoCsvHeader().substr(PsoCsvHeader().find('\n') + 1));
  PsoWriteSessionFile("pso_templates_" + PsoSessionTag() + "_" + PsoSessionStamp() + ".csv",
                      header, rows);
  EOT_DEBUG("[pso] templates: {} of {} produced a pipeline a draw used", used_templates,
            templates.size());
}

}
