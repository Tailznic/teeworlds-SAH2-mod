#!/usr/bin/env python3
"""fng.map floor + spike-cluster analysis (fng_trainbot navigation study).

Prints the walkable floors ("этажи") of a 0.6 map and the spike clusters, so
the server-side bot nav can be tuned against the real geometry.
"""
import struct, zlib, sys

SPIKE_NAMES = {
    2: 'death', 7: 'gold', 8: 'normal', 9: 'red', 10: 'blue',
    14: 'green', 15: 'purple',
}


def load_game_layer(path):
    with open(path, 'rb') as f:
        b = f.read()
    (magic, ver, sz, swap, num_types, num_items, num_raw, item_sz, data_sz) = \
        struct.unpack_from('<4s8i', b, 0)
    pos = 36 + num_types * 12
    item_offsets = list(struct.unpack_from(f'<{num_items}i', b, pos))
    pos += num_items * 4
    data_offsets = list(struct.unpack_from(f'<{num_raw}i', b, pos)); pos += num_raw * 4
    data_sizes = list(struct.unpack_from(f'<{num_raw}i', b, pos)); pos += num_raw * 4
    item_start, data_start = pos, pos + item_sz

    def get_data(idx):
        off = data_start + data_offsets[idx]
        return zlib.decompress(b[off:off + data_sizes[idx]])

    def get_item(idx):
        off = item_start + item_offsets[idx]
        tid_and_id, isz = struct.unpack_from('<2i', b, off)
        return tid_and_id >> 16, struct.unpack_from('<15i', b, off + 8)

    for i in range(num_items):
        tid, ints = get_item(i)
        if tid == 5 and ints[1] == 2 and (ints[6] & 1):
            return ints[4], ints[5], get_data(ints[14])
    raise SystemExit('no GAME layer')



def main(path):
    W, H, raw = load_game_layer(path)
    tile = lambda x, y: raw[(y * W + x) * 4] if 0 <= x < W and 0 <= y < H else 1

    solids, spikes = [], {}
    for y in range(H):
        for x in range(W):
            t = tile(x, y)
            if t == 1:
                solids.append((x, y))
            elif t in SPIKE_NAMES:
                spikes.setdefault(SPIKE_NAMES[t], []).append((x, y))

    print(f'map {path}\nGAME layer {W}x{H} = {W*32}x{H*32} px, solids={len(solids)}')

    # --- floors: standable rows (air over solid, headroom, no kill tile) ---
    def standable(x, y):
        if tile(x, y) or tile(x, y - 1) or tile(x, y - 2):
            return False
        if not tile(x, y + 1):
            return False
        for dx in range(-1, 2):
            if tile(x + dx, y) in SPIKE_NAMES or tile(x + dx, y - 1) in SPIKE_NAMES:
                return False
        return True

    rows = {}
    for y in range(2, H - 2):
        run_start, runs = -1, []
        for x in range(W + 1):
            ok = x < W and standable(x, y)
            if ok and run_start < 0:
                run_start = x
            elif not ok and run_start >= 0:
                if x - run_start >= 4:
                    runs.append((run_start, x - 1))
                run_start = -1
        if runs:
            rows[y] = runs

    # cluster rows into floors: rows 1-3 tiles apart are the same level
    floors, cur = [], None
    for y in sorted(rows):
        if cur and y - cur['ytop'] <= 3:
            cur['ybot'] = y
            cur['runs'] += rows[y]
        else:
            if cur:
                floors.append(cur)
            cur = {'ytop': y, 'ybot': y, 'runs': list(rows[y])}
    if cur:
        floors.append(cur)

    print(f'\n=== FLOORS: {len(floors)} ===')
    for i, fl in enumerate(floors):
        span = sum(r[1] - r[0] + 1 for r in fl['runs'])
        mid = (fl['ytop'] + fl['ybot']) / 2 * 32 + 16
        print(f"floor {i}: tileY {fl['ytop']}..{fl['ybot']} (worldY {fl['ytop']*32}..{(fl['ybot']+1)*32} "
              f"centre {mid:.0f})  runs={len(fl['runs'])} walk_px={span*32}")
        for r in sorted(fl['runs'])[:12]:
            side = 'L' if r[1] < W/2-10 else ('R' if r[0] > W/2+10 else 'M')
            print(f"    x {r[0]:3d}..{r[1]:3d} (worldX {r[0]*32:5d}..{(r[1]+1)*32:5d}) "
                  f"len={(r[1]-r[0]+1)*32:4d}px side={side}")
        if len(fl['runs']) > 12:
            print(f'    ... {len(fl["runs"]) - 12} more runs')

    def floor_of(x, y):
        ty = y // 32
        best, bestd = -1, 1e9
        for i, fl in enumerate(floors):
            d = abs(ty - (fl['ybot'] + 1))
            if d < bestd:
                bestd, best = d, i
        return best

    # --- spike clusters: flood fill over same-kind spike tiles ---
    kind = {}
    for name, cells in spikes.items():
        for c in cells:
            kind[c] = name
    seen, clusters = set(), []
    for c in kind:
        if c in seen:
            continue
        stack, group = [c], []
        seen.add(c)
        while stack:
            cx, cy = stack.pop()
            group.append((cx, cy))
            for nx, ny in ((cx+1, cy), (cx-1, cy), (cx, cy+1), (cx, cy-1),
                           (cx+1, cy+1), (cx-1, cy-1), (cx+1, cy-1), (cx-1, cy+1)):
                n = (nx, ny)
                if n in kind and n not in seen and kind[n] == kind[c]:
                    seen.add(n)
                    stack.append(n)
        clusters.append((kind[c], group))

    print(f'\n=== SPIKE CLUSTERS (throw targets): {len(clusters)} ===')
    for name, g in sorted(clusters, key=lambda it: min(p[1] for p in it[1])):
        xs = [p[0] for p in g]; ys = [p[1] for p in g]
        cx = (min(xs) + max(xs) + 1) * 16.0
        cy = (min(ys) + max(ys) + 1) * 16.0
        print(f'{name:7s} n={len(g):3d} tileX {min(xs):3d}..{max(xs):3d} '
              f'tileY {min(ys):3d}..{max(ys):3d} centre=({cx:.0f},{cy:.0f}) '
              f'floor={floor_of(cx, cy)} w={(max(xs)-min(xs)+1)*32}px '
              f'h={(max(ys)-min(ys)+1)*32}px')


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1
         else r'C:\Users\tailznic\Documents\fng-0.6.5-win64\fng.map')

