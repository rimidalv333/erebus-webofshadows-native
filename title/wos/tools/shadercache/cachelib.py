import re, zstandard
def load_cache(path):
    s=open(path).read()
    ents=[]
    for m in re.finditer(r'\{ (0x[0-9A-Fa-f]+), ([^}]*)\}', s.split('g_shaderCacheEntries[] = {')[1].split('};')[0]):
        vals=[int(x.strip(),0) for x in m.group(2).split(',')]
        ents.append([int(m.group(1),16)]+vals)
    blob_txt=s.split('g_compressedSpirvCache[] = {')[1].split('};')[0]
    comp=bytes(int(x) for x in blob_txt.split(',') if x.strip())
    dsize=int(re.search(r'g_spirvCacheDecompressedSize = (\d+)',s).group(1))
    data=zstandard.ZstdDecompressor().decompress(comp, max_output_size=dsize)
    return s,ents,data
F=['hash','dxilOff','dxilSize','spirvOff','spirvSize','spec','vlOff','vlCount','vfetch','usesF','interp','fullDxilOff','fullDxilSize','fullSpirvOff','fullSpirvSize','posDxilOff','posDxilSize','posSpirvOff','posSpirvSize','velDxilOff','velDxilSize','velSpirvOff','velSpirvSize']
