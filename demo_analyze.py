#!/usr/bin/env python3
# Teeworlds 0.6 .demo parser + movement analysis (SNH/fng_trainbot study)
import struct, math, sys
from collections import defaultdict

# --- Huffman (exact port of src/engine/shared/huffman.cpp) -----------------
FREQ = [
1<<30,4545,2657,431,1950,919,444,482,2244,617,838,542,715,1814,304,240,754,212,647,186,
283,131,146,166,543,164,167,136,179,859,363,113,157,154,204,108,137,180,202,176,
872,404,168,134,151,111,113,109,120,126,129,100,41,20,16,22,18,18,17,19,
16,37,13,21,362,166,99,78,95,88,81,70,83,284,91,187,77,68,52,68,
59,66,61,638,71,157,50,46,69,43,11,24,13,19,10,12,12,20,14,9,
20,20,10,10,15,15,12,12,7,19,15,14,13,18,35,19,17,14,8,5,
15,17,9,15,14,18,8,10,2173,134,157,68,188,60,170,60,194,62,175,71,
148,67,167,78,211,67,156,69,1674,90,174,53,147,89,181,51,174,63,163,80,
167,94,128,122,223,153,218,77,200,110,190,73,174,69,145,66,277,143,141,60,
136,53,180,57,142,57,158,61,166,112,152,92,26,22,21,28,20,26,30,21,
32,27,20,17,23,21,30,22,22,21,27,25,17,27,23,18,39,26,15,21,
12,18,18,27,20,18,15,19,11,17,33,12,18,15,19,18,16,26,17,18,
9,10,25,22,22,17,20,16,6,16,15,20,14,18,24,335,1517]
EOF_SYM = 256

def build_tree():
    freq = list(FREQ); freq[EOF_SYM] = 1
    nsym = 257
    leaf0 = [0xffff]*514; leaf1 = [0xffff]*514; numbits = [0]*514; symbol = list(range(514))
    for i in range(nsym):
        numbits[i] = 0xFFFFFFFF  # marker: symbol node before codes assigned
    left_f = freq[:]; left_id = list(range(nsym))
    nleft = nsym; num_nodes = nsym
    while nleft > 1:
        # BubbleSort descending (exact port: strict <, one pass shrinks Size)
        changed = 1
        size = nleft
        while changed:
            changed = 0
            for i in range(size - 1):
                if left_f[i] < left_f[i+1]:
                    left_f[i], left_f[i+1] = left_f[i+1], left_f[i]
                    left_id[i], left_id[i+1] = left_id[i+1], left_id[i]
                    changed = 1
            size -= 1
        leaf0[num_nodes] = left_id[nleft-1]   # smallest -> child0
        leaf1[num_nodes] = left_id[nleft-2]   # 2nd smallest -> child1
        left_id[nleft-2] = num_nodes
        left_f[nleft-2] = left_f[nleft-1] + left_f[nleft-2]
        num_nodes += 1; nleft -= 1
    root = num_nodes - 1
    # assign code depths (only needed to mark symbol leaves: all symbol nodes
    # keep numbits=0xFFFFFFFF -> treated as leaf in decode via !=0 check)
    return leaf0, leaf1, numbits, symbol, root, num_nodes

L0, L1, NB, SYM, ROOT, NN = build_tree()

def huff_decompress(data, limit = 8*1024*1024):
    out = bytearray(); bits = 0; bc = 0; i = 0; n = len(data)
    while True:
        while bc < 24 and i < n:
            bits |= data[i] << bc; bc += 8; i += 1
        node = ROOT
        while NB[node] == 0:            # walk tree bit by bit (LSB first)
            if bc == 0: raise ValueError("huffman: out of bits")
            b = bits & 1; bits >>= 1; bc -= 1
            node = L0[node] if b == 0 else L1[node]
            if node == 0xffff: raise ValueError("huffman: bad node")
        if node == EOF_SYM: break
        out.append(node & 0xff)
        if len(out) > limit: raise ValueError("huffman: runaway")
    return bytes(out)

# --- variable int (exact port of compression.cpp Unpack) -------------------
def vi_unpack(b, i):
    first = b[i]; sign = (first >> 6) & 1; v = first & 0x3F
    while True:
        if not (first & 0x80): break
        i += 1; first = b[i]; v |= (first & 0x7F) << 6
        if not (first & 0x80): break
        i += 1; first = b[i]; v |= (first & 0x7F) << 13
        if not (first & 0x80): break
        i += 1; first = b[i]; v |= (first & 0x7F) << 20
        if not (first & 0x80): break
        i += 1; first = b[i]; v |= (first & 0x7F) << 27
        break
    i += 1
    v &= 0xFFFFFFFF
    if sign: v ^= 0xFFFFFFFF
    if v >= 0x80000000: v -= 0x100000000
    return v, i

def vi_decompress(b):
    out = bytearray(); i = 0; n = len(b)
    while i < n:
        v, i = vi_unpack(b, i)
        out += struct.pack('<i', v)
    return bytes(out)

def s32(x):
    x &= 0xFFFFFFFF
    return x - 0x100000000 if x >= 0x80000000 else x

# --- demo container --------------------------------------------------------
CDemoHeader = 176; CTimeline = 4 + 64*4   # header + timeline markers struct
CHUNK_TICK = 0x80; CHUNK_KEY = 0x40
CHUNK_SNAPSHOT = 1; CHUNK_MESSAGE = 2; CHUNK_DELTA = 3

# static item sizes (bytes) for delta unpacking — mirrors ms_aObjSizes
STATIC = {1:40, 2:24, 3:20, 4:16, 5:12, 6:32, 7:16, 8:60, 9:88,
          10:20, 11:68, 12:12, 13:8, 14:8, 15:8, 16:8, 17:12, 18:12, 19:12, 20:12}

def parse_demo(path):
    raw = open(path, 'rb').read()
    if raw[:7] != b'TWDEMO\x00': raise ValueError('not a TWDEMO file')
    ver = raw[7]
    netver = raw[8:72].split(b'\x00')[0].decode('latin1')
    mapname = raw[72:136].split(b'\x00')[0].decode('latin1')
    mapsize = struct.unpack('>I', raw[136:140])[0]
    gtype = raw[144:152].split(b'\x00')[0].decode('latin1')
    length = struct.unpack('>I', raw[152:156])[0]
    timestamp = raw[156:176].split(b'\x00')[0].decode('latin1')
    off = raw.find(b'DATA', CDemoHeader, CDemoHeader + 512)  # v6 header is longer
    if off < 0: raise ValueError('embedded map not found')
    embedded = raw[off:off + mapsize]
    off += mapsize                            # skip embedded map data
    out = parse_demo_from(path, off, ver)
    out['mapbytes'] = embedded
    out.update(ver=ver, netver=netver, mapname=mapname, mapsize=mapsize,
               gtype=gtype, length=length, timestamp=timestamp)
    return out

def parse_demo_from(path, off, ver=6):
    raw = open(path, 'rb').read()
    last = None           # last full snapshot: dict key->tuple
    tick = -1
    snaps = []            # (tick, state_dict)
    messages = []         # (tick, bytes)
    while off < len(raw):
        b = raw[off]; off += 1
        if b & CHUNK_TICK:                              # tick marker
            # ver 6 (DDNet/bestclient recorder) sets bit 0x20 on EVERY
            # relative marker — the delta lives in the low 5 bits only
            # (verified: 250 relative markers exactly bridge the
            # SERVER_TICK_SPEED*5 gap between two keyframes). vanilla
            # ver<=4 encodes the delta in all 6 low bits.
            d = b & (0x1F if ver >= 6 else 0x3F)
            if d == 0:
                tick = struct.unpack('>i', raw[off:off+4])[0]; off += 4
            else:
                tick += d
            continue
        ctype = (b & 0x60) >> 5
        csize = b & 0x1F
        if csize == 30: csize = raw[off]; off += 1
        elif csize == 31: csize = struct.unpack('<H', raw[off:off+2])[0]; off += 2
        payload = raw[off:off+csize]; off += csize
        data = vi_decompress(huff_decompress(payload))
        if ctype == CHUNK_SNAPSHOT:
            last = snapshot_items(data); snaps.append((tick, dict(last)))
        elif ctype == CHUNK_DELTA:
            if last is None: raise ValueError('delta before keyframe @%d' % tick)
            last = apply_delta(last, data); snaps.append((tick, dict(last)))
        elif ctype == CHUNK_MESSAGE:
            messages.append((tick, data))
    return dict(snaps=snaps, messages=messages)

def snapshot_items(buf):
    data_size, num = struct.unpack_from('<ii', buf, 0)
    offs = struct.unpack_from('<%di' % num, buf, 8)
    base = 8 + 4*num
    items = {}
    for k in range(num):
        st = base + offs[k]
        en = base + (offs[k+1] if k+1 < num else data_size)
        tid = struct.unpack_from('<i', buf, st)[0]
        nints = (en - st - 4) // 4
        items[(tid >> 16, tid & 0xFFFF)] = struct.unpack_from('<%di' % nints, buf, st+4)
    return items

def apply_delta(state, buf):
    ints = struct.unpack('<%di' % (len(buf)//4), buf)
    ndel, nupd = ints[0], ints[1]
    p = 3
    for k in range(ndel):
        key = ints[p] & 0xFFFFFFFF; p += 1
        state.pop(((key >> 16) & 0xFFFF, key & 0xFFFF), None)
    for _ in range(nupd):
        if p + 2 > len(ints): raise ValueError('truncated delta')
        t = ints[p]; p += 1
        if t < 0: raise ValueError('bad type')
        i = ints[p]; p += 1
        sz = STATIC.get(t & 0xFFFF, 0)
        if not sz: sz = ints[p] * 4; p += 1
        n = sz // 4
        data = ints[p:p+n]; p += n
        key = (t, i)
        old = state.get(key)
        if old is not None:
            state[key] = tuple(s32(a + b) for a, b in zip(old, data))
        else:
            state[key] = tuple(data)
    return state

# --- message helpers (fixed ids from protocol enum) ------------------------
def vi_list(b):
    out = []; i = 0
    while i < len(b):
        v, i = vi_unpack(b, i); out.append(v)
    return out

def parse_msgs(messages):
    """returns (chats, kills); tolerant per-message parsing"""
    chats = []; kills = []
    for tick, data in messages:
        try:
            i0 = 0; mid, i1 = vi_unpack(data, 0)
            if mid == 3:        # Sv_Chat: team, cid, string
                team, i2 = vi_unpack(data, i1)
                cid, i3 = vi_unpack(data, i2)
                end = data.find(b'\x00', i3)
                if end < 0: continue
                msg = data[i3:end].decode('utf-8', 'replace')
                chats.append((tick, cid, team, msg))
            elif mid == 8:      # Sv_KillMsg (rec client enum): killer, victim, weapon, mode
                killer, i2 = vi_unpack(data, i1)
                victim, i3 = vi_unpack(data, i2)
                wep, i4 = vi_unpack(data, i3)
                kills.append((tick, killer, victim, wep))
        except (IndexError, ValueError):
            continue
    return chats, kills

import zlib

# --- teeworlds datafile (.map) parser ---------------------------------------
def parse_datafile(b):
    if b[:4] != b'DATA': raise ValueError('not a datafile')
    ver, size, swaplen, ntypes, nitems, nraw, itemsize, datasize = struct.unpack_from('<8i', b, 4)
    off = 36 + 12 * ntypes
    item_offs = struct.unpack_from('<%di' % nitems, b, off); off += 4 * nitems
    data_offs = struct.unpack_from('<%di' % nraw, b, off); off += 4 * nraw
    if ver == 4:
        off += 4 * nraw
    item_start = off; data_start = off + itemsize
    items = []
    for i in range(nitems):
        base = item_start + item_offs[i]
        typeid, sz = struct.unpack_from('<2i', b, base)
        items.append(((typeid >> 16) & 0xFFFF, typeid & 0xFFFF,
                      b[base + 8:base + 8 + sz]))
    datas = []
    for i in range(nraw):
        st = data_start + data_offs[i]
        en = data_start + (data_offs[i + 1] if i + 1 < nraw else datasize)
        chunk = b[st:en]
        if ver == 4:
            chunk = zlib.decompress(chunk)
        datas.append(chunk)
    return items, datas

def load_game_grid(b):
    items, datas = parse_datafile(b)
    for tid, iid, payload in items:
        if tid != 5:                                     # MAPITEMTYPE_LAYER
            continue
        ints = struct.unpack_from('<%di' % (len(payload) // 4), payload)
        # Layer{Ver,Type,Flags} Tilemap{Ver,W,H,Flags,Color*4,CEnv,CEnvOff,Img,Data,...}
        if len(ints) >= 15 and ints[1] == 2 and (ints[6] & 1):   # TILES + GAME
            w, h, d = ints[4], ints[5], ints[14]
            raw = datas[d]
            return [[raw[(r * w + c) * 4] for c in range(w)] for r in range(h)], w, h
    raise ValueError('game layer not found')

SPIKE_TILE = {8: 'normal', 9: 'red', 10: 'blue', 7: 'gold', 14: 'green', 15: 'purple'}

def grid_stats(grid, w, h, label):
    cnt = {}
    for row in grid:
        for v in row:
            cnt[v] = cnt.get(v, 0) + 1
    spikes = {SPIKE_TILE[k]: v for k, v in cnt.items() if k in SPIKE_TILE}
    return ['[%s] %dx%d solid=%d death=%d entities=%d spikes=%s' % (
        label, w, h, cnt.get(1, 0) + cnt.get(3, 0), cnt.get(2, 0),
        sum(v for k, v in cnt.items() if k > 128), spikes or '-')], spikes

def spike_list(grid, w, h):
    return [(c, r, SPIKE_TILE[grid[r][c]])
            for r in range(h) for c in range(w) if grid[r][c] in SPIKE_TILE]

CTYPE = dict(laser=3, char=9, pinfo=10, cinfo=11, ev_death=17)
WNAME = {0: 'hammer', 1: 'gun', 2: 'shotgun', 3: 'grenade', 4: 'laser', 5: 'ninja',
         -1: 'world', -2: 'script', -3: 'spike'}
TEAM = {-1: 'spec', 0: 'red', 1: 'blue', 2: 'green', 3: 'purple'}
TICKS_PS = 50.0

def decode_name(info):
    p = b''.join((s32(x) & 0xFFFFFFFF).to_bytes(4, 'big') for x in info[:4])
    p = bytes(b ^ 0x80 for b in p)
    return p.split(b'\x00')[0].decode('utf-8', 'replace')

def tile_at(grid, w, h, wx, wy):
    tx, ty = int(wx) // 32, int(wy) // 32
    if not (0 <= tx < w and 0 <= ty < h):
        return None
    return grid[ty][tx]

def solid_at(grid, w, h, wx, wy):
    v = tile_at(grid, w, h, wx, wy)
    return v in (1, 3)

def pct(a, b):
    return 0.0 if not b else 100.0 * a / b

def analyze():
    out = []
    P = lambda *a: out.append(' '.join(str(x) for x in a))
    demo = parse_demo(DEMO)
    snaps, messages = demo['snaps'], demo['messages']
    P('=== DEMO ===')
    P('netver:', demo['netver'], ' map:', demo['mapname'], demo['mapsize'], 'B',
      ' type:', demo['gtype'], ' ver:', demo['ver'], ' ts:', demo['timestamp'])
    P('header length:', demo['length'], 's')
    t0, t1 = snaps[0][0], snaps[-1][0]
    P('ticks:', t0, '..', t1, '=', round((t1 - t0) / TICKS_PS, 1), 's  samples:',
      len(snaps), ' avg dt =', round((t1 - t0) / max(1, len(snaps) - 1), 1), 'ticks')

    grid, W, H = load_game_grid(demo['mapbytes'])
    for l in grid_stats(grid, W, H, 'demo map')[0]: P(l)
    spikes = spike_list(grid, W, H)
    try:
        g2, W2, H2 = load_game_grid(open(FNGMAP, 'rb').read())
        for l in grid_stats(g2, W2, H2, 'server fng.map')[0]: P(l)
    except Exception as e:
        P('server map parse failed:', e)

    names, teams, scores = {}, {}, {}
    for tick, st in snaps:
        for (t, i), v in st.items():
            if t == CTYPE['cinfo']: names[i] = decode_name(v)
            elif t == CTYPE['pinfo']: teams[i] = v[2]; scores[i] = v[3]
    P('')
    P('=== ROSTER (last seen) ===')
    for i in sorted(names):
        P(' cid=%-2d team=%-7s score=%-4d %s'
          % (i, TEAM.get(teams.get(i), '?'), scores.get(i, 0), names[i]))
    cid = next((i for i, n in names.items() if n == TARGET), None)
    if cid is None:
        P('TARGET not found!'); return '\n'.join(out)
    P('')
    P('=== %s (cid=%d) ===' % (TARGET, cid))

    samples = []
    for tick, st in snaps:
        ch = st.get((CTYPE['char'], cid))
        if ch is None:
            samples.append(None); continue
        samples.append(dict(tick=tick, x=ch[1], y=ch[2],
                            vx=ch[3] / 256.0, vy=ch[4] / 256.0,
                            dirn=ch[6], jump=ch[7], hp=ch[8], hs=ch[9],
                            hx=ch[11], hy=ch[12], health=ch[16],
                            wep=ch[19], atk=ch[21]))
    alive = [s for s in samples if s]
    P('samples with tee:', len(alive), '/', len(samples),
      '=', pct(len(alive), len(samples)).__round__(1), '% of demo time')
    if not alive:
        return '\n'.join(out)
    dticks = [b['tick'] - a['tick'] for a, b in zip(samples, samples[1:]) if a and b]
    med_dt = sorted(dticks)[len(dticks) // 2] if dticks else 16

    # --- movement ---
    dist = 0.0; speeds = []; still = 0; pairs = 0
    for a, b in zip(alive, alive[1:]):
        d = math.hypot(b['x'] - a['x'], b['y'] - a['y'])
        dist += d
        dt = b['tick'] - a['tick']
        if 0 < dt <= med_dt * 3:
            pairs += 1
            if a['x'] == b['x'] and a['y'] == b['y']: still += 1
            if d <= 100 * dt: speeds.append(d / dt)   # drop respawn teleports
    speeds.sort()
    dur = (alive[-1]['tick'] - alive[0]['tick']) / TICKS_PS
    P('')
    P('--- movement ---')
    P('alive window: %.1fs  path: %.0f px = %.0f tiles' % (dur, dist, dist / 32))
    P('ticks without self-movement: %.0f%% (frozen by enemy or standing)'
      % pct(still, pairs))
    vsp = sorted(math.hypot(s['vx'], s['vy']) for s in alive)
    P('speed from velocity field: avg=%.1f p50=%.1f p95=%.1f px/tick (= avg %.1f tiles/s)'
      % (sum(vsp) / len(vsp), vsp[len(vsp) // 2], vsp[int(len(vsp) * 0.95)],
         sum(vsp) / len(vsp) * TICKS_PS / 32))
    if speeds:
        P('speed from positions: avg=%.2f p50=%.2f p95=%.2f max=%.2f px/tick'
          % (sum(speeds) / len(speeds), speeds[len(speeds) // 2],
             speeds[int(len(speeds) * 0.95)], speeds[-1]))
    xs = [s['x'] / 32 for s in alive]; ys = [s['y'] / 32 for s in alive]
    P('x range: %.0f..%.0f (map %d)  y range: %.0f..%.0f (map %d, 0=top)'
      % (min(xs), max(xs), W, min(ys), max(ys), H))
    cols = [min(2, int(x / (W / 3.0))) for x in xs]
    rows = [min(2, int(y / (H / 3.0))) for y in ys]
    P('x thirds L/M/R: %s' % ' / '.join('%.0f%%' % pct(cols.count(k), len(cols)) for k in range(3)))
    P('y thirds T/M/B: %s' % ' / '.join('%.0f%%' % pct(rows.count(k), len(rows)) for k in range(3)))
    cross = sum(1 for a, b in zip(rows, rows[1:]) if a != b and 2 in (a, b) and 0 in (a, b))
    cross += sum(1 for a, b in zip(rows, rows[1:]) if (a == 1 and b != 1))
    P('vertical band changes: %d = %.1f/min'
      % (cross, 0 if dur == 0 else cross / (dur / 60)))
    air = sum(1 for s in alive if not solid_at(grid, W, H, s['x'], s['y'] + 15))
    P('airborne: %.0f%% of samples' % pct(air, len(alive)))

    flips = 0; runs = []; cur = 0; last_dir = 0
    for s in alive:
        d = s['dirn']
        if d != 0:
            if last_dir != 0 and d == -last_dir:
                flips += 1; runs.append(cur); cur = 0
            elif last_dir != 0 and d != last_dir:
                runs.append(cur); cur = 0
            cur += 1; last_dir = d
    runs.append(cur)
    P('direction reversals: %d = %.1f/s  avg hold %.0f ticks (%.2fs) longest %d'
      % (flips, flips / max(0.01, dur), sum(runs) / max(1, len(runs)),
         sum(runs) / max(1, len(runs)) / TICKS_PS, max(runs) if runs else 0))
    gj = aj = 0; pj = alive[0]['jump'] & 3
    for s in alive[1:]:
        j = s['jump'] & 3
        if not (pj & 1) and (j & 1): gj += 1
        if not (pj & 2) and (j & 2): aj += 1
        pj = j
    P('jumps: ground=%d air=%d (%.1f+%.1f per min)'
      % (gj, aj, gj / max(0.01, dur / 60), aj / max(0.01, dur / 60)))

    # --- hook ---
    P('')
    P('--- hook ---')
    jump_ticks = []
    pj = alive[0]['jump'] & 3
    for s in alive[1:]:
        j = s['jump'] & 3
        if (j & 1) and not (pj & 1): jump_ticks.append(s['tick'])
        if (j & 2) and not (pj & 2): jump_ticks.append(s['tick'])
        pj = j
    launches = grabs_pl = grabs_gr = hits_wall = 0
    hook_down = []; boosts = []; grab_ranges = []
    for idx in range(1, len(alive)):
        s = alive[idx]; p = alive[idx - 1]
        if s['hs'] == 4 and p['hs'] != 4:                 # -> FLYING
            launches += 1
            if s['hy'] > s['y'] + 12 and abs(s['hx'] - s['x']) < 64:
                near = min((abs(s['tick'] - jt) for jt in jump_ticks), default=999)
                hook_down.append(near <= max(med_dt * 3, 10))
        if s['hs'] == 5 and p['hs'] != 5:                 # -> GRABBED
            hp = s['hp']
            if 0 <= hp < 64 and hp != cid: grabs_pl += 1
            else: grabs_gr += 1
            grab_ranges.append(math.hypot(s['hx'] - s['x'], s['hy'] - s['y']))
            v0 = math.hypot(s['vx'], s['vy']); vmax = v0
            for k in range(idx, min(idx + 6, len(alive))):
                vmax = max(vmax, math.hypot(alive[k]['vx'], alive[k]['vy']))
            boosts.append(vmax - v0)
        if s['hs'] == 1 and p['hs'] == 4: hits_wall += 1  # FLYING -> RETRACT
    occ = {}
    for s in alive: occ[s['hs']] = occ.get(s['hs'], 0) + 1
    P('launches=%d  grabs: player=%d ground/wall=%d  hit-nohook=%d'
      % (launches, grabs_pl, grabs_gr, hits_wall))
    P('hook state time: ' + ', '.join(
        '%s=%.0f%%' % ({-1: 'retracted', 0: 'idle', 1: 'retract', 4: 'flying',
                        5: 'grabbed'}.get(k, str(k)), pct(v, len(alive)))
        for k, v in sorted(occ.items())))
    if grab_ranges:
        grab_ranges.sort()
        P('grab range px: avg=%.0f p50=%.0f max=%.0f (= %.1f tiles)'
          % (sum(grab_ranges) / len(grab_ranges), grab_ranges[len(grab_ranges) // 2],
             grab_ranges[-1], sum(grab_ranges) / len(grab_ranges) / 32))
    hdj = sum(1 for ok in hook_down if ok)
    P('hooks aimed straight DOWN: %d (%.0f%% of launches); with jump within +-%d'
      ' ticks: %d = the human hook-down+jump dodge'
      % (len(hook_down), pct(len(hook_down), launches), max(med_dt * 3, 10), hdj))
    if boosts:
        boosts.sort()
        P('speed gain during grab: avg=%.2f p50=%.2f p95=%.2f max=%.2f px/tick'
          % (sum(boosts) / len(boosts), boosts[len(boosts) // 2],
             boosts[int(len(boosts) * 0.95)], boosts[-1]))

    # --- shots ---
    shots = [s for a, s in zip(alive, alive[1:]) if s['atk'] > a['atk'] + 1]
    P('')
    P('--- shooting ---')
    P('shots: %d = %.1f/min  weapons used: %s'
      % (len(shots), len(shots) / max(0.01, dur / 60),
         sorted({WNAME.get(s['wep'], str(s['wep'])) for s in alive})))
    if shots:
        idx_map = {t: i for i, (t, _) in enumerate(snaps)}
        dists = []
        for s in shots:
            st = snaps[idx_map[s['tick']]][1]
            best = 1e9
            for (t, i), v in st.items():
                if t == CTYPE['char'] and i != cid:
                    best = min(best, math.hypot(v[1] - s['x'], v[2] - s['y']))
            if best < 1e9: dists.append(best / 32)
        if dists:
            dists.sort()
            P('nearest enemy at shot: avg=%.1f p50=%.1f tiles'
              % (sum(dists) / len(dists), dists[len(dists) // 2]))

    # --- frags ---
    P('')
    P('--- frags ---')
    deaths = sum(1 for tick, st in snaps for (t, i), v in st.items()
                 if t == CTYPE['ev_death'] and v[2] == cid)
    chats, kills = parse_msgs(messages)
    mine = [k for k in kills if k[1] == cid]   # killer == me
    igot = [k for k in kills if k[2] == cid]   # victim == me
    P('kill msgs: as killer=%d as victim=%d  DEATH events=%d'
      % (len(mine), len(igot), deaths))
    for tick, k, v, wp in kills:
        P('  t+%6.1fs %-14s -> %-14s wep=%s(%d)'
          % ((tick - t0) / TICKS_PS, names.get(k, str(k)), names.get(v, str(v)),
             WNAME.get(wp, '?'), wp))

    # --- spike proximity / spike deaths ---
    P('')
    P('--- kill tiles / spikes ---')
    near = {c: 0 for _, _, c in spikes}
    mind = []
    for s in alive:
        best = 1e9; bestc = None
        for (sx, sy, col) in spikes:
            d = max(abs(sx + 0.5 - s['x'] / 32), abs(sy + 0.5 - s['y'] / 32))
            if d < best: best = d; bestc = col
        mind.append(best)
        if best <= 1.5: near[bestc] = near.get(bestc, 0) + 1
    P('within 1.5 tiles of a spike: %.0f%% of samples; nearest colour share:'
      % pct(sum(near.values()), len(alive)))
    for c, n in sorted(near.items(), key=lambda kv: -kv[1]):
        P('   %-7s %5.1f%%' % (c, pct(n, len(alive))))
    mind.sort()
    P('distance to nearest spike: p10=%.1f p50=%.1f tiles'
      % (mind[len(mind) // 10], mind[len(mind) // 2]))
    sd = dd = od = 0
    for tick, st in snaps:
        for (t, i), v in st.items():
            if t == CTYPE['ev_death'] and v[2] == cid:
                tv = tile_at(grid, W, H, v[0], v[1])
                if tv in SPIKE_TILE: sd += 1
                elif tv == 2: dd += 1
                else: od += 1
    P('death events on: spike tile=%d death tile=%d other=%d' % (sd, dd, od))

    # --- heat map ---
    P('')
    P('--- occupancy heat map (6x6, %% of samples, row 0=top) ---')
    hm = [[0] * 6 for _ in range(6)]
    for s in alive:
        r = min(5, int(s['y'] / 32 / H * 6)); c = min(5, int(s['x'] / 32 / W * 6))
        hm[r][c] += 1
    for r in range(6):
        P('  ' + ' '.join('%4.0f' % pct(hm[r][c], len(alive)) for c in range(6)))

    # --- comparison ---
    P('')
    P('--- all players: speed & reversal comparison ---')
    for i in sorted(names):
        ser = []
        for tick, st in snaps:
            ch = st.get((CTYPE['char'], i))
            if ch: ser.append(dict(tick=tick, x=ch[1], y=ch[2], dirn=ch[6]))
        if len(ser) < 10: continue
        sp = []
        for a, b in zip(ser, ser[1:]):
            dt = b['tick'] - a['tick']
            if 0 < dt <= med_dt * 3:
                sp.append(math.hypot(b['x'] - a['x'], b['y'] - a['y']) / dt)
        fl = 0; ld = 0
        for s in ser:
            if s['dirn'] != 0:
                if ld != 0 and s['dirn'] == -ld: fl += 1
                ld = s['dirn']
        d2 = (ser[-1]['tick'] - ser[0]['tick']) / TICKS_PS
        sp.sort()
        P('  %-16s samples=%-4d avg=%.2f p95=%.2f reversals=%.1f/s'
          % (names[i], len(ser), sum(sp) / max(1, len(sp)),
             sp[int(len(sp) * 0.95)] if sp else 0, fl / max(0.01, d2)))

    P('')
    P('--- chat ---')
    for tick, c, tm, msg in chats[:30]:
        P('  t+%6.1fs %-14s %s' % ((tick - t0) / TICKS_PS, names.get(c, str(c)), msg))
    return '\n'.join(out)

import os
DEMO = os.environ.get('DEMO',
    r'C:\Users\tailznic\AppData\Roaming\DDNet\demos\fngTraining_2026-09-23_06-00-23.demo')
FNGMAP = os.environ.get('FNGMAP',
    r'C:\Users\tailznic\Documents\fng-0.6.5-win64\fng.map')
TARGET = os.environ.get('TARGET', 'BANANA6789')

if __name__ == '__main__':
    text = analyze()
    open('demo_report.txt', 'w', encoding='utf-8').write(text)
    import sys
    try:
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    except Exception:
        pass
    print(text)