import struct, zlib, sys

def analyze(path):
    with open(path, 'rb') as f:
        b = f.read()
    magic, ver, sz, swap, num_types, num_items, num_raw, item_sz, data_sz = struct.unpack_from('<4s8i', b, 0)
    print(f"Header: ver={ver} types={num_types} items={num_items} raw={num_raw}")
    pos = 36
    types = []
    for _ in range(num_types):
        t, start, num = struct.unpack_from('<3i', b, pos)
        types.append((t, start, num))
        pos += 12
    item_offsets = list(struct.unpack_from(f'<{num_items}i', b, pos))
    pos += num_items * 4
    data_offsets = list(struct.unpack_from(f'<{num_raw}i', b, pos))
    pos += num_raw * 4
    data_sizes = list(struct.unpack_from(f'<{num_raw}i', b, pos))
    pos += num_raw * 4
    item_start = pos
    data_start = pos + item_sz

    def get_data(idx):
        off = data_start + data_offsets[idx]
        sz = data_sizes[idx]
        return zlib.decompress(b[off:off+sz])

    def get_item(idx):
        off = item_start + item_offsets[idx]
        tid_and_id, sz = struct.unpack_from('<2i', b, off)
        return tid_and_id >> 16, tid_and_id & 0xFFFF, b[off+8:off+8+sz]

    game_w = game_h = None
    game_tiles = None
    for i in range(num_items):
        tid, iid, payload = get_item(i)
        if tid == 5:
            lver, ltype, lflags = struct.unpack_from('<3i', payload, 0)
            if ltype == 2:
                # CMapItemLayer: ver(4), type(4), flags(4)
                # CMapItemLayerTilemap: ver(4), w(4), h(4), flags(4), color(16), colenv(4), colenvoff(4), image(4), data(4)
                # payload offsets (in ints):
                # 0: ver, 1: type, 2: lflags, 3: tilemap_ver, 4: w, 5: h, 6: tilemap_flags,
                # 7..10: color, 11: colenv, 12: colenvoff, 13: image, 14: data
                ints = struct.unpack_from('<15i', payload, 0)
                w, h, flags = ints[4], ints[5], ints[6]
                image, data_idx = ints[13], ints[14]
                print(f"Tile layer: {w}x{h} flags={flags} (GAME={bool(flags & 1)}) data_idx={data_idx}")
                if flags & 1:
                    raw = get_data(data_idx)
                    game_w, game_h = w, h
                    game_tiles = raw
                    break

    if not game_tiles:
        print("NO GAME LAYER FOUND")
        return

    print(f"\n=== GAME LAYER: {game_w}x{game_h} ({len(game_tiles)//4} tiles) ===")
    spikes = { 'normal': [], 'red': [], 'blue': [], 'gold': [], 'green': [], 'purple': [], 'death': [] }
    solids = []
    spawns = { 'neutral': [], 'red': [], 'blue': [] }
    for y in range(game_h):
        for x in range(game_w):
            idx_in_buf = (y * game_w + x) * 4
            idx = game_tiles[idx_in_buf]
            if idx == 1: solids.append((x, y))
            elif idx == 2: spikes['death'].append((x, y))
            elif idx == 7: spikes['gold'].append((x, y))
            elif idx == 8: spikes['normal'].append((x, y))
            elif idx == 9: spikes['red'].append((x, y))
            elif idx == 10: spikes['blue'].append((x, y))
            elif idx == 14: spikes['green'].append((x, y))
            elif idx == 15: spikes['purple'].append((x, y))
            elif idx == 192: spawns['neutral'].append((x, y))
            elif idx == 193: spawns['red'].append((x, y))
            elif idx == 194: spawns['blue'].append((x, y))

    print(f"Solids: {len(solids)}")
    for k, v in spikes.items():
        if v:
            xs = [p[0] for p in v]
            ys = [p[1] for p in v]
            print(f"Spikes {k:7s}: count={len(v):3d}  x:[{min(xs):3d}..{max(xs):3d}]  y:[{min(ys):3d}..{max(ys):3d}]")
    for k, v in spawns.items():
        if v:
            xs = [p[0] for p in v]
            ys = [p[1] for p in v]
            print(f"Spawns {k:7s}: count={len(v):3d}  x:[{min(xs):3d}..{max(xs):3d}]  y:[{min(ys):3d}..{max(ys):3d}]")

    shelves = []
    for y in range(1, game_h):
        for x in range(game_w):
            idx_cur = game_tiles[(y * game_w + x) * 4]
            idx_above = game_tiles[((y - 1) * game_w + x) * 4]
            if idx_cur == 1 and idx_above == 0:
                shelves.append((x, y))
    print(f"Walkable shelf tops: {len(shelves)}")
    by_y = {}
    for x, y in shelves:
        by_y.setdefault(y, []).append(x)
    print("Prominent horizontal surfaces (top 10 by length):")
    for y, xs in sorted(by_y.items(), key=lambda it: -len(it[1]))[:10]:
        print(f"  y={y:3d} (world Y={y*32:5d}): {len(xs):3d} blocks, x from {min(xs)} to {max(xs)}")

if __name__ == '__main__':
    p = sys.argv[1] if len(sys.argv) > 1 else r'C:\Users\tailznic\Documents\fng-0.6.5-win64\fng.map'
    analyze(p)
