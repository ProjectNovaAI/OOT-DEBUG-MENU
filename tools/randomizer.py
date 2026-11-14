"""Randomizer: shuffles what chests, pots, grass and other drops give, which enemies stand where and where loading
zones lead, and writes the result as the Randomizer mod.

    python tools/randomizer.py --game <install folder> [--seed TEXT] [options]

The install folder is the one that holds romfs/ (index.json and romfs.bin, as the setup unpacks the game) and mods/.
The tool reads every area's stage files (Banc/<area>/<area>[_Child|_Adult]_{Static,Dynamic}.bcett.byml.zs), changes
the actors' parameters there and writes the changed stage files to mods/Randomizer/romfs, with the seed, the options
and every change in mods/Randomizer/spoiler.txt. The game's Randomizer tab (runtime/randomizer.c) runs it with
--pending --status: the files then wait in romfs.new until the game starts again. Run by hand, turn the mod on in the
Mods tab; it applies at the next start. A new seed rewrites the folder.

Item locations (Location):
- Chests: actors Obj_TBox*; Dynamic.Drop__DropActor is the actor that comes out, Dynamic.GetEventName an event played
  after it (Navi's comment on the item). A chest is one location even where the area has a child and an adult stage
  file that both place it (same Hash).
- Drops (--drops): pots, grass, rocks, bushes, trees, crates, beehives and enemies whose Dynamic.Drop__DropActor names
  their item. Only items the game drops as an object go to them (droppable): a drop naming a chest-only item, such as
  a sword, gives nothing. Drops give their item each time the object is broken, as the vanilla ones do.
- Small keys and boss keys stay in their own chests: the game counts a key for the area it is picked up in.
- Maps and compasses stay, or with --maps-compasses own_dungeon move between the chests of their own dungeon.
- Left alone: the Treasure Chest Shop's game chests and other chests that open again (IsEnableRepeatOpen), chests that
  give a second item when the first is owned (Drop__CheckActor: the Deku and Hylian Shield chests), and the areas
  DreamWorld and BossRushIntervalRoom.
- Navi's comment (GetEventName) goes with the chest only while the chest keeps its own item.
- Items the game gives outside stage actors (songs, NPC gifts, Gold Skulltula rewards, shops, scrubs) stay where they
  are: those are in the event flows.

Enemies (--enemies): an enemy that no link, AI group or event refers to (referenced) becomes another of the same
group (ENEMY_GROUPS: ground, air, water) or any ground or air enemy, the same in the child and the adult file.

Entrances (--entrances): the loading zones (FieldAndDungeonEntranceEventRequestActor) are paired with the zone that
leads back; a shuffle swaps whole pairs of the same kind (pair_kind), so the way back still leads back. A zone takes
the destination parameters of the zone it now stands for (the next stage, start position and arrival event).

Logic (--logic glitchless, the default): each area has the set of items that reaching every location in it may take
(AREA_NEEDS: what the vanilla game needs to get there, taken generously, so an area is never assumed open early; a
dungeon behind another's entrance also needs what that entrance needs). The items that open areas (PROGRESSION) are
placed by assumed fill: each goes to a location that can be reached with all the progression items not yet placed,
so every location, Ganon's Castle included, can be reached in the result. The other items fill the rest at random.
--logic none places everything at random: a seed may then lock an item behind itself (the debug menu can give it).
Overworld and all entrances use no logic: it knows only the vanilla field.

Other files of the stage are read from the mods below this one when they replace them (nx2navi.ini [mods] order), so
a mod that changes the same stage files keeps its changes in the randomized ones; this mod must then sit above it.
"""
import argparse, copy, hashlib, json, os, random, re, shutil, struct, sys, time

try:
    import zstandard

    def _zd(data, dict_bytes=None):
        d = zstandard.ZstdDecompressor(dict_data=zstandard.ZstdCompressionDict(dict_bytes)) if dict_bytes else zstandard.ZstdDecompressor()
        return d.decompress(data, max_output_size=1 << 30)

    def _zc(data):
        return zstandard.ZstdCompressor(level=19).compress(data)
except ImportError:
    try:   # Python 3.14's own zstd
        from compression import zstd

        def _zd(data, dict_bytes=None):
            return zstd.decompress(data, zstd_dict=zstd.ZstdDict(dict_bytes)) if dict_bytes else zstd.decompress(data)

        def _zc(data):
            return zstd.compress(data, level=19)
    except ImportError:   # reported by main through --status, not at import
        def _zd(data, dict_bytes=None):
            raise SystemExit('this Python has no zstd: install the zstandard package or use Python 3.14')

        _zc = _zd

MOD_NAME = 'Randomizer'
EXCLUDE_AREAS = ('DreamWorld', 'BossRushIntervalRoom')

# ---- the game's files

class RomFS:
    """The unpacked RomFS: index.json lists files[{path, offset, size}] in romfs.bin. A .zs file is zstd, made with one
    of the dictionaries in Pack/ZsDic.pack.zs or with none."""

    def __init__(self, folder):
        self.folder = folder
        d = json.load(open(os.path.join(folder, 'index.json'), encoding='utf-8'))
        self.idx = {f['path']: f for f in d['files']}
        self.bin = open(os.path.join(folder, 'romfs.bin'), 'rb')
        self.dicts = list(sarc(_zd(self.raw('Pack/ZsDic.pack.zs'))).values())

    def raw(self, path):
        f = self.idx[path]
        self.bin.seek(f['offset'])
        return self.bin.read(f['size'])

    def get(self, path, data=None):
        r = self.raw(path) if data is None else data
        if not path.endswith('.zs'):
            return r
        for dd in self.dicts:
            try:
                return _zd(r, dd)
            except Exception:
                pass
        return _zd(r)


def sarc(b):
    assert b[:4] == b'SARC'
    e = '<' if b[6:8] == b'\xff\xfe' else '>'
    dataoff = struct.unpack(e + 'I', b[0xc:0x10])[0]
    n = struct.unpack(e + 'H', b[0x1a:0x1c])[0]
    sfat, out = 0x20, {}
    nameoff = sfat + n * 16 + 8
    for i in range(n):
        _, fa, s, en = struct.unpack(e + 'IIII', b[sfat + i * 16:sfat + i * 16 + 16])
        no = (fa & 0xffffff) * 4 + nameoff
        out[b[no:b.index(b'\0', no)].decode()] = b[dataoff + s:dataoff + en]
    return out

# ---- BYML (little endian, version 2 and later), keeping each value's type so a file can be written back as it was.
# A dict is a Python dict of key -> node, an array a list of nodes, any other value a tuple (type, value).

T_STR, T_ARR, T_DICT, T_STRTAB = 0xA0, 0xC0, 0xC1, 0xC2
T_BOOL, T_INT, T_FLOAT, T_UINT, T_INT64, T_UINT64, T_DOUBLE, T_NULL = 0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xFF


def byml_read(b):
    assert b[:2] == b'YB', 'not a little-endian BYML file'
    ver = struct.unpack_from('<H', b, 2)[0]
    kt, st, root = struct.unpack_from('<III', b, 4)

    def table(off):
        if not off:
            return []
        n = struct.unpack_from('<I', b, off)[0] >> 8
        offs = struct.unpack_from('<%dI' % n, b, off + 4)
        return [b[off + o:b.index(b'\0', off + o)].decode('utf-8') for o in offs]
    keys, strs = table(kt), table(st)

    def val(t, v):
        if t == T_STR:
            return (t, strs[v])
        if t in (T_ARR, T_DICT):
            return node(v)
        if t in (T_INT64, T_UINT64, T_DOUBLE):
            return (t, struct.unpack_from({T_INT64: '<q', T_UINT64: '<Q', T_DOUBLE: '<d'}[t], b, v)[0])
        if t in (T_BOOL, T_INT, T_FLOAT, T_UINT, T_NULL):
            return (t, v)   # the raw 32 bits
        raise ValueError('BYML value type 0x%02x is not handled' % t)

    def node(off):
        t, n = b[off], struct.unpack_from('<I', b, off)[0] >> 8
        if t == T_ARR:
            types = b[off + 4:off + 4 + n]
            vo = off + 4 + ((n + 3) & ~3)
            return [val(types[i], struct.unpack_from('<I', b, vo + 4 * i)[0]) for i in range(n)]
        if t == T_DICT:
            d = {}
            for i in range(n):
                e = off + 4 + 8 * i
                k, tt, v = struct.unpack_from('<I', b, e)[0] & 0xffffff, b[e + 3], struct.unpack_from('<I', b, e + 4)[0]
                d[keys[k]] = val(tt, v)
            return d
        raise ValueError('BYML node type 0x%02x is not handled' % t)
    return ver, (node(root) if root else None)


def byml_write(ver, root):
    keys, strs = set(), set()

    def walk(n):
        if isinstance(n, dict):
            for k, v in n.items():
                keys.add(k)
                walk(v)
        elif isinstance(n, list):
            for v in n:
                walk(v)
        elif n[0] == T_STR:
            strs.add(n[1])
    walk(root)
    keys = sorted(keys, key=lambda s: s.encode())
    strs = sorted(strs, key=lambda s: s.encode())
    K = {k: i for i, k in enumerate(keys)}
    S = {s: i for i, s in enumerate(strs)}
    out = bytearray(16)

    def align(a):
        while len(out) % a:
            out.append(0)

    def strtab(items):
        if not items:
            return 0
        align(4)
        o = len(out)
        enc = [s.encode() + b'\0' for s in items]
        out.extend(struct.pack('<I', T_STRTAB | (len(items) << 8)))
        pos = 4 + 4 * (len(items) + 1)
        for e in enc:
            out.extend(struct.pack('<I', pos))
            pos += len(e)
        out.extend(struct.pack('<I', pos))
        for e in enc:
            out.extend(e)
        return o
    kt, st = strtab(keys), strtab(strs)

    def scalar(n):
        t, v = n
        if t == T_STR:
            return S[v]
        if t in (T_INT64, T_UINT64, T_DOUBLE):
            align(8)
            o = len(out)
            out.extend(struct.pack({T_INT64: '<q', T_UINT64: '<Q', T_DOUBLE: '<d'}[t], v))
            return o
        return v

    def tcode(n):
        return T_DICT if isinstance(n, dict) else T_ARR if isinstance(n, list) else n[0]

    def container(n):
        align(4)
        o = len(out)
        if isinstance(n, dict):
            items = sorted(n.items(), key=lambda kv: K[kv[0]])
            out.extend(struct.pack('<I', T_DICT | (len(items) << 8)))
            slots = []
            for k, v in items:
                out.extend(struct.pack('<I', K[k] | (tcode(v) << 24)))
                slots.append(len(out))
                out.extend(b'\0\0\0\0')
            vals = [v for k, v in items]
        else:
            out.extend(struct.pack('<I', T_ARR | (len(n) << 8)))
            out.extend(bytes(tcode(v) for v in n))
            align(4)
            slots = []
            for v in n:
                slots.append(len(out))
                out.extend(b'\0\0\0\0')
            vals = n
        for s, v in zip(slots, vals):
            struct.pack_into('<I', out, s, container(v) if isinstance(v, (dict, list)) else scalar(v))
        return o
    r = container(root)
    out[0:16] = b'YB' + struct.pack('<HIII', ver, kt, st, r)
    return bytes(out)

# ---- item locations

class Location:
    """One place that gives an item: a chest, or a pot, grass, a rock, a tree, a crate, a beehive or an enemy that drops
    one (kind 'drop'). Where it is (every stage file that places it) and what it held."""

    def __init__(self, kind, area, h, gyaml, item):
        self.kind, self.area, self.hash, self.gyaml, self.vanilla = kind, area, h, gyaml, item
        self.places = []   # (stage file, index in its Actors)
        self.item = None

    def name(self):
        return '%s %s %016x (%s)' % (self.area, 'chest' if self.kind == 'chest' else 'drop', self.hash, self.gyaml)


def stage_files(rom):
    return sorted(p for p in rom.idx if p.startswith('Banc/') and p.endswith('.bcett.byml.zs') and p.count('/') == 2)


def folder_of(path):
    return path.split('/')[1]


def stem_of(path):
    """Banc/KokiriForest/KokiriForest_Child_Static.bcett.byml.zs -> KokiriForest_Child, the name the game calls it by."""
    return path.split('/')[2].split('.')[0].rsplit('_', 1)[0]


# Drops that stay: the bugs some soft soil needs, keys (a key counts for the area it is found in), minigame keys.
KEEP_DROPS = ('Animal_Insect', 'Item_Dungeon_SmallKey', 'Item_Dungeon_BossKey', 'Obj_MiniGame_SmallKey')


def find_locations(stages, drops):
    """stages: path -> parsed root. The chests, and with drops the other actors that drop a set item."""
    locs = {}
    for path in sorted(stages):
        area = folder_of(path)
        if area in EXCLUDE_AREAS:
            continue
        for i, a in enumerate(stages[path].get('Actors', [])):
            g = a.get('Gyaml', (0, ''))[1]
            dyn = a.get('Dynamic', {})
            if not isinstance(dyn.get('Drop__DropActor'), tuple):
                continue
            if dyn.get('IsEnableRepeatOpen', (0, 0))[1] or 'Drop__CheckActor' in dyn:
                continue
            item = dyn['Drop__DropActor'][1]
            kind = 'chest' if g.startswith('Obj_TBox') else 'drop'
            if kind == 'drop' and (not drops or item.startswith(KEEP_DROPS) or 'SmallKey' in item):
                continue
            h = a['Hash'][1]
            if h not in locs:
                locs[h] = Location(kind, area, h, g, item)
            locs[h].places.append((path, i))
    return [locs[h] for h in sorted(locs, key=lambda h: (locs[h].kind, locs[h].area, h))]


def item_for(loc, item, actors):
    """The actor of `item` that suits the place: a chest gives TBox_<x>, the world drops Obj_<x>, when both exist."""
    if loc.kind == 'drop' and item.startswith('TBox_') and 'Obj_' + item[5:] in actors:
        return 'Obj_' + item[5:]
    if loc.kind == 'chest' and item.startswith('Obj_') and 'TBox_' + item[4:] in actors:
        return 'TBox_' + item[4:]
    return item

# ---- what the logic knows

SMALL_KEY, BOSS_KEY = 'Item_Dungeon_SmallKey', 'Item_Dungeon_BossKey'
MAP_COMPASS = ('Item_Dungeon_Map', 'Item_Dungeon_Compass')
SWORD, SLING, BOMBS, BOOMERANG = 'Item_Sword_Kokiri', 'Item_Range_SlingShot', 'Item_BombBag_Small', 'Item_Boomerang'
HOOK, BOW, HAMMER, IRON = 'Item_Hookshot', 'Item_Range_Bow', 'Item_Hammer', 'Item_HeavyBoot'
LONG, LENS, HOVER = 'Item_Longshot', 'Item_GlassTruth', 'Item_HoverBoot'
SILVER, MIRROR, GOLD = 'Item_Gauntlet_Silver', 'Item_Shield_Mirror', 'Item_Gauntlet_Gold'
# The chest items that open the way. The Longshot does all the Hookshot does.
PROGRESSION = (SWORD, SLING, BOMBS, BOOMERANG, HOOK, BOW, HAMMER, IRON, LONG, LENS, HOVER, SILVER, MIRROR, GOLD)
NAMES = {SWORD: 'Kokiri Sword', SLING: 'Fairy Slingshot', BOMBS: 'Bomb Bag', BOOMERANG: 'Boomerang', HOOK: 'Hookshot',
         BOW: 'Fairy Bow', HAMMER: 'Megaton Hammer', IRON: 'Iron Boots', LONG: 'Longshot', LENS: 'Lens of Truth',
         HOVER: 'Hover Boots', SILVER: 'Silver Gauntlets', MIRROR: 'Mirror Shield', GOLD: 'Golden Gauntlets'}

CHILD = {SWORD, SLING}                        # leaving the forest: the Deku Tree done
CHILD_X = CHILD | {BOMBS}                     # and the grottos and rocks explosives open
ADULT = CHILD_X | {BOOMERANG}                 # the three Spiritual Stones
SHADOW = ADULT | {HOOK, BOW, HAMMER, IRON, LONG, LENS, HOVER}
SPIRIT = SHADOW | {SILVER, MIRROR}
AREA_NEEDS = {
    'KokiriForest': set(),
    'InsideTheDekuTree': {SWORD},
    'LostWood': CHILD_X, 'SacredForestMeadow': CHILD_X, 'KakarikoVillage': CHILD_X, 'ZoraRiver': CHILD_X,
    'ZoraDomain': CHILD_X, 'DeathMountainTrail': CHILD_X, 'Market': CHILD_X, 'HyruleCastle': CHILD_X,
    'DodongoCavern': CHILD,
    'InsideJabuJabuBelly': ADULT,
    'Graveyard': ADULT,
    'HyruleField': ADULT | {HAMMER, IRON},     # the boulder grotto and the heart piece under water
    'GoronCity': ADULT | {HAMMER},
    'DeathMountainCrater': ADULT | {HOOK, HAMMER},
    'BottomOfTheWell': ADULT | {LENS},         # the Song of Storms is learnt as an adult
    'ForestTemple': ADULT | {HOOK, BOW},
    'FireTemple': ADULT | {HOOK, BOW, HAMMER},
    'IceCavern': ADULT | {HOOK},
    'ZoraFountain': ADULT | {IRON},
    'WaterTemple': ADULT | {HOOK, BOW, IRON, LONG},
    'GerudoValley': ADULT | {HOOK, BOW, HAMMER},
    'GerudoFortress': ADULT | {HOOK, BOW},
    'HountedWasteland': ADULT | {HOOK, BOW, LONG, HOVER},
    'ShadowTemple': SHADOW,
    'SpiritTemple': SPIRIT, 'DesertColossus': SPIRIT,
    'GerudoTrainingGround': SPIRIT,
    'InsideGanonCastle': SPIRIT | {GOLD},
}
area_needs = dict(AREA_NEEDS)   # with shuffled dungeon entrances, a dungeon also needs what its new door needs


def needs(loc):
    return area_needs.get(loc.area, set(PROGRESSION))   # an area not listed: open last


def counted(items):
    """What owning these items lets one do: the Longshot counts as the Hookshot too."""
    s = set(items)
    if LONG in s:
        s.add(HOOK)
    return s


def reachable(locs, have):
    """The locations one can open owning `have` plus whatever the reachable ones hold."""
    owned = set(have)
    while True:
        own = counted(owned)
        open_ = [l for l in locs if needs(l) <= own]
        more = owned | {l.item for l in open_ if l.item in PROGRESSION}
        if more == owned:
            return open_
        owned = more

# ---- the item shuffle


def droppable(item, drop_items, actors):
    """An item the world can drop (a pot, grass, a rock): one some drop gives in the game, or a chest item whose world
    version exists. The rest (equipment, dungeon items) only comes out of chests: a pot given one drops nothing."""
    return item in drop_items or (item.startswith('TBox_') and 'Obj_' + item[5:] in actors)


def shuffle(locs, opts, rnd, logic, can_drop=lambda it: True):
    """Fills locs (their items all None) with what they held, shuffled. A drop location only gets items can_drop
    allows; the progression items go to chests."""
    fixed = [l for l in locs if l.vanilla in (SMALL_KEY, BOSS_KEY)]
    dungeon_items = [l for l in locs if l.vanilla in MAP_COMPASS]
    if opts.maps_compasses == 'vanilla':
        fixed += dungeon_items
        dungeon_items = []
    for l in fixed:
        l.item = l.vanilla
    if opts.maps_compasses == 'own_dungeon':   # first, among their own dungeon's chests
        for area in sorted({l.area for l in dungeon_items}):
            spots = [l for l in locs if l.area == area and l.kind == 'chest' and l.item is None]
            for it in [l.vanilla for l in dungeon_items if l.area == area]:
                rnd.choice([s for s in spots if s.item is None]).item = it
    pool = [l.vanilla for l in locs if l.vanilla not in (SMALL_KEY, BOSS_KEY) and l.vanilla not in MAP_COMPASS]
    major = [it for it in pool if it in PROGRESSION]
    rest = [it for it in pool if it not in PROGRESSION]
    rnd.shuffle(major)
    if logic == 'glitchless':
        # assumed fill: each progression item goes where it can be reached without it
        for i, it in enumerate(major):
            spots = [l for l in reachable(locs, major[i + 1:]) if l.item is None and (l.kind == 'chest' or can_drop(it))]
            if not spots:
                return False
            rnd.choice(spots).item = it
    else:
        rest += major
    rnd.shuffle(rest)
    empty = [l for l in locs if l.item is None]
    assert len(empty) == len(rest), (len(empty), len(rest))
    for l in [l for l in empty if l.kind == 'drop']:   # the drops first, from what can drop
        k = next((j for j, it in enumerate(rest) if can_drop(it)), None)
        if k is None:
            return False
        l.item = rest.pop(k)
    for l, it in zip([l for l in empty if l.kind == 'chest'], rest):
        l.item = it
    return True


def beatable(locs):
    return len(reachable(locs, [])) == len(locs)

# ---- enemies

# Enemies that stand on their own: no links, not named by another actor or an AI group (a room's "defeat them all"
# condition). Each is swapped for another of its group (--enemies same_kind) or of any group but water (any_kind).
ENEMY_GROUPS = {
    'ground': ['Enemy_Dekubaba', 'Enemy_BabyDodongo', 'Enemy_TorchSlug', 'Enemy_Wolfos', 'Enemy_Wolfos_White',
               'Enemy_Beamos', 'Enemy_LikeLike', 'Enemy_ReDead', 'Enemy_Gibdo', 'Enemy_Dodongo', 'Enemy_Tailpasaran',
               'Enemy_Moblin', 'Enemy_Moblin_Club', 'Enemy_Freezard', 'Enemy_Freezard_Fixed', 'Enemy_Spike', 'Enemy_Amos',
               'Enemy_Leever_Fixed', 'Enemy_Tektite_Red', 'Enemy_Stalfos', 'Enemy_Stalfos_Weak', 'Enemy_Dinolfos',
               'Enemy_Lizalfos', 'Enemy_Peahat', 'Enemy_Scrub_Mad'],
    'air': ['Enemy_Keese', 'Enemy_Keese_Fire', 'Enemy_Keese_Ice', 'Enemy_Guay_Fixed', 'Enemy_Bubble_Red',
            'Enemy_Bubble_Blue', 'Enemy_Bubble_Green', 'Enemy_Poe', 'Enemy_Shabom'],
    'water': ['Enemy_Octorok', 'Enemy_Tektite_Blue', 'Enemy_ShellBlade', 'Enemy_Stinger', 'Enemy_Biri'],
}
REF_KEYS = ('Dst', 'Src', 'Reference', 'InstanceID', 'ID', 'Hash')


def referenced(root):
    """Hashes named in the stage's links and AI groups."""
    out = set()

    def walk(n):
        if isinstance(n, dict):
            for k, v in n.items():
                if k in REF_KEYS and isinstance(v, tuple):
                    out.add(v[1])
                walk(v)
        elif isinstance(n, list):
            for v in n:
                walk(v)
    for a in root.get('Actors', []):
        walk(a.get('Links', []))
    walk(root.get('AiGroups', []))
    return out


def shuffle_enemies(stages, how, rnd):
    """Gives each lone enemy another kind. Returns [(area, hash, old, new)]."""
    group_of = {g: k for k, gs in ENEMY_GROUPS.items() for g in gs}
    picks, done = {}, []
    for path in sorted(stages):
        if folder_of(path) in EXCLUDE_AREAS:
            continue
        root = stages[path]
        bound = referenced(root)
        for a in root.get('Actors', []):
            g = a.get('Gyaml', (0, ''))[1]
            grp = group_of.get(g)
            h = a.get('Hash', (0, 0))[1]
            if not grp or a.get('Links') or h in bound:
                continue
            key = (folder_of(path), h)
            if key not in picks:
                pool = ENEMY_GROUPS[grp] if how == 'same_kind' or grp == 'water' else ENEMY_GROUPS['ground'] + ENEMY_GROUPS['air']
                picks[key] = rnd.choice(pool)
                done.append((folder_of(path), h, g, picks[key]))
            a['Gyaml'] = (T_STR, picks[key])
    return done

# ---- entrances

DUNGEONS_CHILD = ('InsideTheDekuTree', 'DodongoCavern', 'InsideJabuJabuBelly', 'BottomOfTheWell')
DUNGEONS_ADULT = ('ForestTemple', 'FireTemple', 'WaterTemple', 'ShadowTemple', 'SpiritTemple', 'IceCavern',
                  'GerudoTrainingGround')
BOSS_STAGES = ('GohmaStage', 'DodongoStage', 'MorphaStage')
NO_SHUFFLE = ('GanonCastle', 'CrumblingGanonCastle', 'FinalBattleField', 'GanonStage', 'InsideGanonCastle') + EXCLUDE_AREAS
ENTRANCE = 'FieldAndDungeonEntranceEventRequestActor'


def find_entrances(stages):
    """The loading zones that lead both ways: [(end a, end b)], an end being (area folder, entrance name), and each
    end's zone actors {end: [(path, index)]}. A zone's Name is the start position the other side's zone sends to."""
    folder = {stem_of(p): folder_of(p) for p in stages}
    places, target = {}, {}
    for path in sorted(stages):
        for i, a in enumerate(stages[path].get('Actors', [])):
            if a.get('Gyaml', (0, ''))[1] != ENTRANCE or 'Name' not in a:
                continue
            dyn = a.get('Dynamic', {})
            nb = dyn.get('NextBancParamNameChild', dyn.get('NextBancParamNameAdult', (0, '')))[1]
            sp = dyn.get('NextStartPosName', (0, ''))[1]
            end = (folder_of(path), a['Name'][1])
            places.setdefault(end, []).append((path, i))
            if nb in folder and sp:
                target.setdefault(end, set()).add((folder[nb], sp))
    pairs, seen = [], set()
    for end, ts in sorted(target.items()):
        if len(ts) != 1 or end in seen:
            continue
        other = next(iter(ts))
        if target.get(other) == {end} and other != end:
            pairs.append((end, other))
            seen |= {end, other}
    return pairs, places


def pair_kind(a, b):
    """The kind of a two-way connection and its ends as (outer, inner): the field side first for a dungeon door, the
    dungeon side first for a boss door."""
    fa, fb = a[0], b[0]
    if fa in NO_SHUFFLE or fb in NO_SHUFFLE:
        return None, a, b
    dun = DUNGEONS_CHILD + DUNGEONS_ADULT
    for x, y in ((a, b), (b, a)):
        if y[0] in BOSS_STAGES and x[0] in dun:
            return 'boss', x, y
        if y[0] in dun and x[0] not in dun + BOSS_STAGES:
            return ('dungeon_child' if y[0] in DUNGEONS_CHILD else 'dungeon_adult'), x, y
    if fa not in dun + BOSS_STAGES and fb not in dun + BOSS_STAGES and fa != fb:
        return 'overworld', a, b
    return None, a, b


def shuffle_entrances(stages, how, rnd, mixed=False, any_age=False):
    """Reconnects loading zones: an end's zones get the destination the zones leading to its new partner had, so both
    ways of each new connection match. how: dungeons, bosses, overworld, all. Separate pools (mixed False): child
    dungeons swap with child dungeons, adult with adult, boss rooms with boss rooms and field connections with field
    connections. mixed: the chosen kinds and the field connections are one pool, so a dungeon door can lead to a field
    area and a field exit into a dungeon. any_age: child and adult dungeons are one pool (separate pools). Returns [(outer end, new inner end)] and {dungeon: the dungeons whose doors
    now lead to it}."""
    pairs, places = find_entrances(stages)
    kinds = {'dungeons': ('dungeon_child', 'dungeon_adult'), 'bosses': ('boss',), 'overworld': ('overworld',),
             'all': ('dungeon_child', 'dungeon_adult', 'boss', 'overworld')}[how]
    if mixed:
        kinds = tuple(set(kinds) | {'overworld'})
    groups = {}
    for a, b in pairs:
        k, outer, inner = pair_kind(a, b)
        if k in kinds:
            if k == 'overworld' and mixed and rnd.random() < 0.5:   # either side of a field connection may be the inner
                outer, inner = inner, outer
            key = 'mixed' if mixed else 'dungeon' if any_age and k.startswith('dungeon') else k
            groups.setdefault(key, []).append((outer, inner))
    dest = {}   # end -> {age suffix of the stage file: the Dynamic of a zone that leads to that end}
    for a, b in pairs:
        for x, y in ((a, b), (b, a)):   # y's zones lead to x
            for path, i in places[y]:
                dest.setdefault(x, {})[stem_of(path)[len(folder_of(path)):]] = copy.deepcopy(stages[path]['Actors'][i]['Dynamic'])
    new_pairs, moved = [], {}
    for k in sorted(groups):
        g = groups[k]
        if k == 'overworld':
            g = [(b, a) if rnd.random() < 0.5 else (a, b) for a, b in g]
        inner = [b for a, b in g]
        rnd.shuffle(inner)
        for (outer, old), new in zip(g, inner):
            new_pairs.append((outer, new))
            if k.startswith('dungeon') and old[0] != new[0]:
                moved.setdefault(new[0], set()).add(old[0])
    for x, y in new_pairs:
        for src, dst in ((x, y), (y, x)):   # src's zones now lead to dst
            for path, i in places[src]:
                age = stem_of(path)[len(folder_of(path)):]
                d = dest[dst].get(age) or next(iter(dest[dst].values()))
                stages[path]['Actors'][i]['Dynamic'] = copy.deepcopy(d)
    return new_pairs, moved

# ---- the mod


def mod_order(game):
    """Mod folder names in the game's order (top first), from nx2navi.ini [mods] mods_order."""
    ini = os.path.join(game, 'nx2navi.ini')
    order, section = [], ''
    if os.path.isfile(ini):
        for line in open(ini, encoding='utf-8', errors='replace'):
            line = line.strip()
            if line.startswith('['):
                section = line.strip('[]').lower()
            elif section == 'mods' and line.split('=')[0].strip() == 'mods_order':
                order = [s for s in line.split('=', 1)[1].strip().split('|') if s]
    return order


MODS_DIR = None   # the mods folder (--mods; default <game>/mods)


def mod_romfs(game, name):
    """The RomFS root a mod uses: its chosen setting's, else its own romfs folder."""
    base = os.path.join(MODS_DIR or os.path.join(game, 'mods'), name)
    sets = os.path.join(base, 'settings')
    if os.path.isdir(sets):
        ini = open(os.path.join(game, 'nx2navi.ini'), encoding='utf-8', errors='replace').read() if os.path.isfile(os.path.join(game, 'nx2navi.ini')) else ''
        m = re.search(r'^\s*mods_settings\s*=\s*(.*)$', ini, re.M)
        chosen = dict(e.split('=', 1) for e in (m.group(1).split('|') if m else []) if '=' in e)
        for setting in os.listdir(sets):
            values = sorted(os.listdir(os.path.join(sets, setting)), key=lambda v: (not v.isdigit(), int(v) if v.isdigit() else 0, v))
            v = chosen.get(setting, values[0] if values else '')
            r = os.path.join(sets, setting, v, 'romfs')
            if os.path.isdir(r):
                return r
    r = os.path.join(base, 'romfs')
    return r if os.path.isdir(r) else None


def load_stage(rom, game, path, below):
    """A stage file's bytes, from the first mod below this one in the order that replaces it, else the game's."""
    for name in below:
        r = mod_romfs(game, name)
        if r and os.path.isfile(os.path.join(r, path)):
            return rom.get(path, open(os.path.join(r, path), 'rb').read()), name
    return rom.get(path), None


def main():
    ap = argparse.ArgumentParser(description='Shuffle what the game gives and where it leads, and write the Randomizer mod.')
    ap.add_argument('--game', required=True, help='the install folder (holds romfs/ and mods/)')
    ap.add_argument('--seed', default=None, help='any text; the same seed and options give the same result')
    ap.add_argument('--logic', choices=('glitchless', 'none'), default='glitchless',
                    help='glitchless: every item can be reached; none: anything anywhere')
    ap.add_argument('--chests', choices=('on', 'off'), default='on', help='shuffle what the chests hold')
    ap.add_argument('--drops', choices=('off', 'own_pool', 'with_chests'), default='off',
                    help='what pots, grass, rocks, trees, crates, beehives and enemies with a set drop give: shuffled '
                         'among themselves, or in one pool with the chests')
    ap.add_argument('--maps-compasses', choices=('vanilla', 'own_dungeon'), default='own_dungeon')
    ap.add_argument('--enemies', choices=('off', 'same_kind', 'any_kind'), default='off',
                    help='lone enemies become others: of their kind (ground, air, water) or any but water')
    ap.add_argument('--entrances', choices=('off', 'dungeons', 'bosses', 'overworld', 'all'), default='off',
                    help='reconnect loading zones: dungeon doors, the boss rooms that have their own stage, the field '
                         'and town connections, or all of them; overworld and all turn the logic off')
    ap.add_argument('--entrance-pools', choices=('separate', 'mixed'), default='separate',
                    help='separate: dungeons lead only to dungeons (of the same age), boss rooms to boss rooms, field '
                         'connections to field connections; mixed: the shuffled entrances and the field connections are '
                         'one pool, so a dungeon can be anywhere (turns the logic off)')
    ap.add_argument('--dungeon-ages', choices=('same', 'any'), default='same',
                    help='same: child dungeons swap with child dungeons and adult with adult; any: a dungeon door can '
                         'lead to a dungeon of either age')
    ap.add_argument('--no-spoiler', action='store_true', help='write no spoiler.txt')
    ap.add_argument('--pending', action='store_true',
                    help='leave the new files in romfs.new and spoiler.new for the game to move into place at its next start')
    ap.add_argument('--mods', default=None, help='the mods folder (default: <game>/mods)')
    ap.add_argument('--status', default=None,
                    help='a file that gets "ok: <summary>" or "error: <why>" at the end (the Randomizer tab of the game reads it)')
    opts = ap.parse_args()
    try:
        summary = generate(opts)
    except BaseException as e:   # SystemExit included: the status file must say why the run ended
        why = str(e) if isinstance(e, SystemExit) else '%s: %s' % (type(e).__name__, e)
        if opts.status:
            write_status(opts.status, 'error: ' + why.replace('\n', ' '))
        raise
    if opts.status:
        write_status(opts.status, 'ok: ' + summary)


def write_status(path, text):
    tmp = path + '.tmp'
    with open(tmp, 'w', encoding='utf-8') as f:
        f.write(text + '\n')
    os.replace(tmp, path)


def generate(opts):
    global MODS_DIR
    MODS_DIR = opts.mods or os.path.join(opts.game, 'mods')
    seed = opts.seed or '%d' % (time.time_ns() % 10**10)
    logic = opts.logic
    mixed = opts.entrances != 'off' and opts.entrance_pools == 'mixed'
    if (opts.entrances in ('overworld', 'all') or mixed) and logic != 'none':
        logic = 'none'
        print('entrances %s, %s pools: the logic only knows the vanilla field, so it is off for this seed'
              % (opts.entrances, opts.entrance_pools))
    rom = RomFS(os.path.join(opts.game, 'romfs'))
    actors = {p[len('Pack/Actor/'):-len('.pack.zs')] for p in rom.idx if p.startswith('Pack/Actor/') and p.endswith('.pack.zs')}
    order = mod_order(opts.game)
    below = order[order.index(MOD_NAME) + 1:] if MOD_NAME in order else [m for m in order if m != MOD_NAME]

    stages, version, source, before = {}, {}, {}, {}
    for path in stage_files(rom):
        data, src = load_stage(rom, opts.game, path, below)
        version[path], stages[path] = byml_read(data)
        source[path] = src
        before[path] = copy.deepcopy(stages[path])
    key = '|'.join([seed, logic, opts.chests, opts.drops, opts.maps_compasses, opts.enemies, opts.entrances]
                   + (['mixed'] if opts.entrance_pools == 'mixed' else [])
                   + (['any_age'] if opts.dungeon_ages == 'any' else []))   # separate pools keep older seeds' results
    rnd = random.Random(hashlib.sha256(key.encode()).digest())

    report = []
    if opts.entrances != 'off':
        conns, moved = shuffle_entrances(stages, opts.entrances, rnd, opts.entrance_pools == 'mixed',
                                         opts.dungeon_ages == 'any')
        for d, vias in moved.items():   # a dungeon behind another's door needs what reaching that door needs too
            area_needs[d] = set(AREA_NEEDS.get(d, set())).union(*(AREA_NEEDS.get(v, set()) for v in vias))
        report += ['', 'Entrances (%d connections):' % len(conns)]
        report += ['  %s %s <-> %s %s' % (a[0], a[1], b[0], b[1]) for a, b in conns]
    if opts.enemies != 'off':
        swaps = shuffle_enemies(stages, opts.enemies, rnd)
        report += ['', 'Enemies (%d changed):' % len(swaps)]
        report += ['  %s %016x: %s -> %s' % s for s in swaps]

    locs = find_locations(stages, opts.drops != 'off')
    if opts.chests == 'off':
        locs = [l for l in locs if l.kind != 'chest']
    drop_items = {l.vanilla for l in locs if l.kind == 'drop'}
    pools = [locs] if opts.drops != 'own_pool' else [[l for l in locs if l.kind == 'chest'], [l for l in locs if l.kind == 'drop']]
    for attempt in range(200):
        for l in locs:
            l.item = None
        ok = all(shuffle(p, opts, rnd, logic, lambda it: droppable(it, drop_items, actors)) for p in pools if p)
        if ok and (logic == 'none' or beatable(locs)):
            break
    else:
        sys.exit('no placement found for seed %s' % seed)
    for l in locs:
        if l.item == l.vanilla:
            continue
        for path, i in l.places:
            dyn = stages[path]['Actors'][i]['Dynamic']
            dyn['Drop__DropActor'] = (T_STR, item_for(l, l.item, actors))
            dyn.pop('GetEventName', None)   # Navi's comment was on the old item

    # the new files go to a staging folder first and replace the mod's romfs folder in one step at the end, so the
    # folder never holds half a seed. With --pending they stay in romfs.new (and the spoiler in spoiler.new): the game
    # indexes its mods' files at start, so a running game moves them into place at its next start (randomizer.c).
    out = os.path.join(MODS_DIR, MOD_NAME)
    stage = os.path.join(out, 'romfs.new')
    if os.path.isdir(stage):
        shutil.rmtree(stage)
    changed = [p for p in sorted(stages) if stages[p] != before[p]]
    for path in changed:
        data = byml_write(version[path], stages[path])
        assert byml_read(data)[1] == stages[path]
        dst = os.path.join(stage, path.replace('/', os.sep))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'wb') as f:
            f.write(_zc(data))
    os.makedirs(stage, exist_ok=True)
    if not opts.pending:
        old = os.path.join(out, 'romfs')
        if os.path.isdir(old):
            shutil.rmtree(old)
        os.replace(stage, old)
    lines = ['Randomizer seed %s' % seed,
             'Options: logic %s, chests %s, drops %s, maps and compasses %s, enemies %s, entrances %s (%s pools, dungeons of %s age)'
             % (logic, opts.chests, opts.drops, opts.maps_compasses, opts.enemies, opts.entrances, opts.entrance_pools,
                opts.dungeon_ages),
             '%d chests, %d drops, %d changed stage files' % (sum(l.kind == 'chest' for l in locs),
                                                               sum(l.kind == 'drop' for l in locs), len(changed))]
    lines += ['Stage files taken from the mod %s: %s' % (m, ', '.join(p for p in changed if source[p] == m))
              for m in sorted({source[p] for p in changed if source[p]})]
    lines += ['', 'Where the progression items are:']
    lines += ['  %-18s %s' % (NAMES[it], l.name()) for it in PROGRESSION for l in locs if l.item == it]
    lines += report
    lines += ['', 'Every item location:']
    lines += ['  %s: %s (was %s)' % (l.name(), l.item, l.vanilla) for l in locs]
    sp = os.path.join(out, 'spoiler.new' if opts.pending else 'spoiler.txt')
    with open(sp, 'w', encoding='utf-8') as f:   # empty with --no-spoiler, so an old seed's spoiler never stays
        f.write('' if opts.no_spoiler else '\n'.join(lines) + '\n')
    print('seed %s: %d item locations, %d stage files written to %s' % (seed, len(locs), len(changed), out))
    if MOD_NAME in order and any(order.index(m) < order.index(MOD_NAME) for m in {source[p] for p in changed if source[p]}):
        print('note: move %s above the mods it builds on in the Mods order' % MOD_NAME)
    return 'seed %s, %d item locations, %d stage files' % (seed, len(locs), len(changed))


if __name__ == '__main__':
    main()
