#!/usr/bin/env python3
"""Native T0 execution of shipped core, mutation twins, simulator and fuzz cases."""
from pathlib import Path
import os
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'bridge/combat_core.c').read_text()
MUTANTS = {
    'sender': ('(host ? sender != slot : sender != 0)', '0'),
    'slot': ('slot >= CB_PLAYERS', 'slot >= CB_PLAYERS - 1'),
    'epoch': ('if (e->epoch != epoch)', 'if (0)'),
    'id': ('!e->id ||', '0 ||'),
    'target': ('!e->target ||', '0 ||'),
    'kind-min': ('e->kind < 200', 'e->kind < 199'),
    'kind-max': ('e->kind > 259', 'e->kind > 260'),
    'zero-damage': ('!e->amount ||', '0 ||'),
    'impossible-damage': ('e->amount > 32767', 'e->amount > 65535'),
    'weapon': ('e->weapon > 4095', '0'),
    'flags': ('(e->flags & ~(CB_KILL | CB_CRIT))', '0'),
    'reserved': ('|| e->reserved', '|| 0'),
    'finite': ('!isfinite(e->pos[i])', '0'),
    'world-bound': ('fabsf(e->pos[i]) > 1000000', '0'),
    'clamp': ('e->amount = CB_MAX_DAMAGE;', 'e->amount = e->amount;'),
    'dedupe': ('(g->seen[slot] & (UINT64_C(1) << delta))', '0'),
    'stale-id': ('if (delta >= 64 ||', 'if (delta >= 64) return 1; if (delta >= 64 ||'),
    'rate': ('g->count[slot] >= CB_RATE', 'g->count[slot] > CB_RATE'),
    'live-target': ('e->target != target', '0'),
    'live-kind': ('e->kind != kind', '0'),
    'live-hp': ('hp <= 0', 'hp < 0'),
    'live-position': ('fabsf(e->pos[i] - pos[i]) > 64', '0'),
    'live-finite': ('!isfinite(pos[i])', '0'),
    'attack-type-min': ('type > 0', 'type >= 0'),
    'attack-type-max': ('type <= 65535', 'type <= 65536'),
    'attack-part-min': ('part >= -1', 'part >= -2'),
    'attack-part-max': ('part <= 64', 'part <= 65'),
    'history-bound': ('if (s->size < CB_KEEP)', 'if (1)'),
    'rolling-expiry': ('tick - s->ticks[i] < 50', 'tick - s->ticks[i] <= 50'),
    'fight-idle': ('now - s->last >= CB_IDLE', 'now - s->last > CB_IDLE'),
    'layout-overflow': ('l.y = h * 36 / 100', 'l.y = h * 70 / 100'),
}

def run():
    with tempfile.TemporaryDirectory(prefix='combat-') as folder:
        folder = Path(folder)
        for name, twin in [('baseline', None), *MUTANTS.items()]:
            source = folder / f'{name}.c'
            code = SOURCE
            if twin:
                old, new = twin
                if old not in code:
                    raise SystemExit(f'FAIL missing mutation site: {name}')
                code = code.replace(old, new)
            source.write_text(code)
            binary = folder / name
            subprocess.run([os.environ.get('CC_NATIVE', 'cc'), '-std=c99', '-O1', '-g',
                            '-fsanitize=undefined', '-fno-sanitize-recover=all',
                            '-I', str(ROOT / 'bridge'), str(ROOT / 'tools/test_combat.c'),
                            str(source), '-lm', '-o', str(binary)], check=True, timeout=30)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            if twin:
                if not result.returncode or 'FAIL line' not in result.stderr:
                    raise SystemExit(f'FAIL mutant not assertion-killed: {name}: {result.stderr}')
                print(f'PASS mutant caught: {name}')
            else:
                print(result.stdout, end='')
                if result.returncode:
                    raise SystemExit(result.stderr)
        print(f'PASS {len(MUTANTS)} combat mutation twins')
        adapter = (ROOT / 'bridge/combat.c').read_text()
        twins = {
            'source-budget': ('if (!cb_validate(&outgoing, &c, my_slot, CB_REPORT, cur_epoch, clock_ms())) return;', 'cb_validate(&outgoing, &c, my_slot, CB_REPORT, cur_epoch, clock_ms());'),
            'host-only-apply': ('!role_host || sender <= 0', 'sender <= 0'),
            'live-puppet': ('!gob_alive(puppets[sender])', '0'),
            'host-only-receive': ('|| e->epoch != cur_epoch) return;', ') return;'),
            'reset-dedupe': ('combat.started = 0; dirty = 1;', 'combat.started = 0; dirty = 1; memset(&combat.guard, 0, sizeof(combat.guard));'),
        }
        for name, twin in [('adapter', None), *twins.items()]:
            code = adapter
            if twin:
                old, new = twin
                if code.count(old) != 1:
                    raise SystemExit(f'FAIL adapter mutation site: {name}')
                code = code.replace(old, new)
            source = folder / f'{name}.c'
            source.write_text(code)
            binary = folder / name
            subprocess.run([os.environ.get('CC_NATIVE', 'cc'), '-std=c99', '-O1', '-g',
                            '-fsanitize=undefined', '-fno-sanitize-recover=all',
                            '-Wno-unused-parameter', '-I', str(ROOT / 'bridge'),
                            '-DCOMBAT_SOURCE="' + str(source) + '"',
                            str(ROOT / 'tools/test_combat_adapter.c'),
                            str(ROOT / 'bridge/combat_core.c'), '-lm', '-o', str(binary)],
                           check=True, timeout=30)
            result = subprocess.run([str(binary)], cwd=folder, capture_output=True, text=True, timeout=30)
            if twin:
                if not result.returncode or 'FAIL line' not in result.stderr:
                    raise SystemExit(f'FAIL adapter mutant not assertion-killed: {name}: {result.stderr}')
                print(f'PASS adapter mutant caught: {name}')
            else:
                print(result.stdout, end='')
                if result.returncode:
                    raise SystemExit(result.stderr)

if __name__ == '__main__':
    run()
