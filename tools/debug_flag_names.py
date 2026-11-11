"""Recover the names of the game's flags.

The flag list (GameData/GameDataList.Product.110.byml.zs) holds only each flag's key, the MurmurHash3 (32 bit, seed 0) of
its name. The names are used all over the other data files, so this hashes every identifier-like string of the small romfs
files and of the file names, keeps the ones whose hash is a key, and then tries `Struct.Member` names built from those.

    python tools/debug_flag_names.py OUT.json        (run with the python that debug_romfs_extract.py needs)

OUT.json maps the key (8 hex digits) to [kind, name, where it was found]; put it under build/. A few of the built names are
chance matches (a member name under a file-name stem that is no struct): judge by the name.
"""
import itertools, json, os, re, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import debug_romfs_extract as rx

M = 0xffffffff
FLAG_LIST = 'GameData/GameDataList.Product.110.byml.zs'
SKIP_DIRS = {'Tex', 'TerrainArc', 'Model', 'Bake', 'Sound', 'Phive', 'PreComputedCulling', 'Voice', 'Effect', 'Font2', 'Shader', 'Movie',
             'CameraAnimation'}
MEMBERS = ['Life', 'MaxLife', 'MP', 'MPMax', 'CurrentRupee', 'StockNum', 'SmallKeyNum', 'IsGetMap', 'IsGetCompass', 'IsGetBossKey',
           'EquipArmor', 'EquipBoot', 'EquipHeadArmor', 'Pos', 'Dir', 'YRad', 'BancName', 'BancMainPath', 'LocationName', 'DungeonRoomID',
           'MapIconPos', 'MapIconYRad', 'CurrentPos', 'CurrentDir', 'IsValid', 'Item', 'Second', 'Minute', 'Hour']


def murmur3(data):
    h, n = 0, len(data)
    r = n & 3
    for i in range(0, n - r, 4):
        k = (int.from_bytes(data[i:i + 4], 'little') * 0xcc9e2d51) & M
        h ^= (((k << 15) | (k >> 17)) & M) * 0x1b873593 & M
        h = ((((h << 13) | (h >> 19)) & M) * 5 + 0xe6546b64) & M
    if r:
        k = (int.from_bytes(data[n - r:], 'little') * 0xcc9e2d51) & M
        h ^= (((k << 15) | (k >> 17)) & M) * 0x1b873593 & M
    h ^= n
    h ^= h >> 16
    h = (h * 0x85ebca6b) & M
    h ^= h >> 13
    h = (h * 0xc2b2ae35) & M
    return h ^ (h >> 16)


def byml(b):
    """A BYML file as dicts and lists (binary nodes become None)."""
    e = '>' if b[:2] == b'BY' else '<'
    u32 = lambda o: struct.unpack_from(e + 'I', b, o)[0]
    u24 = lambda o: int.from_bytes(b[o:o + 3], 'big' if e == '>' else 'little')

    def strings(o):
        return [b[o + u32(o + 4 + 4 * i):b.index(b'\0', o + u32(o + 4 + 4 * i))].decode('utf-8', 'replace') for i in range(u24(o + 1))] if o else []

    keys, strs = strings(u32(4)), strings(u32(8))

    def value(t, at):
        v = u32(at)
        if t == 0xA0: return strs[v]
        if t in (0xC0, 0xC1, 0x20, 0x21): return node(v)
        if t == 0xD0: return bool(v)
        if t == 0xD1: return struct.unpack_from(e + 'i', b, at)[0]
        if t == 0xD2: return struct.unpack_from(e + 'f', b, at)[0]
        if t == 0xD3: return v
        if t in (0xD4, 0xD5, 0xD6): return struct.unpack_from(e + {0xD4: 'q', 0xD5: 'Q', 0xD6: 'd'}[t], b, v)[0]
        return None

    def node(o):
        t, n = b[o], u24(o + 1)
        if t == 0xC0:
            first = o + 4 + ((n + 3) & ~3)
            return [value(b[o + 4 + i], first + 4 * i) for i in range(n)]
        if t == 0xC1: return {keys[u24(o + 4 + 8 * i)]: value(b[o + 7 + 8 * i], o + 8 + 8 * i) for i in range(n)}
        return {'#%08x' % u32(o + 4 + 8 * i): value(b[o + 4 + 8 * n + i], o + 8 + 8 * i) for i in range(n)}

    return node(u32(12))


def main(out):
    kinds = {}
    for kind, entries in byml(rx.get(FLAG_LIST))['Data'].items():
        for entry in entries:
            if entry['Hash'] <= M: kinds.setdefault(entry['Hash'], kind)
    found, seen, idents = {}, set(), set()

    def feed(s, where):
        if s in seen: return
        seen.add(s)
        h = murmur3(s)
        if h in kinds and h not in found: found[h] = [kinds[h], s.decode(), where]

    word = re.compile(rb'[A-Za-z0-9_.\-/]{3,120}')
    for path, info in rx.idx.items():
        for part in re.split(r'[/.]', path):
            feed(part.encode(), 'file name')
            idents.add(part)
        if path.split('/')[0] in SKIP_DIRS or info['size'] > 40 << 20 or path.endswith(('.webm', '.bntx.zs', '.flnt.zs', '.bfres.zs')): continue
        try:
            data = rx.get(path)
        except Exception:
            continue
        actor_names = path.startswith(('Pack/Actor/', 'RSDB/', 'Game/'))
        for m in word.finditer(data):
            feed(m.group(), path)
            if actor_names and re.fullmatch(rb'[A-Za-z][A-Za-z0-9_]{2,60}', m.group()): idents.add(m.group().decode())
    direct = len(found)
    structs = sorted({v[1].split('.')[0] for v in found.values() if v[0] == 'Struct' or '.' in v[1]} | {'PouchData', 'DungeonInfo'})
    members = sorted({v[1].split('.')[-1] for v in found.values() if '.' in v[1]} | set(MEMBERS))
    for s in structs:
        for i in itertools.chain(idents, members): feed(('%s.%s' % (s, i)).encode(), 'built')
    for h, v in list(found.items()):   # one more level: PouchData.<item>.StockNum, DungeonInfo.<dungeon>.SmallKeyNum ...
        if v[0] == 'Struct' and '.' in v[1]:
            for m in members: feed(('%s.%s' % (v[1], m)).encode(), 'built')
    json.dump({'%08x' % h: v for h, v in sorted(found.items(), key=lambda x: (x[1][0], x[1][1]))}, open(out, 'w'), indent=0)
    print('%d of %d flag keys named (%d found as they are, %d built)' % (len(found), len(kinds), direct, len(found) - direct))


if __name__ == '__main__':
    main(sys.argv[1])
