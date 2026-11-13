#!/usr/bin/env python3
"""WebOfShadowsRecomp for the PS5 - where the emulated GPU's command processor spends its time.

    wos/tools/profile-report.py wos-profile.txt [build/ps5/link/llvm-pie.elf] [TOP]

wos-profile.txt is what the title writes beside eboot.bin (/app0) every ten seconds:
the instruction pointers the GPU Commands thread was sampled at, as offsets from
wos_profile_anchor (wos/app/wos_ps5_threads.cpp). This names their functions from
the linked title's symbols (the build's llvm-pie.elf, the eboot that was run) and
prints the functions by share of the samples, and the same grouped by library
(ReXGlue's GPU, RADV, the C++ library...).

SPDX-License-Identifier: MIT
"""
import bisect
import collections
import os
import re
import subprocess
import sys


def nm_tool():
    here = os.path.dirname(os.path.abspath(__file__))
    for path in (os.path.join(here, '..', '..', '..', 'llvm-20', 'bin', 'llvm-nm'), 'llvm-nm-20', 'llvm-nm'):
        try:
            subprocess.run([path, '--version'], capture_output=True, check=True)
            return path
        except (OSError, subprocess.CalledProcessError):
            pass
    raise SystemExit('llvm-nm not found')


def symbols(elf):
    out = subprocess.run([nm_tool(), '-C', '-n', '--defined-only', elf], capture_output=True,
                         text=True, check=True).stdout
    addrs, names, anchor = [], [], None
    for line in out.splitlines():
        m = re.match(r'^([0-9a-fA-F]+) ([tTwW]) (.*)$', line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        name = m.group(3)
        if name == 'wos_profile_anchor':
            anchor = addr
        addrs.append(addr)
        names.append(name)
    if anchor is None:
        raise SystemExit(f'{elf}: no wos_profile_anchor (an older build?)')
    return addrs, names, anchor


def group(name):
    if 'rex::graphics::vulkan' in name:
        return 'ReXGlue GPU (Vulkan)'
    if 'rex::graphics' in name:
        return 'ReXGlue GPU (common)'
    if name.startswith('rex::'):
        return 'ReXGlue other'
    if name.startswith(('radv_', 'ac_', 'vk_', 'si_', 'nir_', 'util_', '_mesa', 'ralloc', 'gfx')):
        return 'RADV / Mesa'
    if name.startswith(('std::', '__', 'operator ', 'mem', 'str')):
        return 'C/C++ library'
    if '?' in name:
        return 'outside the title (system libraries)'
    return 'other'


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    profile = sys.argv[1]
    elf = sys.argv[2] if len(sys.argv) > 2 else 'build/ps5/link/llvm-pie.elf'
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 50
    addrs, names, anchor = symbols(elf)
    limit = addrs[-1] + (1 << 20)

    def name_of(offset):
        if not offset:
            return None
        address = (anchor + offset) & ((1 << 64) - 1)
        if address > limit:
            return None
        i = bisect.bisect_right(addrs, address) - 1
        return names[i] if i >= 0 else None

    self_time = collections.Counter()
    inclusive = collections.Counter()
    outside_callers = collections.Counter()
    total = 0
    for line in open(profile):
        if line.startswith('#') or not line.strip():
            continue
        fields = line.split()
        if len(fields) == 2:  # the first format: offset count
            fields = [fields[1], fields[0]]
        count = int(fields[0])
        frames = [int(v, 16) for v in fields[1:]]
        total += count
        own = name_of(frames[0])
        self_time[own or '? (outside the title: system libraries, the kernel)'] += count
        seen = set()
        for i, offset in enumerate(frames):
            n = name_of(offset)
            if n and n not in seen:
                seen.add(n)
                inclusive[n] += count
        if own is None:
            caller = next((name_of(o) for o in frames[1:] if name_of(o)), None)
            outside_callers[caller or '?'] += count
    print(f'{total} samples of GPU Commands')
    groups = collections.Counter()
    for name, count in self_time.items():
        groups[group(name)] += count
    print('\nby part (own time):')
    for name, count in groups.most_common():
        print(f'  {100.0 * count / total:5.1f}%  {name}')
    print(f'\ntop {top} functions, own time:')
    for name, count in self_time.most_common(top):
        print(f'  {100.0 * count / total:5.1f}%  {name[:150]}')
    if len(inclusive):
        print(f'\ntop {top} functions, with what they call (as far as the stacks reach):')
        for name, count in inclusive.most_common(top):
            print(f'  {100.0 * count / total:5.1f}%  {name[:150]}')
        print('\noutside the title, by the title function that called:')
        for name, count in outside_callers.most_common(25):
            print(f'  {100.0 * count / total:5.1f}%  {name[:150]}')


if __name__ == '__main__':
    main()
