// core/profiling.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#if defined(EOT_PROFILING) && defined(REXGLUE_ENABLE_PROFILING)

#include <tracy/Tracy.hpp>

#define EOT_CPU_ZONE(name) ZoneNamedN(___tracy_scoped_zone, name, TracyIsStarted)
#define EOT_CPU_ZONE_DYN(name)                                                                     \
  ZoneTransientN(TracyConcat(__tracy_cpu_zone, TracyLine), name, TracyIsStarted)
#define EOT_PROFILER_CONNECTED() (TracyIsStarted && TracyIsConnected)
#define EOT_FRAME_MARK()                                                                           \
  do {                                                                                             \
    if (TracyIsStarted) {                                                                          \
      FrameMark;                                                                                   \
    }                                                                                              \
  } while (0)
#define EOT_PLOT(name, value)                                                                      \
  do {                                                                                             \
    if (TracyIsStarted) {                                                                          \
      TracyPlot(name, static_cast<double>(value));                                                 \
    }                                                                                              \
  } while (0)

#else

#define EOT_CPU_ZONE(name) ((void)0)
#define EOT_CPU_ZONE_DYN(name) ((void)0)
#define EOT_PROFILER_CONNECTED() (false)
#define EOT_FRAME_MARK() ((void)0)
#define EOT_PLOT(name, value) ((void)0)

#endif
