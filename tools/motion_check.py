#!/usr/bin/env python3
"""Score per-frame engine motion probes for Kurt, Max and Doc, including input suppression twins."""
import argparse
import math
from pathlib import Path
import re

PATTERN = re.compile(r'^lua: qa-motion (l[123]_(?:idle|forward|back|left|right|jump)) type (-?\d+) frames (\d+) bad (\d+) planar (\S+) rise (\S+) fall (\S+) dx (\S+) dy (\S+)$', re.M)
CASES = ('idle', 'forward', 'back', 'left', 'right', 'jump')


def check(logs):
    errors = []
    if len(logs) < 2:
        errors.append('at least two real game logs are required')
    for game, log in enumerate(logs, 1):
        rows = {}
        for match in PATTERN.finditer(log.replace('\r', '')):
            tag, kind, frames, bad, *numbers = match.groups()
            if tag in rows:
                errors.append(f'game {game}: duplicate {tag}')
            try:
                rows[tag] = (int(kind), int(frames), int(bad), *map(float, numbers))
            except ValueError:
                errors.append(f'game {game}: malformed probe {tag}')
        if 'LUA ERROR' in log:
            errors.append(f'game {game}: Lua error')
        for level in range(1, 4):
            for case in CASES:
                tag = f'l{level}_{case}'
                row = rows.get(tag)
                if row is None:
                    errors.append(f'game {game}: missing {tag}')
                    continue
                kind, frames, bad, planar, rise, fall, dx, dy = row
                if kind != 99 + level or frames < 20 or bad or min(planar, rise, fall) < 0 or not all(math.isfinite(v) for v in row):
                    errors.append(f'game {game}: invalid probe {tag}')
                    continue
                if case == 'idle' and max(planar, rise, fall) > 0.2:
                    errors.append(f'game {game}: {tag} fixture is moving without input')
                elif case in ('forward', 'back', 'left', 'right') and planar < 0.5:
                    errors.append(f'game {game}: {tag} did not move')
                elif case == 'jump' and rise < 0.25:
                    errors.append(f'game {game}: {tag} did not rise')
            for a, b in (('forward', 'back'), ('left', 'right')):
                first, second = rows.get(f'l{level}_{a}'), rows.get(f'l{level}_{b}')
                if first and second and first[-2] * second[-2] + first[-1] * second[-1] >= -0.25:
                    errors.append(f'game {game}: level {level} {a}/{b} are not opposite directions')
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('logs', nargs='+', type=Path)
    args = parser.parse_args()
    errors = check([path.read_text(encoding='latin-1') for path in args.logs])
    for error in errors:
        print('FAIL ', error)
    print(f'motion: {len(args.logs)} games, {len(errors)} failures')
    return bool(errors)


if __name__ == '__main__':
    raise SystemExit(main())
