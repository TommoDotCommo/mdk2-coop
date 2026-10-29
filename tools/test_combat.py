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
    'zero-damage': ('(!e->amount && host != CB_REPORT) ||', '0 ||'),
    'zero-report': ('(!e->amount && host != CB_REPORT) ||', '!e->amount ||'),
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
}
HUD_MUTANTS = {
    'no-merge': ('q->used && !q->kill &&', '0 && q->used && !q->kill &&'),
    'merge-any-enemy': ('q->target == e->target &&', '1 &&'),
    'merge-any-player': ('q->attacker == e->attacker &&', '1 &&'),
    'wrong-colour': ('pick->attacker = e->attacker;', 'pick->attacker = 0;'),
    'merge-forever': ('now - q->born <= CBH_MERGE_CAP_MS', '1'),
    'merge-window': ('now - q->last <= CBH_MERGE_MS &&', 'now - q->last <= CBH_MERGE_MS * 10 &&'),
    'merge-amount': ('p->amount += e->amount;', 'p->amount = e->amount;'),
    'merge-into-kill': ('!q->kill &&', '1 &&'),
    'kill-flag-lost': ('if (kill) p->kill = 1;', ''),
    'kill-life': ('p->kill ? CBH_KILL_MS : CBH_POP_MS', 'CBH_POP_MS'),
    'kill-style': ('out->size = p->kill ? 2 :', 'out->size = p->kill ? 1 :'),
    'pop-expiry': ('!p->used || age >= life', '!p->used'),
    'hitmarker-teammate': ('if (mine) {', 'if (1) {'),
    'hit-expiry': ('if (age >= life) return 0;', ''),
    'ticker-merge': ('t->attacker == e->attacker && t->kind == e->kind', '1'),
    'ticker-bound': ('if (s->nticks < CBH_TICKS) s->nticks++;', 's->nticks++;'),
    'rank-order': ('s->fight[all[i - 1]] < s->fight[p]', 's->fight[all[i - 1]] > s->fight[p]'),
    'fade-stays': ('float f = 1.0f - (float)(now - end) / (float)CBH_FADE_MS;', 'float f = 1.0f;'),
    'no-peek': ('if (h->peek_until > now) a = 1;', ''),
    'fight-time': ('now - s->last >= CB_IDLE ? s->last : now', 'now'),
    'enemy-fallback': ('default: return "Enemy";', 'default: return "";'),
    'enemy-raw-id': ('case 207: return "Doganboy";', 'case 207: return "T207";'),
    'count-format': ('if (v < 1000) snprintf', 'if (v < 1000000) snprintf'),
    'time-format': ('"%u:%02u"', '"%u:%u"'),
    'layout-overlap': ('l.ticker.y = l.panel.y + l.panel.h + l.pad;', 'l.ticker.y = l.panel.y;'),
    'layout-wide': ('l.panel_max.w = l.panel.w = w * 15 / 100;', 'l.panel_max.w = l.panel.w = w * 30 / 100;'),
    'layout-tall': ('maxh = h * 20 / 100', 'maxh = h * 40 / 100'),
    'layout-zone': ('top = h * 36 / 100', 'top = h * 70 / 100'),
    'layout-columns': ('l.total_x = l.name_x + l.name_w + l.pad;', 'l.total_x = l.name_x;'),
    'layout-text-fit': ('l.dps_w = cbh_text_w(5, px_big);', 'l.dps_w = cbh_text_w(1, px_big);'),
    'layout-edge-margin': ('int margin = w * 25 / 1000 < 12 ? 12 : w * 25 / 1000', 'int margin = 0'),
    'layout-dialog-zone': ('top = h * 36 / 100', 'top = h * 22 / 100'),
    'layout-no-column-row': ('l.header_h = 2 * l.line_h + l.pad;', 'l.header_h = l.line_h + l.pad;'),
    'avoid-none': ('dy = tags[i][1] - gap - y1;', 'dy = dy;'),
    'avoid-down': ('dy = tags[i][1] - gap - y1;', 'dy = tags[i][3] + gap - y0;'),
    'avoid-one-pass': ('for (pass = 0; pass <= n; pass++)', 'for (pass = 0; pass < 1; pass++)'),
    'layout-collapsed': ('l.panel.h = expanded ? ', 'l.panel.h = 1 ? '),
    'layout-hitmarker': ('l.hit.x = (w - l.hit.w) / 2;', 'l.hit.x = w - l.hit.w;'),
}


def run_hud(folder):
    source = (ROOT / 'bridge/combat_hud.c').read_text()
    for name, twin in [('hud', None), *HUD_MUTANTS.items()]:
        code = source
        if twin:
            old, new = twin
            if code.count(old) != 1:
                raise SystemExit(f'FAIL missing hud mutation site: {name}')
            code = code.replace(old, new)
        path = folder / f'hud_{name}.c'
        path.write_text(code)
        binary = folder / f'hud_{name}'
        subprocess.run([os.environ.get('CC_NATIVE', 'cc'), '-std=c99', '-O1', '-g',
                        '-fsanitize=undefined', '-fno-sanitize-recover=all',
                        '-I', str(ROOT / 'bridge'), str(ROOT / 'tools/test_combat_hud.c'),
                        str(path), str(ROOT / 'bridge/combat_core.c'), '-lm', '-o', str(binary)],
                       check=True, timeout=30)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
        if twin:
            if not result.returncode or 'FAIL line' not in result.stderr:
                raise SystemExit(f'FAIL hud mutant not assertion-killed: {name}: {result.stderr}')
            print(f'PASS hud mutant caught: {name}')
        else:
            print(result.stdout, end='')
            if result.returncode:
                raise SystemExit(result.stderr)
    print(f'PASS {len(HUD_MUTANTS)} combat HUD mutation twins')


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
            'hud-feed-host': ('    cbh_note(&hud, &c, attacker == my_slot, clock_ms());\n', ''),
            'hud-feed-client': ('        cbh_note(&hud, &c, c.attacker == my_slot, clock_ms());\n', ''),
            'hud-mine': ('cbh_note(&hud, &c, c.attacker == my_slot, clock_ms());', 'cbh_note(&hud, &c, 1, clock_ms());'),
            'toggle-peek': ('{ meter = !meter; cbh_peek(&hud, clock_ms()); }', '{ meter = !meter; }'),
            'meter-default': ('static int meter, keys[3];', 'static int meter = 1, keys[3];'),
            'epoch-hud': ('cbh_reset(&hud); epoch = cur_epoch;', 'epoch = cur_epoch;'),
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
                            str(ROOT / 'bridge/combat_core.c'), str(ROOT / 'bridge/combat_hud.c'),
                            '-lm', '-o', str(binary)],
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
        run_hud(folder)

if __name__ == '__main__':
    run()
