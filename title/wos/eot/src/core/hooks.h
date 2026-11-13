/**
 * @file    core/hooks.h
 * @brief   Macros for marking recompiled functions as intentional no-ops.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/hook.h>

#define EOT_NOOP(subroutine)                                                    \
  extern "C" REX_FUNC(subroutine) {                                            \
    (void)ctx;                                                                 \
    (void)base;                                                                \
  }

#define EOT_NOOP_RETURN(subroutine, value)                                      \
  extern "C" REX_FUNC(subroutine) {                                            \
    (void)base;                                                                \
    ctx.r3.u64 = (value);                                                      \
  }
