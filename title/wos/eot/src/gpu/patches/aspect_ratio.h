// WoS port of EdgeOfTimeRecomp's renderer: what the renderer reads from Edge of Time's
// game patches (gpu/patches/aspect_ratio.cpp there: its aspect-ratio and movie patches,
// which reach into Edge of Time's own code). Web of Shadows has none of them: the picture
// keeps the game's 16:9, and no movie needs its resolve skipped.
#pragma once

namespace eot::gpu {

inline float ConfiguredAspectRatio() { return 16.0f / 9.0f; }
inline void ApplyAspectRatio() {}
inline bool LayoutIsWidescreen() { return true; }
inline bool MacWide() { return false; }
inline float ViewWidening() { return 1.0f; }
inline bool WidenedViewEye(float (&)[3]) { return false; }
inline bool TakeMovieDrawnFlag() { return false; }
inline bool TakeMovieResolveSkip() { return false; }

}
