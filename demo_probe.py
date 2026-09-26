#!/usr/bin/env python3
# Probe: locate where demo chunk stream starts after embedded map data.
import struct, sys
import demo_analyze as da

PATH = r'C:\Users\tailznic\AppData\Roaming\DDNet\demos\fngTraining_2026-09-23_06-00-23.demo'

b = open(PATH, 'rb').read()
ms = struct.unpack('>I', b[136:140])[0]
print('file len', len(b), 'mapsize', ms)
print('bytes at 436 (expect map magic DATA):', b[436:440])
cand = [176 + ms, 176 + 260 + ms, 436 + ms]
# scan for plausible full tick marker: 0x80/0xC0, BE tick small, then valid chunk hdr
hits = []
for p in range(400, len(b) - 16):
    if b[p] in (0x80, 0xC0):
        tick = struct.unpack('>I', b[p+1:p+5])[0]
        if 0 <= tick <= 5_000_000:
            nxt = b[p+5]
            if (nxt & 0x60) in (0x20, 0x40, 0x60) and (nxt & 0x80) == 0:
                hits.append((p, hex(b[p]), tick, hex(nxt)))
print('marker-like hits (first 15):')
for h in hits[:15]:
    print('  ', h)
# also check candidates: try full parse from each candidate offset
for off in sorted(set(cand + [h[0] for h in hits[:5]])):
    if off >= len(b):
        continue
    try:
        m = da.parse_demo_from(PATH, off)
        print('PARSE OK from', off, 'snaps', len(m['snaps']), 'msgs', len(m['messages']),
              'ticks', m['snaps'][0][0] if m['snaps'] else None, '..',
              m['snaps'][-1][0] if m['snaps'] else None)
    except Exception as e:
        print('parse fail from', off, ':', type(e).__name__, e)