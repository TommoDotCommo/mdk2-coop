#!/usr/bin/env python3
"""Game-free check of the Steam Input layout in dist/steam-input/ (Steam Deck, standard K9).

Parses the VDF and asserts that the layout presents an Xbox gamepad for every physical button (only xinput_button
bindings, no keyboard or mouse output), that the back buttons are L4 = View (the co-op layer) and R4 = right stick
click (push to talk) with L5 and R5 unbound, and that the title names the layer. Then it breaks the layout on
purpose (mutants) and requires each break to be caught.

Usage: test_steam_input.py [layout.vdf]"""
from pathlib import Path
import re
import sys

DEFAULT = Path(__file__).resolve().parent.parent / 'dist' / 'steam-input' / 'mdk2-coop_deck.vdf'
TOKEN = re.compile(r'"((?:[^"\\]|\\.)*)"|([{}])')


def parse(text):
    """VDF to nested lists of (key, value) pairs: a block is a list, a string is a str."""
    stack = [[]]
    pending = None
    for m in TOKEN.finditer(text):
        s, brace = m.group(1), m.group(2)
        if brace == '{':
            block = []
            stack[-1].append((pending, block))
            stack.append(block)
            pending = None
        elif brace == '}':
            stack.pop()
        elif pending is None:
            pending = s
        else:
            stack[-1].append((pending, s))
            pending = None
    if len(stack) != 1:
        raise ValueError('unbalanced braces')
    return stack[0]


def get(block, key):
    return [v for k, v in block if k == key]


def walk_bindings(block, path=()):
    for k, v in block:
        if isinstance(v, list):
            yield from walk_bindings(v, path + (k,))
        elif k == 'binding':
            yield path, v


def button_bindings(root):
    """input name (button_back_left_upper...) -> list of bindings, from the 'switches' group"""
    out = {}
    for group in get(root, 'group'):
        if get(group, 'mode') == ['switches']:
            for inputs in get(group, 'inputs'):
                for name, node in inputs:
                    out[name] = [b for _, b in walk_bindings(node)]
    return out


def check(text):
    """Return the list of problems; empty when the layout is as specified."""
    problems = []
    try:
        top = get(parse(text), 'controller_mappings')[0]
    except (ValueError, IndexError) as e:
        return ['not a controller_mappings VDF: %s' % e]
    if get(top, 'controller_type') != ['controller_neptune']:
        problems.append('controller_type is not controller_neptune (Steam Deck)')
    if get(top, 'title') != ['MDK2 Co-op (pad layer)']:
        problems.append('title is not "MDK2 Co-op (pad layer)"')
    desc = (get(top, 'description') or [''])[0]
    for word in ('L4', 'View', 'R4', 'talk', 'ping', 'player list'):
        if word not in desc:
            problems.append('description does not mention %r' % word)
    allb = [b for _, b in walk_bindings(top)]
    other = sorted({b for b in allb if not b.lower().startswith('xinput_button ')})
    if other:
        problems.append('non-gamepad bindings (the layout must present an Xbox pad only): %s' % other)
    have = {b.lower().split(' ', 1)[1] for b in allb if b.lower().startswith('xinput_button ')}
    for need in ('a', 'b', 'x', 'y', 'dpad_up', 'dpad_down', 'dpad_left', 'dpad_right', 'shoulder_left',
                 'shoulder_right', 'start', 'select', 'joystick_left', 'joystick_right'):
        if need not in have:
            problems.append('Xbox button missing: %s' % need)
    sw = button_bindings(top)
    want = {'button_back_left_upper': ['xinput_button select'],            # L4 = View
            'button_back_right_upper': ['xinput_button joystick_right'],   # R4 = right stick click
            'button_back_left': [], 'button_back_right': []}               # L5, R5 unbound
    for name, exp in want.items():
        got = [b.lower() for b in sw.get(name, ['<missing>'])]
        if got != exp:
            problems.append('%s is %s, expected %s' % (name, got, exp))
    if sw.get('button_menu', []) != ['xinput_button select']:
        problems.append('the Deck View button (button_menu) must stay select')
    if sw.get('button_escape', []) != ['xinput_button start']:
        problems.append('the Deck Menu button (button_escape) must stay start')
    return problems


def main():
    path = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT
    text = path.read_text(encoding='utf-8-sig')
    fails = 0
    p = check(text)
    print('layout %s: %s' % (path.name, 'ok' if not p else '; '.join(p)))
    fails += bool(p)
    mutants = {
        'L4 is not View': lambda t: re.sub(r'("button_back_left_upper".*?"binding"\s*"xinput_button )select', r'\1start', t,
                                           count=1, flags=re.S),
        'R4 unbound': lambda t: re.sub(r'("button_back_right_upper".*?"bindings"\s*\{\s*)"binding"\s*"[^"]*"', r'\1', t,
                                       count=1, flags=re.S),
        'L5 bound': lambda t: re.sub(r'("button_back_left"\s*\{.*?"bindings"\s*\{)', r'\1"binding" "xinput_button A"', t, count=1, flags=re.S),
        'keyboard binding': lambda t: t.replace('xinput_button A"', 'key_press SPACE"', 1),
        'not a Deck layout': lambda t: t.replace('controller_neptune', 'controller_xboxone', 1),
        'wrong title': lambda t: t.replace('MDK2 Co-op (pad layer)', 'Gamepad', 1),
    }
    for name, mutate in mutants.items():
        bad = mutate(text)
        if bad == text:
            print('mutant %r did not change the layout (test bug)' % name)
            fails += 1
            continue
        found = check(bad)
        print('mutant %r: %s' % (name, 'caught' if found else 'NOT CAUGHT'))
        fails += not found
    print('steam-input: %s' % ('FAILED' if fails else 'passed'))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
