#!/usr/bin/env python3
"""fix93: find the view-list element of a draw (by vertex buffer) and dump it.
listtrace.py CAPTURE VB_HEX"""
import sys, struct
src = open('/home/az/ps5/PS5_WebOfShadowsTitle/wos/tools/wos_capture.py').read().replace('\nmain()\n', '\n')
g = {}; exec(src, g); be = g['be32']
ev, bl = g['read'](sys.argv[1]); want = int(sys.argv[2], 16)
draws = [e for e in ev if e['dev'] is not None and e['name'].startswith('Draw')]
k14 = sorted(k for k in bl if k[0] == 14)
objs = set()
for i, e in enumerate(draws):
    if be(e['dev'], 0x480 + 95 * 8) & ~3 == want and i < len(k14):
        obj, view = struct.unpack('>II', bl[k14[i]]); objs.add(obj)
        print('draw #%d obj %08x view %08x' % (i + 1, obj, view))
lists = sorted(k for k in bl if k[0] == 16)
print(len(lists), 'lists drawn')
for k in lists:
    b = bl[k]; view, n = struct.unpack_from('>II', b, 0)
    for j in range(n):
        o = 8 + 100 * j; e = struct.unpack_from('>I', b, o)[0]; body = b[o + 4:o + 100]
        if struct.unpack_from('>I', body, 68)[0] in objs:
            print('list %d (view %08x, %d elements) element %d @%08x:' % (k[1], view, n, j, e))
            print('   ' + ' '.join('%08x' % w for w in struct.unpack('>24I', body)))
