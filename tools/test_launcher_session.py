#!/usr/bin/env python3
"""Native T0 lifecycle simulator and source mutants; never launches Windows."""
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
}

def run(directory, header):
    (directory / 'launcher_session.h').write_text(header)
    binary = directory / 'test'
    subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-I', str(directory),
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
