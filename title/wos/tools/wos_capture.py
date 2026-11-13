#!/usr/bin/env python3
"""Reads a wos_capture_frame capture (dump/capture_N.bin, see wos/app/wos_ps5_capture.cpp).

  wos_capture.py capture_N.bin            summary: calls, draws per target, state per draw
  wos_capture.py capture_N.bin --draw K   one draw's full state
  wos_capture.py capture_N.bin --blobs D  write every blob (textures, streams, shaders) to D
"""
import struct, sys, os, collections

DEV = 0x6080

def be32(b, o): return struct.unpack_from('>I', b, o)[0]
def bef(b, o): return struct.unpack_from('>f', b, o)[0]

def read(path):
    data = open(path, 'rb').read()
    o = 0
    devices = {}
    events = []
    blobs = {}
    while o + 8 <= len(data):
        tag = data[o:o+4].decode('latin1'); n = struct.unpack_from('<I', data, o+4)[0]; p = data[o+8:o+8+n]; o += 8 + n
        if tag == 'CALL':
            name = p[:32].split(b'\0')[0].decode()
            thread, = struct.unpack_from('<I', p, 32)
            regs = struct.unpack_from('<8I', p, 36)
            f1, = struct.unpack_from('<f', p, 68)
            events.append({'name': name, 'thread': thread, 'regs': regs, 'f1': f1,
                           'at_r5': p[72:136], 'at_r7': p[136:200], 'dev': None})
        elif tag == 'DEVF':
            va, = struct.unpack_from('<I', p, 0); devices[va] = bytearray(p[4:4+DEV])
            events[-1]['dev'] = bytes(devices[va])
        elif tag == 'DEVD':
            va, cnt = struct.unpack_from('<II', p, 0); d = devices[va]
            for i in range(cnt):
                idx, val = struct.unpack_from('<II', p, 8 + 8*i)
                struct.pack_into('<I', d, 4*idx, val)
            events[-1]['dev'] = bytes(d)
        elif tag == 'BLOB':
            kind, key, ln = struct.unpack_from('<III', p, 0); blobs[(kind, key)] = p[12:12+ln]
        elif tag == 'SWAP':
            events.append({'name': 'SWAP', 'regs': (0,)*8, 'dev': None})
    return events, blobs

def state(d):
    s = {}
    s['rt0'] = be32(d, 0x3098); s['depth'] = be32(d, 0x30A8)
    s['vs'] = be32(d, 0x3198); s['ps'] = be32(d, 0x3194)
    s['blend0'] = be32(d, 0x2938); s['colorctl'] = be32(d, 0x293C); s['depthctl'] = be32(d, 0x2934)
    s['colormask'] = be32(d, 0x28DC); s['modectl'] = be32(d, 0x2948); s['decl'] = be32(d, 0x2E2C)
    tex = []
    for t in range(32):
        fc = d[0x480+24*t:0x480+24*t+24]
        w = [be32(fc, 4*i) for i in range(6)]
        if w[0] & 3 == 2:
            tex.append((t, be32(d, 0x3100+4*t) if t < 16 else 0, w))
    s['tex'] = tex
    s['vsc'] = [[bef(d, 0x780+16*r+4*c) for c in range(4)] for r in range(16)]
    s['psc'] = [[bef(d, 0x1780+16*r+4*c) for c in range(4)] for r in range(16)]
    return s

def main():
    path = sys.argv[1]
    events, blobs = read(path)
    if '--blobs' in sys.argv:
        out = sys.argv[sys.argv.index('--blobs')+1]; os.makedirs(out, exist_ok=True)
        names = {1:'tex', 2:'vtx', 3:'idx', 4:'vs', 5:'ps', 6:'ib', 7:'mesh', 8:'cbobj'}
        for (k, key), b in blobs.items():
            open(os.path.join(out, f'{names[k]}_{key:08x}.bin'), 'wb').write(b)
        print(len(blobs), 'blobs'); return
    draws = [e for e in events if e['dev'] is not None]
    if '--draw' in sys.argv:
        k = int(sys.argv[sys.argv.index('--draw')+1]); e = draws[k]; s = state(e['dev'])
        print(e['name'], [hex(r) for r in e['regs']])
        for key, v in s.items():
            if key in ('vsc', 'psc'):
                for r, c in enumerate(v): print(f'  {key}{r}', ['%.4g' % x for x in c])
            elif key == 'tex':
                for t, obj, w in v: print(f'  tex{t} obj {obj:#x} fc ' + ' '.join(f'{x:08x}' for x in w),
                                          'blob' if (1, w[1] & 0xFFFFF000) in blobs else '')
            else: print(f'  {key} {v:#x}')
        return
    print(len(events), 'calls,', len(draws), 'draws,', len(blobs), 'blobs')
    print(collections.Counter(e['name'] for e in events).most_common())
    last = None
    for i, e in enumerate(events):
        if e['dev'] is None:
            if e['name'] not in ('wos_eot_SetTexture', 'wos_eot_SetStreamSource', 'wos_eot_SetIndices', 'RingConstants'):
                print(f'      {e["name"]} ' + ' '.join(f'{r:#x}' for r in e['regs'][:6]))
            continue
        s = state(e['dev'])
        key = (s['rt0'], s['depth'], s['ps'] != 0, s['blend0'], s['colormask'])
        if key != last:
            print(f'  draw#{draws.index(e)} {e["name"]} rt0={s["rt0"]:#x} ds={s["depth"]:#x} ps={s["ps"]:#x} '
                  f'blend={s["blend0"]:#x} mask={s["colormask"]:#x} depthctl={s["depthctl"]:#x} '
                  f'tex={[(t, hex(w[1])) for t, o, w in s["tex"]][:4]}')
            last = key

main()
