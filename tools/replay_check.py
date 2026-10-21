#!/usr/bin/env python3
"""Final replay checks supplement statecheck with strict participant/life/log assertions."""
import math
from pathlib import Path
import re
import sys

import statecheck


def check(dumps, logs, players):
    errors = []
    if len(dumps) != players or len(logs) != players:
        errors.append("missing participant artifacts")
    slots = {d["me"] for d in dumps}
    if len(slots) != players or None in slots:
        errors.append("missing or duplicate player slots")
    for i, (dump, log) in enumerate(zip(dumps, logs), 1):
        if dump["life"] != 0 or not dump["player"] or dump["player"][1] <= 0:
            errors.append(f"game {i}: player missing, down or dead")
        if not dump["player"] or not all(math.isfinite(x) for x in dump["player"][2]):
            errors.append(f"game {i}: invalid player position")
        if set(dump["puppet"]) != slots - {dump["me"]}:
            errors.append(f"game {i}: missing teammate puppet")
        if re.search(r"(?im)^.*(?:lua error|error:|stack traceback|unknown game build).*$", log):
            errors.append(f"game {i}: game or Lua error in log")
    return errors


def main():
    folder, count = Path(sys.argv[1]), int(sys.argv[2])
    # Final dumps have the highest sequence number, including a retry if one was needed.
    states = sorted(folder.glob("state_route_*_host.txt"), key=lambda p: int(p.name.split("_")[2]))
    if not states:
        print("FAIL no final state dumps")
        return 1
    tag = states[-1].name.removesuffix("_host.txt").removeprefix("state_")
    retry = tag.replace("route_", "retry_")
    if (folder / f"state_{retry}_host.txt").exists():
        tag = retry
    paths = [folder / f"state_{tag}_{'host' if i == 1 else 'client' if i == 2 else f'client{i}'}.txt"
             for i in range(1, count + 1)]
    try:
        dumps = [statecheck.load(p) for p in paths]
        logs = [(folder / f"game-{i}.log").read_text(encoding="latin-1") for i in range(1, count + 1)]
    except (OSError, ValueError) as exc:
        print(f"FAIL missing or invalid replay artifacts: {exc}")
        return 1
    errors = check(dumps, logs, count)
    for error in errors:
        print("FAIL", error)
    return bool(errors)


if __name__ == "__main__":
    sys.exit(main())
