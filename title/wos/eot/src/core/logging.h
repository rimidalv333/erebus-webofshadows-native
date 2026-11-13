/**
 * @file    core/logging.h
 * @brief   EOT_* logging macros bound to the eot log category.
 *
 *          Levels, so a player's log stays readable and a developer's has it
 *          all (`--log_level=debug`):
 *
 *            INFO   one line per boot-time fact or coarse event: the device
 *                   and its settings, caches ready, loading screens, the
 *                   perf and pso summaries, anything a cvar explicitly asked
 *                   to print (eot_diag_frame, eot_trace_frames, ...).
 *            DEBUG  anything that repeats per object, per call or per frame:
 *                   shader registrations, surfaces, resolves, samplers,
 *                   texture mirrors, package loads, post-FX state changes,
 *                   pipeline misses.
 *            WARN   something the renderer could not model; rate-limit the
 *                   per-draw ones (DiagShouldLog, or a counter in the text).
 *            ERROR  a failure that costs the frame or the session.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once
#include <rex/logging.h>

REXLOG_DEFINE_CATEGORY(eot)

#define EOT_TRACE(...) REXLOG_CAT_TRACE(::rex::log::eot(), __VA_ARGS__)
#define EOT_DEBUG(...) REXLOG_CAT_DEBUG(::rex::log::eot(), __VA_ARGS__)
#define EOT_INFO(...) REXLOG_CAT_INFO(::rex::log::eot(), __VA_ARGS__)
#define EOT_WARN(...) REXLOG_CAT_WARN(::rex::log::eot(), __VA_ARGS__)
#define EOT_ERROR(...) REXLOG_CAT_ERROR(::rex::log::eot(), __VA_ARGS__)
#define EOT_CRITICAL(...) REXLOG_CAT_CRITICAL(::rex::log::eot(), __VA_ARGS__)
