// WoS port of EdgeOfTimeRecomp's renderer: the one debug hook the renderer reads
// (eot's goliath/debug/freecam.h). WoS has no free camera; draws are never forced
// two-sided.
#pragma once

namespace eot::debug {
inline bool FreecamDrawTwoSided() { return false; }
}
