#!/usr/bin/env python3
"""Summarise a campaign bot run (local/campaign/runs/<time>/, from campaign_run.py) as one table.

    campaign_report.py [RUN_FOLDER]          default: the newest run; writes report.md beside it

Per segment: the result, time, what the leader broke and gave up on, the world comparison, how far
followers were from the leader, and from each game's bridge log the Lua errors, desync reports,
crashes and the hero's lowest health.
"""
import ast
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def leader_stats(bot_log):
    m = re.search(r"RESULT: \w+ in \d+s: (\{.*\}); evidence", bot_log)
    if not m:
        return {}
    try:
        return ast.literal_eval(m.group(1))
    except (ValueError, SyntaxError):
        return {}


def game_facts(log_text):
    return {
        'lua_errors': len(re.findall(r'LUA ERROR', log_text)),
        'desyncs': len(re.findall(r'DESYNC', log_text)),
        'crash': bool(re.search(r'\bcrash:', log_text)),
        'deaths': len(re.findall(r'life: \w+ -> downed', log_text)),
    }


def report(run):
    rows = []
    for folder in sorted(p for p in run.iterdir() if p.is_dir()):
        result = json.loads((folder / 'results.json').read_text()) if (folder / 'results.json').exists() else {}
        bot = (folder / 'bot.log').read_text(errors='replace') if (folder / 'bot.log').exists() else ''
        stats = leader_stats(bot)
        lead = stats.get('leader', ({}, {}))
        lead = lead[1] if isinstance(lead, tuple) else lead
        games = [game_facts(p.read_text(encoding='latin-1')) for p in sorted(folder.glob('bridge_*.log'))]
        sweeps = len(re.findall(r'fire sweep: destroyed', bot))
        rows.append({
            'segment': folder.name, 'result': result.get('result', '?'), 'seconds': result.get('seconds', ''),
            'destroyed': len(lead.get('destroyed', [])), 'sweeps': sweeps, 'gave_up': len(lead.get('gave_up', [])),
            'world': {True: 'match', False: 'DIFF'}.get(stats.get('world_ok'), '?'),
            'gap': max(stats.get('gaps', {0: 0}).values() or [0]),
            'lua_errors': sum(g['lua_errors'] for g in games), 'desyncs': sum(g['desyncs'] for g in games),
            'crash': any(g['crash'] for g in games), 'downs': sum(g['deaths'] for g in games),
            'stuck': next((l.split('stalled toward ')[1] for l in bot.splitlines() if 'stalled toward' in l), ''),
        })
    head = ['segment', 'result', 'seconds', 'destroyed', 'sweeps', 'gave_up', 'world', 'gap', 'lua_errors',
            'desyncs', 'crash', 'downs', 'stuck']
    lines = ['| ' + ' | '.join(head) + ' |', '|' + '---|' * len(head)]
    for r in rows:
        lines.append('| ' + ' | '.join(str(r[h]) for h in head) + ' |')
    passed = sum(r['result'] == 'PASS' for r in rows)
    text = f'# Campaign bot run {run.name}\n\n{passed}/{len(rows)} segments passed.\n\n' + '\n'.join(lines) + '\n'
    (run / 'report.md').write_text(text)
    return text


def main(argv=None):
    args = (argv if argv is not None else sys.argv[1:])
    if args and args[0] in ('-h', '--help'):
        print(__doc__)
        return 0
    runs = ROOT / 'local/campaign/runs'
    run = Path(args[0]) if args else max(runs.iterdir(), key=lambda p: p.name)
    print(report(run))
    return 0


if __name__ == '__main__':
    sys.exit(main())
