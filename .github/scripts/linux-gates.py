#!/usr/bin/env python3
"""Run qa_all's portable offline gates; name every Windows-only omission."""
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import qa_all

WINDOWS = {
    'rear-gate-routing', 'launcher-audio', 'chain-load', 'player-list-core',
    'projectile-core', 'controller-core', 'redirect-core', 'revive-core',
    'network-core', 'pickups-core', 'diagnostics-core', 'crash-context', 'anim-guard',
}


def main():
    names = {g.name for g in qa_all.OFFLINE}
    assert WINDOWS <= names, 'update the Windows-only gate list after renaming gates'
    lines = ['SKIP Windows/PowerShell-only offline gates: ' + ', '.join(sorted(WINDOWS)),
             'discovery-core and oracles-routes-input also skip their Windows subtests on Linux.']
    print('\n'.join(lines), flush=True)
    if os.environ.get('GITHUB_STEP_SUMMARY'):
        with open(os.environ['GITHUB_STEP_SUMMARY'], 'a') as summary:
            summary.write('\n'.join(lines) + '\n')
    command = [sys.executable, 'tools/qa_all.py', 'offline']
    for gate in qa_all.OFFLINE:
        if gate.name not in WINDOWS:
            command += ['--gate', gate.name]
    return subprocess.call(command, cwd=ROOT)


if __name__ == '__main__':
    sys.exit(main())
