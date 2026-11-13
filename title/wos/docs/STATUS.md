# Where the port stands (for whoever picks it up next)

## Runs on the console (firmware 13.00)

- Boots to the title screen, cutscenes (Bink, needs `render_target_path_vulkan = "fsi"`),
  gameplay in the city. Sound, pad, saves.
- Built with LLVM's own clang 20.1.8 (`wos/tools/get-llvm20.sh`): Ubuntu 26.04's clang-20
  crashes, and clang 21 makes an eboot firmware 13.00 refuses (EXECFAIL error=12).
- Codegen fixes in `wos/codegen/wos_recomp_manifest.toml` (function boundaries checked
  against the disassembly; 166 pointer-to-member thunks found only by running).
- Runtime fixes and probes in `wos/patches/rexglue/` (applied by `build-rexglue.sh`,
  listed in the fork's `.wos-patches`):
  - 0001 pipeline storage: seek before append (the PS5 libc loses the writes otherwise)
  - 0002-0005 the performance probe (the `[WoS-PERF]` lines every 10 s, also in klog)
  - 0006 shared memory requests answered without the global lock when in sync

## Performance (the open problem)

Measured in the city, fsi, 1x resolution, pipelines cached:

- 7-10 frames a second; 30 in menus (the game's own cap is 30).
- The GPU is never waited for. One CPU thread is the limit: the emulated GPU's command
  processor, 85-100% busy translating 3,000-7,000 draws a frame (about 16 us each).
- Within draws: shared memory requests (index and vertex buffers) 35-43% of the thread,
  of which 23-30% waiting for the global critical region (guest kernel calls hold it);
  textures 5%, render targets 3-4%, pipelines 3%, bindings 6-10%.
- Memory protection and access faults: about 1%. Occlusion queries: none used.
- A clock read is 883 ns on the console (clock_gettime is a system call): the title
  now wraps clock_gettime with the TSC (wos/app/wos_ps5_hooks.cpp).

After 0006 and the TSC clock (patch 0018): a clock read 17 ns, draws about 4 us, the
command processor 30-40% busy and waiting for the game 60-70%: 11-14 frames a second.
The game's own threads now hold the frame rate. `ignore_thread_affinities = true` (guest
threads off cores 0-5) gave nothing measurable. The `[WoS-THREADS]` lines (patch 0019)
split each busy guest thread's time: running its code, waits, sleeps, critical sections,
spin locks, file reads, VdSwap.

Patch 0019's thread lines: the main game thread ("Main XThread") runs its own code
94-95% of the time in the city (the limit); the three job queue workers about 60%.
(A thread blocked in a long wait shows as running 100% until the wait returns: the
host threads and idle XThreads at 100% are that.) Patch 0020 builds the recompiled code
with the small code model (ReXGlue's large one makes every call an indirect one) and
samples the main thread's and workers' link registers (`samples '...'` lines: the
return addresses they spend their time under, to be matched to functions offline).

Patch 0020's samples (small code model gave no measurable change): the main thread
spent 22-27% inside NtPulseEvent (the runtime's event pulse slept 10 us, far longer on
the console; the game's job queue pulses constantly), and much of the rest in its job
system's atomics, which the generated code brackets with mfmsr/mtmsrd: ReXGlue makes
each the global critical region (contended by every thread and the kernel). Patch 0021:
the pulse yields for 10 us instead (wos/patches/rexglue/0007), and the MSR regions take
a lock per guest CPU (wos/shim/wos_msr.h, written into the generated PCH by
wos_game.cmake). The costs line also times a 10 us sleep.

Next ideas, in order: see what 0006 and the TSC clock gave; then the remaining draw
cost (primitive processing outside its shared memory request, bindings); then fewer
global-lock users on the guest side (XObject::GetNativeObject in every guest wait).
A native renderer (as Liberty Recompiled has for GTA IV) is the road to 60.
