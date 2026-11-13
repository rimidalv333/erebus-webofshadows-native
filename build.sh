#!/usr/bin/env bash
# Spider-Man: Web of Shadows for the PS5 (PPSA31622) - release builder.
#
#   ./build.sh [path/to/game.iso]
#
# Builds the PS5 title folder from YOUR OWN copy of the Xbox 360 game (USA, title
# ID 41560815, as an Xbox 360 disc image / XISO). Everything it downloads and builds
# goes into ./wos-build next to this script; the result is ./PPSA31622, which you
# copy to /data/homebrew/ on the console yourself (FTP, USB, ...). It never connects
# to the console.
#
# Runs on Linux (Ubuntu 24.04 tested) and on Windows inside WSL2 (Ubuntu 24.04).
# Keep this folder on the Linux filesystem (e.g. ~/WoS-PS5), not under /mnt/c:
# a build there is many times slower.
#
# Re-running is safe: finished steps are skipped. Options (environment):
#   WOS_CODEGEN_OPTS   recompiler options by name (see title/wos/tools/codegen.sh); default:
#                      the console-tested set below. WOS_CODEGEN_OPTS="" = none (slowest, safest)
#   WOS_JOBS           compilers at once (default 6; fewer if short of memory)
#   WOS_NO_APT=1       do not install system packages (check only)
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
work=${WOS_BUILD_DIR:-$here/wos-build}
title_id=PPSA31622
out=$here/$title_id
# WOS_BUILD_DIR is only the working folder (sources, objects, caches). The finished title
# folder always goes to $out, next to this script.
case "$(realpath -m "$work")/" in
    "$(realpath "$here")/" | "$(realpath "$here")"/title/* | "$(realpath "$here")"/deps/* | "$(realpath -m "$out")"/*)
        echo "error: WOS_BUILD_DIR ($work) is the release folder itself or part of it." >&2
        echo "       It is only the working folder; the game always ends up in $out." >&2
        echo "       Leave it unset, or point it at a separate folder." >&2
        exit 1 ;;
esac
echo "working folder: $work"
echo "result:         $out"

# --- Pins: the exact revisions this release was built and console-tested with --------
# (all bundled in deps/ by tools/make-deps.sh; title/wos/tools/setup.sh and the scripts
# it runs check them: rexglue-sdk f47ccdd, PS5_Vulkan 5b5e4fc, PS5 Mesa 7b59ef2, the
# payload SDK 1de8b37 + PS5_Vulkan's pin, extract-xiso 3f5b62c, LLVM 20.1.8)
rexglue_revision=f47ccdde34b3a992f7872a27619fc588040c229f

say() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }
fail() { printf '\n\033[1;31merror: %s\033[0m\n' "$*" >&2; exit 1; }

# --- 0. Where we are ----------------------------------------------------------------
[[ $(uname -s) == Linux ]] || fail "run this on Linux, or on Windows inside WSL2 (Ubuntu 24.04)"
[[ $EUID -ne 0 ]] || fail "run this as your normal user, not root (it asks for sudo when it needs it)"
case $here in
    /mnt/[a-z]/*) echo "warning: this folder is on the Windows drive ($here): the build will be very slow." \
        "Move it into your Linux home (e.g. ~/WoS-PS5) for a much faster build." >&2 ;;
esac
for part in title shader-cache release-data; do
    [[ -e $here/$part ]] || fail "this release is incomplete: $here/$part is missing"
done
[[ -f $here/deps-archive/SHA256SUMS || -d $here/deps ]] ||
    fail "this release is incomplete: there is no deps-archive/ (nor deps/)"
free_gb=$(df -Pk "$here" | awk 'NR == 2 { print int($4 / 1048576) }')
(( free_gb >= 27 )) || echo "warning: only $free_gb GB free here; the first build needs about 25 GB" >&2

# --- 1. System packages -------------------------------------------------------------
say "1/7 system packages"
packages=(git clang-20 lld-20 llvm-20 llvm-20-dev libclang-20-dev libclang-rt-20-dev
    libclc-20-dev libllvmspirvlib-20-dev llvm-spirv-20 cmake ninja-build python3-pip python3-venv
    python3-numpy python3-pil python3-mako python3-yaml python3-ply glslang-tools
    bison flex pkg-config rsync curl wget unzip zstd make xz-utils gzip
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev
    libxtst-dev libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libegl-dev
    libdrm-dev libgbm-dev libasound2-dev libpulse-dev libudev-dev libdbus-1-dev libx11-xcb-dev libxcb1-dev)
command -v dpkg-query > /dev/null || fail "this needs a Debian/Ubuntu system (apt); Ubuntu 24.04 is the tested one"
missing=()
for package in "${packages[@]}"; do
    dpkg-query -W -f='${Status}' "$package" 2> /dev/null | grep -q "install ok installed" || missing+=("$package")
done
if (( ${#missing[@]} )); then
    note "missing: ${missing[*]}"
    [[ -z ${WOS_NO_APT:-} ]] || fail "install them (sudo apt install ...) and run this again"
    sudo apt-get update
    sudo apt-get install -y "${missing[@]}"
else
    note "all there"
fi
# RADV needs a newer meson than Ubuntu's: our own, in a venv inside the build folder
mkdir -p "$work"
if [[ ! -x $work/venv/bin/meson ]]; then
    python3 -m venv --system-site-packages "$work/venv"
    "$work/venv/bin/pip" install -q "meson>=1.4"
fi
export PATH="$work/venv/bin:$PATH"
note "meson $(meson --version)"

# --- The bundled dependencies: deps-archive/ unpacked into deps/ (once per archive) ----
if [[ -f $here/deps-archive/SHA256SUMS ]]; then
    archive_id=$(sha256sum "$here/deps-archive/SHA256SUMS" | cut -c1-64)
    if [[ $(cat "$here/deps/.archive-id" 2> /dev/null) != "$archive_id" ]]; then
        say "unpacking the bundled dependencies (deps-archive/ -> deps/)"
        (cd "$here/deps-archive" && sha256sum --quiet -c SHA256SUMS) ||
            fail "deps-archive/ is damaged or incomplete (a part is missing or differs): download it again"
        rm -rf "$here/deps.partial"
        mkdir -p "$here/deps.partial"
        cat "$here"/deps-archive/deps.tar.zst.* | zstd -dc -T0 | tar -x -C "$here/deps.partial" ||
            fail "could not unpack deps-archive/"
        rm -rf "$here/deps"
        mv "$here/deps.partial/deps" "$here/deps"
        rmdir "$here/deps.partial"
        printf '%s\n' "$archive_id" > "$here/deps/.archive-id"
        note "done"
    fi
fi
for part in WebOfShadowsRecomp rexglue-sdk PS5_Vulkan PS5_PayloadSDK PS5_Mesa EdgeOfTimeRecomp \
        extract-xiso llvm-20 downloads; do
    [[ -e $here/deps/$part ]] || fail "this release is incomplete: deps/$part is missing"
done

# --- 2. Sources in the build folder ---------------------------------------------------
say "2/7 sources"
# The title (this release's code) and the bundled part of WebOfShadowsRecomp, copied in
# so the build never writes into the release itself
rsync -a "$here/title/" "$work/PS5_WebOfShadowsTitle/"
# patch folders exactly as released (a patch dropped from the release must not linger here)
rsync -a --delete "$here/title/wos/patches/" "$work/PS5_WebOfShadowsTitle/wos/patches/"
mkdir -p "$work/WebOfShadowsRecomp"
rsync -a "$here/deps/WebOfShadowsRecomp/" "$work/WebOfShadowsRecomp/"
# The shader cache the native renderer draws with
mkdir -p "$work/wos_shader_cache"
if [[ ! -f $work/wos_shader_cache/wos_shader_cache.cpp ||
        $here/shader-cache/wos_shader_cache.cpp.gz -nt $work/wos_shader_cache/wos_shader_cache.cpp ]]; then
    gzip -dc "$here/shader-cache/wos_shader_cache.cpp.gz" > "$work/wos_shader_cache/wos_shader_cache.cpp"
fi
note "title, WebOfShadowsRecomp sources and the shader cache in $work"
# The bundled dependencies (deps/), copied in once: their builds write into them
for dep in rexglue-sdk PS5_Vulkan PS5_PayloadSDK PS5_Mesa EdgeOfTimeRecomp llvm-20; do
    if [[ ! -e $work/$dep ]]; then
        note "copying $dep"
        rm -rf "$work/$dep.partial"
        cp -a "$here/deps/$dep" "$work/$dep.partial"
        mv "$work/$dep.partial" "$work/$dep"
    fi
done
# The files the setup would otherwise download, where its scripts look for them
for cache in "$work/PS5_WebOfShadowsTitle/.deps/native" "$work/PS5_Vulkan/.deps/native"; do
    mkdir -p "$cache"
    [[ -f $cache/ps5-payload-sdk.zip ]] || cp "$here/deps/downloads/ps5-payload-sdk.zip" "$cache/"
done
mkdir -p "$work/PS5_Vulkan/.deps/native/zlib"
for zlib in "$here"/deps/downloads/zlib-*.tar.gz; do
    [[ -f $work/PS5_Vulkan/.deps/native/zlib/${zlib##*/} ]] || cp "$zlib" "$work/PS5_Vulkan/.deps/native/zlib/"
done
# nothing below may stop at a login prompt or reach for a repository that is gone
export GIT_TERMINAL_PROMPT=0
# compilers at once, everywhere in the build (more can run a WSL out of memory)
export WOS_JOBS=${WOS_JOBS:-6}
# The recompiler options this release is console-tested with: registers kept in C++ locals
# (non_argument_as_local with tools/nonarg-fix.py), no link register bookkeeping. skip_msr is
# left out on purpose (about 1% and not proven over long sessions).
export WOS_CODEGEN_OPTS=${WOS_CODEGEN_OPTS-"cr_as_local ctr_as_local xer_as_local reserved_as_local non_volatile_as_local non_argument_as_local skip_lr"}
note "dependencies from $here/deps"
export WOS_REXGLUE_REVISION=$rexglue_revision

# --- Exact tool versions ----------------------------------------------------------------
# A machine can have several versions of clang, LLVM or CMake, and the build's scripts
# take whichever they find first by name (the payload SDK and Mesa prefer the newest,
# e.g. LLVM 21). Everything here is pinned to the versions this release was tested with,
# and checked, so another installed version can never be picked up:
#   console code and host tools: LLVM's release of 20.1.8 (deps/llvm-20)
#   RADV's host compiler tools:  Ubuntu's LLVM 20, its SPIR-V translator and libclc
#   CMake / Ninja:               Ubuntu's (not a pip or snap one in ~/.local/bin)
say "toolchain: pinning and checking exact versions"
llvm=$work/llvm-20/bin
sysllvm=/usr/lib/llvm-20/bin
pins=$work/pinned-bin
rm -rf "$pins"
mkdir -p "$pins"
pin() { # pin <name> <target>
    [[ -x $2 ]] || fail "missing $2 (for $1)"
    ln -s "$2" "$pins/$1"
}
for name in clang clang-20 clang-21 clang-22; do pin "$name" "$llvm/clang-20"; done
for name in clang++ clang++-20 clang++-21 clang++-22; do pin "$name" "$llvm/clang++"; done
pin clang-cpp "$llvm/clang-cpp"
for name in llvm-config llvm-config-21 llvm-config-22; do pin "$name" "$llvm/llvm-config"; done
for tool in llvm-ar llvm-ranlib llvm-nm llvm-objcopy llvm-strip llvm-readelf llvm-readobj; do
    for name in "$tool" "$tool-21" "$tool-22"; do pin "$name" "$llvm/$tool"; done
done
pin cmake /usr/bin/cmake
pin ninja /usr/bin/ninja
export PATH="$pins:$PATH"
export LLVM_CONFIG=$llvm/llvm-config PS5_CLANG=$llvm/clang-20
export WOS_HOST_CC=$llvm/clang WOS_HOST_CXX=$llvm/clang++
hash -r
# every name must resolve to the pinned file and report the pinned version
want=20.1.8
for name in clang clang++ clang-21 clang++-21 llvm-config llvm-config-21 llvm-ar llvm-ranlib; do
    [[ $(command -v "$name") == "$pins/$name" ]] || fail "$name resolves to $(command -v "$name"), not $pins/$name"
    "$name" --version 2>&1 | grep -q "$want" || fail "$name is not LLVM $want: $("$name" --version 2>&1 | head -1)"
done
[[ $(llvm-config --version) == "$want" ]] || fail "llvm-config reports $(llvm-config --version), not $want"
v=$("$sysllvm/llvm-config" --version 2>/dev/null) || fail "no $sysllvm/llvm-config (sudo apt install llvm-20-dev)"
[[ $v == 20.* ]] || fail "$sysllvm/llvm-config is $v, not 20.x"
s=$(pkg-config --modversion LLVMSPIRVLib 2>/dev/null) || fail "no LLVMSPIRVLib (sudo apt install libllvmspirvlib-20-dev)"
[[ $s == 20.* ]] || fail "LLVMSPIRVLib is $s, not 20.x (RADV's tools need the one matching LLVM 20)"
[[ -x $sysllvm/llvm-spirv || -x /usr/bin/llvm-spirv-20 ]] || fail "no llvm-spirv-20 (sudo apt install llvm-spirv-20)"
c=$(cmake --version | sed -n '1s/.* //p')
[[ $(printf '%s\n3.25\n' "$c" | sort -V | head -1) == 3.25 ]] ||
    fail "Ubuntu's CMake is $c; this build needs 3.25 or later"
note "LLVM $want (console, host tools), system LLVM $v + SPIR-V $s (RADV), CMake $c"

# --- 3. Toolchain, RADV and the runtime (long, first run only) ------------------------
say "3/7 the console toolchain, RADV and the runtime (the first run takes an hour or more)"
bash "$work/PS5_WebOfShadowsTitle/wos/tools/setup.sh" 2>&1 | tee "$work/setup.log" |
    grep -E '^(==>|  ok|  missing|  cloning|error)' || true
[[ -f $work/PS5_Vulkan/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a ]] ||
    fail "RADV did not build: see $work/setup.log"
[[ -f $work/rexglue-sdk/out/ps5-amd64/librexgpu-xenos.a ]] ||
    fail "the ReXGlue runtime did not build: see $work/setup.log"
note "done (log: $work/setup.log)"

# --- 4. The game's files from your disc image -----------------------------------------
say "4/7 your game's files"
xiso=$work/tools/extract-xiso/build/extract-xiso
if [[ ! -x $xiso ]]; then
    rm -rf "$work/tools/extract-xiso"
    mkdir -p "$work/tools"
    cp -a "$here/deps/extract-xiso" "$work/tools/extract-xiso"
    cmake -S "$work/tools/extract-xiso" -B "$work/tools/extract-xiso/build" -DCMAKE_BUILD_TYPE=Release > /dev/null
    cmake --build "$work/tools/extract-xiso/build" -j > /dev/null
fi
[[ -x $xiso ]] || fail "extract-xiso did not build"
# codegen reads the game from WebOfShadowsRecomp/game (a real folder: its find does not follow links)
game=$work/WebOfShadowsRecomp/game
if [[ ! -f $game/.extracted ]]; then
    iso=${1:-}
    while [[ -z $iso || ! -f $iso ]]; do
        [[ -n $iso ]] && echo "  not a file: $iso"
        read -r -p "Path to your Spider-Man: Web of Shadows Xbox 360 disc image (.iso): " iso
        iso=${iso%\"}; iso=${iso#\"}
        # a Windows path pasted into WSL
        if [[ $iso =~ ^([A-Za-z]):[\\/](.*)$ ]] && command -v wslpath > /dev/null; then
            iso=$(wslpath "$iso")
        fi
    done
    rm -rf "$game" "$game.partial"
    mkdir -p "$game.partial"
    note "extracting $iso"
    (cd "$game.partial" && "$xiso" -x "$(realpath "$iso")" > "$work/extract-xiso.log" 2>&1) ||
        fail "extract-xiso could not read $iso (see $work/extract-xiso.log): is it an Xbox 360 disc image?"
    # extract-xiso puts the files in a folder named after the image
    inner=$(find "$game.partial" -maxdepth 2 -type f -iname default.xex -printf '%h\n' -quit)
    [[ -n $inner ]] || fail "no default.xex in $iso: not Spider-Man: Web of Shadows for the Xbox 360?"
    mv "$inner" "$game"
    rm -rf "$game.partial"
    touch "$game/.extracted"
fi
xex=$(find "$game" -maxdepth 1 -type f -iname default.xex -print -quit)
note "default.xex: $(stat -c %s "$xex") bytes, $(find "$game" -type f | wc -l) files"
# The release's hooks and fixes are tied to this exact executable (USA disc, media ID
# 638BF87A, version 1, no title update): any other default.xex would recompile into a game
# whose patched functions sit elsewhere.
xex_want=3183f0beab0df589137da4430de4ac6716d281fd732b7c6b71d153140add479c
xex_have=$(sha256sum "$xex" | cut -d' ' -f1)
if [[ $xex_have != "$xex_want" ]]; then
    [[ ${WOS_ALLOW_OTHER_XEX:-0} == 1 ]] ||
        fail "this default.xex is not the one this release supports (SHA-256 $xex_have).
       It needs the USA disc (media ID 638BF87A, default.xex SHA-256 $xex_want);
       see README.md. To extract a different image, delete $game first."
    echo "warning: WOS_ALLOW_OTHER_XEX=1 - building an unsupported default.xex" >&2
fi

# --- 5. Recompile the game's code ------------------------------------------------------
say "5/7 recompiling default.xex (codegen)"
stamp=$work/WebOfShadowsRecomp/generated/.xex-sha256
xex_sum=$(sha256sum "$xex" | cut -d' ' -f1)
# (with the recompiler options: changing them recompiles the game)
xex_sum="$xex_sum nafix=1 fast=${WOS_CODEGEN_FAST:-0} opts=$(printf '%s\n' ${WOS_CODEGEN_OPTS:-} | sort -u | tr '\n' ' ')"
if [[ -f $stamp && $(< "$stamp") == "$xex_sum" && -f $work/WebOfShadowsRecomp/generated/default/sources.cmake ]]; then
    note "already done for this default.xex"
else
    bash "$work/PS5_WebOfShadowsTitle/wos/tools/codegen.sh"
    echo "$xex_sum" > "$stamp"
fi

# fix118: the game's window-strip loop calls wos_draw_vertex_list (wos_strip_merge in the toml);
# idempotent, one generated file
python3 "$work/PS5_WebOfShadowsTitle/wos/tools/strip-merge-patch.py" \
    "$work/WebOfShadowsRecomp/generated/default" | sed 's/^/    /' ||
    echo "    (wos_strip_merge will have no effect in this build)"

# --- 6. Build the title ------------------------------------------------------------------
say "6/7 building the title (compiling the recompiled game takes a while)"
(cd "$work/PS5_WebOfShadowsTitle" && WOS_NATIVE=1 WOS_TITLE_APP=game bash ps5/tools/build.sh)
dist=$work/PS5_WebOfShadowsTitle/dist/$title_id
[[ -f $dist/eboot.bin ]] || fail "the build made no $dist/eboot.bin"

# --- 7. The title folder for the console ---------------------------------------------------
say "7/7 assembling $out"
rm -rf "$out.partial"
mkdir -p "$out.partial"
rsync -a --exclude gamedata --exclude lossless "$dist/" "$out.partial/"
rsync -a "$here/release-data/" "$out.partial/"
# the game's files: hard links to the extracted ones (no second ~7 GB copy) where the
# filesystem allows, a copy otherwise
if ! cp -al "$game" "$out.partial/gamedata" 2> /dev/null; then
    rm -rf "$out.partial/gamedata"
    mkdir -p "$out.partial/gamedata"
    rsync -a --exclude .extracted "$game/" "$out.partial/gamedata/"
fi
rm -f "$out.partial/gamedata/.extracted"
rm -rf "$out"
mv "$out.partial" "$out"
size=$(du -sh "$out" | cut -f1)
cat << EOF

Done: $out ($size)

Copy the whole $title_id folder to /data/homebrew/ on your PS5 (any FTP client,
or a USB drive), so that /data/homebrew/$title_id/eboot.bin exists, then launch
"Spider-Man: Web of Shadows" from the home screen (ShadowMountPlus).
Settings are in $title_id/wos_recomp.toml.
EOF
