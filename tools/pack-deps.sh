#!/usr/bin/env bash
# Pack deps/ into deps-archive/ for the git repository (run by the release maker after
# make-deps.sh; players never run this):
#
#   tools/pack-deps.sh
#
# deps/ holds files over GitHub's 100 MB limit and git repositories of its own, so it is
# not committed as is: it becomes one tar (symlinks, permissions and the pinned .git
# folders kept), zstd-compressed and split into 49 MB parts (under GitHub's 50 MB warning), with their SHA-256 sums.
# build.sh checks the sums and unpacks it into deps/ on the player's machine.
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
release=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$release"
[[ -d deps/rexglue-sdk && -d deps/llvm-20 ]] || { echo "error: no deps/ to pack (run tools/make-deps.sh)" >&2; exit 1; }
command -v zstd > /dev/null || { echo "error: needs zstd (sudo apt install zstd)" >&2; exit 1; }
rm -rf deps-archive.partial
mkdir deps-archive.partial
echo "==> packing deps/ ($(du -sh deps | cut -f1)); this takes a while"
tar --exclude=deps/.archive-id -cf - deps | zstd -15 -T0 -q |
    split -b 49M -d -a 3 - deps-archive.partial/deps.tar.zst.
(cd deps-archive.partial && sha256sum deps.tar.zst.* > SHA256SUMS)
rm -rf deps-archive
mv deps-archive.partial deps-archive
# deps/ as it is now is what the archive holds
sha256sum deps-archive/SHA256SUMS | cut -c1-64 > deps/.archive-id
ls -la deps-archive | sed 's/^/  /'
echo "==> done: commit deps-archive/ (deps/ itself stays out of git)"
