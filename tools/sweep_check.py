#!/usr/bin/env python3
"""Score actual checkpoint sweep rows; missing evidence is a failure, warnings are retained."""
import argparse
from pathlib import Path
import re


def check(text, expected, players):
    errors, rows = [], {}
    for line in text.splitlines():
        if not re.match(r"^\| \d+-\d+ \|", line):
            continue
        fields = [f.strip() for f in line.strip("|").split("|")]
        checkpoint = fields[0]
        if checkpoint in rows:
            errors.append(f"{checkpoint}: duplicate row")
        rows[checkpoint] = fields
    for checkpoint in expected:
        f = rows.get(checkpoint)
        if f is None:
            errors.append(f"{checkpoint}: missing result")
            continue
        try:
            if len(f) != 11:
                raise ValueError("malformed row")
            if f[1] != "yes" or f[9] != "yes":
                raise ValueError("load or process survival failed")
            if list(map(int, f[2].split("/"))) != [players - 1] * players:
                raise ValueError("missing or extra puppets")
            enemies = list(map(int, f[3].split("/")))
            if len(enemies) != players or len(set(enemies)) != 1:
                raise ValueError("enemy binding mismatch")
            # "ent orphan ... deleted" records successful removal of a client-local
            # spawner duplicate, not a surviving extra enemy. Keep that count as evidence.
            if int(f[4]) < 0 or any(int(f[i]) != 0 for i in (5, 7)):
                raise ValueError("invalid orphan count, Lua error or HP mismatch")
            state = list(map(int, f[8].split("/")))
            if len(state) != 2 or state[0] != 0 or state[1] < 0:
                raise ValueError("world mismatch or invalid state report")
            if "not settled" in f[10] or "dump saved" in f[10]:
                raise ValueError("unsettled world or crash dump")
        except ValueError as exc:
            errors.append(f"{checkpoint}: {exc}")
    for checkpoint in rows.keys() - set(expected):
        errors.append(f"{checkpoint}: unexpected result")
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("--players", type=int, required=True, choices=range(2, 17))
    parser.add_argument("--expected", nargs="+", required=True)
    args = parser.parse_args()
    errors = check(args.report.read_text(), args.expected, args.players)
    for error in errors:
        print("FAIL ", error)
    print(f"sweep: {len(args.expected)} checkpoints expected, {len(errors)} failures")
    print("RESULT: FAIL" if errors else "RESULT: PASS")
    return bool(errors)


if __name__ == "__main__":
    raise SystemExit(main())
