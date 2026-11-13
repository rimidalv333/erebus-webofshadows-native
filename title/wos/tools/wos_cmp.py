# fix52: emulated vs native capture of the same spot. Each draw's vertex stream 0 address comes
# from the device snapshot's vertex fetch constant (slot 95 = stream 0: window 0x480 + ...; read
# via the D3D fetch block at device kFetchConstants). Prints the vertex buffers the emulated
# frame draws but the native frame does not (key: buffer + prim/base/start/count), with their call stacks (blob 11, by draw order).
import sys,collections,re
__file__='/home/az/ps5/PS5_WebOfShadowsTitle/wos/tools/wos_stream.py'
exec(open(__file__).read())
h=open('/home/az/ps5/PS5_WebOfShadowsTitle/wos/eot/src/gpu/d3d.h').read()
FETCH=int(re.search(r'kFetchConstants\s*=\s*(0x[0-9A-Fa-f]+)',h).group(1),16)
def stream0(d):
    # vertex fetch constant for stream 0 = fetch dword pair index 95 (3 per slot: slot 31, 3rd)
    o=FETCH+31*24+16
    return be(d,o)&0xFFFFFFFC
def draws(path, emulated):
    ev,bl=read(path)
    st={key:[be(b,4*i) for i in range(len(b)//4)] for (k,key),b in bl.items() if k==11}
    keys=sorted(st) if emulated else sorted(key for (k,key) in bl if k==9)
    out=[];n=0
    for e in ev:
        if e['name'] in ('DrawIndexedVertices','DrawVertices'):
            d=e['dev']; k=keys[n] if n<len(keys) else None; n+=1
            if d is None: continue
            out.append(((stream0(d),)+tuple(e['regs'][1:5]),be(d,0x3198),be(d,0x3194),tuple(st.get(k,())[:9])))
    return out
em=draws(sys.argv[1],True); na=draws(sys.argv[2],False)
print('emulated draws',len(em),'native draws',len(na))
nv=set(x[0] for x in na)
miss=collections.Counter(); ex={}
for x in em:
    if x[0] not in nv:
        k=(x[3]); miss[k]+=1; ex[k]=x
print('emulated draws whose stream-0 buffer the native frame never draws:',sum(miss.values()))
for k,v in miss.most_common(20): print(v,'vs %x ps %x'%(ex[k][1],ex[k][2]),' '.join('%08X'%w for w in k))
