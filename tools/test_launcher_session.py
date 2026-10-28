#!/usr/bin/env python3
"""Native T0 lifecycle + pregame chat/voice simulator, fuzzer and source mutants; never launches Windows."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'bridge/tests/launcher_session_test.c'
HEADER = (ROOT / 'bridge/launcher_session.h').read_text()
MUTANTS = {
    'unvalidated-roster': (' || !valid ||', ' || 0 ||'),
    'oversized-countdown': (' || left > 3000', ''),
    'negative-countdown': (' || left < -1', ''),
    'closed-session-revival': (' || s->phase == LS_OFF', ''),
    'client-authority': ('!s->host || s->phase', '0 || s->phase'),
    'version-mismatch-start': (' || !compatible ||', ' || 0 ||'),
    'countdown-extension': ('if (s->phase != LS_COUNTDOWN) s->deadline', 'if (1) s->deadline'),
    'host-loss': ('> 4000', '> 40000'),
    'stale-countdown-start': ('> 1000', '> 10000'),
    'early-start': ('(int32_t)(now - s->deadline) >= 0', '(int32_t)(now - s->deadline) >= -100'),
    'clock-wrap': ('(int32_t)(now - s->deadline) >= 0', 'now >= s->deadline'),
    'leave': ('s->phase = LS_OFF;', 's->phase = LS_LOBBY;'),
    'chat-control-chars': ('if (c < 32 || c >= 127) continue;', ''),
    'chat-pipe-escape': ("if (replace_pipe && c == '|') c = '_';", ''),
    'chat-c1-endpoint': ('if (!s->host || (s->phase != LS_LOBBY && s->phase != LS_COUNTDOWN) || !endpoint_ok) return 0;',
                         'if (!s->host || (s->phase != LS_LOBBY && s->phase != LS_COUNTDOWN)) return 0;'),
    'chat-c1-nonmember': ('if (!m || m == &c->members[0]) return 0;', 'if (!m) m = &c->members[0];'),
    'chat-c1-max-len': ('if (len - (size_t)(bar + 1 - pkt) > LS_CHAT_MAX) return 0;', ''),
    'chat-flood-limit': ('if (m->chat_burst >= LS_CHAT_BURST_MAX) return 0;', ''),
    'chat-dedup-seq': ('if (m->have_chat_seq && seq == m->last_chat_seq) return 0;', ''),
    'chat-m1-endpoint': ('if (s->host || (s->phase != LS_LOBBY && s->phase != LS_COUNTDOWN) || !endpoint_ok) return 0;',
                         'if (s->host || (s->phase != LS_LOBBY && s->phase != LS_COUNTDOWN)) return 0;'),
    'chat-m1-nonmember': ('m = ls_comm_find(c, id);\n    if (!m) return 0;', 'm = &c->members[0];'),
    'voice-magic': ('out->magic != LS_VOICE_MAGIC || ', ''),
    'voice-version': ('out->version != LS_VOICE_VERSION', '0'),
    'voice-size': ('len != sizeof(struct ls_voice_pkt)', '0'),
    'voice-adpcm-index': ('out->index > 88 || ', ''),
    'voice-endpoint': ('if ((s->phase != LS_LOBBY && s->phase != LS_COUNTDOWN) || !endpoint_ok) return 0;',
                       'if (s->phase != LS_LOBBY && s->phase != LS_COUNTDOWN) return 0;'),
    'voice-self-echo': ('if (pkt.sender_id == self_id) return 0;', ''),
    'voice-nonmember': ('if (!m || (s->host && m == &c->members[0])) return 0;', 'if (!m) m = &c->members[0];'),
    'voice-dedup-seq': ('if (m->have_voice_seq) {', 'if (0) {'),
    'voice-relay-exclude-sender': ('if (c->members[i].id != pkt.sender_id)', 'if (1)'),
    'voice-mute': ('if (m->muted || m->volume <= 0)', 'if (0)'),
    'voice-volume-scale': ('if (m->volume != 100)', 'if (0)'),
    'voice-vad-threshold': ('(ls_pcm_level(pcm, LS_VOICE_FRAME) >= vad_threshold)', '1'),
    'voice-speaking-last': ('if (pkt.flags & LS_VOICE_FLAG_LAST) m->talking_until = 0;', 'if (0) m->talking_until = 0;'),
}

def run(directory, header):
    (directory / 'launcher_session.h').write_text(header)
    binary = directory / 'test'
    subprocess.run(['cc', '-std=c99', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-Wno-unused-but-set-variable', '-I', str(directory),
                    str(SOURCE), '-o', str(binary)], check=True, capture_output=True)
    return subprocess.run([str(binary)], capture_output=True)

with tempfile.TemporaryDirectory() as temp:
    directory = Path(temp)
    result = run(directory, HEADER)
    assert result.returncode == 0, result.stderr.decode()
    print(result.stdout.decode().strip())
    for name, (before, after) in MUTANTS.items():
        assert before in HEADER, name
        result = run(directory, HEADER.replace(before, after, 1))
        assert result.returncode != 0, 'SURVIVED: ' + name
        print('KILLED:', name)
