// What the display actually shows, every ten seconds in the log and the test
// results: how long each frame stayed on screen, in vblanks (a steady 30 fps on
// a 59.94 Hz output is every frame for exactly two; ones and threes are the
// judder a frames-a-second count does not show), frames that were never shown,
// and how long a frame takes from the game handing it over (VdSwap) and from its
// flip being queued to it appearing.
//
// RADV's VideoOut WSI (PS5_Vulkan) keeps the output's handle to itself; the
// title links with --wrap=sceVideoOutOpen and --wrap=sceVideoOutSubmitFlip
// (wos/wos_game.cmake) to learn it and to time each flip queued. A thread then
// reads VideoOut's flip status, which carries the argument of the flip shown
// last and the time it was shown, and its vblank status for the vblank period.
//
// wos_display_pacing (wos_recomp.toml, on by default; false to compare as it was
// measured): the game's vblank follows the display's (rexglue patch 0023) instead
// of the emulator's 60.00 Hz timer, which drifts against the 59.94 Hz output, so
// the game's frames keep one phase with the display's refreshes; and each flip
// shown for at least two vblanks (VideoOut's flip rate), so a frame right after a
// late one waits instead of showing for a single refresh; and a vblank held back
// from the game when two of its frames wait to be shown, so a late frame does not
// leave a frame more of latency behind it.
//
// wos_adaptive_sync (native renderer): a frame that missed its refresh is shown at once
// (VideoOut's HSYNC flip, a tear line in that one refresh) instead of waiting a whole
// refresh more. A drop to 45-55 fps then shows each frame as long as it took to make
// (an even 18-22 ms) where vsync alone alternates 16.7 and 33.3 ms, the judder that
// makes a small drop feel like a big one. Frames that are on time flip at the vblank
// as before: no tearing at 60.
//
// wos_even_pacing (native renderer): a frame is presented only once the one before it
// is on screen. Without it the renderer runs up to two frames ahead of the display
// (queued->screen ~30 ms at 60 fps): the queue hides a slow frame but the game's
// clock then steps by times the display does not show (a frame that took 22 ms shown
// for 16.7, then faster ones while the queue refills), and input waits two refreshes
// more. With it each frame's time on screen is the time the game stepped.
//
// SPDX-License-Identifier: MIT
//

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>

#include <x86intrin.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/flags.h>
#include <rex/ui/startup_prospero.h>

REXCVAR_DEFINE_BOOL(wos_display_pacing, true, "WoS",
                    "The game's vblank from the display's; each frame shown for two vblanks or more");
REXCVAR_DEFINE_BOOL(wos_adaptive_sync, false, "WoS",
                    "Native renderer: a frame that missed its refresh is shown at once (a tear "
                    "line) instead of a refresh later; on-time frames stay vsynced");
REXCVAR_DEFINE_BOOL(wos_even_pacing, false, "WoS",
                    "Native renderer: present a frame only once the previous one is on screen "
                    "(no frames queued ahead of the display)");

extern "C" {
int __real_sceVideoOutOpen(int32_t user, int32_t bus, int32_t index, const void* parameter);
int __real_sceVideoOutSubmitFlip(int32_t handle, int32_t buffer_index, uint32_t mode,
                                 int64_t argument);
int sceVideoOutGetFlipStatus(int32_t handle, uint64_t status[16]);
int sceVideoOutGetVblankStatus(int32_t handle, void* status);
uint64_t sceKernelGetTscFrequency(void);
// RADV's VideoOut WSI: each flip shows for at least rate + 1 vblanks
int wsi_videoout_set_flip_rate(int rate);
// rexglue patch 0023: a vblank count for the guest's vblank to follow
void rex_gpu_set_host_vblank_counter(uint64_t (*counter)());
}

namespace wos::pacing {
namespace {

std::atomic<int> g_handle{-1};

// The game's frames: the time of each VdSwap, by its number
constexpr uint32_t kSwapRing = 1024;
std::atomic<uint64_t> g_swaps{0};
uint64_t g_swap_tsc[kSwapRing];

// Each flip queued, by its argument: when, and how many frames the game had
// handed over by then
constexpr uint32_t kFlipRing = 1024;
struct Flip {
  std::atomic<int64_t> argument{0};
  uint64_t tsc = 0;
  uint64_t swaps = 0;
};
Flip g_flips[kFlipRing];

struct Window {
  uint64_t shown = 0;
  uint64_t vblanks[6] = {};  // on screen for 1, 2, 3, 4, 5+ vblanks (index 0 unused)
  uint64_t never_shown = 0;
  double game_ms_sum = 0, game_ms_max = 0;
  double queue_ms_sum = 0, queue_ms_max = 0;
  uint64_t latency_count = 0;
};

// The latest flip queued (its argument)
std::atomic<int64_t> g_last_submitted{0};

// --- wos_adaptive_sync / wos_even_pacing ---------------------------------------------
// VideoOut's flip modes (as the PS4's): at the next vblank, or at the next scan line
constexpr uint32_t kFlipVsync = 1, kFlipHsync = 2;
// The vblank period in TSC ticks, once Watch has measured it (0 before)
std::atomic<uint64_t> g_vblank_ticks{0};
// Every flip handed to VideoOut (one per present), and the presents the renderer made
std::atomic<uint64_t> g_flip_calls{0}, g_presents{0};
// When the pacing wait saw a flip appear (for a flip status without a usable time)
std::atomic<int64_t> g_seen_argument{0};
std::atomic<uint64_t> g_seen_tsc{0};
// For the log: late frames shown at once, and the pacing wait
std::atomic<uint64_t> g_late_flips{0}, g_pace_waits{0}, g_pace_wait_ticks{0}, g_pace_timeouts{0};
std::atomic<bool> g_hsync_refused{false};

bool AdaptiveSync() {
  static const bool on = REXCVAR_GET(wos_adaptive_sync);
  return on && !g_hsync_refused.load(std::memory_order_relaxed);
}
bool EvenPacing() {
  static const bool on = REXCVAR_GET(wos_even_pacing);
  return on;
}

// The mode for a flip the driver queues for the next vblank: at once when the frame
// before it is on screen and a vblank has passed since it appeared (this frame is
// late, and vsync would hold it for the rest of this refresh too).
uint32_t LateFlipMode(int32_t handle, uint64_t now) {
  const uint64_t period = g_vblank_ticks.load(std::memory_order_relaxed);
  if (!period) return kFlipVsync;
  uint64_t flip[16] = {};
  if (sceVideoOutGetFlipStatus(handle, flip) != 0) return kFlipVsync;
  const int64_t shown = int64_t(flip[3]);
  // a flip still waiting for its vblank: this one follows it, in order
  if (shown <= 0 || shown != g_last_submitted.load(std::memory_order_relaxed)) return kFlipVsync;
  static const uint64_t tsc_hz = sceKernelGetTscFrequency();
  uint64_t shown_tsc = flip[2];
  if (shown_tsc > now || now - shown_tsc > tsc_hz) {
    if (g_seen_argument.load(std::memory_order_acquire) != shown) return kFlipVsync;
    shown_tsc = g_seen_tsc.load(std::memory_order_relaxed);
    if (shown_tsc > now || now - shown_tsc > tsc_hz) return kFlipVsync;
  }
  uint64_t vblank[8] = {};
  if (sceVideoOutGetVblankStatus(handle, vblank) != 0 || !vblank[2] || vblank[2] > now) {
    return kFlipVsync;
  }
  // the latest vblank (a stale status carried forward by whole periods)
  const uint64_t phase = (now - vblank[2]) % period;
  const uint64_t last_vblank = now - phase;
  // on time: no vblank since the frame on screen appeared
  if (last_vblank <= shown_tsc + period / 2) return kFlipVsync;
  // nearly at the next vblank anyway: wait for it rather than tear the last lines
  if (period - phase < tsc_hz / 800) return kFlipVsync;
  return kFlipHsync;
}
// The game's vblank: vblanks followed and vblanks held back
std::atomic<uint64_t> g_vblanks_followed{0}, g_vblanks_held{0};

// The display's vblank count, for the guest vblank (0 when unreadable); called
// by the emulator's vblank thread alone. A vblank is held back from the game
// while two of its frames wait to be shown: with each frame shown for two
// vblanks, a frame late once leaves the next ones a vblank behind for good,
// a frame more of latency, unless the game waits one vblank longer once.
uint64_t DisplayVblankCount() {
  static uint64_t last_real = 0, held = 0, held_at = 0;
  uint64_t status[8] = {};
  const int handle = g_handle.load(std::memory_order_relaxed);
  if (handle < 0 || sceVideoOutGetVblankStatus(handle, status) != 0 || !status[0]) {
    return 0;
  }
  const uint64_t real = status[0];
  if (real != last_real) {
    if (last_real) {
      g_vblanks_followed.fetch_add(real - last_real, std::memory_order_relaxed);
    }
    last_real = real;
    uint64_t flip[16] = {};
    if (sceVideoOutGetFlipStatus(handle, flip) == 0) {
      const int64_t shown = int64_t(flip[3]);
      const int64_t waiting = g_last_submitted.load(std::memory_order_relaxed) - shown;
      if (shown > 0 && waiting >= 2 && real - held_at >= 8 && real - held > 0) {
        ++held;
        held_at = real;
        g_vblanks_held.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
  return real - held;
}

double TicksToMs(uint64_t ticks) {
  static const double ms_per_tick = 1000.0 / double(sceKernelGetTscFrequency());
  return double(ticks) * ms_per_tick;
}

void Log(const char* line) {
  REXLOG_INFO("[WoS-PACE] {}", line);
  prospero_startup_log(line);
}

void Watch() {
  const int handle = g_handle.load();
  const uint64_t tsc_hz = sceKernelGetTscFrequency();
  // The vblank period, from VideoOut's vblank count and time (words 0 and 2),
  // checked against the outputs the console offers; 59.94 Hz when unreadable
  uint64_t vblank_ticks = uint64_t(double(tsc_hz) / 59.94006);
  {
    uint64_t first[8] = {}, last[8] = {};
    const int r0 = sceVideoOutGetVblankStatus(handle, first);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const int r1 = sceVideoOutGetVblankStatus(handle, last);
    uint64_t flip[16] = {};
    sceVideoOutGetFlipStatus(handle, flip);
    char line[256];
    std::snprintf(line, sizeof line,
                  "pacing: vblank status %d/%d count %llu->%llu tsc %llu->%llu; flip status "
                  "%llx %llx %llx %llx %llx %llx %llx %llx; now %llu",
                  r0, r1, (unsigned long long)first[0], (unsigned long long)last[0],
                  (unsigned long long)first[2], (unsigned long long)last[2],
                  (unsigned long long)flip[0], (unsigned long long)flip[1],
                  (unsigned long long)flip[2], (unsigned long long)flip[3],
                  (unsigned long long)flip[4], (unsigned long long)flip[5],
                  (unsigned long long)flip[6], (unsigned long long)flip[7],
                  (unsigned long long)__rdtsc());
    Log(line);
    if (r0 == 0 && r1 == 0 && last[0] > first[0] && last[2] > first[2]) {
      const uint64_t measured = (last[2] - first[2]) / (last[0] - first[0]);
      const double ms = TicksToMs(measured);
      if ((ms > 16.0 && ms < 17.4) || (ms > 8.0 && ms < 8.7)) {
        vblank_ticks = measured;
      }
    }
    g_vblank_ticks.store(vblank_ticks, std::memory_order_relaxed);
    std::snprintf(line, sizeof line, "pacing: vblank %.3f ms (%.2f Hz)", TicksToMs(vblank_ticks),
                  1000.0 / TicksToMs(vblank_ticks));
    Log(line);
  }

  if (REXCVAR_GET(wos_display_pacing)) {
    // Only where the display's refresh is the game's (a 59.94 Hz output, the game
    // at its 60): not for the 120 Hz experiment or a 120 Hz output
    const double game_hz = REXCVAR_GET(video_mode_refresh_rate);
    const double display_ms = TicksToMs(vblank_ticks);
    char line[160];
    if (display_ms > 12.0 && game_hz > 55.0 && game_hz < 65.0) {
      rex_gpu_set_host_vblank_counter(&DisplayVblankCount);
      const int result = wsi_videoout_set_flip_rate(1);
      std::snprintf(line, sizeof line,
                    "pacing: the game's vblank follows the display's; each flip shown for at "
                    "least 2 vblanks (%s)",
                    result == 0 ? "set" : "REFUSED");
    } else {
      std::snprintf(line, sizeof line,
                    "pacing: not applied (display %.2f Hz, the game's %.2f Hz)", 1000.0 / display_ms,
                    game_hz);
    }
    Log(line);
  }

  if (REXCVAR_GET(wos_adaptive_sync) || REXCVAR_GET(wos_even_pacing)) {
    char line[160];
    std::snprintf(line, sizeof line, "pacing: adaptive sync %s, even pacing %s",
                  REXCVAR_GET(wos_adaptive_sync) ? "on" : "off",
                  REXCVAR_GET(wos_even_pacing) ? "on" : "off");
    Log(line);
  }

  int64_t last_argument = 0;
  uint64_t last_shown_tsc = 0;
  uint64_t late_before = 0, waits_before = 0, wait_ticks_before = 0, timeouts_before = 0;
  uint64_t unusable_times = 0;
  Window window;
  uint64_t window_swaps = g_swaps.load(std::memory_order_relaxed);
  uint64_t followed_before = 0, held_before = 0;
  auto window_start = std::chrono::steady_clock::now();
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    uint64_t status[16] = {};
    if (sceVideoOutGetFlipStatus(handle, status) == 0) {
      const int64_t argument = int64_t(status[3]);
      if (argument != 0 && argument != last_argument) {
        const uint64_t now = __rdtsc();
        // When it was shown: the status's own time when plausible (no later than
        // now, within 50 ms), else this poll's (2 ms late at most)
        uint64_t shown_tsc = status[2];
        if (shown_tsc > now || now - shown_tsc > tsc_hz / 20 || shown_tsc < last_shown_tsc) {
          shown_tsc = now;
          ++unusable_times;
        }
        if (last_argument != 0 && argument > last_argument) {
          const uint64_t on_screen = shown_tsc - last_shown_tsc;
          const uint64_t vblanks =
              std::clamp<uint64_t>((on_screen + vblank_ticks / 2) / vblank_ticks, 1, 5);
          ++window.vblanks[vblanks];
          window.never_shown += uint64_t(argument - last_argument - 1);
        }
        ++window.shown;
        const Flip& flip = g_flips[uint64_t(argument) % kFlipRing];
        if (flip.argument.load(std::memory_order_acquire) == argument && flip.tsc &&
            shown_tsc >= flip.tsc) {
          const double queue_ms = TicksToMs(shown_tsc - flip.tsc);
          window.queue_ms_sum += queue_ms;
          window.queue_ms_max = std::max(window.queue_ms_max, queue_ms);
          // From the latest frame the game had handed over when the flip was
          // queued: the frame shown is that one or an earlier one, so this is the
          // least it took
          double game_ms = 0;
          if (flip.swaps) {
            const uint64_t swap_tsc = g_swap_tsc[(flip.swaps - 1) % kSwapRing];
            if (swap_tsc && shown_tsc >= swap_tsc) {
              game_ms = TicksToMs(shown_tsc - swap_tsc);
            }
          }
          window.game_ms_sum += game_ms;
          window.game_ms_max = std::max(window.game_ms_max, game_ms);
          ++window.latency_count;
        }
        last_argument = argument;
        last_shown_tsc = shown_tsc;
      }
    }

    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - window_start).count();
    if (seconds >= 10.0) {
      const uint64_t swaps = g_swaps.load(std::memory_order_relaxed);
      const uint64_t timed = window.vblanks[1] + window.vblanks[2] + window.vblanks[3] +
                             window.vblanks[4] + window.vblanks[5];
      auto share = [&](uint64_t n) { return timed ? 100.0 * double(n) / double(timed) : 0.0; };
      const double count = window.latency_count ? double(window.latency_count) : 1.0;
      char line[400];
      std::snprintf(
          line, sizeof line,
          "display: %.1f shown/s (game %.1f/s); on screen 1 vblank %.1f%%, 2 %.1f%%, 3 %.1f%%, "
          "4 %.1f%%, 5+ %.1f%%; never shown %llu; game->screen >= %.0f ms (max %.0f), "
          "queued->screen %.0f ms (max %.0f); game vblank %s (held back %llu)%s",
          double(window.shown) / seconds, double(swaps - window_swaps) / seconds,
          share(window.vblanks[1]), share(window.vblanks[2]), share(window.vblanks[3]),
          share(window.vblanks[4]), share(window.vblanks[5]),
          (unsigned long long)window.never_shown, window.game_ms_sum / count,
          window.game_ms_max, window.queue_ms_sum / count, window.queue_ms_max,
          g_vblanks_followed.load(std::memory_order_relaxed) != followed_before
              ? "follows the display"
              : "from its timer",
          (unsigned long long)(g_vblanks_held.load(std::memory_order_relaxed) - held_before),
          unusable_times ? " (flip times from polling)" : "");
      followed_before = g_vblanks_followed.load(std::memory_order_relaxed);
      held_before = g_vblanks_held.load(std::memory_order_relaxed);
      Log(line);
      if (REXCVAR_GET(wos_adaptive_sync) || REXCVAR_GET(wos_even_pacing)) {
        const uint64_t late = g_late_flips.load(std::memory_order_relaxed);
        const uint64_t waits = g_pace_waits.load(std::memory_order_relaxed);
        const uint64_t wait_ticks = g_pace_wait_ticks.load(std::memory_order_relaxed);
        const uint64_t timeouts = g_pace_timeouts.load(std::memory_order_relaxed);
        const uint64_t shown = window.shown ? window.shown : 1;
        std::snprintf(line, sizeof line,
                      "pace: late frames shown at once %llu (%.1f%% of those shown)%s; waited for "
                      "the display before %llu presents, %.1f ms each; %llu timeouts",
                      (unsigned long long)(late - late_before),
                      100.0 * double(late - late_before) / double(shown),
                      g_hsync_refused.load(std::memory_order_relaxed) ? " - REFUSED by VideoOut, off"
                                                                       : "",
                      (unsigned long long)(waits - waits_before),
                      waits != waits_before
                          ? TicksToMs(wait_ticks - wait_ticks_before) / double(waits - waits_before)
                          : 0.0,
                      (unsigned long long)(timeouts - timeouts_before));
        Log(line);
        late_before = late;
        waits_before = waits;
        wait_ticks_before = wait_ticks;
        timeouts_before = timeouts;
      }
      window = Window{};
      window_swaps = swaps;
      window_start = now;
    }
  }
}

}  // namespace

// The game handed over a frame (wos_ps5_hooks.cpp's VdSwap wrapper)
void NoteGameFrame() {
  const uint64_t n = g_swaps.load(std::memory_order_relaxed);
  g_swap_tsc[n % kSwapRing] = __rdtsc();
  g_swaps.store(n + 1, std::memory_order_release);
}

}  // namespace wos::pacing

extern "C" int __wrap_sceVideoOutOpen(int32_t user, int32_t bus, int32_t index,
                                      const void* parameter) {
  const int handle = __real_sceVideoOutOpen(user, bus, index, parameter);
  if (handle >= 0) {
    int expected = -1;
    if (wos::pacing::g_handle.compare_exchange_strong(expected, handle)) {
      std::thread(wos::pacing::Watch).detach();
    }
  }
  return handle;
}

extern "C" int __wrap_sceVideoOutSubmitFlip(int32_t handle, int32_t buffer_index, uint32_t mode,
                                            int64_t argument) {
  using namespace wos::pacing;
  Flip& flip = g_flips[uint64_t(argument) % kFlipRing];
  flip.tsc = __rdtsc();
  flip.swaps = g_swaps.load(std::memory_order_acquire);
  flip.argument.store(argument, std::memory_order_release);
  uint32_t use_mode = mode;
  if (mode == kFlipVsync && AdaptiveSync()) {
    use_mode = LateFlipMode(handle, flip.tsc);
  }
  int result = __real_sceVideoOutSubmitFlip(handle, buffer_index, use_mode, argument);
  if (use_mode != mode) {
    if (result == 0) {
      g_late_flips.fetch_add(1, std::memory_order_relaxed);
    } else {
      // VideoOut does not take that mode here: as before, for the rest of the run
      g_hsync_refused.store(true, std::memory_order_relaxed);
      char line[120];
      std::snprintf(line, sizeof line,
                    "pacing: VideoOut refused an HSYNC flip (0x%08x); adaptive sync is off",
                    unsigned(result));
      Log(line);
      result = __real_sceVideoOutSubmitFlip(handle, buffer_index, mode, argument);
    }
  }
  g_flip_calls.fetch_add(1, std::memory_order_release);
  if (result == 0) {
    g_last_submitted.store(argument, std::memory_order_relaxed);
  }
  return result;
}

// --- wos_even_pacing: the renderer's present (wos/eot/src/gpu/present.cpp) ------------
// Before a frame's swapchain image is acquired: wait until every frame presented so
// far is on screen. Called under the renderer's lock, by the one thread that presents.
extern "C" void wos_pace_before_present() {
  using namespace wos::pacing;
  if (!EvenPacing()) return;
  const int handle = g_handle.load(std::memory_order_relaxed);
  if (handle < 0) return;
  static const uint64_t tsc_hz = sceKernelGetTscFrequency();
  static bool synced = false;
  static int64_t offset = 0;        // flips made before the presents counted here
  static uint32_t strikes = 0;      // waits in a row that timed out
  static uint32_t rest = 0;         // presents left without waiting, after three strikes
  const int64_t presents = int64_t(g_presents.load(std::memory_order_relaxed));
  int64_t calls = int64_t(g_flip_calls.load(std::memory_order_acquire));
  if (!synced || calls - offset > presents || rest) {
    // the first frame, a flip that was not one of these presents, or resting: take
    // the count as it stands
    offset = calls - presents;
    synced = true;
    if (rest) --rest;
    return;
  }
  const uint64_t start = __rdtsc();
  bool waited = false;
  for (;;) {
    calls = int64_t(g_flip_calls.load(std::memory_order_acquire));
    if (calls - offset >= presents) {
      // every present has had its flip queued: is the last one showing?
      const int64_t last = g_last_submitted.load(std::memory_order_relaxed);
      uint64_t flip[16] = {};
      if (sceVideoOutGetFlipStatus(handle, flip) != 0 || int64_t(flip[3]) >= last) {
        if (waited) {
          g_seen_tsc.store(__rdtsc(), std::memory_order_relaxed);
          g_seen_argument.store(last, std::memory_order_release);
        }
        strikes = 0;
        break;
      }
    }
    if (__rdtsc() - start > tsc_hz / 20) {
      // 50 ms and it is not showing (a present that made no flip, a paused output):
      // go on, and after three in a row stop waiting for a while
      g_pace_timeouts.fetch_add(1, std::memory_order_relaxed);
      offset = int64_t(g_flip_calls.load(std::memory_order_acquire)) - presents;
      if (++strikes >= 3) {
        strikes = 0;
        rest = 300;
      }
      break;
    }
    waited = true;
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  if (waited) {
    g_pace_waits.fetch_add(1, std::memory_order_relaxed);
    g_pace_wait_ticks.fetch_add(__rdtsc() - start, std::memory_order_relaxed);
  }
}

// A present the swapchain took: its flip follows
extern "C" void wos_pace_presented() {
  wos::pacing::g_presents.fetch_add(1, std::memory_order_release);
}
