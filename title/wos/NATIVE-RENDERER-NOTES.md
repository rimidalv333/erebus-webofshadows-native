# Native renderer — feasibility notes (session 2026-10-23/24, phase 1)

Goal: replace the emulated Xenos GPU with a native Vulkan renderer that hooks
Web of Shadows' D3D API calls (as Edge of Time's "plume" renderer does), to get
a true higher frame rate instead of per-draw PM4 translation.

## Verdict from phase 1: FEASIBLE, de-risked.
The WoS D3DDevice struct is the SAME SHAPE as Edge of Time's, with small offset
shifts (XDK 6995 / 2008 vs EoT's 2011). EoT's per-hook ALGORITHMS port; we
re-derive the offset constants and reuse the BSD renderer's Vulkan plumbing
(~40-60% reusable: device, present, texture upload).

## Anchors found (WoS recompiled funcs, generated/default/*.cpp)
- D3D library region: ~0x82AB7000 - 0x82AD5000 (~300 functions)
- D3DDevice::CreateDevice core:  sub_82ABAE28  (VdInitializeRingBuffer, VdEnableRingBufferRPtrWriteBack)
- Present (Swap):                sub_82AC4E48  (VdSwap, VdGetSystemCommandBuffer)
- GPU engine init:               sub_82ACD7B8  (VdInitializeEngines, VdSetGraphicsInterruptCallback; stamps 6995)
- SetTexture/sampler/fetch setters cluster: 0x82AB7900 - 0x82AB8300 (small 5-37 insn funcs, device+sampler*24 pattern)

## Device offsets: WoS (found) vs Edge of Time (src/gpu/d3d.h)
| Field                   | WoS                | EoT                  |
|-------------------------|--------------------|----------------------|
| Fetch constants base    | 0x484 (1156)       | kFetchConstants 0x480|
| Fetch stride            | 24 (6 dwords)      | 24                   |
| Dirty/pending mask grp3 | device+24          | kDevPendingGroup3 24 | (exact)
| Sampler mip block       | ~0x2E94 (11924)    | kDevSamplerMinMip 12332|
| Ring buffer base/rptr   | device+14836/14840 | —                    |
| (EoT ref: kDeviceSize 0x6080, kTextureObject0 0x3260, kPixelShader 0x32F4, kVertexShader 0x32F8) |

## Reference material
- Edge of Time renderer (BSD, reusable): /root/work/EdgeOfTimeRecomp/src/gpu/
  - hooks: src/gpu/hooks/{state,resource,stubs,package_load}.cpp (22 D3D hooks)
  - device layout: src/gpu/d3d.h
  - Vulkan plumbing: device.cpp, present.cpp, textures.cpp, pipeline/, shaders/
- rexglue GPU plugin seam: include/rex/system/interfaces/graphics.h (IGraphicsSystem)
  — the renderer replaces this, injected at build time (rex_gpu_create).
- hedge-dev XenosRecomp (MIT, shader microcode->SPIR-V): reference, not drop-in.

## Next steps (resume here)
1. Map all ~22 D3D entry points to sub_ addresses (DrawIndexedVertices,
   DrawVertices, SetVertexShader, SetPixelShader, SetStreamSource, SetIndices,
   SetRenderTarget, SetDepthStencilSurface, SetViewport, Resolve, ClearF,
   Begin/End[Indexed]Vertices, XGRegisterVertex/PixelShader, UnlockResource, Swap).
2. Complete the device offset table (diff EoT's d3d.h constant-by-constant).
3. Fork EoT renderer into the title; get Vulkan device/present building on our stack.
4. Prototype one hook (SetTexture or ClearF) end to end before mapping the rest.

## KEY INSIGHT (session 2): how EoT actually hooks (resolves the strategy)
EoT does NOT reimplement the D3D functions. It --wraps the recompiled
D3DDevice_* functions:
  - lets the original run (__imp__D3DDevice_Xxx) — its PM4 output is ignored
  - reads the ARGUMENTS from registers (r3=device, r4.. = params)
  - reads device state via the offset table
  - issues a native Vulkan draw/state change
See EdgeOfTimeRecomp/src/gpu/hooks/state.cpp D3DDevice_DrawIndexedVertices.

Therefore the ONLY per-function knowledge needed is:
  (a) which sub_XXXX is which D3D function (to --wrap it by address), and
  (b) the device struct offsets (already compatible, small shifts).
The function internals/PM4 do NOT need understanding.

Identification strategy: match by ARGUMENT SIGNATURE + call frequency + the
device offsets each touches, cross-referenced with EoT's d3d.h/state.cpp logic.
DrawIndexedVertices = (device, prim, baseVertex, startIndex, count), hot.

## Session 2 progress — API surface identification
Method that works: rank D3D-region funcs by EXTERNAL (game-side) call sites.
Public API entry points (ext calls high, int low):
  sub_82AB9300 (ext43) — thunk → 0x82AB8CC8
  sub_82AB9028 (ext36) — SetRenderTarget/SetStreamSource-like: (device, object),
                 stores object@12456, reads object+28/+32 → device+10376/10560, dirty bits
  sub_82ABD328 (ext34), sub_82ABFCB0 (ext25), sub_82ABCD20 (ext15),
  sub_82AB8420 (ext15), sub_82AB7588 (ext14), sub_82ABECF0 (ext14) ...

Identification is CONFIRMED-DOABLE but methodical: each candidate needs its body
read and matched to an EoT hook by (arg signature + device offsets touched + the
EoT logic in state.cpp/resource.cpp). ~22 functions, a few confidently placed so far.

## EoT hook ordering (from its config) — cross-check for relative layout
Resolve, ClearF, SetViewport, SetStreamSource, SetIndices, SetRenderTarget,
SetDepthStencilSurface, SetGammaRamp, SetPWLGamma, BeginVertices,
BeginIndexedVertices, DrawVertices, DrawIndexedVertices, Swap, SetTexture,
CreatePixelShader, SetPixelShader, CreateVertexShader, SetVertexShader.
(Different XDK point rev — relative order may hold, absolute addrs won't.)

## Realistic effort (updated)
Phase 1 (identify 22 funcs + full offset table): the methodical core. Each
function ~careful read. This is the bulk of the "weeks". De-risked (layout
compatible) but not fast. No automation shortcut found that's reliable — EoT's
compiled bodies aren't available to byte-match against.

## Session 2 — CONFIRMED function mappings
| D3D function | WoS sub_  | confidence | evidence |
|--------------|-----------|-----------|----------|
| SetTexture   | 82ABD328  | HIGH | FastSetTexture logic: packs 6 fetch dwords @device+1152+stage*24, mip-clamp vs device+11950/11976, dirty@device+24, tex obj@device+(stage+3136)*4 |
| ClearF       | 82ABFCB0  | HIGH | (device,flags,rect,color,z=f1,stencil=r8) — exact match to EoT ClearF |
| SetRT/Stream | 82AB9028  | MED  | (device,object); obj@12456; reads obj+28/+32 → device+10376/10560 |
| SetVtxTexture?| 82AB8CC8 (thunk 82AB9300) | MED | stores tex ptr @device+(idx+3110)*4, no fetch packing |

## CONFIRMED device offsets (high confidence)
- Fetch constants: base 1152 (0x480), stride 24 — EXACT match to EoT kFetchConstants
- Sampler min-mip: device+11950 ; max-mip: device+11976
- Dirty/pending mask: device+24 (64-bit) = EoT kDevPendingGroup3
- Pixel tex array: device+12544 + stage*4 ; vertex tex array: device+12440

## Method that works (for resuming)
1. Rank D3D-region funcs (0x82AB7000-0x82AD5000) by external/game-side call count
   → that's the public API surface (see scratchpad script).
2. Read each candidate, match arg signature (r3=device, r4+=params, f1=float) and
   device offsets touched against EoT hooks in src/gpu/hooks/state.cpp & resource.cpp.
3. State setters (SetTexture, ClearF, SetViewport, Set*Shader) are easiest — distinct sigs.
4. Draw funcs (DrawIndexedVertices/DrawVertices) are harder — called from deeper engine
   layers, not co-located with SetTexture. Next: trace from the present/kickoff path, or
   find the common draw-emit helper they all call.

## STATUS: 3-4 of ~22 mapped with confidence. The device layout is confirmed
## compatible. This is steady multi-session work; no reliable automation shortcut.

## Session 2 cont. — DRAW FAMILY FOUND (the key targets)
| D3D function | WoS sub_ | ext | confidence | evidence |
|--------------|----------|-----|-----------|----------|
| DrawIndexedVertices | 82ABF0E0 | 5  | HIGH | 5 args (device,prim,baseVtx,startIdx,count); flushes pending groups @0/8/16/24/32; tail does index-buffer offset arithmetic (lwzx on arg<<3) |
| DrawVertices        | 82ABECF0 | 14 | HIGH | 4 args; flushes pending state groups @device+0/8/16/24/32 (=EoT kDevPendingGroup0..4); most-called draw |
| DrawVerticesUP/Begin| 82ABE800 | 2  | MED  | count*stride, writes UP buffer @13444/13448/13456 |
| (shared pre-draw state-flush helper: 82ACBAF8; apply-pending helper) |

## Pending-state dirty groups CONFIRMED at device + 0, 8, 16, 24, 32
Exactly matches EoT kDevPendingGroup0..4. The draws read these and flush each.

## Session 2 cont. — more offsets
- Render-target slots: 4 RTs compared as pairs device+12440/12728, 12444/12732,
  12448/12736, 12452/12740 (current vs pending). sub_82AC8B70 (ext6) does RT-change
  check / flush. (EoT render target handling in state.cpp SetRenderTarget.)
- Index buffer binding: device+12436 (EoT kIndexBuffer 0x31F4=12788, same region).
- Shader pointer store seen @device+12708 region (vs EoT kVertexShader 13048).
- Flag byte (kDevShaderFlags-equiv): device+10940/10941 (EoT 11070).

## MAPPED SO FAR (session 2 total): 6 functions
DrawIndexedVertices 82ABF0E0 (HIGH) | DrawVertices 82ABECF0 (HIGH) |
DrawVerticesUP/Begin 82ABE800 (MED) | SetTexture 82ABD328 (HIGH) |
ClearF 82ABFCB0 (HIGH) | SetRenderTarget-area 82AC8B70 (MED)
Plus device offset table substantially filled.

## NEXT (resume): SetVertexShader/SetPixelShader (apply literal table, flag@10941),
## SetStreamSource, SetIndices, SetViewport, Swap, then scaffold + prototype one hook.

## MAJOR FINDING (session 2 autonomous) — the renderer foundation ALREADY RUNS ON PS5
Edge of Time's renderer is built on **plume** (thirdparty/plume: a cross-platform
render interface over Vulkan/D3D12) + SDL3. ~16k lines; draw.cpp alone is 3478.

CRITICAL: PS5_EdgeOfTimeTitle ALREADY got plume running on the console:
- reeot/plume_test.cpp: plume::CreateVulkanInterface() gets Vulkan from the SAME
  RADV we link; surface via VK_KHR_display (same as our VideoOut WSI).
- It created device+queue+swapchain and cleared the screen R/G/B/W on the actual
  PS5 display. Ran successfully: results/2026-10-12-test5.txt.
- There's a ps5/patches/plume-ps5.patch for the plume checkout.

=> The hardest infra risk ("can this renderer stack boot on jailbroken PS5?") is
   ALREADY ANSWERED YES. We are not pioneering plume-on-PS5; it exists and works.

## Revised architecture for the WoS native renderer
1. Foundation: plume on RADV (proven) — device/queue/swapchain/present. REUSE.
2. The game's D3D calls → our --wrapped hooks (addresses being mapped) →
   translate to plume draw/state calls (EoT's draw.cpp is the template, ~3478 lines,
   game-agnostic-ish: reads device struct + builds plume pipelines/draws).
3. Shader translation: Xenos microcode → SPIR-V. EoT uses its own path; XenosRecomp
   (MIT) is the reference. This is the remaining big unknown to validate.

## What this means for a boot-capable prototype
- Clear-screen on PS5 via plume: PROVEN (reuse plume_test).
- Hook ClearF → plume clearColor: small, high-confidence first step.
- Hook one textured draw: needs shader translation working. That's the real gate.

## THE REAL GATE — shader translation (session 2 autonomous)
EoT does NOT translate shaders at runtime. It uses a PRECOMPILED EMBEDDED cache:
- At BUILD time: XenosRecomp scans the game's shader containers (extracted from
  the game files), translates Xenos microcode -> DXIL + SPIR-V, bakes them into
  the exe as g_shaderCacheEntries[] (gpu/shaders/shader_cache.h).
- At RUNTIME: look up translated SPIR-V by shader hash. No live translation.
- shader_dump.h: "the game's shader containers, gathered for XenosRecomp where the
  game is" — i.e. must be extracted from WoS's own files per build.

IMPLICATION: a working native renderer needs:
  (a) extract WoS's shaders from its game files,
  (b) run XenosRecomp on them (MIT, but "won't work out of the box" per its README;
      shader container reverse-engineered "just enough for Unleashed" — WoS may need
      more research), producing the SPIR-V cache,
  (c) embed that cache.
XenosRecomp submodule in the EoT checkout is EMPTY (not fetched) — need to clone
from hedge-dev/XenosRecomp to even start this.

## BOOT-CONFIDENCE ASSESSMENT (honest, session 2)
PROVEN / low risk:
- plume renderer foundation runs on PS5 (clear screen R/G/B/W, test5 Oct-12). REUSE.
- D3D function hooking model (--wrap named recompiled funcs). Mechanism known.
- Device struct layout compatible with EoT; offsets being mapped (6+ done).
- Clear-screen native-render prototype: achievable with LOW effort.

REMAINING RISK (the gate to rendering the actual game):
- Shader translation via XenosRecomp on WoS shaders = the biggest unknown.
  "Won't work out of the box"; needs per-game reverse-engineering of WoS's shader
  container. Until this works, no guest geometry renders (only clears/UI).
- draw.cpp (3478 lines) porting: translate hooked draws -> plume. Large but
  game-agnostic-ish; EoT's is the template.

VERDICT: A prototype that BOOTS and clears the screen natively is HIGH confidence
and low effort (plume proven + ClearF hook mapped). A prototype that RENDERS THE
GAME WORLD depends on XenosRecomp working for WoS — that's the make-or-break, and
it's the thing to validate next before committing to the full port.

## SHADER GATE — much more passable than feared (session 2 autonomous)
Cloned XenosRecomp (hedge-dev, MIT) to scratchpad/xenosrecomp. Key findings:
- It scans files for the Xbox 360 shader container magic: (flags & 0xFFFFFF00)
  == 0x102A1100 (XenosRecomp/main.cpp line 91). This magic is the STANDARD XDK
  shader container signature — same across all Xbox 360 games, NOT game-specific.
  => WoS's standard XDK 6995 shaders will carry this magic; XenosRecomp will FIND them.
- Core ALU/vector instruction translation "works in most cases" — the bulk of shaders.
- Constant buffers are standard (256 vtx / 224 pixel float4 regs) — WoS shares this.
- Game-specific risks (from README), all bounded "modify the tool" work not "write from scratch":
  1. Shader container reflection: "just enough for Unleashed" — WoS may need more. #1 risk.
  2. Control flow (while/switch pc): barely tested; complex WoS shaders might break.
  3. Vertex-fetch g_SwappedTexcoords: tuned for Unleashed semantics; WoS may need more.

COULD NOT VERIFY HERE: WoS's shaders are in its GAME DATA FILES (not the xex, not
in this sandbox — only default.xex present). Scanning the xex: 0 magic hits (expected;
shaders live in the extracted disc assets the player provides). Need the game's data
files to confirm the shaders scan + translate.

## UPDATED VERDICT (end of autonomous session 2)
Confidence a native renderer can BOOT + clear screen: HIGH (plume proven on PS5).
Confidence it can RENDER THE GAME: MEDIUM-and-rising. The two scariest unknowns both
moved in our favor:
  - renderer foundation on PS5: PROVEN (not a risk).
  - shader container format: standard magic, XenosRecomp will find WoS's shaders.
The residual risk is translation QUALITY (instructions/control-flow/vertex-fetch for
WoS specifically) + the draw.cpp porting volume. Both bounded, both "adapt known code."

NEXT CONCRETE STEP to raise confidence to HIGH: get WoS's game data files (the
extracted disc, which the user has), run XenosRecomp on them, and see how many of
WoS's shaders translate cleanly. That single experiment decides the whole project.

## Session 2 fork — additional function mappings

### NEWLY CONFIRMED (HIGH confidence)
| D3D function  | WoS sub_  | evidence |
|---------------|-----------|----------|
| SetIndices    | 82AB8540  | 2-arg (device, ib=r4); StampReplaced on old @12436; stores ib → device+12436 (=kIndexBuffer). Exact match to EoT FastSetIndices. ext=6 |
| SetStreamSource | 82AB8420 | 6-arg (device,stream,vb,offset,stride,mask); reads vb+24(addr)/vb+28(size); writes fetch-slot addr with the ((addr>>20...)&0x1000)+(addr&0x1FFFFFFF) formula; stwx vb→device+12460+stream*4 (=kStreamObject0); stb stride→device+12528+stream (=kStreamStride0); ORs mask→PendingGroup3(dev+24), 0x80000→PendingGroup2(dev+16). Exact match to EoT FastSetStreamSource. ext=15 |

### NEWLY MAPPED (MED confidence)
| D3D function  | WoS sub_  | evidence |
|---------------|-----------|----------|
| SetDepthStencilSurface | 82AB9028 | 2-arg (device, surface=r4); stores surface→device+12456 (depth slot, =kDepthSurface); reads surface+28/+32; checks all 5 RT/depth slots (12440-12456 vs pending 12728-12744) for ownership change. ext=36. (Could be SetRenderTarget(0); both are pass-through in EoT so distinction is low-stakes.) |
| Swap          | 82AC4E48  | 3-arg (device, front=r4, r5); contains VdSwap + VdGetSystemCommandBuffer; increments frame counter @16560. The present implementation. Called by game sub_82939360. ext=1 |

### CONFIRMED DEVICE OFFSET SHIFT: WoS = EoT - 352 (exact, 6/6 fields)
| field           | EoT   | WoS   |
|-----------------|-------|-------|
| kIndexBuffer    | 12788 | 12436 |
| kRenderTarget0  | 12792 | 12440 |
| kDepthSurface   | 12808 | 12456 |
| kStreamObject0  | 12812 | 12460 |
| kStreamStride0  | 12880 | 12528 |
| kTextureObject0 | 12896 | 12544 |
kVbAddress=24, kVbSize=28 (object offsets, SAME as EoT).
Predicted (unverified): kVertexShader≈12696, kPixelShader≈12692.

### NOT YET RESOLVED
- SetVertexShader / SetPixelShader: the clean -352 shift does NOT locate them.
  No small 2-arg setter stores a shader ptr at 12692/12696, nor ORs the EoT
  marker constants (0x80000/0x120000) into PendingGroup2 except SetStreamSource.
  The &0x7F flag-clear (kDevShaderFlags) appears ONLY in sub_82AC98A8 (ext=0,
  1-arg, writes a command ring @13376) — likely an internal shader-resolve, not
  the public setter. Conclusion: WoS's XDK structures shader binding differently
  around this region; needs deeper tracing (follow the funcs the game calls right
  before draws that touch the shader sub-object). Do NOT guess an address here.
- SetViewport: EoT hook is PASS-THROUGH (no Fast path) — low priority. Not located
  (no func reads vp+16/+20 floats and writes 6 viewport floats to device).
- CreateVertexShader/CreatePixelShader: r3 = shader blob (NOT device-relative),
  so device-offset scanning can't find them; EoT hooks are pass-through debug only.
- SetRenderTarget: EoT hook is PASS-THROUGH. Candidate overlaps SetDepthStencil
  (82AB9028). Low priority for a first prototype.

### KEY STRATEGIC NOTE
Many remaining funcs (SetRenderTarget, SetDepthStencilSurface, SetViewport,
Create*Shader) are PASS-THROUGH in EoT — they only need NAMING (for trace), not
Fast reimplementation. The functions needing real native logic are: the 3 draws
(done), SetTexture (done), SetStreamSource (done), SetIndices (done), and
Set*Shader (UNRESOLVED). So the boot-critical set is ~90% mapped; the shader
setters are the main remaining gap.

## SHADER LOCATION — resolved (session 2, from user's gamedata scan)
Scanned user's 5GB gamedata (751 files, 118 "hits", 271 raw containers). Analysis:
- The sound/*.pck (99) and movies/*.bik hits are FALSE POSITIVES — the 3-byte
  0x102A11 magic appears by chance in compressed audio/video.
- The REAL shaders are in the **.XEPACK archives** (GAME.XEPACK, MEGACITY.XEPACK,
  ACT*/level packs L03,H07,...). 1-4 real containers each. ~80 XEPACK files.
- default.xex: 0 hits (shaders are in the packs, loaded at runtime).

KEY: WoS packs shaders in Treyarch **.XEPACK** archives. EoT's dumper
(src/gpu/shaders/shader_dump.cpp + thirdparty/PKZLib) unpacks .pkz/.pak
(BUCompress, Sonic-Team format) — a DIFFERENT format. So we can't reuse the
unpacker directly; XEPACK needs its own handling.

### Two ways to extract WoS shaders (both viable):
1. RUNTIME DUMP (preferred, matches EoT's xex-fallback approach): the recompiled
   game already loads+decompresses XEPACK into memory at runtime. Add a dump hook
   that scans guest memory for VALID shader containers (flags 0x102A11xx AND words
   0x1C,0x20 zero AND sane virtual/physical sizes — EoT's validation filters the
   media false-positives). Write them to a file → feed XenosRecomp. No XEPACK
   format RE needed; the game does the unpacking for us.
2. STATIC UNPACKER: reverse-engineer the XEPACK format and unpack on PC. More work.

### Validation note
Our raw 3-byte scan over-counts (media false positives). EoT validates container
structure (words 0x1C/0x20 zero + size sanity). The real WoS shader count is
likely ~100-300 distinct shaders (typical for a game this size), extractable once
we dump from runtime memory or unpack XEPACK.

## UPDATED STATUS after scan
- Shaders CONFIRMED present, in .XEPACK packs. Standard 0x102A11 container magic.
- Extraction path: runtime dump hook (bounded, EoT has the validation template).
- This is one more concrete step, but NOT a new unknown — it's "adapt EoT's dumper
  for XEPACK/runtime-memory instead of .pkz". The shader TRANSLATION question
  (XenosRecomp on the extracted containers) is still the final gate, now reachable.

## SESSION 3 — EXTRACTION DONE, TRANSLATION PARTIAL (the decoder bug is located)

### Extraction: SOLVED and shipped
- The runtime dump hook is built and committed (branch `shader-dump-hook`,
  `wos/app/wos_ps5_shader_dump.cpp`, cvar `wos_shader_dump`). It walks guest
  memory REGION BY REGION via the heap API (`LookupHeap` / `QueryBaseAndSize` /
  `QueryRegionInfo`), scanning only committed+readable pages. A flat sweep
  CRASHES: this runtime's guest arena FAULTS on an unmapped page (it does NOT
  read as zero). Guest virtual base is 0x1000000000; scan range [0x10000,
  0xE0000000) (host_address_offset is 0 there; only the 0xE0.. physical heap is
  shifted). Writes /app0/wos-shaders.bin: each container + 16-byte pad, the
  format XenosRecomp's directory scanner reads.
- Three play sessions dumped cleanly: 1160 (first level), 988 (city swing),
  867 (menu/intro). Combined DEDUPED corpus = 2541 distinct shaders.
  Entropy ~4.2 bits/byte = real uncompressed microcode (compressed XEPACK was
  ~7.8). The earlier "271 containers from a static byte scan" were FALSE
  POSITIVES (random 0x102A11 in compressed data); ignore that number.

### XenosRecomp: built for Linux, works
- Build: `scratchpad/xenosrecomp` (hedge-dev XenosRecomp + submodules). cmake +
  clang, DXC via thirdparty/dxc-bin/lib/x64/libdxcompiler.so. Binary at
  build/XenosRecomp/XenosRecomp. The bundled XenosRecomp/shader_common.h is the
  include arg (3rd positional). EoT uses the STOCK XenosRecomp (uninit submodule)
  — no game-specific adaptation exists upstream.
- ALWAYS run it in DIRECTORY MODE (point at a folder). Single-file mode has no
  container validation and SEGFAULTS on these inputs — that is a harness
  artifact, not a translator bug. The go/no-go harness `scratchpad/wosxl/run-xl.py`
  wraps each shard in its own dir/subprocess (crash-isolated).

### Translation result: 16% full / 84% empty-body (the real number)
- 100% of containers PARSE; zero crashes in directory mode. Constant-table and
  sampler REFLECTION parses perfectly — recovers real WoS uniform names
  (gDirLightsColor, gShadowSampler, gIBLParams, diffuseSampler1/2, etc.) and
  emits both SPIR-V and D3D12 paths. Dynamic constant indexing works
  (select(...min(INDEX,N)...) guards). That was the biggest format risk: PASSED.
- BUT only ~16% of shaders get a real instruction body. ~84% come out as a
  correct shell (declarations + constants) with an EMPTY main(). NOT trivial
  stubs: the empties have a MEDIAN 2.3 KB of microcode (they are the BIGGER /
  more complex shaders; the small ones translate fine).

### The decoder bug — LOCATED EXACTLY (shader_recompiler.cpp)
- The first control-flow SCAN loop (~line 1513) computes
  `instrSize = min(instrSize, address*12)` for every Exec/CondExec `address`
  (line ~1554). This is meant to find where the CF table ends (= where the
  first ALU clause begins). `shader->size` (== physicalSize - physOff) is the
  WHOLE instruction stream (CF table + all ALU clauses).
- Instrumented trace (authoritative, directory mode) of empty shader 0268
  (shader.size=3036, 253 instr-slots): the scan runs correctly through real CF
  (opcodes 2 ExecEnd, 3 CondExec, 5 CondExecPred, 8 LoopEnd, 11 CondJmp, 15
  MarkVsFetchDone) with large sane addresses (3728, 338, 1119, 1624, 2048 — none
  clamp since *12 all exceed 3036). Then at instrAddr=1476:
      opcode=3 (CondExec) address=1  ->  instrSize 3036 -> 12   <-- THE BUG
  A legit CF instruction with a small/backward `address=1` collapses instrSize
  to 12, so the CF-EMIT loop runs ~1 slot (two count=0 Nops) and emits nothing.
- Root cause: the single-min clamp assumes every exec `address` points FORWARD to
  a monotonic ALU region and the smallest marks the table end. WoS shaders
  contain backward/early references (address*12 <= instrAddress) that are branch
  targets, NOT the table terminus. Sonic Unleashed (what XenosRecomp was written
  for) evidently had none, so the heuristic held there.

### The fix is NON-TRIVIAL (do not one-line it)
- Tried: "only clamp when address*12 > instrAddress" (ignore back-refs).
  RESULT: for shaders whose only clamp candidates are back-refs, instrSize then
  never shrinks from the full stream, so the CF-EMIT loop reads ALU-clause bytes
  AS control-flow -> garbage exec address/count -> `instructionCode = code +
  address*3` runs out of bounds -> SEGFAULT. The clamp doubles as the ALU bounds
  guard; removing it unsafely is worse than the empty body.
- The correct fix must determine the CF-table/ALU boundary properly for WoS's
  encoding, e.g.: find the table end as the smallest FORWARD exec target, but if
  none exists, fall back to detecting the first ExecEnd/sequence terminator, and
  independently bound the ALU emit by the container's physical size so a wrong
  boundary can't read OOB. Needs: confirm the exact Xenos CF-table terminator
  semantics (ExecEnd marks last CF word), and verify shader->size vs the true
  CF-table length. This is a focused shader_recompiler.cpp change + re-validate
  against the 2541-shader corpus (target: full-body rate up from 16%).

### State / how to resume
- Corpus: scratchpad/corpus (2541 deduped .bin). Harness: scratchpad/wosxl/run-xl.py.
- XenosRecomp pristine (git-reverted all instrumentation). Trace method: add
  `fprintf(stderr, "WOSSCAN ...")` at the clamp (line ~1554) and
  `fprintf(stderr,"WOSCF ...")` before `instructionCode = code + address*3`
  (line ~1722), build with -DWOS_CF_TRACE, run in DIRECTORY mode.
- Title shader-dump hook: committed, works on console (verified: 1160 shaders,
  no crash, ~37 MB mapped memory scanned per pass).

## SESSION 3 (cont.) — DEEPER FINDING: the decoder issue is a shader-ISA problem, bigger than one clamp

After instrumenting XenosRecomp's real decode path (authoritative; hand-decoding
in Python proved unreliable and is abandoned), the "empty body" cause is NOT
simply the min(address*12) clamp. Evidence:

1. In a WORKING shader (0389, container=2584 bytes, shader.size=1404), the execs
   that actually emit ALU have address=2800/2878/3820 -> clause byte offsets
   33600/34536/45840, i.e. 32-44 KB PAST THE END of the 2584-byte container.
   The ALU microcode XenosRecomp tries to read is NOT inside the dumped
   container. 0389 only "worked" because single-file mode read adjacent heap
   memory that happened to decode into plausible HLSL -- so some of the "16%
   full" shaders are partly reading neighbouring memory, not their own code.

2. So `address` for WoS is either (a) in a different unit/base than the 12-byte
   instruction-slot index XenosRecomp assumes (it was written for Unleashed), or
   (b) points into a SEPARATE microcode region the guest stores apart from the
   0x102A1100 container (so our memory dump captured the container header +
   reflection + CF table, but not the ALU blob it references).

3. address values cluster near the 12-bit field max (4095); address % (size/12)
   lands in-range but that's unverified numerology.

IMPLICATION FOR THE PORT (honest reassessment):
- Extraction of the CONTAINER is solved, but we likely need to ALSO capture the
  referenced ALU microcode region. Options to investigate next session:
  (a) Dump a much larger contiguous window around each container from guest
      memory (if ALU code is adjacent in RAM, a bigger grab captures it), then
      re-test XenosRecomp -- cheapest experiment, do this FIRST.
  (b) Determine the true meaning of exec.address for WoS's shader build (XDK
      6995) by comparing against a KNOWN-GOOD decode -- e.g. run a WoS shader
      through a reference Xenos disassembler (Xenia's shader translator has a
      mature, well-tested Xbox360 microcode decoder) and diff against XenosRecomp.
  (c) Confirm whether WoS stores ALU code inline or in a separate heap block by
      inspecting the guest D3D shader-create path in the recompiled code.
- This is NO LONGER "fix one clamp." It is "correctly locate + decode WoS's ALU
  microcode," which is real shader-ISA work. The renderer path is still viable
  (container, reflection, SPIR-V emission all proven), but shader translation is
  a multi-session effort, not a one-patch fix. Recommend experiment (a) first:
  it's a change to OUR dump hook (grab a bigger window), cheap to try, and would
  immediately tell us if the ALU code is simply adjacent-in-RAM.

### Clean state
- XenosRecomp: pristine (git checkout, verified). All instrumentation removed.
- Corpus: scratchpad/corpus (2541 deduped). Harness uses SINGLE-FILE mode
  (run-xl.py line 48) -- that is the working path; directory mode crashes even
  pristine (main.cpp scan), do not use it.
- Baseline measured under single-file mode: ~16% full body, but see finding #1 --
  some of those read neighbouring memory, so the true correct-translation rate
  is UNKNOWN and probably lower until the ALU-location question is resolved.

## SESSION 3 (cont.) — XENIA CROSS-REFERENCE: root cause confirmed, fix is a real RE project

Fetched Xenia's microcode decoder (src/xenia/gpu/ucode.h, shader.h). Findings:

1. BITFIELDS MATCH. Xenia's ControlFlowExecInstruction is byte-identical to
   XenosRecomp's: address:12, count:3, isYield:1, sequence:12, ... So the field
   EXTRACTION is not the bug. address IS a 12-bit instruction index.

2. THE DIFFERENCE: Xenia never parses a container to find the CF table. The
   Xbox360 GPU hands Xenia the exact microcode pointer + dword count via the
   SET_SHADER command (ucode_dwords + ucode_dword_count are INPUTS). XenosRecomp
   instead must locate the CF table inside the 0x102A1100 FILE container, and its
   ONLY mechanism for finding where the CF table ends is the min(address*12)
   clamp. That heuristic is what breaks on WoS.

3. Xenia's authoritative CF terminator: DoesControlFlowOpcodeEndShader() = opcode
   is kExecEnd(2)/kCondExecEnd(4)/kCondExecPredEnd(6)/kCondExecPredCleanEnd(14).
   The shader's control flow ends at the first such op. XenosRecomp does NOT use
   this to bound the table.

4. BUT instrumented trace of 0268 shows the "CF ops" after instrAddr~288 are
   incoherent (opcodes 5,11,2,4,3,8,15,9,12 with addresses 3728,338,1119,...):
   these are ALU-clause / name-table bytes being MISREAD as CF instructions. The
   real CF table is short (first few entries). So "stop at first ExecEnd" alone
   is not enough either, because a bogus ExecEnd appears early in the misread
   data. The core problem: XenosRecomp cannot reliably tell where WoS's CF table
   ends vs where ALU clauses begin, because WoS's container lays this out
   differently than Sonic Unleashed (XDK 6995 vs Unleashed's build).

5. Shader struct dump (0389 vs 0268) — candidate fields that may encode the true
   CF-table size / layout (UNVERIFIED): word[2]=0x1000XX00 (marker + low val),
   word[5]=interpolatorInfo (0x7508/0x7d29), word[3], word[8..]=0xf0XX run
   (interpolator/vertex-element table). Resolving which field gives the true
   CF-table length is the open RE task.

### HONEST VERDICT on the native renderer
- The renderer PLUMBING is proven: extraction, container reflection (real uniform
  names), SPIR-V emission, PS5 plume present (session 2). Those are real.
- SHADER TRANSLATION is the gate and it is a GENUINE multi-session RE project, not
  a one-patch fix: XenosRecomp's container/CF-boundary logic is Unleashed-specific
  and must be re-derived for WoS's shader build. The cleanest route is likely to
  adopt Xenia's shader-translator front end (mature, correct Xbox360 decoder with
  proper CF termination) and emit SPIR-V from its IR, rather than patch
  XenosRecomp's fragile boundary heuristic. That is a substantial effort.
- Recommend next session (in priority order):
  1. Determine the true CF-table size field in WoS's Shader struct (compare
     several shaders; cross-check against a correct decode of the same shader).
  2. If that resolves cleanly -> patch XenosRecomp's boundary detection to use the
     real size + ExecEnd terminator, re-run the 2541 corpus.
  3. If not -> evaluate porting Xenia's shader_translator as the front end.

### Clean state (verified)
- XenosRecomp pristine (git checkout). Build: CC=clang CXX=clang++ cmake -G Ninja.
- Corpus scratchpad/corpus (2541). Harness scratchpad/wosxl/run-xl.py (SINGLE-FILE
  mode only). Xenia refs saved: scratchpad/xenia_ucode.h, xenia_shader.h.
- Title shader-dump hook committed on branch shader-dump-hook, works on console.

## SESSION 3 (cont.) — PROBLEM ISOLATED to container ucode layout (Xenia confirms units are correct)

Read Xenia's shader_translator.cc (the authoritative decoder). Definitive findings:

1. UNITS ARE CORRECT. Xenia: ucode is 3-dword "slots"; a CF pair and an ALU
   instruction are each 3 dwords. exec.address() is a slot index; ALU clause =
   ucode_data + address*3 dwords (shader_translator.cc:369). XenosRecomp does the
   identical thing (code + address*3; code = shaderData + virtualSize +
   physicalOffset). So address units / *12 math are NOT the bug.

2. BOUNDARY LOGIC IS EQUIVALENT. Xenia (line 60-72): cf_pair_bound =
   min(total_slots, exec.address()) over the scan, loop bounded by the shrinking
   cf_pair_bound. XenosRecomp's min(instrSize, address*12) is the same idea. Both
   would clamp 0268 to ~nothing on its address=1 exec. Xenia is NOT magically
   better on this input -> the input itself is the problem.

3. ROOT CAUSE (isolated): the bytes at code_off = virtualSize + physicalOffset do
   NOT decode as a dense, valid Xbox360 CF table for WoS. Evidence: CF ops are
   sparse/scattered (0268: op at pair24, then nothing till pair77-84, with ASCII
   name-table bytes embedded at pair~24). A real CF table is DENSE from slot 0,
   ending in an ExecEnd within a few dozen slots. Tried alternative bases (virt,
   shaderOffset, physOff, 0) — NONE yields a dense CF table. So either:
   (a) WoS's 0x102A1100 container stores the raw ucode at an offset we haven't
       correctly derived (the Shader/DefinitionTable struct fields compose
       differently for XDK 6995 than XenosRecomp assumes), or
   (b) the ucode in the container is transformed/interleaved vs the raw ucode the
       GPU executes (Xenia gets raw ucode from SET_SHADER; the FILE container may
       pack it differently).

4. The "16% full" from earlier is unreliable: single-file mode read neighbouring
   heap memory for out-of-range addresses, so some of those outputs are partly
   garbage. True correct-translation rate with current extraction ~ unknown/low.

### THIS IS REAL PROGRESS, not a dead end
The open question is now SPECIFIC and bounded: "where/how is the raw Xbox360
ucode stored inside WoS's 0x102A1100 container?" — a container-format RE task,
not an open-ended ISA problem. Resolving it unblocks the whole translation path
(reflection + SPIR-V emission already proven to work).

### CONCRETE NEXT STEPS (priority order) for next session
1. Get a REFERENCE: find the Xbox360 D3D shader container (.102A1100 / "XPS"/
   shader blob) format docs, OR use a known tool that extracts raw ucode from
   such a container (Xenia loads ucode from GPU memory, not the file, so its
   loader is not directly it; look at the XDK D3DDevice::CreatePixelShader path,
   or tools like 'xenia-vfs-dump', or the Durango/Xenon shader compiler output
   format). The container has: header(36B) + constantTable + definitionTable +
   Shader struct + ucode. We need the exact ucode offset+size formula.
2. VALIDATE against one shader: decode its CF table from the correct offset,
   confirm it's dense and ends in ExecEnd within N slots, and that exec addresses
   point to in-bounds ALU clauses. Then feed XenosRecomp the corrected offset.
3. ALTERNATIVELY: capture ucode the way Xenia does — hook the recompiled game's
   D3D SetPixelShader/SetVertexShader (NATIVE-RENDERER-NOTES session-1 found the
   D3D region 0x82AB7000-0x82AD5000 and the device struct offsets) to grab the
   raw ucode POINTER the game hands the GPU, instead of parsing the file
   container. This sidesteps the container-format question entirely and gives
   exactly what Xenia/XenosRecomp's decoder wants. STRONGEST OPTION — it reuses
   the session-1 D3D hook work and the session-3 dump-hook infrastructure.

### State (verified clean)
- XenosRecomp pristine (git checkout). Xenia refs: scratchpad/xenia_{ucode,shader,
  shader_translator}.{h,cc}. Corpus: scratchpad/corpus (2541). Harness: single-file.
- Title shader-dump hook committed (branch shader-dump-hook), works on console.

## SESSION 3 (cont.) — BREAKTHROUGH: clean ucode + working translator, via the emulator itself

The container-format dead end is BYPASSED. Key realization: rexglue (the emulated
GPU already rendering WoS) contains a complete, correct Xenos->SPIR-V translator
(src/graphics/pipeline/shader/, Xenia-derived: shader.cpp, spirv_translator*.cpp).
It locates each shader's ucode from the GPU COMMAND STREAM (the way Xenia does,
via SET_SHADER), not from the file pack container -- so it never has the
container-offset problem that blocked the static XenosRecomp path.

rexglue patch 0029 (wos/patches/rexglue/) turns on the Vulkan-path shader dump
(the D3D12 path already had it) via the existing dump_shaders=<path> cvar.

VERIFIED on console (dump_shaders="/app0/shaderdump", one level):
- 485 distinct shaders dumped: 244 vertex + 241 pixel.
- 485/485 raw ucode files (.ucode.bin.vert/frag) are clean: every one a multiple
  of 12 bytes (valid 3-dword slots), median 840 B, max 4548 B.
- Disassembly (.ucode.vert/frag) is a PERFECT decode: dense exec blocks, real
  tfetch2D/mul/add/dp3/mad/dp2add/rsq with correct registers, samplers (tf0,tf4,
  tf6...), co-issued scalar+vector pairs, serialize markers, _sat/_abs modifiers,
  ending in ExecEnd. This is exactly what the container path could NEVER produce.
- The 2 translated SPIR-V files that dumped (.vulkan.bin) are VALID SPIR-V
  (magic 0x07230203, 48KB vert / 257KB frag). The translator produces correct
  SPIR-V; the dump only caught 2 because my hook fires on first-translate and
  most shaders' SPIR-V variant is produced on a different path (minor, fixable --
  but mostly MOOT, see below).

### THE ANSWER to "can WoS shaders be translated": YES, DEFINITIVELY
The emulator translates all of them correctly every frame. The native renderer
does NOT need XenosRecomp or any container RE. It needs to REUSE rexglue's
SpirvShader translator (proven correct) on the ucode captured from the command
stream, and feed the resulting SPIR-V to plume (proven on PS5, session 2).

### REVISED native-renderer architecture (the real plan now)
Instead of "static-extract + XenosRecomp" (blocked), the path is:
1. The command processor already hands each draw its VS/PS Shader objects with
   clean ucode + a GetOrCreateTranslation(SPIR-V) method. These are the exact
   inputs plume needs.
2. A native renderer = a thinner command-processor path that, per draw, takes the
   already-translated SPIR-V (rexglue's SpirvShader) + the device/register state
   (session-1 device offsets) + vertex/index/texture bindings, and issues plume
   (RADV) draws directly -- skipping the heavy parts of full Xenos emulation.
   This is EXACTLY what EoT's plume renderer does for Edge of Time, except EoT
   replaces D3D while we'd tap rexglue's translation layer.
3. The shader question is CLOSED. Remaining work is the draw-path bridge:
   rexglue pipeline/register state -> plume pipeline + draw. Substantial but
   well-defined, and every input is proven to exist.

### Deliverables this session
- Clean shader corpus from console: 485 shaders (ucode + disasm), confirming the
  translator works on real WoS content across a level.
- rexglue patch 0029 committed (dump on Vulkan path).
- Title shader-dump hook (branch shader-dump-hook) -- now largely superseded by
  0029's dump_shaders for the native-renderer purpose, but harmless.

## SESSION 3 (cont.) — ARCHITECTURE LOCKED: no external plume needed; hook D3D draws, reuse rexglue

User's WSL has NO Edge of Time / plume. Good news: NOT NEEDED. rexglue already
contains a full Vulkan renderer (src/ui/vulkan/: vulkan_provider, vulkan_device,
vulkan_presenter, vulkan_display_prospero) AND the SpirvShader translator
(src/graphics/pipeline/shader/). These ARE our "plume" - already built, on-system,
proven (they render the game now).

CRITICAL architecture finding - what the native renderer actually is:
- rexglue's VulkanCommandProcessor::IssueDraw (src/graphics/vulkan/command_processor.cpp
  :3841) is ALREADY a native-ish Vulkan draw path (translated SPIR-V + bindings ->
  real RADV draw). The perf bottleneck is NOT GPU translation; it's the
  SINGLE-THREADED PM4 ring parse: CommandProcessor::WorkerThreadMain ->
  ExecutePrimaryBuffer -> ExecutePacket (command_processor.cpp:276/707/757). That
  is the "GPU Commands" thread from the stall dumps.
- Therefore a native renderer that just reuses IssueDraw does NOT help - it still
  goes through the PM4 parse. The version worth building hooks the game's GUEST
  D3D CALLS directly, before they become PM4 packets, skipping the ring entirely
  (this is what EoT's plume does: replace D3D, not the GPU).

THE PLAN (D3D-hook native path):
- Hook the recompiled game's D3D draw/state functions (session-1 addresses, all
  confirmed present in generated/default/):
    DrawVertices        = 0x82ABECF0 (sub_82ABECF0) - 4 args, most-called
    DrawIndexedVertices = 0x82ABF0E0 (sub_82ABF0E0) - 5 args (dev,prim,baseVtx,startIdx,count)
    DrawVerticesUP/Begin= 0x82ABE800
    CreateDevice        = 0x82ABAE28 ; Swap/Present = 0x82AC4E48 ; SetTexture = 0x82ABD328
  Device struct offsets: pending groups @device+0/8/16/24/32; fetch constants
  @0x484; see session-1 table above.
- Per hooked draw: read device struct state (shaders, fetch constants = vertex/
  texture bindings), get translated SPIR-V from rexglue's Shader/SpirvShader for
  the current VS/PS ucode, bind, and issue a direct Vulkan draw on rexglue's
  VulkanProvider device - NOT via the PM4 ring.
- Gated so only the native title uses it.

TWO-TITLE SETUP (decided): new PPSA99311 "Spider-Man: Web of Shadows (Native)" as
an ENV-SWITCHED build target of the SAME source tree (WOS_NATIVE=1 -> different
param.json with conceptId 99311 / contentId ...PPSA99311..., + native code path).
No folder clone; both install side-by-side. param.json to clone:
  conceptId "99310"->"99311"; contentId UP9000-PPSA99310_00... -> PPSA99311_00...;
  titleName + " (Native)".

PROTOTYPE MILESTONE (bigger first step, user's pick): hook DrawVertices +
DrawIndexedVertices, LOG every call with args + current VS/PS ucode hash, and
attempt routing ONE draw to a direct rexglue-Vulkan draw. Expected first result:
boots, logs draws (proves interception works), native draw likely wrong/absent
(debuggable). This is the foundation; playable is many iterations away.

### Honest perf note
The native path's win comes from killing the single-threaded PM4 parse, not from
better GPU work. Whether it beats the current paced 30/120 modes is unproven until
a real draw volume runs. Worth building, but the payoff is a hypothesis to test,
not a guarantee.

## SESSION 3 (cont.) — MILESTONE REACHED: draw interception PROVEN ON HARDWARE

PPSA99311 "Web of Shadows (Native)" built, deployed, launched, and ran (reached
intro, choppy only from first-run shader/pipeline warmup - normal). Klog confirms:
- "native renderer ON (observing draws)" + wos_native='true' read from the toml.
- The weak-symbol override draw hooks FIRE CORRECTLY with clean, decoded args:
    DrawIndexedVertices: device=0073D080 (stable real D3DDevice*), prim=6
      (triangle list), idxCount 5..3521 (UI quads to large world meshes). 20,480+
      indexed draws in ~20s.
    DrawVertices: device=0073D080, prim=4 and 13, real startVtx (728,1212,212...)
      and count. 8192+ draws.
- All fields sane: device pointer consistent, primitive types valid D3D enums,
  counts scale correctly. This is genuine interception of the game's real draw
  stream, BEFORE the PM4 ring, with full args.

=> The core feasibility question of the whole native-renderer effort is ANSWERED
   YES on hardware: we can cleanly intercept every draw with its state. The hook
   is observe-only (tail-calls the original), so the game still renders through
   emulation and stays stable - a safe base to build on.

### Hook mechanism (confirmed working on HW)
Strong extern "C" definitions of sub_82ABECF0 (DrawVertices) and sub_82ABF0E0
(DrawIndexedVertices) in wos/app/wos_ps5_native.cpp override the recompiler's
weak guest symbols at link time, catching ALL call sites (direct+indirect), then
tail-call __imp__sub_*. Args read from PPCContext: r3=device, r4=prim, then
Draw: r5=startVtx r6=count ; DrawIndexed: r5=baseVtx r6=startIdx r7=idxCount.

### NEXT STEPS (native draw path, in order)
1. Read per-draw STATE from the device struct (session-1 offsets): current VS/PS
   ucode pointers (device+shader offsets), fetch constants @0x484 (vertex streams
   + textures), render target. Map device=0073D080 fields to the bound shaders.
2. Match the bound VS/PS ucode to a rexglue Shader/translation (reuse its
   SpirvShader; we have 485 shaders + the translator proven). Get SPIR-V per draw.
3. First real native draw: on ONE chosen draw (e.g. a fullscreen or simple prim),
   build a plume/rexglue-Vulkan pipeline from that SPIR-V + vertex layout, bind
   the vertex/index buffers (guest memory via membase), issue a direct vkCmdDraw,
   and present via rexglue's VulkanPresenter. Expect wrong/partial first; iterate.
4. Decide frame ownership: native path takes the frame (clears + draws) vs. runs
   alongside. Start with native drawing into its own target to compare.

### Build/run recap (works)
- Emulation: bash ps5/tools/build.sh -> dist/PPSA99310.
- Native: WOS_NATIVE=1 bash ps5/tools/build.sh -> dist/PPSA99311 (now signs its
  own eboot.bin; the earlier miss was fixed in build.sh - remove $build/eboot.elf
  when WOS_NATIVE so the link/sign re-runs for the native param).
- Each title needs its own gamedata/ copy.

## SESSION 3 (cont.) — DEVICE STATE READ LIVE: shader offsets CONFIRMED, full draw state in hand

Ran the probe on hardware (PPSA99311). The device struct (device=0073D080) reads
cleanly per draw. The one session-1 gap - where the shader pointers live - is now
CONFIRMED on hardware:

  device+12692 = PIXEL shader pointer   (e.g. 00D3F230, 00C3B8F0, 00C3D020)
  device+12696 = VERTEX shader pointer  (e.g. 00D3EBA0, 00C3A2D0)

They change per draw (different shaders bound), are valid guest heap pointers
(0x00Cx/0x00Dx), and pair consistently. My session-1 predictions (12692/12696)
were exactly right.

Full per-draw state confirmed from the device struct (all live-verified):
- Index buffer   device+12436 : EAC30538 (indexed draws) / 00000000 (DrawVertices)
- Vertex stream0 device+12460 : 00C0D0A8 (valid VB ptr); stride @12528 top byte = 4
- Fetch const 0  device+1152  : fc0=8A004956 fc1=1D82105D (Xenos fetch: GPU addr+size)
- Pixel tex 0    device+12544 : 0105332C (texture object ptr)
- Render targets device+12728/12744 : 82F70E08 / 82F70D80 (physical-range surfaces)
- Vtx/RT slot    device+12440 : 82F70CF8 / 01053A28 (varies)

=> We can now read, for any intercepted draw: its VS+PS shader pointers, vertex
   stream + stride, index buffer, textures, and render target. That is the
   COMPLETE input a native draw needs. The shader pointers (00Cx/00Dx) point to
   the game's D3D shader objects, which wrap the ucode - the next step resolves
   those to the ucode bytes and matches them to a rexglue SpirvShader translation.

### NEXT STEPS (toward first native draw)
1. Follow device+12696 (VS) / +12692 (PS) -> the D3D shader object -> its ucode
   pointer+length. (The object layout: find the ucode ptr field; compare against
   a known shader hash from the earlier dump_shaders capture to confirm.)
2. Hash the ucode and look it up in rexglue's shader cache / build a SpirvShader
   from it to get SPIR-V (the translator is proven; 485 shaders already dumped).
3. First real native draw: pick one draw, build a Vulkan pipeline from VS+PS
   SPIR-V + the vertex-stream layout, bind VB (device+12460 -> guest mem via
   membase) and IB (device+12436), issue vkCmdDraw on rexglue's VulkanProvider
   device, into a scratch target; compare. Iterate.

### Observations (perf, for later)
Native title ran at 120Hz-mode speeds (~34-52 fps), choppy only during first-run
texture/pipeline warmup (hitch log: textures +50-60/frame early, settling). Still
pure emulation rendering - the native path is observe-only. cp translate ~80-99%
confirms the PM4 parse is the busy path, as expected; the native-draw payoff is to
offload that.

## SESSION 3 (cont.) — DRAW-DECODE CHAIN COMPLETE: both shader containers located on HW

Confirmed on hardware (PPSA99311, stable, no faults). Both VS and PS microcode
containers are found for EVERY draw. The containers are embedded INLINE in the
D3D shader object (not pointed to):

  VS: device+12696 -> object -> 0x102A1100 container INLINE at object+0x368
  PS: device+12692 -> object -> 0x102A1100 container INLINE at object+0x28

(The object itself is a resource wrapper: obj[0]=0x00100006 VS / 0x00100007 PS.
The VS offset 0x368 matches EoT's kVertexShcontainer exactly; PS differs - EoT
0x28 matches here too. The PS object even carries the original HLSL source path,
e.g. "C:\...\hmeshkin\Desktop\x...".)

Header fields read correctly and vary per shader (e.g. VS virt=792 phys=408
shaderOff=0x280; PS virt=272 phys=48 shaderOff=0xE8; other shaders 492/312,
356/160, 336/508 - all distinct, all MAGIC OK).

=> THE FULL PER-DRAW SHADER CHAIN WORKS:
   DrawVertices/DrawIndexedVertices (hook) -> device struct (0073D080)
     -> VS/PS pointer (device+12696 / +12692)
       -> shader object -> inline 0x102A1100 container (+0x368 VS / +0x28 PS)
         -> ucode bytes (virtual[0..virtualSize] + physical[0..physicalSize])
   These ucode bytes are exactly what rexglue's SpirvShader translator consumes
   (proven: the 485-shader dump_shaders capture translated to valid SPIR-V). So
   every draw can now be matched to its translated SPIR-V.

CONFIRMED OFFSETS (WoS XDK 6995):
  device+12696 = VS object ptr ; device+12692 = PS object ptr
  VS container = VSobj + 0x368 ; PS container = PSobj + 0x28
  container hdr: flags@0, virtualSize@4, physicalSize@8, constTblOff@0x10,
    defTblOff@0x14, shaderOff@0x18 ; ucode virtual bytes start at the container,
    physical bytes at container+virtualSize (EoT guest_shaders.cpp convention).

### HASH-MATCH ATTEMPT (2026-10-24) -- RESULT: expected mismatch, strategy corrected
Hashed each draw's D3D container (XXH3 over virtualSize+physicalSize bytes from
container start) and logged `hash=...`. NONE of the draw-side hashes
(901FD9D9.., D23316B4.., EC188074.., 470ECFBD.., 2878A5EA.., 6FC7DDA3..) appear
in the pipeline_cache "Creating graphics pipeline state with VS <hash>" list.
This is NOT a boundary bug -- it proved the two hashes measure DIFFERENT THINGS.

WHY (root cause, confirmed by reading rexglue source):
  rexglue NEVER sees the 0x102A1100 D3D container. It hashes the RAW GPU ucode
  that arrives via the PM4 command stream, in
  CommandProcessor::ExecutePacketType3_IM_LOAD /_IMMEDIATE
  (src/graphics/command_processor.cpp:1623 and :1653):
      LoadShader(type, addr, memory_->TranslatePhysical<uint32_t*>(addr), size_dwords)
      -> pipeline_cache_->LoadShader(type, host_ptr, dword_count)
      -> XXH3_64bits(host_ptr, dword_count * 4)   (pipeline_cache.cpp:897)
  i.e. XXH3 over `size_dwords` HOST-NATIVE (already byte-swapped) ucode dwords at
  the physical address the IM_LOAD packet names. The container is the GAME's D3D
  representation; the game's own D3D runtime (the sub_82.. funcs) parses the
  container and emits IM_LOAD with the extracted ucode. Container bytes != ucode
  bytes (different layout, different endianness handling), so a container hash can
  never equal the ucode hash. rexglue has no 0x102A1100 parser anywhere (confirmed:
  grep finds the magic only in the unrelated lossless_dll framegen code).

KEY CONSEQUENCE -- we don't need to reproduce the hash at all:
  rexglue's CommandProcessor exposes the already-resolved, already-translated
  shaders for the current draw via PUBLIC getters:
      include/rex/graphics/command_processor.h:90-91
      Shader* active_vertex_shader() const;
      Shader* active_pixel_shader() const;
  These are set in IM_LOAD (command_processor.cpp:1627/1630) and carry the
  VulkanShader -> VulkanTranslation -> SPIR-V. So the native renderer reads the
  draw's translated SPIR-V straight off the command processor; it does not hash,
  parse the container, or re-translate.

### LOOP CLOSED (2026-10-24 20:09) -- CONFIRMED ON HW
The CP-getter route works and the hashes MATCH. Native build b8adae0, PPSA99311:
  [WoS-NATIVE-XLATE] active VS hash=B6C9863F710683EC dwords=24
  [WoS-NATIVE-XLATE] active PS hash=A4A965C189287B99 dwords=9
  ...and in the SAME klog's pipeline-cache lines:
  "Creating graphics pipeline state with VS B6C9863F710683EC, PS A4A965C189287B99"
  (also VS AB1E86137A0240E8 for the second shader set; PS A4A.. shared).
So from a hooked D3D draw we now obtain, with zero re-derivation, the live
rex::graphics::Shader* that rexglue has ALREADY translated to SPIR-V and built a
pipeline from. Stable, no faults, across both shader sets seen. The chain:
  DrawVertices hook -> Runtime::instance()->graphics_system()
  -> static_cast<GraphicsSystem*>->command_processor()
  -> active_vertex_shader()/active_pixel_shader()  == the pipeline-cache shader.
This is the step the whole hashing detour was for; it is DONE.

NOTE on which draws get probed: ProbeFirstDraws only fires for n<=4 of each of
DrawVertices / DrawIndexedVertices, so XLATE covers the earliest draws (UI/boot
geometry: VB stride 4, fetch consts often 0 for the very first). Later/world
draws will show other shaders; widen the probe if we want those too.

### NEXT STEPS (toward first native draw)
1. [DONE] Reach the live CommandProcessor and read active VS/PS at draw time;
   hashes match the pipeline-cache lines. Container decode kept as game-side
   ground truth / ucode fallback.
   KEY GROUND-TRUTH FROM THIS KLOG (device=0073D080):
     ib=EAC30538.. (indexed path), vb0=EAC3050C / 00C0D0A8, stride0=4,
     fetch consts fc0/fc1 nonzero once world draws start (e.g. 8A004956 1D82105D).
2. [IN PROGRESS] Read vertex layout from rexglue's OWN analysis, not raw dwords:
   shader->vertex_bindings() (per binding: fetch_constant, stride_words,
   attributes[].fetch_instr.attributes = {data_format, offset, stride, is_signed,
   is_integer, exp_adjust}) + GraphicsSystem::register_file()->GetVertexFetch(slot)
   for the live VB base (address<<2) / size(words) / endian. Logged as
   [WoS-NATIVE-VTX]. (xe_gpu_vertex_fetch_t: dword0 = type:2|address:30 [dwords],
   dword1 = endian:2|size:24 [words].)
   KLV run 2026-10-25 22:18 (build ce63091):
     - VS B6C9863F710683EC: 0 vertex bindings. EXPECTED -- this is a fullscreen/
       procedural VS (generates positions from vertex index, no VB input); a UI/
       post pass. Nothing to cross-check; self-consistent.
     - VS AB1E86137A0240E8: "not ucode-analyzed yet" on its first draw. rexglue
       analyzes ucode LAZILY on its GPU thread at pipeline-build time, so our hook
       (game draw thread) can beat it. We must NOT call AnalyzeUcode ourselves:
       translator.cpp:61 has an early-out guard but takes NO lock, so it would
       race rexglue's thread on the same Shader. FIX (build 9ffee6b): widen probe to
       also sample later draws (n<=4 OR n%4096==0) so we re-observe the shader
       after rexglue has analyzed it.
   [DONE] Validated 2026-10-25 22:28 (build 9ffee6b), in-game. Real layouts, all
   self-consistent (attr offsets fit stride, real Xenos formats, valid VB bases):
     - VS 1E6883FCCDE1F688 (27 dw): 1 binding, fetchConst=0, stride=7w(28B), 2 attr
         attr0 32_32_32_FLOAT off0  attr1 32_32_32_32_FLOAT off3w  (pos + color/tan)
         live VB e.g. 1E513A00 size=21w. (simple immediate/dynamic geo)
     - VS A244643FAE1EE515: 1 binding, fetchConst=95, stride=8w(32B), 5 attr
         16_16_16_16_FLOAT off0, 16_16_16_16_FLOAT off2w, 2_10_10_10 off4w,
         16_16_16_16 off5w, 2_10_10_10 off7w.  VB 1E88C310 size=912w.
         (compact character/skinned vertex: pos, ?, packed normal, UV, packed tan)
     - VS F9B27C7FA02CE6F3: 1 binding, fetchConst=95, stride=23w(92B), 4 attr
         32_32_32_FLOAT off0, 32_32_32_32_FLOAT off4w, ...  VB 1FB80040 size=7728w.
         (fat world-geometry vertex)
   NOTES / gotchas learned:
     - Read VB base/size from the GPU REGISTER FILE (GetVertexFetch(slot)), NOT the
       device-struct +1152 staging dwords: the staging fc0/fc1 is slot 0 only and
       differs from the real per-slot GPU reg. Register read gives correct addrs.
     - A fetch const read before it's set shows garbage (seen once:
       base=8A000000 size=8225793w). Sanity-check size (<= a few MB in words) and
       that base lands in a committed guest heap before trusting/binding it.
     - fetchConst=95 is the common "current stream" slot for world/char draws;
       fetchConst=0 for the simple dynamic-geo VS.
     - endian=2 on every binding = 8-in-32 (whole-dword) byte swap -> the VB bytes
       in guest mem are big-endian; a native draw must swap per-attribute (rexglue
       already bakes this into its shared-memory upload; our native path must too).

### STEP 3 RECON (2026-10-25) -- how rexglue's Vulkan draw actually works
Read VulkanCommandProcessor::IssueDraw (command_processor.cpp:3841-4460, ~620
lines). Findings that decide the approach:
  - There is NO classic vertex-buffer binding. Vertices + indices live in ONE big
    VulkanSharedMemory VkBuffer; the translated SPIR-V fetches vertices BY ADDRESS
    (manual vfetch). The draw is just CmdVkDraw(host_vertex_count,1,0,0) (or
    CmdVkDrawIndexed with an index buffer sub-range of shared memory). So "bind the
    VB from the layout" is the WRONG mental model -- the layout feeds the pipeline's
    vertex-fetch, and the data path is shared-memory residency (RequestRange).
  - ConfigurePipeline() CANNOT be called in isolation. It requires a
    PrimitiveProcessor::ProcessingResult and a RenderTargetCache RenderPassKey,
    both produced by the first ~200 lines of IssueDraw out of deeply private
    members (primitive_processor_, render_target_cache_, regs). It also needs the
    VulkanTranslation with the right per-draw "modification" (GetCurrentVertex/
    PixelShaderModification) -- i.e. shader variants keyed on runtime state.
  - The draw emits into deferred_command_buffer_, rexglue's single guest command
    buffer for the current submission, tied to the render-target cache's current
    framebuffer/renderpass. Issuing our own draw into a separate scratch target
    means standing up our own CB + renderpass + descriptor plumbing OR reusing its
    submission (and then we are mid-way through its state machine).
  - Everything (textures, samplers, render targets, system constants, descriptor
    sets, shared-memory residency) is coupled private state mutated as a unit.

IMPLICATION: a truly independent "native draw" that reproduces ConfigurePipeline +
all its inputs = re-implementing ~500 lines of IssueDraw against private members we
can't reach from the title. Reaching them cleanly needs a rexglue patch (we already
have the patch pipeline: wos/patches/rexglue/*.patch, auto-applied). The realistic
path is NOT "call rexglue's private pipeline builder from the title"; it is one of:
  OPTION A (recommended first): prove the concept with a SELF-CONTAINED draw. In the
    title, create our own tiny VkDevice-sharing renderer (own renderpass + offscreen
    image + pipeline from the two SPIR-V modules we already pull from the Shader*),
    upload ONE draw's vertices/indices from guest mem into our own VkBuffer honoring
    the [WoS-NATIVE-VTX] layout (incl. the endian=2 swap), vkCmdDraw, read back,
    checksum. This touches NONE of rexglue's state -> cannot destabilize the live
    renderer. It proves "we can natively render this geometry with these shaders"
    end-to-end. Downside: we re-implement vertex-fetch as real VkVertexInput (the
    SPIR-V expects manual fetch from shared memory, so we either (a) use the
    "modification" variant that does vertex-input, or (b) feed a vertex buffer the
    way the base translation expects -- needs checking which SPIR-V variant we hold).
  OPTION B (the real integration, later): add a rexglue patch exposing a
    "RenderOneDrawNatively" entry on VulkanCommandProcessor that runs the normal
    IssueDraw but into our target, or that hands back pipeline+layout+renderpass for
    the current state so the title can bind+draw. This is the eventual productization
    but it is invasive and should come AFTER Option A proves the pixels.

DECISION: user chose Option A (2026-10-25).

### OPTION A -- SPIR-V CONTRACT RECON (2026-10-25), refines A into A1/A2
Inspected spirv_translator.cpp/.h. rexglue's translated VS does NOT use VkVertexInput.
It reads vertices from a STORAGE BUFFER "XeSharedMemory" at descriptor set
kDescriptorSetSharedMemoryAndEdram binding 0, computing the address from
gl_VertexIndex (manual vfetch). The SPIR-V expects a 4-set descriptor env:
  0 kDescriptorSetSharedMemoryAndEdram (vertices/indices/textures backing store)
  1 kDescriptorSetConstants            (system + float constant uniform buffers)
  2 kDescriptorSetTexturesVertex
  3 kDescriptorSetTexturesPixel
=> To run rexglue's SPIR-V verbatim we must reproduce ALL of that (shared-memory
   contents, system/float constants, textures, samplers). That is rebuilding
   rexglue's binding env = defeats A's isolation. So split A:

A1 (DO FIRST -- pure plumbing proof, zero rexglue coupling):
  Our OWN trivial shaders: passthrough VS reading a CLASSIC bound vertex buffer
  (VkVertexInput from the [WoS-NATIVE-VTX] layout: e.g. VS 1E6883FC = loc0
  R32G32B32_SFLOAT off0, loc1 R32G32B32A32_SFLOAT off12, stride 28) + a flat-color
  PS. Share the live VkDevice via provider()->vulkan_device() (device(),
  physical_device(), functions(), queue_family_graphics_compute(), AcquireQueue).
  Upload ONE draw's vertices (and indices) from guest mem (membase+base, with the
  endian=2 8in32 dword swap) into our own VkBuffer. Render to our OWN offscreen
  VkImage + renderpass with our OWN command buffer + queue submit. Read back,
  checksum/log non-background pixel count. Proves: device sharing, guest-mem
  upload w/ correct swap+layout, offscreen native draw, readback -- all WITHOUT
  touching any rexglue state (cannot destabilize the live renderer). Does NOT
  prove rexglue's shaders render (own shaders used).

A1 IMPLEMENTED (2026-10-25, awaiting first HW run):
  Files: wos/app/wos_ps5_native_draw.{h,cpp} (RenderOneDrawA1), shaders in
  shaders/glsl/wos_native/native.{vert,frag} compiled + embedded as
  wos/app/wos_native_shaders.h. Added to wos/wos_game.cmake. New cvar
  wos_native_a1 (default false; true in wos_recomp.native.toml).
  Flow: in ProbeVertexLayout, when armed + first time we see VS 1E6883FC with a
  binding stride of 28B and a readable VB, fire RenderOneDrawA1 ONCE (g_a1_done).
  RenderOneDrawA1: provider()->vulkan_device() -> shared VkDevice; all vk calls
  through device/instance Functions tables (PS5 has no global vk symbols). Creates
  own host-visible vertex buffer (copies vcount*stride bytes from guest mem,
  bswap32 each word for endian=2), own R8G8B8A8 offscreen image + renderpass
  (clear black -> draw -> TRANSFER_SRC) + pipeline (our native.vert/frag, classic
  VkVertexInput: loc0 R32G32B32_SFLOAT@0, loc1 R32G32B32A32_SFLOAT@12, stride 28),
  records clear+draw+CopyImageToBuffer, submits on AcquireQueue(graphics,0) with
  our OWN fence, waits, reads back, counts non-black pixels. Frees everything.
  Touches ZERO rexglue state. Logs [WoS-NATIVE-A1] ... "N of 65536 px rendered".
  BUILD GOTCHA (fixed): must #include <rex/ui/vulkan/api.h> FIRST so
  VK_ENABLE_BETA_EXTENSIONS is set before any vulkan_core.h include, else
  vulkan.hpp references AMDX/CUDA beta enums the core header then lacks. Do not
  include the system <vulkan/vulkan.h>.
  Both titles build clean (99310 + 99311, identical eboot bytes, cvar-gated).
  NEXT (HW): run PPSA99311, reach gameplay, expect one
  "[WoS-NATIVE-A1] offscreen draw OK: ... -> <N> of 65536 px rendered". N>0 proves
  A1. (Geometry may land partly off-clip since we pass positions through untransf-
  ormed; any N>0 still proves the plumbing. If N==0 but "draw OK", positions were
  fully off-clip -> add an NDC fallback/scale. If it logs a Vulkan failure instead,
  address that specific step.)

A1 PROVEN ON HW (2026-10-25 23:21, build 2d4bede):
  [WoS-NATIVE-A1] offscreen draw OK: 3 verts, stride 28B -> 10368 of 65536 px rendered
  (VB=1EF5E280, VS 1E6883FC). 10368/65536 ~= one solid triangle in the 256x256
  target. First native pixels. Shared VkDevice, physical VB translate, guest-mem
  vertex copy + endian swap, own pipeline/renderpass/image, queue submit, readback
  -- all working, game kept running normally (zero rexglue state touched).
  The three fixes it took: (1) arm the cvar / use WOS_NATIVE=1 so the native toml
  is packaged; (2) VB readability -- actually (3) the address is GUEST PHYSICAL, so
  translate via physical_membase (TranslatePhysical), not the virtual membase.
  A1 = DONE.

A2 (after A1): layer rexglue's real translated SPIR-V on the proven A1 plumbing,
  reproducing the descriptor env set-by-set (start: shared memory + constants;
  textures last). This naturally converges with Option B.

### A2 RECON (2026-10-25) -- the real SPIR-V's binding contract, and the choice
rexglue's translated shaders bind 4 descriptor sets (spirv_translator.h:327):
  0 kDescriptorSetSharedMemoryAndEdram : the shared-memory storage buffer (the VS
    manual-fetches vertices by address from here). rexglue holds this as a VkBuffer
    VulkanSharedMemory::buffer() and the set as command_processor
    shared_memory_and_edram_descriptor_set_ (PRIVATE, line 650).
  1 kDescriptorSetConstants : FIVE uniform buffers -- system, float-vertex,
    float-pixel, bool/loop, fetch (ConstantBuffer enum :312), filled per draw by
    IssueDraw's UpdateSystemConstantValues + UpdateBindings (viewport xform, ndc
    scale/offset, laid-out fetch constants, float consts).
  2 kDescriptorSetTexturesVertex, 3 kDescriptorSetTexturesPixel : per-material.
Sets 0-1 share ONE immutable layout across all pipelines; the live sets sit in
command_processor current_graphics_descriptor_sets_[] (PRIVATE, line 855), built
against a PipelineLayoutProvider->GetPipelineLayout(). A pipeline using rexglue's
(compatible) layout can BIND rexglue's already-populated sets.

TWO WAYS TO DO A2:
  A2-byhand: rebuild set 0 + allocate & FILL the 5 UBOs ourselves (re-deriving the
    intricate UpdateSystemConstantValues math). Large, fragile, likely subtly wrong.
  A2-viapatch (RECOMMENDED, chosen): a small rexglue patch exposing, for the
    current draw, the VkPipeline + VkPipelineLayout + current_graphics_descriptor_
    sets_[] rexglue ALREADY built. Title then binds those + vkCmdDraw into our OWN
    offscreen target (A1's image/renderpass/cmd/queue). Correct by construction,
    small, and it IS the start of Option B. The patch only ADDS const accessors;
    our call site stays gated by wos_native_a1, so the live path is unchanged off.

### A2 IMPLEMENTED (2026-10-25, awaiting first HW run)
Patch: wos/patches/rexglue/0030-native-draw-snapshot.patch (NEW; auto-applied,
listed in .wos-patches). It captures a read-only WosNativeDrawSnapshot in IssueDraw
right after UpdateBindings (non-indexed draws only): pipeline, layout, the 4
descriptor sets, host_vertex_count, VS/PS hash, and the guest render_pass +
framebuffer (+extent) the pipeline was built against. A standalone light header
rex/graphics/vulkan/wos_native_snapshot.h exposes a POD mirror + free getter
WosNativeGetDrawSnapshot(IGraphicsSystem*, WosNativeSnapshot*) so the title reads it
WITHOUT including command_processor.h (which drags in the SPIR-V translator/glslang
headers not on the title's include path -- learned the hard way).
Title: wos/app/wos_ps5_native_draw.cpp RenderOneDrawA2() -- fetch snapshot, then on
our OWN one-shot command buffer: vkCmdBeginRenderPass(rexglue render_pass +
framebuffer, no clear) -> BindPipeline(rexglue pipeline) -> BindDescriptorSets(
rexglue sets, rexglue layout) -> vkCmdDraw(host_vertex_count) -> End -> submit on
AcquireQueue + wait on our fence. Gated by new cvar wos_native_a2 (default false;
true in native toml). Fires once, on the simple target VS 1E6883FC, from
ProbeTranslatedShaders. Both rexglue and both titles build clean.
NOTE: unlike A1 (offscreen), A2 draws into rexglue's LIVE render target, so a
success is VISIBLE on screen -- that is the proof the real shader renders through
our path. Risk: submitting our own cmd buffer on the shared queue mid-frame; kept
to one triangle, once. Render pass compatibility is guaranteed because we use
rexglue's actual render pass + framebuffer.
NEXT (HW): run PPSA99311 into gameplay; expect
  [WoS-NATIVE-A2] binding rexglue draw: VS .. PS .. verts=3 fb=WxH sets=4
  [WoS-NATIVE-A2] drew rexglue's shader natively: VS .. 3 verts into live target
and NO device-loss / crash. If vkQueueSubmit fails or device lost -> the shared-
queue mid-frame submit is the cause; fall back to an offscreen A2 (own framebuffer
compatible with rexglue's render pass -- needs the color VkFormat, which we'd then
add to the snapshot). If it draws but nothing visible, the triangle may be where
the game's UI already is; try a later/world VS or just trust the log + no-crash.

PLUMBING CONFIRMED REACHABLE FROM TITLE:
  Runtime::instance()->graphics_system()->provider()  (cast ui::VulkanProvider*)
    ->vulkan_device() -> device()/physical_device()/functions()/
      queue_family_graphics_compute()/AcquireQueue(). functions() is the loaded
  dispatch table (PS5 has no global vk symbols -> MUST call through it).
  First target geometry: VS 1E6883FC draw (stride 7w=28B, pos+color, VB ~1E513A00).

### NEXT STEP (3) -- FIRST NATIVE DRAW
We now have, per draw: translated VS+PS (the rex::graphics::Shader*) AND a
validated vertex layout + live VB/IB. Build order:
  a. Get the VulkanShader translations for the active VS/PS. pipeline_cache
     LoadShader(type, host_ptr, dwords) returns the cached VulkanShader keyed by
     ucode_data_hash (same object rexglue uses) -> its VulkanTranslation has SPIR-V.
  b. Build (or fetch cached) a VkPipeline: rexglue's VulkanPipelineCache already
     turns (VS,PS,vertex layout,render state) into a pipeline for its OWN draws.
     Cheapest path = reuse that cache rather than hand-rolling VkGraphicsPipeline.
     Investigate: can we ask the pipeline cache for the pipeline of the CURRENT
     register state (it does this every draw in IssueDraw)? If so, the "native
     draw" is mostly: snapshot state -> ask cache -> bind -> vkCmdDraw to a scratch
     RT. If not, hand-roll a minimal VkGraphicsPipeline from the two SPIR-V modules
     + a VkVertexInputState derived from the [WoS-NATIVE-VTX] layout.
  c. Bind VB/IB from guest mem (membase + base_address), set viewport/scissor from
     regs, vkCmdDraw(Indexed). Render into an OFFSCREEN scratch target first (do
     NOT fight rexglue for the real swapchain yet); read back / checksum to prove
     geometry landed, compare vs emulation. Then graduate to the real target.
  FIRST TARGET SHADER: VS 1E6883FCCDE1F688 + its PS -- simplest layout (1 binding,
  2 float attrs, stride 7w), appears constantly, easy to reason about.

### A2 CRASH (2026-10-25 23:47) -- threading hazard, redesign needed
First armed A2 run SIGSEGV'd: signal 11, fault address 0x0 (null read), on thread
"JQ worker 1 (CP..." (a rexglue GPU command-processor worker), rip inside eboot
(0x0416a9d8) -- i.e. the crash is in emulator/rexglue code, triggered BY A2, not in
our Vulkan calls directly.
ROOT CAUSE: RenderOneDrawA2 runs on the GAME DRAW THREAD (called from
ProbeTranslatedShaders, itself called from the DrawVertices hook). It reads the
command processor's snapshot (pipeline + TRANSIENT descriptor sets) and submits its
own command buffer on the SHARED queue. But rexglue's own GPU thread concurrently
mutates that state and RECYCLES those transient descriptor sets every submission
(current_graphics_descriptor_sets_ are per-submission transient allocations). Between
our snapshot read and our bind/submit, rexglue frees/reuses them -> null handle ->
crash. Also two threads submitting to one VkQueue without external sync is itself UB.
CONCLUSION: handing rexglue's live pipeline/descriptor handles to another thread is
unsafe by construction. The native draw that REUSES rexglue's state must run ON
rexglue's GPU thread, where that state is valid and no recycling races it.
REDESIGN (A2 v2): move the native draw INTO rexglue (the 0030 patch), executed on
the command-processor thread inside IssueDraw, right after UpdateBindings, gated by
a cvar the patch reads (wos_native_a2). The title side no longer submits anything;
it just turns the flag on. The draw can still go to an offscreen target we own (set
up once, on the GPU thread) or into rexglue's render pass. This keeps all VkQueue
submits on rexglue's thread. Revert the title-thread RenderOneDrawA2 submit.

## SESSION 4 -- PORTING EDGE OF TIME'S RENDERER: P0 (shaders) DONE, P1 (D3D map) DONE

### P0: whole shader containers harvested and verified (2026-10-29)
wos_ps5_shader_harvest.cpp (cvar wos_shader_harvest) hooks SetPixelShader
sub_82AB41A8 / SetVertexShader sub_82AB4460 and reassembles each object's container:
virtual part inline (PS +0x28, VS +0x368) + physical part via the pointer (PS +0x18,
VS +0x20). v1 read that pointer as a CPU address -> garbage microcode. It is a GPU
address: D3D's commit (sub_82ACC888) emits IM_LOAD with
(p & 0x1FFFFFFE) + (p >= 0xE0000000 ? 0x1000 : 0) and reads PHYSICAL memory. v2 reads
it with memory->TranslatePhysical() of that value. Pointers look like 0xFD3EEFC0 /
0xFFC037E0 (0xE0+ physical view).
RESULT (run 2026-10-29 18:45): 504 containers (PS 179, VS 325).
 - PS: 178/179 microcode byte-identical to rexglue's IM_LOADed ucode.
 - VS: every object changes ONCE, at its first draw: D3D's bind-time patcher
   (sub_82ACBC50) rewrites the vfetch instructions in place. The patched copy matches
   rexglue (153 of 159 VS virtual parts); the original is what XenosRecomp (and EoT,
   which hashes at register time) should use. Both copies are in the harvest.
 - rexglue's dump key is XXH3 of the microcode as guest BIG-endian bytes.
 - VS containers with flags 0x102A1121 carry offsets in fields 0x1C/0x20 (second shader
   record); XenosRecomp's directory scan requires them zero -> zero them before
   recompiling AND before hashing at runtime, or keys will not match.
XENOSRECOMP: upstream: 474/504 to SPIR-V. EoT's fork (goliathret/reeot-XenosRecomp,
branch reeot, built with clang Release): 504/504 translate, 500/504 to SPIR-V; the 4
failures are 3 PATCHED VS copies (originals compile) + 1 PS. Output
wos_shader_cache.cpp (500 entries) in EoT's format. Game-derived: never committed.
EoT's renderer REQUIRES the fork's ABI (REEOT_RECOMP: 256 bools g_Booleans(i), loop
constants, swapped-attribute masks, packed DEC3N...), not upstream's.

### P1: the 22 functions EoT's renderer hooks, in WoS
  EoT name                       WoS                notes
  D3DDevice_DrawVertices         sub_82ABECF0       (dev, prim, start, count)
  D3DDevice_DrawIndexedVertices  sub_82ABF0E0
  D3DDevice_BeginVertices        sub_82ABE800       (dev, prim r4, count r5, stride r6) -> ptr;
                                                    DrawVerticesUP sub_82ABECA8 wraps it
  D3DDevice_BeginIndexedVertices NOT IN BINARY      only draws/BeginVertices/2 cmd-buffer fns
                                                    call the commit sub_82ACC888; EoT leaves
                                                    it unmodelled anyway -> skip
  D3DDevice_ClearF               sub_82ABE618       (dev, flags, rect, float4* color, f1 z, r8
                                                    stencil). Clear(D3DCOLOR) sub_82ABE740 calls
                                                    it. CORRECTS earlier ClearF=ABFCB0.
  D3DDevice_Resolve              sub_82ABFCB0       flags&7 = source (RT index 3110 / depth).
                                                    CORRECTS earlier tentative ABDA38 (that is a
                                                    clear helper of sub_82ABDFA0).
  D3DDevice_SetViewport          sub_82AB8B98       (dev, D3DVIEWPORT9*); SetViewportF
                                                    sub_82AB8C18; both -> internal sub_82AB8930
  D3DDevice_SetGammaRamp         sub_82AB8658       (dev, flags r4, RAMP r5) -- EoT reads r4!
  D3DDevice_SetPWLGamma          sub_82AB8720       same shape; shadow at dev+15004 (0x3A9C, EoT
                                                    0x3C20), same layout (3 x 512 bytes)
  D3DDevice_Swap                 sub_82AC4E48       (dev, front texture r4, params r5)
  D3DDevice_CreatePixelShader    sub_82AB48A8       XGRegisterPixelShader is INLINED here: hook
                                                    Create and register the returned object
  D3DDevice_CreateVertexShader   sub_82AB49B8
  XGRegisterVertexShader         sub_82AB4368       (obj r3, physical r4); 1 caller (CreateVS)
  XGRegisterPixelShader          (inlined, above)
  D3D_UnlockResource             sub_82AB5048       internal unlock (resource r3, base, mip):
                                                    lock count -1, flush dirty range. All 4
                                                    public unlocks (AB5408, AB5460, ABB3A0,
                                                    ABB3C8) go through it.
  D3DDevice_SetTexture           sub_82ABD328       slot table dev+12544 (3136*4)
  D3DDevice_SetPixelShader       sub_82AB41A8
  D3DDevice_SetVertexShader      sub_82AB4460
  D3DDevice_SetRenderTarget      sub_82AB8CC8
  D3DDevice_SetDepthStencilSurface sub_82AB9028
  D3DDevice_SetIndices           sub_82AB8540
  D3DDevice_SetStreamSource      sub_82AB8420
WATCH: sub_82AC2118 (1 game caller, sub_82914058) and sub_82AC1D40 (sub_8293ABA0)
also commit shaders -- precompiled command buffers (RunCommandBuffer-like). Draws
inside those bypass the Draw hooks; count them in P3.

### P2: device offsets (EoT d3d.h -> WoS), verified from WoS code
  same:  pending mask 0x0, fetch 0x480, VS/PS float 0x780/0x1780, bool/loop 0x2780..,
         register shadow 0x2880-0x2A5C (checked 0x2884, 0x2908, 0x2920, 0x2954, 0x2A18)
  -0x160: IndexBuffer 0x3094, RT0 0x3098, Depth 0x30A8, Stream0 0x30AC,
          StreamStride0 0x30F0, Texture0 0x3100, PixelShader 0x3194, VertexShader 0x3198
  OTHER:  VertexDeclaration 0x2E2C (EoT 0x2FB8, -0x18C; SetVertexDeclaration
          sub_82AB4630), gamma shadow 0x3A9C (EoT 0x3C20, -0x184)
  object layouts checked: texture fetch +0x1C, surface info +0x18, size +0x24.
  vertex declaration (from the VS patcher sub_82ACBC50): element count +0x18, elements
  +0x34, 12 bytes each, usage at +9, usage index at +10 -- same as EoT. MaxStream +0x1C
  not yet seen in WoS code.

### CORRECTION (2026-10-29): why harvest v1 read garbage
Not "a GPU address read as a CPU one": WoS's shader physical pointers are 0xE0000000+
virtual addresses, and on the PS5 the runtime maps that range 4 KiB off - every access
above 0xE0000000 must add 0x1000 (rex::memory::GuestPtr, REX_PHYS_HOST_OFFSET). v1 read
base + va; v2's TranslatePhysical(p & 0x1FFFFFFF + 0x1000) happens to be the same fix.
Any title-side code touching guest memory must use GuestPtr (headless fixed: its fence
write-back store at 0xFFC0F200 had been landing 4 KiB low).

## SESSION 4 (cont.) -- HEADLESS: the CPU-side ceiling, and two runtime costs found

wos_headless (wos/app/wos_ps5_headless.cpp): no emulated GPU (config.graphics = nullptr,
ReXApp detached), D3D waits ended by stubbing the spin step sub_82AC0C10 (-> 0), fences
marked passed after InsertFence sub_82ABA098, IsFencePending sub_82ABAC10 -> 0, Swap
sub_82AC4E48 counted and skipped. Runs indefinitely.
  - first: gameplay 49-56 fps, main thread 40% asleep, nothing saturated
  - wos_sleep_probe: the job system polls with Sleep(0) (829635F0 / 82962FD0), each
    ~160-190 us on the PS5 (sched_yield). wos_sleep0_spin_us = 2 spins instead.
    -> main thread busy 97%, but fps unchanged
  - wos_profile LR samples: 40-53% after NtPulseEvent (82B16C9C, PulseEvent wrapper
    sub_82B16C88): rexglue's pulse (patch 0007) yields until 10 us pass = ~160 us.
    wos_fast_pulse: auto-reset events just set, manual ones set/10 us PAUSE spin/reset.
    -> headless gameplay 67-77 fps (peaks 93-121), main thread time spread over game code.
  Both are title-side --wrap's (wos_ps5_threads.cpp), off by default.

## SESSION 4 (cont.) -- P3: EDGE OF TIME'S RENDERER LINKED INTO THE WoS TITLE

wos/eot/src: EdgeOfTimeRecomp af29aca src/gpu (minus patches/, package_load, shader_dump)
+ core/{logging,memory_helpers,profiling,hooks,cpu}.h + platform/display + stubs, BSD-3
(wos/eot/LICENSE.EdgeOfTimeRecomp). WoS changes, each marked "WoS port":
  d3d.h offsets (P2); kGuestRenderWidth/Height 1280x720 (EoT 1120x632; to verify);
  fast setters and FastDrawFlush off (EoT-XDK internals) - WoS's own D3D runs under
  every hook; gamma ramp in r5 and shadow 0x3A9C; no BeginIndexedVertices; no
  XGRegisterPixelShader (CreatePixelShader registers); shader key hashes the container
  with 0x1C/0x20 zeroed (as the cache was built); TAA camera global off; aspect/movie
  patches stubbed (gpu/patches/aspect_ratio.h); EoT's ring stubs replaced by headless's.
wos/eot/wos_eot_names.h renames the hooks wos_eot_* and their originals to WoS subs;
wos/app/wos_ps5_native_renderer.cpp owns the sub_ overrides (renderer on: EoT's hook,
off: the game's own), plus draws (wos_ps5_native.cpp), Set*Shader (harvest) and Swap
(headless) branch the same way. cvar wos_native_renderer (implies the headless stubs).
Build: wos/eot/wos_eot.cmake (plume from ../EdgeOfTimeRecomp with plume-ps5.patch,
zstd/smol-v from its XenosRecomp, renderer shaders via dxc, shader cache from
../wos_shader_cache/wos_shader_cache.cpp). plume's VMA collides with the runtime's ->
prefix_vma.py renames plume's VMA symbols (llvm-objcopy). Links: eboot 109.8 MB.
Shader cache: EoT XenosRecomp at the pinned a031503, 504 harvested containers -> 500.
