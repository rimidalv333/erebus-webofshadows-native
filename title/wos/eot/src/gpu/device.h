// gpu/device.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <atomic>
#include <functional>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/types.h>

#include <plume_render_interface.h>

#include "core/cpu.h"
#include "gpu/resources.h"

namespace rex::ui {
class Window;
}

namespace eot::gpu {

constexpr u32 kNumFrames = 2;

constexpr u32 kBindlessTextureCount = 32768;
constexpr u32 kBindlessSamplerCount = 512;

constexpr u32 kSamplerLinearClamp = 0;
constexpr u32 kSamplerPointClamp = 1;
constexpr u32 kReservedSamplerCount = 2;

struct CopyPushConstants {
  u32 resourceDescriptorIndex = 0;
  u32 resourceDescriptorIndex2 = 0;
  float param0 = 1.0f;
  float param1 = 0.0f;
  float rect[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  float colorAdjust[4] = {0.0f, 1.0f, 1.0f, 0.0f};
  float extra[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};
static_assert(sizeof(CopyPushConstants) == 64);

struct PerfCounters {
  f64 draw_ms = 0, resolve_ms = 0, upload_ms = 0, link_ms = 0, pso_ms = 0;
  f64 vertex_copy_ms = 0, index_ms = 0, bind_ms = 0;
  f64 setup_ms = 0, pso_lookup_ms = 0, stream_ms = 0, const_ms = 0, record_ms = 0;
  f64 const_float_ms = 0, rec_state_ms = 0, rec_bind_ms = 0;
  f64 guest_d3d_ms = 0;
  u32 guest_d3d_calls = 0;
  f64 capture_ms = 0, present_wait_ms = 0, worker_idle_ms = 0;
  u32 index_cache_hits = 0, index_cache_misses = 0;
  u32 index_cache_evictions = 0;
  u32 vertex_cache_hits = 0, vertex_cache_misses = 0;
  u32 geometry_vram_binds = 0, geometry_staging_binds = 0;
  u32 const_file_hits = 0;
  u32 const_file_clean_hits = 0;
  u32 const_file_mask_misses = 0;
  u32 pipeline_hot_hits = 0;
  u32 replay_memo_hits = 0;
  u32 vertex_bind_requests = 0, vertex_bind_calls = 0;
  u32 single_stream_draws = 0, target_memo_hits = 0;
  u32 sorted_draws = 0, sort_runs = 0;
  u32 target_memo_miss_gen = 0, target_memo_miss_words = 0, target_memo_miss_sig = 0;
  u32 index_bind_requests = 0, index_bind_calls = 0;
  u32 framebuffer_cache_hits = 0;
  u32 texture_bind_requests = 0, texture_bind_hits = 0;
  u32 stale_alias_white = 0;  // WoS port (fix101)
  u32 pipeline_bind_calls = 0, viewport_bind_calls = 0, scissor_bind_calls = 0;
  u32 pipeline_bind_on_hit = 0;
  u32 stencil_ref_calls = 0;
  u32 texture_barrier_calls = 0, texture_barrier_resources = 0;
  f64 acquire_ms = 0, submit_ms = 0, fence_ms = 0, frame_ms = 0;
  f64 present_blit_ms = 0, present_house_ms = 0;
  f64 replay_targets_ms = 0;
  f64 pace_ms = 0;
  u32 draws = 0, resolves = 0, uploads = 0, links = 0, psos = 0, frames = 0;
  u32 uploads_new = 0, uploads_again = 0, uploads_refresh = 0;
  u32 uploads_again_reloaded = 0;
  u64 upload_blocks = 0;
  f64 upload_synth_ms = 0;
  u32 upload_frame_max = 0;
  f64 upload_frame_max_ms = 0;
  u32 upload_lead[5] = {};
  u32 textures_released = 0;
  u32 uploads_preloaded = 0;
  u32 uploads_skipped = 0;
  u32 resolve_copies = 0;
  u32 draws_skipped = 0;
  u32 surface_transfers = 0;
  u32 resolve_transfers = 0;
  u32 resolve_noops = 0;
  u32 resolve_refreshes = 0;
  u32 alias_deferred = 0;
  u32 alias_copies = 0;
  u32 host_textures = 0, host_views = 0, host_framebuffers = 0, host_parked = 0;
  u32 host_tex_surface = 0, host_tex_mirror = 0, host_tex_guest = 0;
  u32 host_tex_recycled = 0;
  u32 live_textures = 0, live_surfaces = 0, alias_scanned = 0;
  u32 textures_evicted = 0, pool_size = 0;
  f64 alias_scan_ms = 0, msaa_scan_ms = 0;
  f64 resolve_mirror_ms = 0, resolve_fb_ms = 0, resolve_bind_ms = 0;
  u64 vertex_bytes = 0, index_bytes = 0, constant_bytes = 0;
  u32 dead_resolves = 0;
  f64 gpu_ms = 0;
  u32 gpu_frames = 0;
  std::map<u32, std::pair<f64, u32>> gpu_cats;
  std::chrono::steady_clock::time_point last_present{};
};

constexpr u32 kFrameClockRing = 8192;

inline u64 PerfNow() { return eot::cpu::Timestamp(); }
inline f64 g_perf_ms_per_tick = 0.0;
f64 PerfMsPerTickSlow();
bool PinThreadToPhysicalCore(u32 core, const char *what);
bool ThreadSleepsPrecisely();
u32 PhysicalCoreCount();
inline f64 PerfMsPerTick() {
  const f64 v = g_perf_ms_per_tick;
  return v > 0.0 ? v : PerfMsPerTickSlow();
}
struct PerfScopeSampled {
  f64 &acc;
  u64 t0 = 0;
  PerfScopeSampled(f64 &a, u32 &calls) : acc(a) {
    if ((++calls & 15u) == 0)
      t0 = PerfNow();
  }
  ~PerfScopeSampled() {
    if (t0)
      acc += 16.0 * static_cast<f64>(PerfNow() - t0) * PerfMsPerTick();
  }
};

struct PerfScope {
  f64 &acc;
  u64 t0;
  bool stopped = false;
  void stop() {
    if (stopped)
      return;
    acc += static_cast<f64>(PerfNow() - t0) * PerfMsPerTick();
    stopped = true;
  }
  explicit PerfScope(f64 &a) : acc(a), t0(PerfNow()) {}
  ~PerfScope() {
    if (!stopped)
      acc += static_cast<f64>(PerfNow() - t0) * PerfMsPerTick();
  }
};

struct SharedConstants {
  u32 texture2DIndices[16]{};   // c0..c3
  u32 texture3DIndices[16]{};   // c4..c7
  u32 textureCubeIndices[16]{}; // c8..c11
  u32 texture1DIndices[16]{};   // c12..c15
  u32 samplerIndices[16]{};     // c16..c19
  u32 booleans[8]{};            // c20..c21  VS bits 0..127, PS bits 128..255
  u32 swappedTexcoords{};       // c22.x
  float halfPixelOffsetX{};     // c22.y
  float halfPixelOffsetY{};     // c22.z
  float alphaThreshold{};       // c22.w
  u32 swappedNormals{};         // c23.x
  u32 swappedBinormals{};       // c23.y
  u32 swappedTangents{};        // c23.z
  u32 swappedBlendWeights{};    // c23.w
  u32 swappedPositions{};       // c24.x
  u32 sintTexcoords{};
  u32 biasedTextures{};
  u32 packedDec3{};
  i32 loopConstants[32][4]{};   // c25..c56  int4(count, start, step, 0)
  float posScale[4]{1.f, 1.f, 1.f, 1.f};  // c57
  float posOffset[4]{0.f, 0.f, 0.f, 0.f}; // c58
};
static_assert(sizeof(SharedConstants) == 59 * 16);

struct HostTextureTransition {
  HostTexture *host = nullptr;
  plume::RenderTextureLayout layout = plume::RenderTextureLayout::UNKNOWN;
};
constexpr u32 kMaxPendingTransitions = 24;

struct VideoState {
  std::unique_ptr<plume::RenderInterface> render_iface;
  std::unique_ptr<plume::RenderDevice> device;
  std::unique_ptr<plume::RenderPool> transient_mirror_pool;
  std::unique_ptr<plume::RenderCommandQueue> queue;
  std::unique_ptr<plume::RenderCommandList> command_lists[kNumFrames];
  std::unique_ptr<plume::RenderCommandFence> fences[kNumFrames];
  std::unique_ptr<plume::RenderCommandSemaphore> acquire_semaphores[kNumFrames];
  std::vector<std::unique_ptr<plume::RenderCommandSemaphore>> render_semaphores;
  plume::RenderCommandList *command_list = nullptr;
  std::atomic<u32> frame{0};
  u32 next_frame = 1 % kNumFrames;
  u32 recording_slot() const { return frame.load(std::memory_order_relaxed); }

  std::unique_ptr<plume::RenderSwapChain> swap_chain;
  bool present_wait = false;
  std::vector<std::unique_ptr<plume::RenderFramebuffer>> swap_framebuffers;

  std::unique_ptr<plume::RenderPipelineLayout> pipeline_layout;
  std::unique_ptr<plume::RenderDescriptorSet> texture_descriptor_set;
  std::vector<bool> descriptor_slot_used;
  std::unique_ptr<plume::RenderTexture> null_textures[kNullTextureDescriptorCount];
  std::unique_ptr<plume::RenderTextureView> null_texture_views[kNullTextureDescriptorCount];
  bool null_texture_barriers_submitted = false;
  // WoS port (fix101): an opaque-white 1x1 view (the 2D null texture, all channels ONE) for
  // stale cross-format aliases, and the last resolve written at each guest base address
  std::unique_ptr<plume::RenderTextureView> white_texture_view;
  u32 white_texture_index = kInvalidDescriptorIndex;
  std::unordered_map<u32, std::pair<u64, u32>> resolve_writes;  // base -> (seq, xenos format)
  u64 resolve_write_seq = 0;
  std::unique_ptr<plume::RenderDescriptorSet> sampler_descriptor_set;
  std::vector<bool> sampler_descriptor_used;
  std::unique_ptr<plume::RenderSampler> linear_sampler;
  std::unique_ptr<plume::RenderSampler> point_sampler;

  std::unique_ptr<plume::RenderShader> copy_vs;
  std::unique_ptr<plume::RenderShader> blit_ps;
  std::unique_ptr<plume::RenderShader> copy_depth_ps;
  std::unique_ptr<plume::RenderShader> copy_depth_f24_ps;  // WoS port (fix67)
  std::unordered_map<u64, std::unique_ptr<plume::RenderPipeline>> blit_pipelines;
  std::unordered_map<u64, std::unique_ptr<plume::RenderPipeline>> depth_copy_pipelines;
  u32 host_msaa_samples = 1;
  u32 display_refresh_hz = 60;
  std::unique_ptr<plume::RenderShader> resolve_msaa_color_ps[3];
  std::unique_ptr<plume::RenderShader> resolve_msaa_depth_ps[3];
  std::unordered_map<u64, std::unique_ptr<plume::RenderPipeline>> resolve_msaa_pipelines;
  std::unique_ptr<plume::RenderShader> derive_depth_stencil_ps[3];
  std::unordered_map<u64, std::unique_ptr<plume::RenderPipeline>> derive_depth_stencil_pipelines;
  bool stencil_ref_supported = false;

  std::string backend_info;

  std::mutex mutex;
  std::atomic<bool> ready{false};
  std::atomic<bool> shutting_down{false};
  bool quiesced = false;
  std::atomic<bool> resize_requested{false};

  bool command_list_open = false;
  bool command_list_submitted[kNumFrames] = {};
  HostTextureTransition pending_transitions[kMaxPendingTransitions];
  u32 pending_transition_count = 0;
  bool defer_shader_read_transitions = false;

  u64 presented_frames = 0;
  u64 guest_frames = 0;
  u64 captured_presents = 0;

  std::vector<std::unique_ptr<plume::RenderTexture>> texture_graveyard[kNumFrames];
  std::vector<std::unique_ptr<plume::RenderTextureView>> view_graveyard[kNumFrames];
  std::vector<std::unique_ptr<plume::RenderFramebuffer>> framebuffer_graveyard[kNumFrames];
  std::vector<std::unique_ptr<plume::RenderBuffer>> buffer_graveyard[kNumFrames];
  struct RetiredDescriptorSlot {
    u32 slot;
    u32 null_index;
  };
  std::vector<RetiredDescriptorSlot> descriptor_graveyard[kNumFrames];

  std::unordered_map<u32, std::shared_ptr<GuestTexture>> textures;

  PerfCounters perf;
  PerfCounters perf_prev_frame;
  std::vector<f32> frame_walls;
  f64 frame_clock[kFrameClockRing] = {};

  struct TextureSlotCache {
    u32 texVa = 0;
    u8 fc[24] = {};
    u64 generation = ~0ull;
    u64 resourceGeneration = ~0ull;
    u32 samplerPolicy = ~0u;
    GuestTexture *texture = nullptr;
    u32 index = kInvalidDescriptorIndex;
    u32 sampler = 0;
    u8 dimension = 0;
    u8 biasedBits = 0;
  };
  static constexpr u32 kTextureSlotWays = 4;
  TextureSlotCache slot_cache[16][kTextureSlotWays];
  u8 slot_cache_next[16] = {};
  GuestTexture *draw_bound_textures[16] = {};
  std::atomic<u64> texture_generation{1};
  std::unordered_map<u64, std::unique_ptr<GuestSurface>> surfaces;
  struct SurfaceWorkStats {
    bool isDepth = false;
    u32 format = 0, baseTile = 0;
    u32 minWidth = 0, maxWidth = 0, minHeight = 0, maxHeight = 0;
    u32 allocWidth = 0, allocHeight = 0, hostWidth = 0, hostHeight = 0;
    u32 samples = 1;
    u64 draws = 0, clears = 0, resolves = 0;
  };
  struct TextureWorkStats {
    u32 va = 0, width = 0, height = 0, hostWidth = 0, hostHeight = 0, format = 0;
    u64 samples = 0, resolves = 0, deadResolves = 0;
  };
  std::unordered_map<u64, SurfaceWorkStats> surface_work_window;
  std::unordered_map<u64, TextureWorkStats> texture_work_window;
  std::unordered_map<u64, u64> render_area_window;
  u64 resolve_alias_token = 0;
  std::vector<GuestTexture *> resolve_alias_candidates;
  std::map<u32, u32> resolve_ranges;
  u64 resolve_alias_candidates_generation = 0;
  u64 mirror_generation = 1;
  u32 root_cbv_index[3] = {~0u, ~0u, ~0u};
  u32 bound_spec = ~0u;
  std::unordered_map<u32, std::unique_ptr<GuestShader>> shaders;
  std::mutex shaders_mutex;
  std::vector<std::unique_ptr<GuestShader>> shader_graveyard;
  std::atomic<u64> shader_generation{1};

  struct CachedFramebuffer {
    std::unique_ptr<plume::RenderFramebuffer> fb;
    const plume::RenderTexture *attachments[5] = {};
    u32 attachmentCount = 0;
  };
  std::unordered_map<u64, CachedFramebuffer> framebuffers;
  struct PooledHostTexture {
    HostTexture host;
    u64 freedFrame = 0;
    u64 bytes = 0;
  };
  std::vector<PooledHostTexture> host_texture_pool;
  u64 host_texture_pool_bytes = 0;
  const plume::RenderFramebuffer *bound_framebuffer = nullptr;
  const plume::RenderPipeline *bound_pipeline = nullptr;
  struct BoundVertexStream {
    const plume::RenderBuffer *buffer = nullptr;
    u64 offset = 0;
    u32 size = 0;
    u32 stride = 0;
    bool valid = false;
  } bound_vertex_streams[16];
  struct BoundIndexStream {
    const plume::RenderBuffer *buffer = nullptr;
    u64 offset = 0;
    u32 size = 0;
    plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
    bool valid = false;
  } bound_index_stream;
  const plume::RenderTexture *bound_draw_colors[4] = {};
  const plume::RenderTexture *bound_draw_depth = nullptr;
  u32 bound_draw_color_count = 0;
  bool bound_draw_targets_valid = false;
  plume::RenderBuffer *bound_root_buffer[3] = {};
  u64 bound_root_offset[3] = {};
  u64 perf_resets = 0;
  u64 vs_float_constants_stale = ~0ull;
  u64 ps_float_constants_stale = ~0ull;

  enum class GammaMode : u32 { None = 0, Table = 1, Pwl = 2 };
  GammaMode gamma_mode = GammaMode::None;
  uint16_t gamma_table[3][256] = {};  // D3DGAMMARAMP: red/green/blue, 16-bit
  uint16_t gamma_pwl[3][128][2] = {}; // D3DPWLGAMMA: {base, delta} per channel
  bool gamma_lut_dirty = false;
  HostTexture gamma_lut;
  u32 last_front_buffer_va = 0;

  std::atomic<bool> pending_up_armed{false};
  std::mutex guest_mutex;
  u32 current_vs_va = 0, current_ps_va = 0;
  const char *current_origin = "";
  u64 surface_generation = 0;
  struct PendingUpDraw {
    bool valid = false;
    u32 device_va = 0;
    u32 primitive_type = 0;
    u32 vertex_count = 0;
    u32 stride = 0;
    u32 data_va = 0;
    u64 vs_constants_dirty = ~0ull;
    u64 ps_constants_dirty = ~0ull;
    bool has_image = false;
    std::vector<u8> device_image;
  } pending_up;
};

VideoState &state();

class Video {
public:
  static bool CreateHostDevice(rex::ui::Window *window);
  static void RequestShutdown();
  static bool IsShuttingDown();
  static void BeginShutdown();
  static void Shutdown();
  static plume::RenderDevice *HostDevice();
  static u32 OutputWidth();
  static u32 OutputHeight();
  static void RequestResize();

  static void Present(u32 front_buffer_texture_va);
  static void PresentOverlayOnly();
};

std::unique_ptr<plume::RenderBuffer> CreateHostBuffer(plume::RenderDevice *device,
                                                      const plume::RenderBufferDesc &desc,
                                                      const char *tag);
std::unique_ptr<plume::RenderTexture> CreateHostTexture(plume::RenderDevice *device,
                                                        const plume::RenderTextureDesc &desc,
                                                        const char *tag,
                                                        plume::RenderPool *pool = nullptr);
std::unique_ptr<plume::RenderPipeline>
CreateHostGraphicsPipeline(plume::RenderDevice *device,
                           const plume::RenderGraphicsPipelineDesc &desc, const char *tag);

bool BuildPipelineLayout(VideoState &s);
bool BuildHelperPipelines(VideoState &s);
bool BuildSwapFramebuffers(VideoState &s);
plume::RenderPipeline *GetBlitPipeline(VideoState &s, plume::RenderFormat rt_format,
                                       u32 samples = 1);
plume::RenderPipeline *GetResolveMsaaPipeline(VideoState &s, plume::RenderFormat dst_format,
                                              u32 src_samples, bool depth);
plume::RenderPipeline *GetDepthCopyPipeline(VideoState &s, plume::RenderFormat ds_format,
                                            u32 samples = 1, bool float24 = false);
plume::RenderPipeline *GetDeriveDepthStencilPipeline(VideoState &s, plume::RenderFormat ds_format,
                                                     u32 src_samples);
u32 BindStencilSRVLocked(VideoState &s, HostTexture &host);

void BeginCommandList(VideoState &s);
void SubmitOpenListLocked(VideoState &s);
void AdvanceAndWaitReused(VideoState &s);
void ParkTexture(VideoState &s, std::unique_ptr<plume::RenderTexture> t);
void ParkView(VideoState &s, std::unique_ptr<plume::RenderTextureView> v);
void ParkFramebuffer(VideoState &s, std::unique_ptr<plume::RenderFramebuffer> f);
void ParkBuffer(VideoState &s, std::unique_ptr<plume::RenderBuffer> b);
void PresentLocked(VideoState &s, u32 front_buffer_texture_va);
bool CreateOrRecycleHostTexture(VideoState &s, HostTexture &host,
                                const plume::RenderTextureDesc &desc, const char *tag,
                                plume::RenderPool *pool = nullptr);
void EvictHostTexturePool(VideoState &s);
void EvictStaleGuestTextures(VideoState &s);

inline f64 FrameAgeSeconds(const VideoState &s, u64 frame) {
  if (frame >= s.guest_frames)
    return 0.0;
  if (s.guest_frames - frame >= kFrameClockRing)
    return 1e9;
  return s.frame_clock[s.guest_frames % kFrameClockRing] - s.frame_clock[frame % kFrameClockRing];
}

void ParkHostTexture(VideoState &s, HostTexture &host);
void DestroyHostTexture(VideoState &s, HostTexture &host);

u32 AllocateDescriptorSlot(VideoState &s);
u32 BindTextureSRVLocked(VideoState &s, HostTexture &host);
u32 BindTextureSRVSwizzledLocked(VideoState &s, HostTexture &host, u32 swizzle);
void ReleaseTextureSRVLocked(VideoState &s, HostTexture &host);
void DrainDescriptorSlotsLocked(VideoState &s, u32 slot);

void TransitionLocked(VideoState &s, HostTexture &host, plume::RenderTextureLayout layout);
void TransitionManyLocked(VideoState &s, const HostTextureTransition *transitions, u32 count);

plume::RenderColor ArgbToRenderColor(u32 argb);

// WoS port: exact values of an RGBA16F/RGBA8 host image (mip 0) read back and logged -
// per channel min/max/mean, NaN/Inf counts and a few texels - for diagnosis.
bool LogHostTextureStatsLocked(VideoState &s, HostTexture &host, const char *label);

bool DumpHostTextureLocked(VideoState &s, HostTexture &host, const char *path, float scale,
                           u32 lut_index = kInvalidDescriptorIndex,
                           u32 swizzle = kIdentityFetchSwizzle, u32 shrink = 1);

bool DumpHostTextureLevelRawLocked(VideoState &s, HostTexture &host, u32 level, u32 layer,
                                   u32 bytes_per_block, bool block_compressed, const char *path);

void RenderDocInit();
void RenderDocFrameBoundary(u64 guest_frame_just_presented);

void DrainHostDebugMessages(VideoState &s, const char *phase);

bool DiagShouldLog(u64 site, u32 *n_out);

}

namespace eot::gpu {

void SelectPresentBlitMode(u32 src_w, u32 src_h, float dst_w, float dst_h, float extra[4]);

}
