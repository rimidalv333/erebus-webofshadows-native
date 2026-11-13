import struct,sys,xxhash,glob,os
sys.path.insert(0,'/home/az/ps5/handoff-notes/tools'); import cachelib
s,ents,d=cachelib.load_cache('/home/az/ps5/wos_shader_cache/wos_shader_cache.cpp')
ch=set(e[0] for e in ents)
files=glob.glob('*/wos-shader-harvest-v2.bin')+glob.glob('*/shader-misses.bin')+glob.glob('work/allin/*.bin')+['/mnt/user-data/uploads/wos-shader-harvest-v2.bin']
got={}
for f in files:
    data=open(f,'rb').read(); i=0
    while i+16<=len(data):
        fl,vs,ps=struct.unpack_from('>III',data,i)
        if (fl&0xFFFFFF00)==0x102A1100 and vs>=16 and vs+ps<=len(data)-i:
            c=bytearray(data[i:i+vs+ps]); c[0x1C:0x24]=b'\0'*8; h=xxhash.xxh3_64_intdigest(bytes(c))
            if (fl&0xFF)==0 and h in ch and h not in got: got[h]=bytes(c)
            i+=vs+ps
        else: i+=4
for h,c in got.items(): open('c100/in/%016x.bin'%h,'wb').write(c)
nps=sum(1 for e in ents if e[0] in got)
print('cache entries',len(ents),'PS containers found in cache',len(got))
