#!/usr/bin/env python3
"""fix92: trace draws of one vertex buffer to their render-item pushes.
itemtrace.py CAPTURE VB_HEX   (vertex buffer guest address, e.g. 16184fe0)"""
import sys, struct, collections
src = open('/home/az/ps5/PS5_WebOfShadowsTitle/wos/tools/wos_capture.py').read().replace('\nmain()\n', '\n')
g = {}; exec(src, g); be = g['be32']
ev, bl = g['read'](sys.argv[1]); want = int(sys.argv[2], 16)
draws = [e for e in ev if e['dev'] is not None and e['name'].startswith('Draw')]
k14 = sorted(k for k in bl if k[0] == 14); k11 = sorted(k for k in bl if k[0] == 11)
pushes = {}
for k in sorted(k for k in bl if k[0] == 15):
    b = bl[k]; item, view, obj = struct.unpack_from('>III', b, 0)
    st = bl.get((12, k[1]), b'')
    rec = (k[1], item, view, struct.unpack('>%dI' % (len(st) // 4), st)[1:] if st else ())
    pushes.setdefault(obj, []).append(rec)
    if item != obj: pushes.setdefault(item, []).append(rec)
print(len(draws), 'draws', len(k14), 'item notes', sum(len(v) for v in pushes.values()), 'pushes')
for i, e in enumerate(draws):
    d = e['dev']
    if be(d, 0x480 + 95 * 8) & ~3 != want: continue
    obj, view = struct.unpack('>II', bl[k14[i]]) if i < len(k14) else (0, 0)
    s = g['state'](d)
    print('draw #%d %s cnt %d rt %x ps %x | item obj %08x view %08x' % (i + 1, e['name'], e['regs'][4], s['rt0'], s['ps'], obj, view))
    for key, item, pv, stack in pushes.get(obj, []):
        print('   push #%d item %08x into view %08x via %s' % (key, item, pv, ' '.join('%08x' % x for x in stack[:10])))
views = collections.Counter(v for lst in pushes.values() for _, _, v, _ in lst)
print('pushes per view:', ', '.join('%08x:%d' % kv for kv in views.most_common(12)))
