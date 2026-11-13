# WebOfShadowsRecomp for the PS5

A PS5 homebrew title (`PPSA99310`) for [WebOfShadowsRecomp](https://github.com/toowasmir/WebOfShadowsRecomp),
the static recompilation of *Spider-Man: Web of Shadows* (Xbox 360) on the ReXGlue SDK.
It runs on a jailbroken PS5 with the usual homebrew environment (an enabler such as
etaHEN, ShadowMountPlus, an FTP server, and klog for logs).

**Status:** the test app runs on a PS5 on firmware 13.00 (the emulated GPU's Vulkan
device and presenter at 3840x2160, 60 frames a second, the guest memory, the sound).
The game itself is next ([BUILDING.md](BUILDING.md), *Reporting a run*).

## How it works

| Part | Where it comes from |
| --- | --- |
| The game's code | Your own `default.xex`, recompiled to C++ by `rexglue codegen` (`wos/tools/codegen.sh`) |
| The Xbox 360 kernel, memory, threads, audio, pad, fault records | [KongaTime/rexglue-sdk](https://github.com/KongaTime/rexglue-sdk), the ReXGlue fork with the PS5 target, as [PS5_EdgeOfTimeTitle](https://github.com/KongaTime/PS5_EdgeOfTimeTitle) uses it |
| The graphics | The same fork's **emulated Xenos GPU on Vulkan**, linked into the title, on RADV with the PS5's VideoOut display (the route [The Simpsons Game's PS5 port](https://github.com/LoZazaMastro/TheSimpsonsGame-PS5-Builder) takes) |
| The title's frame: launch, klog, packaging, signing | The [PS5 Vulkan Template](https://github.com/mihawk-99/PS5_VulkanTemplate) foundation, as PS5_EdgeOfTimeTitle has it |
| The game's PS5 app (`wos/app/`) | Written for this title: WebOfShadowsRecomp's guest-side hooks without its Windows and D3D12 code |

Edge of Time's PS5 port draws with a renderer of its own (plume) that replaces that game's
Direct3D. Web of Shadows has no such renderer: the PC release emulates the Xbox 360 GPU on
D3D12, and this title emulates it on Vulkan instead. What the PC release adds on top of D3D12
(FSR/CAS, the "2x frame pacer", the occlusion-culling and VRAM presets, the launcher,
keyboard and mouse) is not here; `wos/app/wos_ps5_app.h` lists what was kept, replaced and dropped.

## The title folder

`ps5/tools/build.sh` leaves a complete title folder in `dist/PPSA99310/`:

```
PPSA99310/
├── eboot.bin            the title: the runtime, the emulated GPU, RADV and the recompiled game code
├── sce_sys/             param.json, icon0.png
├── sce_module/libc.prx
├── wos_recomp.toml      settings (resolution scale, anisotropy, ...), editable over FTP
├── gamedata/            ← the player's own extracted Xbox 360 game files go here (default.xex at the top)
├── assets/, licenses/, LEGAL.txt
└── logs/                written by the title at run time
```

The folder is the same for everyone: a player adds their own game files to `gamedata/`
and copies the folder to `/data/homebrew/`. If the title finds no `default.xex` in
`gamedata/`, it says so on screen and closes.

**Before sharing a build:** `eboot.bin` contains code recompiled from the game's
executable (no game assets). WebOfShadowsRecomp's PC release and PS5_EdgeOfTimeTitle's
release are shared on the same basis, but that is your call to make. WebOfShadowsRecomp
also publishes no licence, so ask its author before releasing a build that contains its
code. The title compiles that code from your own checkout; none of it is in this repository.

## Building

See **[BUILDING.md](BUILDING.md)**: one setup script, one codegen script, one build script,
then the steps on the console.

## Licences

`wos/` and the title's tooling are MIT (parts adapted from PS5_EdgeOfTimeTitle, MIT,
KongaTime); the foundation is MIT (Sascha Willems; the PS5 layer, Mihawk). The ReXGlue
runtime is BSD 3-Clause. The built title links the PS5 platform layer of the payload SDK
fork, which is GPL-3.0, so the title as distributed is under GPL-3.0. `LEGAL.txt` and
`licenses/` in the title folder list every part.

Not affiliated with or endorsed by Sony Interactive Entertainment, Activision, Marvel or
Microsoft. Use only a game you own.
