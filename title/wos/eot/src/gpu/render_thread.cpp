// gpu/render_thread.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "gpu/render_thread.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <condition_variable>
#include <memory>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "core/logging.h"
#include "core/profiling.h"
#include "gpu/device.h"
#include "gpu/draw.h"
#include "gpu/settings.h"
#include "gpu/textures.h"
#if defined(__PROSPERO__)
#include <pthread_np.h> // pthread_set_name_np, libkernel's
#endif

namespace eot::gpu {

namespace {

constexpr u64 kQueueSlots = 1024;
constexpr u64 kQueueMask = kQueueSlots - 1;
constexpr u64 kBatch = 64;
constexpr u64 kPublishEvery = 8;
constexpr u32 kIdleSpins = 4000;
constexpr u32 kSortWindow = 512;
constexpr f64 kSortFillMs = 0.08;

struct Queue {
  std::unique_ptr<RenderCommand[]> slots;
  alignas(64) std::atomic<u64> head{0};
  alignas(64) std::atomic<u64> tail{0};
  alignas(64) std::atomic<u64> executed{0};
  alignas(64) std::atomic<bool> consumer_sleeping{false};
  std::atomic<bool> producer_sleeping{false};
  std::atomic<bool> active{false};
  std::atomic<bool> stop{false};
  std::mutex produce_mutex;
  std::mutex cv_mutex;
  std::condition_variable cv_work, cv_space, cv_done;
  alignas(64) u64 next_seq = 1;
  u64 tail_local = 0;
  u64 tail_published = 0;
  u64 head_cache = 0;
};

void PublishTail(Queue &q) {
  if (q.tail_published == q.tail_local)
    return;
  q.tail_published = q.tail_local;
  q.tail.store(q.tail_local, std::memory_order_seq_cst);
  if (q.consumer_sleeping.load(std::memory_order_seq_cst)) {
    std::lock_guard cv_lock(q.cv_mutex);
    q.cv_work.notify_one();
  }
}

Queue &queue() {
  static auto *q = new Queue;
  return *q;
}

void PublishExecuted(Queue &q, u64 seq) {
  q.executed.store(seq, std::memory_order_release);
  std::lock_guard lock(q.cv_mutex);
  q.cv_done.notify_all();
}

void Prefetch(const RenderCommand &c) {
  const auto *base = reinterpret_cast<const char *>(&c);
  for (u32 off = 0; off < offsetof(RenderCommand, draw) + offsetof(DrawPacket, window); off += 64)
    eot::cpu::Prefetch(base + off);
  const auto *image = reinterpret_cast<const char *>(c.draw.window.image);
  for (const DeviceWindow::Block &b : DeviceWindow::kBlocks)
    for (u32 off = 0; off < b.size; off += 64)
      eot::cpu::Prefetch(image + b.base + off);
}

void Execute(VideoState &s, Queue &q, RenderCommand &c) {
  switch (c.type) {
  case RenderCommandType::Draw:
    // WoS port (fix111): a deferred capture's render-thread half (a no-op once done)
    if (!c.draw.deferFailed && !FinishDeferredDraw(s, c.draw))
      c.draw.deferFailed = true;
    if (!c.draw.deferFailed)
      ReplayDrawLocked(s, c.draw);
    break;
  case RenderCommandType::Clear:
    ReplayClearLocked(s, c.clear);
    break;
  case RenderCommandType::Resolve:
    ReplayResolveLocked(s, c.resolve);
    break;
  case RenderCommandType::Present:
    PresentLocked(s, c.va);
    PublishExecuted(q, c.seq);
    break;
  case RenderCommandType::Unlock:
    NotifyResourceUnlockedLocked(s, c.va);
    break;
  case RenderCommandType::Retire:
    ParkBuffer(s, std::unique_ptr<plume::RenderBuffer>(c.buffer));
    c.buffer = nullptr;
    break;
  }
}

u64 WaitForWork(Queue &q, u64 head) {
  for (u32 i = 0; i < kIdleSpins; ++i) {
    const u64 t = q.tail.load(std::memory_order_acquire);
    if (t != head || q.stop.load(std::memory_order_relaxed))
      return t;
    eot::cpu::Relax();
  }
  std::unique_lock lock(q.cv_mutex);
  q.consumer_sleeping.store(true, std::memory_order_seq_cst);
  q.cv_work.wait(lock, [&] {
    return q.tail.load(std::memory_order_seq_cst) != head || q.stop.load(std::memory_order_relaxed);
  });
  q.consumer_sleeping.store(false, std::memory_order_seq_cst);
  return q.tail.load(std::memory_order_acquire);
}

u32 GatherSortRun(Queue &q, u64 head, u64 avail, u64 *keys) {
  const RenderCommand &first = q.slots[head & kQueueMask];
  u32 run = 0;
  u32 first_func = 0;
  for (; run < kSortWindow && run < avail; ++run) {
    const RenderCommand &c = q.slots[(head + run) & kQueueMask];
    if (c.type != RenderCommandType::Draw)
      break;
    if (run && std::memcmp(&c.draw.targets, &first.draw.targets, sizeof(TargetWords)) != 0)
      break;
    u32 func = 0;
    const u64 key = DrawSortKey(c.draw, &func);
    if (!key)
      break;
    if (run == 0)
      first_func = func;
    else if (func != first_func)
      break;
    keys[run] = key;
  }
  return run;
}

void PublishHead(Queue &q, u64 head) {
  q.head.store(head, std::memory_order_seq_cst);
  if (q.producer_sleeping.load(std::memory_order_seq_cst)) {
    std::lock_guard cv_lock(q.cv_mutex);
    q.cv_space.notify_all();
  }
}

void WorkerMain() {
#if defined(_WIN32)
  SetThreadDescription(GetCurrentThread(), L"reeot render");
#elif defined(__APPLE__)
  pthread_setname_np("reeot render");
#elif defined(__PROSPERO__)
  pthread_set_name_np(pthread_self(), "reeot render");
#else
  pthread_setname_np(pthread_self(), "reeot render");
#endif
  PinThreadToPhysicalCore(2, "the render thread");
#if defined(EOT_PROFILING) && defined(REXGLUE_ENABLE_PROFILING)
  if (TracyIsStarted)
    tracy::SetThreadName("render");
#endif
  auto &q = queue();
  auto &s = state();
  static u64 sort_keys[kSortWindow];
  static u32 sort_order[kSortWindow];
  u64 head = q.head.load(std::memory_order_relaxed);
  for (;;) {
    u64 tail;
    {
      PerfScope idle_scope(s.perf.worker_idle_ms);
      tail = WaitForWork(q, head);
    }
    if (q.stop.load(std::memory_order_acquire) || s.shutting_down.load(std::memory_order_acquire))
      break;
    u64 avail = tail - head;
    u32 run = GatherSortRun(q, head, avail, sort_keys);
    if (run && run == avail && run < kSortWindow) {
      const u64 t0 = PerfNow();
      while (static_cast<f64>(PerfNow() - t0) * PerfMsPerTick() < kSortFillMs) {
        const u64 t = q.tail.load(std::memory_order_acquire);
        if (t - head > avail) {
          avail = t - head;
          run = GatherSortRun(q, head, avail, sort_keys);
          if (run < avail || run == kSortWindow)
            break;
        }
        if (q.stop.load(std::memory_order_relaxed))
          break;
        eot::cpu::Relax();
      }
    }
    EOT_CPU_ZONE("render batch");
    std::lock_guard lock(s.mutex);
    if (run >= 2) {
      // fix111: deferred capture halves in queue order first - a later draw may reuse an
      // earlier one's constant upload, which is only byte-swapped by the earlier one's half
      for (u32 i = 0; i < run; ++i) {
        RenderCommand &dc = q.slots[(head + i) & kQueueMask];
        if (dc.type == RenderCommandType::Draw && !dc.draw.deferFailed &&
            !FinishDeferredDraw(s, dc.draw))
          dc.draw.deferFailed = true;
      }
      for (u32 i = 0; i < run; ++i)
        sort_order[i] = i;
      std::stable_sort(sort_order, sort_order + run,
                       [&](u32 a, u32 b) { return sort_keys[a] < sort_keys[b]; });
      for (u32 i = 0; i < run; ++i) {
        if (i + 1 < run)
          Prefetch(q.slots[(head + sort_order[i + 1]) & kQueueMask]);
        Execute(s, q, q.slots[(head + sort_order[i]) & kQueueMask]);
      }
      head += run;
      s.perf.sorted_draws += run;
      s.perf.sort_runs++;
      PublishHead(q, head);
      continue;
    }
    u64 n = std::min<u64>(avail, kBatch);
    for (u64 i = run ? 1 : 0; i < n; ++i) {
      const RenderCommand &c = q.slots[(head + i) & kQueueMask];
      if (c.type == RenderCommandType::Draw && DrawSortKey(c.draw)) {
        n = i;
        break;
      }
    }
    if (!n)
      n = 1;
    for (u64 i = 0; i < n; ++i) {
      RenderCommand &c = q.slots[head & kQueueMask];
      if (i + 1 < n)
        Prefetch(q.slots[(head + 1) & kQueueMask]);
      Execute(s, q, c);
      ++head;
      if ((head & (kPublishEvery - 1)) == 0 || i + 1 == n)
        PublishHead(q, head);
    }
  }
  q.active.store(false, std::memory_order_release);
  std::lock_guard lock(q.cv_mutex);
  q.cv_done.notify_all();
  q.cv_space.notify_all();
}

}

void RenderThreadStart() {
  auto &q = queue();
  if (q.active.load(std::memory_order_acquire))
    return;
  q.slots = std::make_unique<RenderCommand[]>(kQueueSlots);
  q.stop.store(false, std::memory_order_release);
  q.active.store(true, std::memory_order_release);
  std::thread(WorkerMain).detach();
  EOT_INFO("[gpu] render thread started: {} command slots ({} MB), batches of {}", kQueueSlots,
           (kQueueSlots * sizeof(RenderCommand)) >> 20, kBatch);
}

bool RenderThreadActive() { return queue().active.load(std::memory_order_acquire); }

void RenderThreadStop() {
  auto &q = queue();
  q.stop.store(true, std::memory_order_release);
  std::lock_guard lock(q.cv_mutex);
  q.cv_work.notify_all();
  q.cv_space.notify_all();
  q.cv_done.notify_all();
}

RenderEnqueue::RenderEnqueue() : lock_(queue().produce_mutex) {
  auto &q = queue();
  const u64 t = q.tail_local;
  if (t - q.head_cache >= kQueueSlots) {
    q.head_cache = q.head.load(std::memory_order_acquire);
    if (t - q.head_cache >= kQueueSlots) {
      EOT_CPU_ZONE("render queue full");
      PublishTail(q);
      std::unique_lock cv_lock(q.cv_mutex);
      q.producer_sleeping.store(true, std::memory_order_seq_cst);
      q.cv_space.wait(cv_lock, [&] {
        return t - q.head.load(std::memory_order_seq_cst) < kQueueSlots ||
               q.stop.load(std::memory_order_relaxed);
      });
      q.producer_sleeping.store(false, std::memory_order_seq_cst);
      q.head_cache = q.head.load(std::memory_order_acquire);
    }
  }
  if (q.stop.load(std::memory_order_acquire)) {
    static RenderCommand scratch;
    cmd_ = &scratch;
    return;
  }
  cmd_ = &q.slots[t & kQueueMask];
  cmd_->seq = q.next_seq;
}

// WoS port (fix111): wos_capture_prefetch - the next command slot, for writing. 1024 slots of
// ~14 KB each: a slot was last touched a thousand commands ago, so every capture's writes
// (the device window above all, ~0.5 us a draw) missed the cache. Asked for right after a
// commit, the lines arrive while the game runs its own code up to the next draw.
extern "C" bool wos_capture_prefetch();
static void PrefetchSlotForWrite(const RenderCommand &c) {
  const auto *base = reinterpret_cast<const char *>(&c);
  const size_t window_at = offsetof(RenderCommand, draw) + offsetof(DrawPacket, window);
  for (size_t off = 0; off < window_at; off += 64)
    __builtin_prefetch(base + off, 1, 3);
  const auto *image = reinterpret_cast<const char *>(c.draw.window.image);
  for (const DeviceWindow::Block &b : DeviceWindow::kBlocks)
    for (u32 off = 0; off < b.size; off += 64)
      __builtin_prefetch(image + b.base + off, 1, 3);
  const size_t tail_at = offsetof(RenderCommand, draw) + offsetof(DrawPacket, hasCameraVP);
  for (size_t off = tail_at; off < sizeof(RenderCommand); off += 64)
    __builtin_prefetch(base + off, 1, 3);
}

u64 RenderEnqueue::commit(bool publish) {
  auto &q = queue();
  const u64 seq = q.next_seq++;
  if (q.stop.load(std::memory_order_acquire)) {
    committed_ = true;
    return seq;
  }
  cmd_->seq = seq;
  ++q.tail_local;
  static const bool prefetch_next = wos_capture_prefetch();
  if (prefetch_next)
    PrefetchSlotForWrite(q.slots[q.tail_local & kQueueMask]);
  if (publish || q.tail_local - q.tail_published >= kPublishEvery ||
      q.consumer_sleeping.load(std::memory_order_relaxed))
    PublishTail(q);
  committed_ = true;
  return seq;
}

RenderEnqueue::~RenderEnqueue() = default;

void RenderThreadWait(u64 seq) {
  auto &q = queue();
  if (q.executed.load(std::memory_order_acquire) >= seq)
    return;
  std::unique_lock lock(q.cv_mutex);
  q.cv_done.wait(lock, [&] {
    return q.executed.load(std::memory_order_acquire) >= seq ||
           q.stop.load(std::memory_order_relaxed) || !q.active.load(std::memory_order_relaxed);
  });
}

void RenderThreadUnlock(u32 resource_va) {
  RenderEnqueue e;
  e.cmd().type = RenderCommandType::Unlock;
  e.cmd().va = resource_va;
  e.commit();
}

void RenderThreadRetire(plume::RenderBuffer *buffer) {
  if (!buffer)
    return;
  RenderEnqueue e;
  e.cmd().type = RenderCommandType::Retire;
  e.cmd().buffer = buffer;
  e.commit();
}


// WoS port (fix43): honest fences. The game frees and refills memory (textures in the
// menus, skinning and UI data) as soon as a fence it inserted reads as passed; with every
// fence passed at once the render thread - a frame behind, uploading textures from guest
// memory as it replays - read memory already reused: one-frame wrong textures (the stripes
// behind the title banner, the pause panels, the logos vanishing) and broken meshes.
u64 RenderThreadCommittedSeq() {
  std::lock_guard lock(queue().produce_mutex);
  return queue().next_seq - 1;
}
u64 RenderThreadExecutedSeq() { return queue().executed.load(std::memory_order_acquire); }
void RenderThreadPublish() {
  auto &q = queue();
  std::lock_guard lock(q.produce_mutex);
  PublishTail(q);
}

}

extern "C" uint64_t wos_rt_committed() { return eot::gpu::RenderThreadCommittedSeq(); }
extern "C" uint64_t wos_rt_executed() { return eot::gpu::RenderThreadExecutedSeq(); }
extern "C" void wos_rt_publish() { eot::gpu::RenderThreadPublish(); }
extern "C" void wos_rt_wait(uint64_t seq) { eot::gpu::RenderThreadWait(seq); }
extern "C" bool wos_rt_active() { return eot::gpu::RenderThreadActive(); }
