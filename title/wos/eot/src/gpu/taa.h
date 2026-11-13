// gpu/taa.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <functional>

#include <rex/types.h>
#include "gpu/resources.h"

namespace eot::gpu {

struct VideoState;
struct GuestTexture;
struct HostTexture;

namespace taa {

inline constexpr u32 kViewProjectionVa = 0x82496E3C;

bool IsSceneConsumer(u64 ps_hash);

void FrameJitter(const VideoState &s, bool packet_skip, bool rect_list, float *jx, float *jy);
u32 JitterIndex(const VideoState &s, bool packet_skip, bool rect_list);
bool FrameSkip(const VideoState &s, bool packet_skip);

void BeforeSceneConsumerDraw(VideoState &s, GuestTexture *const bound[16],
                             const float *camera_vp, u64 consumer_hash, bool skip);

float CameraMotionPixels(const float prev_vp[16], const float cur_vp[16], float width, float height);
bool FastCameraGate(u32 slot, float motion_px, float width);
void EndFrame(VideoState &s);

void Reset(VideoState &s);
void Shutdown(VideoState &s);

void ForEachImage(const std::function<void(const HostTexture &)> &fn);

}
}

namespace eot::gpu {

struct VideoState;
struct UploadAlloc;

namespace velocity {

inline constexpr u32 kPrevFileOffset = 4096;
inline constexpr float kSentinel = 8.0f;
inline constexpr plume::RenderFormat kFormat = plume::RenderFormat::R16G16_FLOAT;

void BeginCaptureFrame(VideoState &s);

bool PrepareDraw(VideoState &s, u64 key, const u8 *guest_file, u32 regs, UploadAlloc *out);

struct Target {
  HostTexture image;
  HostTexture resolved;
  u64 depthUid = 0;
  u32 slot = 0;
  u64 generation = 0;
  u32 width = 0, height = 0, samples = 1;
  bool needsClear = true;
  u64 frameWritten = ~0ull;
  u64 frameResolved = ~0ull;
};

Target *CurrentFor(VideoState &s, const GuestSurface &depth, u32 width, u32 height, u32 samples);
void BeforeDraw(VideoState &s, Target &t, u32 attachment);
void OnDepthCleared(VideoState &s, const GuestSurface &depth);
VelocityHandle HandleFor(const GuestSurface &depth, u64 frame);
HostTexture *ResolvedImage(VideoState &s, const VelocityHandle &h, u64 frame);

void EndFrame(VideoState &s);
void Shutdown(VideoState &s);

void ForEachImage(const std::function<void(const HostTexture &)> &fn);
u64 HistoryArenaBytes(u32 *chunks = nullptr);

}
}
