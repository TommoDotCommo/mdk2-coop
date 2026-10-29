#!/usr/bin/env python3
"""Mine the game's own level scripts into a progression model per level, for the campaign bot.

The installed game's scripts are the authoritative description of how each level is played: its
rooms (with bounding boxes), doors, checkpoints, pickups, enemies, and the script handlers that
advance it. Nothing from the game is stored in this repository: the model is written under
local/campaign/ from a game copy at run time.

    campaign_mine.py --scripts DIR [--out local/campaign]     DIR: the game's .lua files
    campaign_mine.py --tar <game dir>/mdk2.tar                  read them from the game archive

Model per level (levelN.json): rooms {name: {min, max, center}}, portals [[a, b, point]], objects
[{name, type, room, pos}], checkpoints {cp: {name, pos, section, delete, prevcp}}, events (script
handlers by object), exits (mdkNewGame calls).
"""
import argparse
import io
import json
import re
import tarfile
from pathlib import Path

NUM = r'(-?[\d.]+(?:e[-+]?\d+)?)'
REG = re.compile(r"mdkRegisterObject\('([^']+)',\s*(OBJ_[A-Z0-9_]+),\s*scene,\s*([^,]+),\s*(-?\d+),\s*"
                 + r',\s*'.join([NUM] * 3) + r",\s*" + r',\s*'.join([NUM] * 4) + r",\s*('[^']*'|nil),"
                 r"[^{]*?(?:\{([^}]*)\},\s*\{([^}]*)\}|nil,\s*nil)")
POINT = re.compile(r'points\.([A-Za-z0-9_]+)\s*=\s*\{x=' + NUM + r',y=' + NUM + r',z=' + NUM)
CP = re.compile(r'Level\["scenegraph"\]\["checkpoints"\]\[(\d+)\](.*?)=\s*(.+)')
HANDLER = re.compile(r'^([A-Za-z0-9_]+)\.(On[A-Za-z]+)\s*=', re.M)
EXIT = re.compile(r'mdkNewGame\((\d+),\s*(\d+)')
COMBO = re.compile(r'left\s*==\s*(OBJ_[A-Z_]+)\)\s*and\s*\(right\s*==\s*(OBJ_[A-Z_]+)')

# Object kinds the bot cares about, by OBJ_ name.
PICKUP = re.compile(r'^OBJ_(UZI|SHOTGUN|MAGNUM|DOUBLEA|CARBATTERY|BOXINGGLOVE|JETPACK|ATOMICJETPACK|LASERCANNON|'
                    r'SUPERCHAINGUN|FIREBALLCHAINGUN|GUIDEDROCKET|BFB|GRENADEITEM|SNIPER\w*ITEM|SNIPERSHIELD|CLOAK|'
                    r'DECOYITEM|HAM|APPLE|BAGUETTE|PUMPERNICKEL|POP|BOOZE|TOASTER|LOAF|PIPES|HANDDRYER|DUCTTAPE|MAGNET|'
                    r'FISHBOWL|TOWELS|PLUTONIUM|POSDOO|DIMDES|KURTCOORD)$')
ENEMY_SPAWN = re.compile(r'^OBJ_(SPAWNER|DOGANBOYGENERATOR|SAMSMITEGENERATOR)$')


def level_files(names):
    """levelN -> (lN script with registrations, levelN script with logic). Levels 11 and 12 reuse l10."""
    out = {}
    for n in range(1, 13):
        logic = f'level{n}.lua'
        geo = f'l{min(n, 10)}.lua'
        if logic in names and geo in names:
            out[n] = (geo, logic)
    return out


def mine_level(geo, logic):
    rooms, objects, points = {}, [], {}
    for m in REG.finditer(geo):
        name, kind, parent = m.group(1), m.group(2), m.group(3).strip()
        pos = [float(m.group(i)) for i in (5, 6, 7)]
        if kind == 'OBJ_ROOM' and m.group(13):
            lo = [float(v) for v in m.group(13).split(',')]
            hi = [float(v) for v in m.group(14).split(',')]
            rooms[name] = {'min': lo, 'max': hi, 'center': [(a + b) / 2 for a, b in zip(lo, hi)]}
        elif kind not in ('OBJ_STATICLIGHT', 'OBJ_AMBIENTSOUND'):
            objects.append({'name': name, 'type': kind, 'room': None if parent == 'nil' else parent, 'pos': pos})
    for text in (geo, logic):
        for m in POINT.finditer(text):
            points[m.group(1)] = [float(m.group(i)) for i in (2, 3, 4)]
    checkpoints = {}
    for m in CP.finditer(logic):
        cp, path, value = int(m.group(1)), m.group(2).strip(), m.group(3).strip().strip('"')
        c = checkpoints.setdefault(cp, {'delete': []})
        if path == '[1]':
            c['name'] = value
        elif re.fullmatch(r'\[2\]\[[123]\]', path):
            c.setdefault('pos', [None] * 3)[int(path[4]) - 1] = float(value)
        elif path == '[3]':
            c['section'] = value
        elif path.startswith('["delete"]'):
            c['delete'].append(value)
        elif path == '["prevcp"]':
            c['prevcp'] = int(value)
    events = {}
    for m in HANDLER.finditer(logic):
        events.setdefault(m.group(1), []).append(m.group(2))
    return {
        'rooms': rooms,
        'portals': portals(rooms, objects),
        'objects': objects,
        'points': points,
        'checkpoints': {str(k): v for k, v in sorted(checkpoints.items())},
        'events': events,
        'exits': sorted({(int(a), int(b)) for a, b in EXIT.findall(logic)}),
        'combos': sorted(set(COMBO.findall(logic))),
        'pickups': [o for o in objects if PICKUP.match(o['type'])],
        'spawners': [o for o in objects if ENEMY_SPAWN.match(o['type'])],
        'checkpoint_objects': [o for o in objects if o['type'] == 'OBJ_CHECKPOINT'],
        'doors': [o for o in objects if 'DOOR' in o['type']],
    }


def overlap(a, b, slack=1.0):
    lo = [max(a['min'][i], b['min'][i]) - slack for i in range(3)]
    hi = [min(a['max'][i], b['max'][i]) + slack for i in range(3)]
    if all(lo[i] <= hi[i] for i in range(3)):
        return [(lo[i] + hi[i]) / 2 for i in range(3)]
    return None


def inside(room, p, slack=0.5):
    return all(room['min'][i] - slack <= p[i] <= room['max'][i] + slack for i in range(3))


def portals(rooms, objects):
    """Rooms whose boxes touch are joined at the middle of the shared region; a door joins the
    rooms its position lies in."""
    out = []
    names = sorted(rooms)
    for i, a in enumerate(names):
        for b in names[i + 1:]:
            p = overlap(rooms[a], rooms[b])
            if p:
                out.append([a, b, [round(v, 2) for v in p], 'touch'])
    for d in objects:
        if 'DOOR' not in d['type']:
            continue
        holders = [n for n in names if inside(rooms[n], d['pos'], 3)]
        for i, a in enumerate(holders):
            for b in holders[i + 1:]:
                out.append([a, b, d['pos'], 'door:' + d['name']])
    return out


def read_scripts(args):
    if args.tar:
        with tarfile.open(args.tar) as t:
            return {m.name: t.extractfile(m).read().decode('latin-1')
                    for m in t.getmembers() if m.name.endswith('.lua') and m.isfile()}
    return {p.name: p.read_text(encoding='latin-1') for p in Path(args.scripts).glob('*.lua')}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument('--scripts')
    src.add_argument('--tar')
    ap.add_argument('--out', default=str(Path(__file__).resolve().parents[1] / 'local' / 'campaign'))
    args = ap.parse_args(argv)
    scripts = read_scripts(args)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    for n, (geo, logic) in level_files(scripts).items():
        model = mine_level(scripts[geo], scripts[logic])
        model['level'] = n
        (out / f'level{n}.json').write_text(json.dumps(model, indent=1))
        print(f'level {n}: {len(model["rooms"])} rooms, {len(model["portals"])} portals, '
              f'{len(model["pickups"])} pickups, {len(model["checkpoints"])} checkpoints, '
              f'{len(model["checkpoint_objects"])} checkpoint objects, {len(model["doors"])} doors, '
              f'{len(model["events"])} scripted objects, exits {model["exits"]}, combos {len(model["combos"])}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
