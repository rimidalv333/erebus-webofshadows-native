# SWOS - PS5 (PPSA31622)

This is a native PS5 build of *Spider-Man: Web of Shadows* (Xbox 360), made from your own copy of the game. The game's code is statically recompiled for the PS5 and drawn by Edge of Time's native renderer (no GPU emulation), at 60 fps and up to 4K output, 8x MSAA & 16x anisio.
No game files are included, you need your own disc image (see below).

**State of the release**
The game targets 60 and holds it almost everywhere. There are areas where the lowest drops can be 51 fps, mostly when looking across the skyline from the densest part of the city. That limit is the game's own drawing code on the CPU, not the GPU (the GPU sits at about 2-3 ms a frame).
I have not finished a full playthrough on this build yet (port was mainly tested on an initial version of the game utilizing only GPU emulation).
Still, the native renderer has been tested against a confident >50% of the game with no obvious visual glitches or degradation.
Shaders the renderer has never seen get recorded on the console and added to the bundled shader cache, so the cache (and this repo) will be updated with time.

**Which disc image**
Only the USA release works (Redump name *Spider-Man - Web of Shadows (USA) (En,Fr)*), unpatched, no title update:
```
Title ID      41560815
Media ID      638BF87A
Executable    version 1 (base version 1), disc 1 of 1, XDK 2.0.6995
default.xex   SHA-256 3183f0beab0df589137da4430de4ac6716d281fd732b7c6b71d153140add479c
```
`build.sh` checks `default.xex` and stops if it differs, because the release's fixes sit at fixed addresses inside that exact executable.
For reference, the image I built and tested with (7,835,492,352 bytes):
```
CRC32   ba55e9c3
MD5     004309b6ae542daf01ffa94ae22a1f3c
SHA-1   1debbfa1e5188660db0bd4aa6b7ba1c886a72150
```
A dump with different image checksums (another dump, a trimmed image) still works as long as its `default.xex` matches.

**Building**
Setup: a PC with Linux (Ubuntu 24.04) or Windows with WSL2 (Ubuntu 24.04), x86-64, about 25 GB free and 16 GB of memory or more. The first build takes one to a few hours (it builds the console toolchain and the graphics driver), later builds are a lot faster.
On Windows, open the Ubuntu terminal and keep the folder in your Linux home (e.g. `~/WoS-PS5`), not on `C:`, otherwise the build is many times slower.

```
git clone <this repository> ~/WoS-PS5
cd ~/WoS-PS5
./build.sh "/path/to/Spider-Man - Web of Shadows (USA) (En,Fr).iso"
```

Without a path it asks for one (a Windows path like `D:\Games\wos.iso` works too). It installs the system packages it needs (asks for your password once), unpacks the bundled dependencies (`deps-archive/`) into `deps/`, builds everything else inside `wos-build/` (no downloads besides the system packages), extracts your image, recompiles the game and puts the result in:
```
PPSA31622/
```

The recompiler options this release was tested with are listed here:
cr_as_local ON
ctr_as_local ON
xer_as_local ON
reserved_as_local ON
non_volatile_as_local ON
non_argument_as_local ON
skip_lr ON
skip_msr (Off in this release as I would only be confident using it if I played thru the whole game with it. The performance gain is negligible.)

If something breaks and you want the slow but safest code, build with none (not recommended due to performance degradation):
```
WOS_CODEGEN_OPTS="" ./build.sh "/path/to/your.iso"
```

**Installing**
Copy the whole `PPSA31622` folder to `/data/homebrew/` on the PS5, then launch from the home screen (ShadowMountPlus).
Title art, icon art and snd0.at9 musical track on hover included.
Tested with kstuff 1.13-fpkg-dr-test5, ShadowMount 1.7beta7

**Settings**
Everything is in `PPSA31622/wos_recomp.toml`. The defaults are the values I tested with:
```toml
draw_resolution_scale_x = 2      # 1 = 720p, 2 = 1440p, 3 = 4K
draw_resolution_scale_y = 2
wos_upscale = "lanczos"          # bilinear, bicubic or lanczos
wos_msaa = 4                     # 0, 2, 4 or 8
wos_anisotropy = 16              # 0 = as the game asks, 1 = off, 2-16
wos_strip_merge = 1              # the city's window strips drawn per list instead of per strip
wos_strip_fast_path = 2
wos_adaptive_sync = false
```
`wos_strip_merge` and `wos_strip_fast_path` are what bring the heavy city views from about 3,800 draws a frame down to about 2,400. If you ever see missing or broken building windows, set both to 0 and tell me. This was and still is the main CPU bottleneck.

**Texture modding**
The title comes bundled with a texture mod that only affects button prompts - changing them to PS ones from X360.
Textures can be replaced like so: put `<fingerprint>.bin` in `PPSA31622/mods/textures/` and the renderer loads it instead of the original
(same size required, the game files stay untouched).
To get fingerprints, dump with the emulated renderer (`wos_native_renderer = false`, `wos_texture_dump = true`); textures land in `PPSA31622/mods/dump/`. Convert and edit them with `title/wos/tools/wos_textures.py`:
```
python3 wos_textures.py topng dump/ png/
python3 wos_textures.py tobin dump/<original>.bin edited.png out/
```
Copy `out/<fingerprint>.bin` to `mods/textures/` and switch back to the native renderer. The log shows `[texmods] replaced ...` when it's used.
(NOTE: This mechanism is largely untested, the texture mods was something I implemented during extremely early revisions of this project, where the
only renderer was the emulated X360 GPU, some of the toml switches might not function still. *UNTESTED*)
In theory however, this could allow texture modding for suits, flags or other miscellaneous textures one might want to change.

**Credits**
This builds on the work of:
- **WebOfShadowsRecomp** (toowasmir) - the game's recompilation project, the sources codegen needs are bundled in `deps-archive/`
- **ReXGlue** and **Xenia** - the recompiler, runtime and Xbox 360 GPU emulation, KongaTime's fork with the PS5 target
- **PS5_Vulkan**, **PS5_Mesa** and **PS5_PayloadSDK** (mihawk-99, KongaTime) - Mesa's RADV Vulkan driver for the PS5, the title template and the platform layer
- **Edge of Time Recomp** (KongaTime) - the native renderer this one is built from
- the **PS5 payload SDK** (ps5-payload-dev / John Törnblom)
- **extract-xiso** (in <in@fishtank.com>, XboxDev) - bundled in `deps-archive/`
- **LLVM/Clang**, **Mesa** and their contributors

**Bundled dependencies**
Everything the build needs is pinned to the exact revision I built and tested with and ships in `deps-archive/` with its own license files:
```
rexglue-sdk (third-party sources incl. inja)  f47ccdd          BSD (Xenia portions included)
PS5_Vulkan                                    5b5e4fc          GPL-3.0
PS5_Mesa (RADV)                               7b59ef2          MIT and others (licenses/)
PS5_PayloadSDK                                1de8b37          GPL-3.0
ps5-payload-sdk release                       v0.42            GPL-3.0 (upstream; the zip has no license file)
EdgeOfTimeRecomp (plume, XenosRecomp, dxc)    the used subset  BSD-3-Clause (and their own)
extract-xiso                                  3f5b62c          see its LICENSE.TXT
LLVM / Clang                                  20.1.8           Apache-2.0 WITH LLVM-exception
zlib                                          1.3.2            zlib
```
`build.sh` puts these first in the build's path and checks their versions, so whatever else is installed on the machine (other LLVM versions and so on) is not used.
Each component keeps its own license, the built title carries them in `PPSA31622/licenses/` and `PPSA31622/LEGAL.txt`. This repository is GPL-3.0-or-later as it links GPL-3.0 components.

**Legal**
Not affiliated with or endorsed by Activision, Marvel, Sony or Microsoft. All assets belong to their owners. Only use a disc image of a game you own.

**Known issues / limitations**
Some moves and combos are harder to time at 60 fps (the game was built around 30), but they all still work.
Buildings, shadows, enemies and other characters pop in at a distance, the same as on the original Xbox 360 release. The game decides its own draw distances and this build keeps them.
The heaviest skyline views can still drop to about 51 fps.

NOTICE - **AI assistance**
Generative AI was used for part of the code in this project, to speed up development. Getting from the first emulated-GPU boot to the near-60 fps native build took close to a thousand test iterations: builds, console runs, shader dumps and kernel logs. The AI helped write and adjust code and read through that volume of output. Every change was built and tested on real base PS5 hardware before it went in, and the testing, decisions and direction were mine.

Native renderer in its infancy:
<img width="1568" height="735" alt="image" src="https://github.com/user-attachments/assets/b58404c3-192b-4898-adfd-d40e0179daf3" />
<img width="2148" height="806" alt="image" src="https://github.com/user-attachments/assets/58476424-a23f-461b-90c0-ec223ffb47e5" />

Screenshot from mature build taken straight from console (1080p TV):
<img width="3840" height="2160" alt="20261113_181040_00827035" src="https://github.com/user-attachments/assets/0c083082-b4ad-4b46-af5b-642944eaa320" />
