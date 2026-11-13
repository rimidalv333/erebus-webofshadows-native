import sys,re,zstandard
sys.path.insert(0,'/home/az/ps5/handoff-notes/tools'); import cachelib
F=cachelib.F
base,new,outp=sys.argv[1:4]
s,a,da=cachelib.load_cache(base); _,b,db=cachelib.load_cache(new)
data=bytearray(da); A={e[0]:list(e) for e in a}
def vls(t):
    b=t.split('g_shaderVertexLayouts[] = {',1)[1].split('};',1)[0]
    return [int(x,0) for x in b.split(',') if x.strip()]
VL=vls(s); VLB=vls(open(new).read())
rep=add=0
b=[e for e in b if e[0]!=0x30F8E6E0ECDE3503]
for e in b:
    m=dict(zip(F,e))
    if m['dxilSize'] or m['fullDxilSize'] or m['posDxilSize'] or m['velDxilSize']: raise SystemExit('dxil?')
    def put(k):
        off=len(data); data.extend(db[m[k+'Off']:m[k+'Off']+m[k+'Size']]); return off
    o=A.get(m['hash'])
    if o is None:
        o=list(e); add+=1
        if m['vlCount']:
            om0=dict(zip(F,o)); om0['vlOff']=len(VL); VL.extend(VLB[m['vlOff']:m['vlOff']+2*m['vlCount']]); o=[om0[f] for f in F]
    else:
        rep+=1
        oo=dict(zip(F,o))
        if m['vlCount'] and VLB[m['vlOff']:m['vlOff']+2*m['vlCount']]!=VL[oo['vlOff']:oo['vlOff']+2*oo['vlCount']]:
            print('layout fixed for %016x (old count %d at %d)'%(m['hash'],oo['vlCount'],oo['vlOff']))
            oo['vlOff']=len(VL); oo['vlCount']=m['vlCount']; VL.extend(VLB[m['vlOff']:m['vlOff']+2*m['vlCount']]); o=[oo[f] for f in F]
    om=dict(zip(F,o))
    for k in ('spirv','fullSpirv','posSpirv','velSpirv'):
        om[k+'Off']=put(k); om[k+'Size']=m[k+'Size']
        om[k.replace('Spirv','Dxil').replace('spirv','dxil')+'Off']=0 if k=='spirv' else 0
    for k in ('spec','vfetch','usesF','interp'): om[k]=m[k]
    A[m['hash']]=[om[f] for f in F]
ents=[A[h] for h in sorted(A)]
def fmt(e):
    v=[]
    for f,x in zip(F,e):
        v.append('0x%X'%x if f in ('hash','interp') else str(x))
    return '\t{ '+', '.join(v)+' },'
head,rest=s.split('ShaderCacheEntry g_shaderCacheEntries[] = {',1)
_,after=rest.split('};',1)
comp=zstandard.ZstdCompressor(level=19).compress(bytes(data))
after_vl,_=after.split('g_compressedSpirvCache[] = {',1)
pv=after_vl.split('g_shaderVertexLayouts[] = {',1)
pre_blob=pv[0]+'g_shaderVertexLayouts[] = {'+','.join('0x%X'%x for x in VL)+'};'+pv[1].split('};',1)[1]
tail=s.split('g_compressedSpirvCache[] = {',1)[1].split('};',1)[1]
tail=re.sub(r'g_spirvCacheCompressedSize = \d+',f'g_spirvCacheCompressedSize = {len(comp)}',tail)
tail=re.sub(r'g_spirvCacheDecompressedSize = \d+',f'g_spirvCacheDecompressedSize = {len(data)}',tail)
tail=re.sub(r'g_shaderCacheEntryCount = \d+',f'g_shaderCacheEntryCount = {len(ents)}',tail)
out=head+'ShaderCacheEntry g_shaderCacheEntries[] = {\n'+'\n'.join(fmt(e) for e in ents)+'\n};'+pre_blob+'g_compressedSpirvCache[] = {'+','.join(map(str,comp))+'};'+tail
open(outp,'w').write(out)
print('replaced',rep,'added',add,'entries',len(ents),'decompressed',len(data))
