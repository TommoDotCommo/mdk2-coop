#!/usr/bin/env python3
"""Run the automated test ladder with bounded gates, isolated lab copies and saved reports."""
import argparse
from dataclasses import dataclass, asdict, replace
from datetime import datetime
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


@dataclass(frozen=True)
class Gate:
    name: str
    command: tuple
    players: int = 0
    timeout: int = 600
    result: str = "exit"
    sim: str = ""
    shards: int = 1


OFFLINE = [Gate("build", ("make", "-C", "bridge")),
           Gate("oracles-routes-input", ("bash", "tools/test_replay.sh")),
           Gate("projectile-core", ("make", "-C", "bridge", "test-shots")),
           Gate("controller-core", ("make", "-C", "bridge", "test-pad")),
           Gate("revive-core", ("make", "-C", "bridge", "test-revive")),
           Gate("network-core", ("bash", "tools/test_network.sh"))]
SMOKE = [Gate("two-player", ("bash", "tools/qa.sh"), 2, result="counts"),
         Gate("checkpoint-smoke", ("bash", "tools/sweep.sh", "1-1", "1-3"), 2, result="sweep"),
         Gate("level-smoke", ("bash", "tools/sweep.sh", "--levels"), 2, result="sweep")]
FEATURES = [Gate(name, ("bash", f"tools/{script}.sh"), players, result=result)
            for name, script, players, result in [
                ("multiplayer", "qa_n", 3, "counts"), ("save-reconnect", "qa_save", 3, "counts"),
                ("level-epochs", "qa_epoch", 2, "counts"), ("enemy-targeting", "qa_ai", 3, "counts"),
                ("revive", "qa_revive", 3, "counts"), ("menu", "qa_menu", 3, "counts"),
                ("cutscenes", "qa_cutscene", 2, "counts"), ("minigames", "qa_minigames", 2, "counts"),
                ("level8-intro", "qa_level8_intro", 2, "counts"),
                ("controls", "qa_controls", 1, "counts"), ("launcher", "qa_launcher", 2, "counts"),
                ("character-motion", "qa_motion", 2, "counts"),
                ("hud", "qa_hud", 3, "counts"), ("chat", "qa_chat", 3, "counts"),
                ("voice", "qa_voice", 3, "counts"), ("projectiles", "qa_shots", 3, "projectiles"),
                ("native-projectiles", "qa_shots_native", 3, "counts"),
                ("installer", "qa_installer", 1, "installer")]]
CAMPAIGN = Gate("campaign", ("bash", "tools/sweep.sh"), 2, 5400, "sweep")
LOSSY = Gate("lossy-projectiles", ("bash", "tools/qa_shots_native.sh"), 3, 600, "counts", "80 30 3")
LOSSY_EPOCH = Gate("lossy-level-epochs", ("bash", "tools/qa_epoch.sh"), 2, 600, "counts", "80 30 3")
PROFILES = {"offline": OFFLINE, "smoke": OFFLINE + SMOKE,
            "features": OFFLINE + SMOKE[:1] + FEATURES + [LOSSY, LOSSY_EPOCH],
            "campaign": OFFLINE + [CAMPAIGN],
            "full": OFFLINE + SMOKE[:1] + FEATURES + [LOSSY, LOSSY_EPOCH, CAMPAIGN]}


def score(gate, code, output, timed_out=False):
    if timed_out:
        return "TIMEOUT"
    if code != 0:
        return "FAIL"
    if gate.result == "exit":
        return "PASS"
    if gate.result == "counts":
        counts = re.findall(r"(\d+) passed, (\d+) failed", output)
        return "PASS" if counts and all(int(p) > 0 and int(f) == 0 for p, f in counts) else "FAIL"
    if gate.result == "sweep":
        results = re.findall(r"^RESULT: (PASS|FAIL)$", output, re.M)
        return "PASS" if results and all(r == "PASS" for r in results) else "FAIL"
    if gate.result == "installer":
        checks = re.findall(r"^PASS: (\d+) installer checks", output, re.M)
        return "PASS" if checks and all(int(n) > 0 for n in checks) else "FAIL"
    return "PASS" if re.search(rf"^Projectile checks passed for {gate.players} games$", output, re.M) else "FAIL"


def execute(command, env, logfile, timeout):
    """Kill the entire harness process group on timeout; Windows games are cleaned separately."""
    with logfile.open("w") as stream:
        process = subprocess.Popen(command, cwd=ROOT, env=env, stdout=stream,
                                   stderr=subprocess.STDOUT, start_new_session=True)
        try:
            return process.wait(timeout=timeout), False
        except (subprocess.TimeoutExpired, KeyboardInterrupt):
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            if sys.exc_info()[0] is KeyboardInterrupt:
                raise
            return process.returncode, True


def lab_env():
    exports = subprocess.check_output(["lab", "env", "mdk2"], cwd=ROOT, text=True)
    env = os.environ.copy()
    for line in exports.splitlines():
        tokens = shlex.split(line)
        if len(tokens) != 2 or tokens[0] != "export":
            raise ValueError("invalid lab environment")
        key, value = tokens[1].split("=", 1)
        env[key] = value
    return env


def collect_games(env, slots, folder):
    """Copy diagnostics before the next installer replaces bridge.log; never copy game assets."""
    folder.mkdir()
    for i in range(1, slots + 1):
        game = Path(subprocess.check_output(["wslpath", "-u", env[f"MDK2_DIR{i}"]], text=True).strip())
        for name in ("bridge.log", "coop.ini", "route_status.txt"):
            source = game / "coop" / name
            if source.is_file():
                shutil.copy2(source, folder / f"game-{i}-{name}")
        for source in game.glob("mdk2hd_crash_*.mdmp"):
            shutil.copy2(source, folder / f"game-{i}-{source.name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", choices=PROFILES, nargs="?", default="smoke")
    parser.add_argument("--list", action="store_true", help="show gates without running anything")
    parser.add_argument("--gate", action="append", help="run only named gates from the profile (repeatable)")
    parser.add_argument("--campaign-shards", type=int, choices=range(1, 9), default=1,
                        help="parallel two-game sets for the campaign gate; requires 2 slots per shard")
    parser.add_argument("--route", action="append", type=Path,
                        help="append a recorded two-player progression route (repeatable)")
    args = parser.parse_args()
    gates = list(PROFILES[args.profile])
    gates = [replace(g, shards=args.campaign_shards) if g.name == "campaign" else g for g in gates]
    if args.gate:
        unknown = set(args.gate) - {g.name for g in gates}
        if unknown:
            parser.error("unknown gates: " + ", ".join(sorted(unknown)))
        gates = [g for g in gates if g.name in args.gate]
    for i, route in enumerate(args.route or [], 1):
        if not route.is_file():
            parser.error(f"no route: {route}")
        gates.append(Gate(f"route-{i}", ("bash", "tools/replay.sh", "run", str(route.resolve())),
                          2, 3750, "sweep"))
    if args.list:
        for gate in gates:
            print(f"{gate.name}: {gate.players} games x {gate.shards} sets, timeout {gate.timeout}s; {shlex.join(gate.command)}")
        return 0
    out = ROOT / "local" / "regression" / datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    out.mkdir(parents=True)
    env = os.environ.copy()
    # Prevent inherited test knobs from silently changing what a named profile covers.
    for key in ("MDK2_NO_BUILD", "MDK2_SIM", "MDK2_SHARDS", "MDK2_SWEEP_CHILD", "MDK2_SWEEP_LEVELS", "MDK2_START"):
        env.pop(key, None)
    env["MDK2_BACKGROUND"] = "1"
    rows, leased, lease_code = [], False, 0
    slots = max(g.players * g.shards for g in gates)
    try:
        if slots:
            # Never adopt/release a lease belonging to a manually started session.
            existing = subprocess.run(["lab", "env", "mdk2"], cwd=ROOT, capture_output=True)
            if existing.returncode == 0:
                raise RuntimeError("this worktree already has a lab lease; release it before running qa_all")
            lease_code = subprocess.call(["lab", "acquire", "--slots", str(slots), "--ttl",
                                    str(sum(g.timeout for g in gates) + 600)], cwd=ROOT)
            if lease_code:
                raise RuntimeError("lab is busy" if lease_code == 3 else "lab acquisition failed")
            leased = True
            subprocess.run(["lab", "provision", "mdk2"], cwd=ROOT, check=True)
            # Only import lab exports, preserve the runner's clean test environment.
            env.update({k: v for k, v in lab_env().items() if k.startswith(("LAB_", "MDK2_"))})
            for key in ("MDK2_NO_BUILD", "MDK2_SIM", "MDK2_SHARDS", "MDK2_SWEEP_CHILD", "MDK2_SWEEP_LEVELS", "MDK2_START"):
                env.pop(key, None)
        for gate in gates:
            gate_env = env | {"MDK2_N": str(gate.players or 2)}
            if gate.shards > 1:
                gate_env["MDK2_SHARDS"] = str(gate.shards)
            if gate.sim:
                gate_env["MDK2_SIM"] = gate.sim
            print(f"RUN {gate.name} ({gate.players} games x {gate.shards} sets)", flush=True)
            start = time.monotonic()
            logfile = out / f"{gate.name}.log"
            code, timed_out = execute(gate.command, gate_env, logfile, gate.timeout)
            result = score(gate, code, logfile.read_text(errors="replace"), timed_out)
            rows.append({**asdict(gate), "status": result, "exit_code": code,
                         "seconds": round(time.monotonic() - start, 1), "log": logfile.name})
            print(f"{result} {gate.name} ({rows[-1]['seconds']}s)", flush=True)
            if gate.players:
                collect_games(env, gate.players * gate.shards, out / f"{gate.name}-games")
                # Kill all leased copies, including extra clients started by a failed gate.
                cleanup_env = env | {"MDK2_N": str(slots)}
                cleanup_code, _ = execute(("bash", "-c", 'source tools/mdk.sh; mdk_kill'), cleanup_env,
                                         out / f"{gate.name}-cleanup.log", 45)
                if cleanup_code:
                    raise RuntimeError("game cleanup failed")
            # A broken build or oracle invalidates every later game result.
            if result != "PASS" and not gate.players:
                break
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as exc:
        rows.append({"name": "runner", "status": "FAIL", "error": str(exc)})
        print(f"FAIL runner: {exc}", flush=True)
    except KeyboardInterrupt:
        rows.append({"name": "runner", "status": "INTERRUPTED"})
    finally:
        if leased:
            try:
                released = subprocess.run(["lab", "release"], cwd=ROOT, timeout=60)
                if released.returncode:
                    rows.append({"name": "cleanup", "status": "FAIL"})
            except (OSError, subprocess.SubprocessError) as exc:
                rows.append({"name": "cleanup", "status": "FAIL", "error": str(exc)})
        passed = len(rows) == len(gates) and all(r["status"] == "PASS" for r in rows)
        report = {"profile": args.profile, "result": "BUSY" if lease_code == 3 else "PASS" if passed else "FAIL",
                  "gates": rows}
        (out / "results.json").write_text(json.dumps(report, indent=2) + "\n")
        (out / "results.md").write_text("| gate | result | seconds | log |\n|---|---|---|---|\n" + "".join(
            f"| {r['name']} | {r['status']} | {r.get('seconds', '')} | {r.get('log', '')} |\n" for r in rows))
        print(f"RESULT: {report['result']}\nevidence: {out}", flush=True)
    return 3 if lease_code == 3 else 0 if passed else 1


if __name__ == "__main__":
    def terminate(signum, frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, terminate)
    raise SystemExit(main())
