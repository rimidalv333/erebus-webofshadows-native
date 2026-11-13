# Building, installing and testing WebOfShadowsRecomp for the PS5

You do this once on your PC, then once per change. The PC side runs on **Linux**
(Ubuntu 24.04 is what it was built on). On Windows, use **WSL2 with Ubuntu 24.04**
and run everything below inside it.

## 1. What the PC needs

```
sudo apt install git clang-20 lld-20 llvm-20 llvm-20-dev libclang-20-dev libclang-rt-20-dev \
    libclc-20-dev libllvmspirvlib-20-dev llvm-spirv-20 cmake ninja-build python3-pip \
    python3-numpy python3-pil python3-mako python3-yaml python3-ply glslang-tools \
    bison flex pkg-config rsync curl wget unzip zstd make \
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev \
    libxtst-dev libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libegl-dev \
    libdrm-dev libgbm-dev libasound2-dev libpulse-dev libudev-dev libdbus-1-dev libx11-xcb-dev libxcb1-dev
pip install --user --break-system-packages "meson>=1.4"     # RADV needs a newer meson than Ubuntu's
```

The `libclc`/`spirv` packages are for RADV, the console's Vulkan driver, which the
setup compiles once. The X11/Wayland/sound `-dev` packages are for the `rexglue`
codegen tool, which builds ReXGlue (and its SDL3) for your PC. Ubuntu's meson (1.3) is too old for it; the pip one goes to
`~/.local/bin`, which must be on your `PATH`.

The console's code is compiled with one exact compiler, **LLVM's own release of clang
20.1.8**, which `setup.sh` downloads into `~/ps5/llvm-20` (`wos/tools/get-llvm20.sh`:
a 2 GB download, about 650 MB kept). Your distribution's clang is only used for tools
that run on your PC. Ubuntu 26.04's `clang-20` package crashes on RADV and the runtime
(exit code 139), and a clang 21 build makes an `eboot.bin` firmware 13.00 refuses.

## 2. Set up (once)

Put this repository in a folder of its own: everything else gets cloned beside it.

```
mkdir ~/ps5 && cd ~/ps5
git clone <this repository> PS5_WebOfShadowsTitle
cd PS5_WebOfShadowsTitle
wos/tools/setup.sh
```

`setup.sh` clones what is missing beside it (`rexglue-sdk`, `WebOfShadowsRecomp`,
`PS5_PayloadSDK`, `PS5_Vulkan`, `PS5_Mesa`), then builds the console's pieces:
PS5_Vulkan's SDK, native tool, `libc.prx` and RADV, and the ReXGlue runtime with its
emulated GPU. The first run takes a while (RADV is the long part).
`wos/tools/setup.sh --check` reports what is there without changing anything.

It will remind you about one thing it cannot do: tell PS5_Vulkan your console's address.
Write it in `~/ps5/PS5_Vulkan/.env`:

```
PS5_HOST=192.168.1.50
FTP_PORT=2121
KLOG_PORT=3232
PS5VKCTL_PORT=9111
```

If `zlib.net` is unreachable from your network, PS5_Vulkan's setup cannot download zlib:
put `zlib-1.3.2.tar.gz` from https://github.com/madler/zlib/releases (same file) in
`~/ps5/PS5_Vulkan/.deps/native/zlib/` and run setup again.

## 3. The test app first (no game needed)

Before the game, check the runtime on the console on its own:

```
WOS_TITLE_APP=test ps5/tools/build.sh
```

This builds `dist/PPSA99310/` with the **test app**. It opens the display through the
emulated GPU's Vulkan device and presenter, and shows four seconds of red, green, blue
and white with a small window on top. It then initializes the guest memory and plays a
one-second beep. Copy it to the console (step 6), launch it, and send back
`wos-test-results.txt` from the title's folder. Its `gpu:` lines list the Vulkan
features the console's RADV offers the GPU emulation.

## 4. Recompile your game

Extract your own disc image of *Spider-Man: Web of Shadows* (Xbox 360, USA, title ID
41560815) into `~/ps5/WebOfShadowsRecomp/game/`, so that `game/default.xex` is there:

```
extract-xiso -x "Spider-Man Web of Shadows.iso" -d ~/ps5/WebOfShadowsRecomp/game
```

Then:

```
wos/tools/codegen.sh
```

The first time, this builds the `rexglue` tool for your PC (from the same ReXGlue fork,
whose code template knows the PS5), then recompiles `default.xex` into
`WebOfShadowsRecomp/generated/default/`. Its log is `generated/codegen.log`. That code is
made from your game: don't commit it anywhere.

## 5. Build the game

```
WOS_TITLE_APP=game ps5/tools/build.sh
```

(`WOS_TITLE_APP` is only needed when switching between `test` and `game`; the choice
stays in `build/ps5/` until you change it.) Compiling the recompiled code takes a while.
The result is `dist/PPSA99310/`.

## 6. Put it on the console

Copy your extracted game files into the title folder's `gamedata/`, so that
`dist/PPSA99310/gamedata/default.xex` exists. Then copy the whole `PPSA99310` folder to
`/data/homebrew/` on the PS5 yourself (any FTP client, or a USB drive). None of
these tools connect to the console.

The game files only need copying once: later builds change `eboot.bin`.

ShadowMountPlus puts the title on the home screen as *Spider-Man Web of Shadows*.
Launch it from there.

## Reporting a run

Everything a run writes is in the title's folder on the console:

| File | What it says |
| --- | --- |
| `wos-test-results.txt` | the title's own lines: each start-up stage *before* it happens, then `alive: N s` every 5 seconds and `fps: …` every 10. Where it stops is where the run stopped. |
| `logs/startup.log` | the runtime's start-up stages |
| `logs/fault.log` | a crash's record: the fault, the registers, a backtrace |
| `logs/wos_recomp*.log` | the game's log: settings, GPU creation, warnings |
| klog | the console's log, with the title's lines among them |

Copy them off the console, zip them and send them with a line on what you saw (black
screen, picture, sound, how far it got).

## Settings

`wos_recomp.toml` beside `eboot.bin` holds the settings worth changing; edit it over FTP.
The useful ones:

- `draw_resolution_scale_x` / `_y`: 1 (720p, the default), 2 or 3 (4K)
- `anisotropic_override`: texture filtering, 0 off to 5 (16x)
- `render_target_path_vulkan`: `fsi` (default) or `fbo` (cutscenes and the title screen black)
- `vulkan_sparse_shared_memory`: `false` (default) or `true`, to compare

## Updating

```
git pull                      # this repository
git -C ../rexglue-sdk pull    # the runtime, when a fix lands there
wos/tools/build-rexglue.sh    # rebuild the runtime after pulling it
ps5/tools/build.sh
```

Re-run `wos/tools/codegen.sh` only when the ReXGlue fork's code generator changes.
