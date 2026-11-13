#!/usr/bin/env python3
"""Command-stream analysis for wos_capture_frame / L3+R3 captures (fix34+: blobs 9, 10).

  exec(open('wos/tools/wos_stream.py').read())
  ev, bl = read('capture_pad1_f2384.bin')
  snaps = gpu_states(shifted_spans(bl))      # GPU register file at each DRAW packet
  draws = [e for e in ev if e['name'] in ('DrawIndexedVertices','DrawVertices','BeginVertices')]
  pairs = align(snaps, draws)                # (draw event index, snapshot index), by count
Then compare snaps[k][1][reg] with the device shadow in draws[i]['dev'] (see HANDOFF.md 14).
"""
import struct, collections, sys
import os
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'wos_capture.py')).read().split('\ndef main')[0])
def be(b,o): return struct.unpack_from('>I',b,o)[0]
def packets(b):
    o=0
    while o+4<=len(b):
        h=be(b,o); t=h>>30
        if t==2: o+=4; continue
        if t==1: yield ('T1',h,b[o+4:o+12],o); o+=12; continue
        n=((h>>16)&0x3fff)+1
        yield (('T0' if t==0 else 'T3'),h,b[o+4:o+4+4*n],o); o+=4*(1+n)
KNOWN={0x10,0x21,0x22,0x23,0x25,0x26,0x27,0x2B,0x2C,0x2D,0x2E,0x2F,0x34,0x35,0x36,0x37,0x3B,0x3C,0x3D,0x3E,0x3F,0x44,0x45,0x46,0x48,0x4A,0x4B,0x4F,0x50,0x51,0x52,0x53,0x54,0x55,0x56,0x58,0x59,0x5A,0x5B,0x5C,0x5D,0x5E,0x60,0x61,0x62,0x63,0x64}
def valid(b):
    o=0
    while o+4<=len(b):
        h=be(b,o); t=h>>30
        if t==2: o+=4; continue
        if t==1: o+=12; continue
        n=((h>>16)&0x3fff)+1
        if t==0:
            if (h&0x7fff)+(1 if h>>15&1 else n)>0x6000: return False
        elif ((h>>8)&0x7f) not in KNOWN: return False
        if o+4*(1+n)>len(b): return False
        o+=4*(1+n)
    return o==len(b)
def shifted_spans(bl):
    """The stream spans (blob 10) in order, as packets. Captures before fix36 recorded
    [last, cur) - one word early (the cursor points at the last word written) - and are
    realigned here; fix36+ captures record [last+4, cur+4) and are used as they are."""
    sp=sorted((key,b) for (k,key),b in bl.items() if k==10)
    exact=sum(valid(b) for k,b in sp)
    if exact*2 >= len(sp):
        return [(k,b) for k,b in sp if valid(b)]
    out=[]
    for i in range(len(sp)-1):
        b=sp[i][1][4:]+sp[i+1][1][:4]
        if valid(b): out.append((sp[i][0],b))
    return out
def gpu_states(sp, mem_blobs=None):
    regs={}
    snaps=[]
    for k,b in sp:
        for typ,h,d,o in packets(b):
            if typ=='T0':
                reg=h&0x7fff; n=len(d)//4
                for i in range(n): regs[reg if h>>15&1 else reg+i]=be(d,4*i)
            elif typ=='T1':
                regs[h&0x7ff]=be(d,0); regs[(h>>11)&0x7ff]=be(d,4)
            else:
                op=(h>>8)&0x7f
                if op==0x2d:
                    off=be(d,0); kind=(off>>16)&0xff; base=[0x4000,0x4800,0x4900,0x4908,0x2000][kind] if kind<5 else None
                    if base is not None:
                        for i in range(len(d)//4-1): regs[base+(off&0x7ff)+i]=be(d,4+4*i)
                elif op in (0x22,0x36):
                    snaps.append((k,dict(regs),op,[be(d,4*i) for i in range(min(3,len(d)//4))]))
    return snaps
def align(snaps, draws):
    pairs=[]; j=0
    for i,e in enumerate(draws):
        want = e['regs'][4] if e['name']=='DrawIndexedVertices' else e['regs'][3] if e['name']=='DrawVertices' else None
        k=j
        while k<len(snaps) and k<j+8:
            ini=snaps[k][3][1] if len(snaps[k][3])>1 else 0
            if want is None or (ini>>16)==want: break
            k+=1
        if k<len(snaps) and k<j+8:
            pairs.append((i,k)); j=k+1
    return pairs

STATE_REGS = [0x2104,0x2105,0x2106,0x2107,0x2108,0x210C,0x210D,0x210E,0x2200,0x2201,0x2202,0x2205,
              0x2209,0x220A,0x220B,0x2380,0x2381,0x2382,0x2383]   # kStateRegs order
def blob9(b):
    """A draw's register-mirror blob (kind 9). fix36+: also what the draw finally used."""
    n = len(STATE_REGS); o = 0
    r = {'mirror_fetch': [be(b, 4*i) for i in range(192)]}; o = 192*4
    r['mirror_state'] = {STATE_REGS[i]: be(b, o+4*i) for i in range(n)}; o += 4*n
    r['imload_vs'], r['imload_ps'] = be(b, o), be(b, o+4); o += 8
    if len(b) > o:
        r['used_fetch'] = [be(b, o+4*i) for i in range(96)]; o += 96*4
        r['used_state'] = {STATE_REGS[i]: be(b, o+4*i) for i in range(n)}; o += 4*n
        r['used_texobj'] = [be(b, o+4*i) for i in range(16)]
    return r
