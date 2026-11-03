#!/usr/bin/env python3
"""Run production portable presence policy and require broken variants to fail."""
from pathlib import Path
import subprocess
import os
import tempfile

ROOT = Path(__file__).resolve().parents[1]
game = (ROOT/'bridge/game.c').read_text()
slots = game[game.index('#define FILTER_SLOTS'):game.index('/* Redirect fn to hook')]
header = (ROOT/'bridge/bridge.h').read_text()
constants = '\n'.join(line for line in header.splitlines()
                      if line.startswith(('#define CLASS_ANIMEVENT ', '#define CLASS_COLLISION ')))
with tempfile.TemporaryDirectory(dir=ROOT/'local', prefix='ai-slots-') as folder:
    folder = Path(folder)
    (folder/'ai_slot_constants.h').write_text(constants)
    for name, variant in [('baseline', slots),
                          ('wrong-strike-slot', slots.replace('CLASS_ANIMEVENT }', 'CLASS_COLLISION }')),
                          ('unscoped-strike', slots.replace('(f == 0 || f == 3)', '(f == 0)'))]:
        (folder/'ai_slots_source.h').write_text(variant)
        binary = folder/name
        subprocess.run(['gcc','-std=c99','-O1','-fno-pie','-no-pie',
                        '-Wno-pointer-to-int-cast','-Wno-int-to-pointer-cast','-I',str(folder),
                        str(ROOT/'bridge/tests/ai_slots_test.c'),'-o',str(binary)],check=True)
        result = subprocess.run([str(binary)],capture_output=True,text=True)
        if name == 'baseline':
            assert result.returncode == 0, result.stderr
            print(result.stdout,end='')
        else:
            assert result.returncode != 0 and 'Assertion' in result.stderr, name
            print(f'PASS native slot mutant caught: {name}')

source = (ROOT / 'bridge/presence.c').read_text()
mutants = {
    'truncated-canopy': ('if (size < n) return 0;', 'if (0) return 0;'),
    'invalid-presence': ('if (s->present > 1) return 0;', 'if (0) return 0;'),
    'invalid-flags': ('(s->flags & ~0x22)', '0'),
    'invalid-animation': ('s->cur != 66 && s->cur != 67 && s->cur != 255', '0'),
    'nonfinite-animation': ('!isfinite(value)', '0'),
    'gear-truncated': ('if (size - n < need) return 0;', 'if (0) return 0;'),
    'gear-bad-fields': ('if (v->hidden > 1 || v->model > 3 || v->nanims > GEAR_ANIMS) return 0;', 'if (0) return 0;'),
    'gear-bad-animation': ('if ((v->cur != 255 && v->cur >= 160) || (v->prev != 255 && v->prev >= 160)) return 0;', 'if (0) return 0;'),
    'gear-bad-id': ('if (v->id[i] >= 160) return 0;', 'if (0) return 0;'),
    'gear-nonfinite': ('if (!(isfinite(value) && fabsf(value) <= 1000000.0f)) return 0;', 'if (0) return 0;'),
    'gear-infinite-end-rejected': ('if (j == 5) continue;', ''),
    'gear-decode-count': ('if (v->nanims > GEAR_ANIMS) return 0;\n        if (size - n <', 'if (0) return 0;\n        if (size - n <'),
    'gear-hidden-lost': ('(unsigned char)(v->hidden | v->model << 1)', '(unsigned char)(v->model << 1)'),
    'gear-model-lost': ('(unsigned char)(v->hidden | v->model << 1)', '(unsigned char)(v->hidden)'),
    'missing-eligibility': ('if (!players[i].eligible) continue;', 'if (0) continue;'),
    'host-only-target': ('i < count;', 'i < 1;'),
    'no-pull': ('next = now - f->loading_since >= 60 ? JOIN_TIMEOUT : JOIN_LOADING;', 'next = JOIN_WAITING;'),
    'no-loading-timeout': ('now - f->loading_since >= 60', '0'),
    'no-timeout': ('now - f->since >= 60', '0'),
    'no-version-check': ('else if (mismatch)', 'else if (0)'),
    'ignore-cancel': ('cancel || f->phase == JOIN_CANCELLED', '0'),
    'no-disconnect': ('f->seen ? JOIN_LOST : JOIN_CONNECTING', 'JOIN_CONNECTING'),
}
with tempfile.TemporaryDirectory(prefix='mdk-presence-') as folder:
    folder = Path(folder)
    for name, mutation in [('baseline', None), *mutants.items()]:
        variant = source
        if mutation:
            old, new = mutation
            assert variant.count(old) == 1, (name, variant.count(old))
            variant = variant.replace(old, new)
        path = folder / (name + '.c')
        path.write_text(variant)
        binary = folder / name
        subprocess.run(['gcc', '-std=c99', '-O1', '-g', '-Wall', '-Wextra', '-Wno-unused-parameter',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
                        '-I', str(ROOT/'bridge'), str(path),
                        str(ROOT/'bridge/tests/presence_test.c'), '-lm', '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=20,
                                env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
        if not mutation:
            print(result.stdout, end='')
            if result.returncode:
                raise SystemExit(result.stderr)
        else:
            if result.returncode == 0 or not ('Assertion' in result.stderr or 'AddressSanitizer' in result.stderr or
                                              'runtime error' in result.stderr):
                raise SystemExit(f'FAIL mutant not caught: {name}: {result.stderr}')
            print(f'PASS mutant caught: {name}')
print(f'PASS: {len(mutants)} presence mutation twins rejected')

# The Win32 adapter runs against fake engine calls; no game or Windows process.
adapter = (ROOT/'bridge/presence_game.c').read_text()
adapter_mutants = {
    'seed-as-native-query': ('ai_store_target(g, ai_select((const float *)(g + GOB_POSITION), g));',
                             'ai_store_target(g, ai_select((const float *)(g + GOB_POSITION), g)); ai_evidence[i].queries++;'),
    'store-idle-actors': ('if (store_idle && !no_targeting', 'if (!no_targeting'),
    'omit-script-scope': ('ai_scope_enter(gob, 0);\n    return 1;', 'return 1;'),
    'omit-canopy-apply': ('if (no_canopy || !child', 'if (1 || !child'),
    'omit-gear-apply': ('if (slot < 0 || slot >= MAX_PLAYERS || !gob_alive(body)) return;\n    if (no_gear) { gear_release(slot); return; }', 'return;'),
    'gear-any-character': ('if (gob_type(body) != gear_kinds[k].owner) continue;\n        g = gear_child(body, k);\n        if (k == GEAR_SHOWN_ONLY) {', 'g = gear_child(body, k);\n        if (k == GEAR_SHOWN_ONLY) {'),
    'gear-build-every-frame': ('if (!g) { gear_retry[slot][k] = now + 2000; continue; }', 'if (!g) { continue; }'),
    'gear-never-removed': ('if (g && gear_owned[slot][k] && g == gob_by_handle(gear_owned[slot][k])) { gear_kill(g); gear_owned[slot][k] = 0; }', ''),
    'gear-hidden-not-applied': ('(*(WORD *)(g + GOB_FLAGS) & ~0x08) | (v->hidden ? 0x08 : 0));\n        gear_left[slot][k]', '(*(WORD *)(g + GOB_FLAGS) & ~0x08));\n        gear_left[slot][k]'),
    'gear-anim-not-applied': ('if (i < v->nanims) memcpy(st, v->anim[i], sizeof(v->anim[0]));\n            else st[0] = 0;', 'if (0) memcpy(st, v->anim[i], sizeof(v->anim[0]));'),
    'gear-release-leaks': ('if (g && gob_alive(g)) gear_kill(g);\n        gear_owned[slot][k] = 0;\n        gear_left[slot][k] = 0;', 'gear_owned[slot][k] = 0;\n        gear_left[slot][k] = 0;'),
    'gear-subs-not-applied': ('((v->subs >> i) & 1 ? 0x08 : 0));', '(*(WORD *)(sub[i] + GOB_FLAGS) & 0x08));'),
    'gear-subs-not-captured': ('v->subs = gear_subs_hidden(g);', 'v->subs = 0;'),
    'gear-children-left-behind': ('if (c && c != g && gob_alive(c) && *(DWORD *)(c + GOB_TARGET) == me) gob_kill(c);', ''),
    'chute-shown-not-applied': ('*(WORD *)(g + GOB_FLAGS) = (WORD)((*(WORD *)(g + GOB_FLAGS) & ~0x08) | (v->hidden ? 0x08 : 0));\n                gear_left[slot][k]', '(void)0;\n                gear_left[slot][k]'),
    'gear-capture-wrong-model': ('v->model = kind == 2 ? 1 : 0;', 'v->model = 0;'),
    'omit-target-context': ('ai_actor = gob;', 'ai_actor = NULL;'),
    'forget-target-restore': ('ai_actor = saved;', 'ai_actor = gob;'),
    'double-stack-clean': ('"movq %1, %%rsp\\n\\t" /* restore caller stack for both caller-pop and callee-pop */',
                           '"addq $32, %%rsp\\n\\t" /* restore caller stack for both caller-pop and callee-pop */'),
    'pointer-as-target-handle': ('*(DWORD *)(gob + GOB_TARGET) = gob_handle(slot);', '*(DWORD *)(gob + GOB_TARGET) = (DWORD)(uintptr_t)target;'),
    'cached-host-projectile': ('return selected ? selected : target;', 'return target;'),
    'actor-only-projectile': ('no_targeting || !world_shooter(shooter)) return target;', 'no_targeting || !gob_is_actor(shooter)) return target;'),
    'aim-aimless-scenery': ('if (!target && !gob_is_actor(shooter)) return target;', ''),
    'omit-stored-target': ('*(DWORD *)(gob + GOB_TARGET) = gob_handle(slot);', '(void)target;'),
    'omit-handle-scope': ('if (target == local_player || puppet_slot(target) >= 0) PLAYER_HANDLE = h;\n        AI_TARGET_HANDLE = h;',
                          '(void)h;'),
    'forget-handle-restore': ('PLAYER_HANDLE = ai_stack[ai_depth].player_handle;', '(void)0;'),
    'wrong-level-target': ('!player_in_level(s)', '0'),
    'downed-target': ('(s == my_slot ? revive_life() : player_life(s)) != LIFE_ALIVE', '0'),
    'ignore-scripted-target': ('(fallback && fallback != local_player && puppet_slot(fallback) < 0)', '0'),
    'infoless-target': ('if (ai_guard_on && (!target || !*(BYTE **)(target + GOB_INFO))) {', 'if (ai_guard_on && !target) {'),
    'aoe-evidence': ('caller == (void *)0x434dfb', '1'),
    'drop-client-receipt': ('local_direct_hits++;', 'local_direct_hits += 0;'),
}
with tempfile.TemporaryDirectory(prefix='mdk-presence-engine-') as folder:
    folder = Path(folder)
    for name, mutation in [('baseline', None), *adapter_mutants.items()]:
        variant = adapter
        if mutation:
            old, new = mutation
            assert variant.count(old) == 1, name
            variant = variant.replace(old, new)
        path = folder/(name+'.c')
        path.write_text(variant)
        binary = folder/name
        subprocess.run(['gcc', '-std=c99', '-O1', '-g', '-Wno-pointer-to-int-cast',
                        '-Wno-int-to-pointer-cast', '-fno-pie', '-no-pie',
                        '-DADAPTER_SOURCE="'+str(path)+'"', '-I', str(ROOT/'bridge'),
                        str(ROOT/'bridge/tests/presence_game_test.c'), str(ROOT/'bridge/presence.c'),
                        '-lm', '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=20)
        if not mutation:
            print(result.stdout, end='')
            if result.returncode: raise SystemExit(result.stderr)
        elif result.returncode == 0 or 'Assertion' not in result.stderr:
            raise SystemExit(f'FAIL adapter mutant not caught: {name}: {result.stderr}')
        else: print(f'PASS adapter mutant caught: {name}')
print(f'PASS: {len(adapter_mutants)} engine adapter mutation twins rejected')

# Compile the production codec/interpolator verbatim without the Windows socket
# adapter. Keeping the boundaries explicit makes a production edit fail loudly.
network = (ROOT/'bridge/net.c').read_text()
codec = network[network.index('#define SNAP_QUANT'):network.index('/* Make gob')]
track = network[network.index('void track_add('):network.index('/* ---- animation state')]
snapshot_mutants = {
    'canopy-not-encoded': ('canopy_encode(&s->canopy, o, (int)(end - o))',
                           '1 /* deliberately omit canopy bytes */'),
    'canopy-not-decoded': ('canopy_decode(p, (int)(end - p), &s->canopy)', '1'),
    'gear-not-encoded': ('gear_encode(&s->gear, o, (int)(end - o))', '1 /* deliberately omit equipment bytes */'),
    'gear-not-decoded': ('gear_decode(p, (int)(end - p), &s->gear)', '1'),
    'gear-lost-on-interpolation': ('*out = t < 0.5f ? *a : *b;', '*out = t < 0.5f ? *a : *b; memset(&out->gear, 0, sizeof(out->gear));'),
    'teleport-blended': ('if (snap_jump(a, b))\n            t->view', 'if (0)\n            t->view'),
    'canopy-lost-on-interpolation': ('*out = t < 0.5f ? *a : *b;',
                                     '*out = t < 0.5f ? *a : *b; out->canopy.present = 0;'),
}
with tempfile.TemporaryDirectory(prefix='mdk-presence-snapshot-') as folder:
    folder = Path(folder)
    for name, mutation in [('baseline', None), *snapshot_mutants.items()]:
        variant = track + codec
        if mutation:
            old, new = mutation
            assert variant.count(old) == 1, name
            variant = variant.replace(old, new)
        path = folder/(name+'.h')
        path.write_text(variant)
        binary = folder/name
        subprocess.run(['gcc', '-std=c99', '-O1', '-g', '-fsanitize=address,undefined',
                        '-fno-pie', '-no-pie', '-DSNAPSHOT_SOURCE="'+str(path)+'"',
                        '-I',str(ROOT/'bridge'), str(ROOT/'bridge/tests/presence_snapshot_test.c'),
                        str(ROOT/'bridge/presence.c'), '-lm', '-o',str(binary)],check=True)
        result = subprocess.run([str(binary)],capture_output=True,text=True,timeout=30,
                                env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
        if not mutation:
            print(result.stdout,end='')
            if result.returncode: raise SystemExit(result.stderr)
        elif result.returncode == 0 or not ('Assertion' in result.stderr or 'AddressSanitizer' in result.stderr):
            raise SystemExit(f'FAIL snapshot mutant not caught: {name}: {result.stderr}')
        else: print(f'PASS snapshot mutant caught: {name}')

header = (ROOT/'bridge/bridge.h').read_text()
event = header[header.index('struct event {'):header.index('/* Players:')]
packet = network[network.index('struct player_packet {'):network.index('/* On the wire')]
packet += network[network.index('static int packet_pack('):network.index('/* settings from coop.ini */')]
macros = '\n'.join(line for line in network.splitlines()
                   if any(line.startswith('#define '+name+' ') for name in
                          ('PLAYER_MAGIC','PACKET_EVENTS','MAX_DATAGRAM','PK_STATE')))
packet_source = macros+'\n'+event+codec+packet
packet_mutants = {
    'drop-oversized-backlog': ('while (!len && reduced.nevents)', 'while (0)'),
    'accept-old-protocol': ('if (p->magic != PLAYER_MAGIC)', 'if (0)'),
}
with tempfile.TemporaryDirectory(prefix='mdk-presence-packet-') as folder:
    folder=Path(folder)
    for name,mutation in [('baseline',None),*packet_mutants.items()]:
        variant=packet_source
        if mutation:
            old,new=mutation
            assert variant.count(old)==1,name
            variant=variant.replace(old,new)
        path=folder/(name+'.h'); path.write_text(variant)
        binary=folder/name
        subprocess.run(['gcc','-std=c99','-O1','-g','-fsanitize=address,undefined',
                        '-fno-pie','-no-pie','-DPACKET_SOURCE="'+str(path)+'"',
                        '-I',str(ROOT/'bridge'),str(ROOT/'bridge/tests/presence_packet_test.c'),
                        str(ROOT/'bridge/presence.c'),'-lm','-o',str(binary)],check=True)
        result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=20,
                              env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
        if not mutation:
            print(result.stdout,end='')
            if result.returncode: raise SystemExit(result.stderr)
        elif result.returncode==0 or not ('Assertion' in result.stderr or 'AddressSanitizer' in result.stderr):
            raise SystemExit(f'FAIL packet mutant not caught: {name}: {result.stderr}')
        else: print(f'PASS packet mutant caught: {name}')

subprocess.run(["python3", str(ROOT / "tools/test_entity_identity.py")], check=True)
