// gpu/memory_report.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <rex/types.h>

namespace plume {
struct RenderBuffer;
struct RenderTexture;
struct RenderTextureDesc;
}

namespace eot::gpu {

struct VideoState;
struct HostTexture;

void TagHostAllocation(plume::RenderTexture *texture, const char *tag, const plume::RenderTextureDesc &desc);
void TagHostAllocation(plume::RenderBuffer *buffer, const char *tag);
void RetagHostAllocation(plume::RenderTexture *texture, const char *tag, const plume::RenderTextureDesc &desc);
void NoteHostRelease(const plume::RenderTexture *texture);
void NoteHostRelease(const plume::RenderBuffer *buffer);

u64 HostTextureBytes(const HostTexture &host);

void MemoryReportTick(VideoState &s);
void RequestMemoryReport();

}
