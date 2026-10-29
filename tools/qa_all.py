#!/usr/bin/env python3
"""Run the automated test ladder with bounded gates, isolated game copies and saved reports."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import threading
import uuid
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
CANCEL = threading.Event()
EXCLUSIVE = {"launcher", "installer", "campaign", "character-motion"}
TEST_KNOBS = ("MDK2_CRASH_GUARD", "MDK2_CRASH_STATE", "MDK2_NO_BUILD", "MDK2_SIM", "MDK2_SHARDS", "MDK2_SWEEP_CHILD",
              "MDK2_SWEEP_LEVELS", "MDK2_START", "MDK2_MOTION_MUTANT", "MUTANT")


@dataclass(frozen=True)
class Gate:
    name: str
    command: tuple
    players: int = 0
    timeout: int = 600
    result: str = "exit"
    sim: str = ""
    shards: int = 1


OFFLINE = [Gate("rear-gate-routing", ("bash", "tools/test_rear_gate.sh")),
           Gate("presence-core", ("python3", "tools/test_presence.py")),
           Gate("launcher-audio", ("bash", "tools/test_launcher_audio.sh")),
           Gate("launcher-profiles", ("python3", "tools/test_launcher_profiles.py")),
           Gate("install-policy", ("python3", "tools/test_install_policy.py")),
           Gate("launcher-session", ("python3", "tools/test_launcher_session.py")),
           Gate("launcher-update", ("python3", "tools/test_launcher_update.py")),
           Gate("ui-layout", ("python3", "tools/test_ui_layout.py")),
           Gate("combat-core", ("python3", "tools/test_combat.py")),
           Gate("build", ("make", "-C", "bridge")),
           Gate("oracles-routes-input", ("bash", "tools/test_replay.sh")),
           Gate("projectile-core", ("make", "-C", "bridge", "test-shots")),
           Gate("controller-core", ("make", "-C", "bridge", "test-pad")),
           Gate("revive-core", ("make", "-C", "bridge", "test-revive")),
           Gate("network-core", ("bash", "tools/test_network.sh")),
           Gate("pickups-core", ("bash", "tools/test_pickups.sh")),
           Gate("diagnostics-core", ("bash", "tools/test_diag.sh")),
           Gate("crash-context", ("bash", "tools/test_crash_context.sh")),
           Gate("anim-guard", ("bash", "tools/test_anim_guard.sh")),
           Gate("crash-watch", ("python3", "-B", "-m", "unittest", "tests/minidump_test.py", "tests/crashwatch_test.py"))]
SMOKE = [Gate("two-player", ("bash", "tools/qa.sh"), 2, result="counts"),
         Gate("checkpoint-smoke", ("bash", "tools/sweep.sh", "1-1", "1-3"), 2, result="sweep"),
         Gate("level-smoke", ("bash", "tools/sweep.sh", "--levels"), 2, result="sweep")]
FEATURES = [Gate(name, ("bash", f"tools/{script}.sh"), players, result=result)
            for name, script, players, result in [
                ("multiplayer", "qa_n", 3, "counts"), ("save-reconnect", "qa_save", 3, "counts"),
                ("save-script-tasks", "qa_save_scripts", 2, "counts"),
                ("level-epochs", "qa_epoch", 2, "counts"), ("enemy-targeting", "qa_ai", 2, "counts"), ("native-room-ai", "qa_room_ai", 2, "counts"),
                ("hans-targeting", "qa_hans", 2, "counts"),
                ("attached-canopy", "qa_presence", 3, "counts"), ("client-join", "qa_join", 2, "counts"),
                ("revive", "qa_revive", 3, "counts"), ("menu", "qa_menu", 3, "counts"),
                ("cutscenes", "qa_cutscene", 2, "counts"), ("minigames", "qa_minigames", 2, "counts"),
                ("level8-intro", "qa_level8_intro", 2, "counts"),
                ("character-wheel", "qa_char", 1, "counts"), ("character-puppet", "qa_char2", 2, "counts"),
                ("emotes", "qa_emote", 1, "counts"), ("crash-log", "qa_crashlog", 1, "counts"),
                ("controls", "qa_controls", 1, "counts"), ("diagnostics", "qa_diag", 2, "counts"), ("triggers", "qa_triggers", 2, "counts"), ("remote-events", "qa_remote", 2, "counts"), ("playtest-sync", "qa_playtest", 2, "counts"), ("proximity", "qa_proximity", 2, "counts"), ("level2-rear-gate", "qa_rear_gate", 2, "counts"), ("anim-guard-live", "qa_animguard", 2, "counts"), ("launcher", "qa_launcher", 2, "counts"),
                ("lobby", "qa_lobby", 3, "counts"),
                ("first-enemy", "qa_sign", 3, "counts"),
                ("character-motion", "qa_motion", 2, "counts"),
                ("combat-startup", "qa_combat_startup", 1, "counts"), ("combat", "qa_combat", 3, "counts"), ("hud", "qa_hud", 3, "counts"), ("chat", "qa_chat", 3, "counts"),
                ("voice", "qa_voice", 3, "counts"), ("projectiles", "qa_shots", 3, "projectiles"),
                ("native-projectiles", "qa_shots_native", 3, "counts"),
                ("installer", "qa_installer", 1, "installer")]]
CAMPAIGN = Gate("campaign", ("bash", "tools/sweep.sh"), 2, 5400, "sweep")
LOSSY = Gate("lossy-projectiles", ("bash", "tools/qa_shots_native.sh"), 3, 600, "counts", "80 30 3")
LOSSY_COMBAT = Gate("lossy-combat", ("bash", "tools/qa_combat.sh"), 3, 600, "counts", "80 30 3")
LOSSY_EPOCH = Gate("lossy-level-epochs", ("bash", "tools/qa_epoch.sh"), 2, 600, "counts", "80 30 3")
PROFILES = {"offline": OFFLINE, "smoke": OFFLINE + SMOKE,
            "features": OFFLINE + SMOKE[:1] + FEATURES + [LOSSY, LOSSY_EPOCH, LOSSY_COMBAT],
            "campaign": OFFLINE + [CAMPAIGN],
            "full": OFFLINE + SMOKE[:1] + FEATURES + [LOSSY, LOSSY_EPOCH, LOSSY_COMBAT, CAMPAIGN]}


def score(gate, code, output, timed_out=False):
    if timed_out:
        return "TIMEOUT"
    if code != 0:
        return "FAIL"
    if re.search(r"^CRASH ", output, re.M):      # a game copy crashed, whatever the suite's own counts say
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
    """Bound a harness and stop its process group on timeout or runner cancellation."""
    with logfile.open("w") as stream:
        process = subprocess.Popen(command, cwd=ROOT, env=env, stdout=stream,
                                   stderr=subprocess.STDOUT, start_new_session=True)
        deadline = time.monotonic() + timeout
        try:
            while not CANCEL.is_set():
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                try:
                    return process.wait(timeout=min(0.25, remaining)), False
                except subprocess.TimeoutExpired:
                    pass
        except KeyboardInterrupt:
            CANCEL.set()
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
        if CANCEL.is_set():
            raise InterruptedError("runner interrupted")
        return process.returncode, True


class BusyError(RuntimeError):
    pass


class Resources:
    """An optional, external allocator owns all machine-specific provisioning."""
    def __init__(self, hook, env, owner, slots):
        self.hook, self.env, self.slots = hook, env.copy(), slots
        self.env["MDK2_QA_OWNER"] = owner
        self.acquired = False
        self.validated = False

    def acquire(self, ttl):
        if self.hook:
            try:
                result = subprocess.run([str(self.hook), "acquire", str(self.slots), str(ttl)],
                                        cwd=ROOT, env=self.env, capture_output=True, text=True, timeout=180)
            except subprocess.TimeoutExpired:
                # The allocator may have reserved copies before it stopped responding.
                self.acquired = True
                raise
            if result.stderr:
                print(result.stderr, file=sys.stderr, end="", flush=True)
            if result.returncode == 3:
                raise BusyError("test resources are busy")
            if result.returncode:
                raise RuntimeError("resource acquisition failed")
            self.acquired = True  # release even if the allocator's output is malformed
            values = json.loads(result.stdout)
            if not isinstance(values, dict) or any(not isinstance(k, str) or not isinstance(v, str)
                                                   for k, v in values.items()):
                raise ValueError("allocator must return a JSON object of environment strings")
            self.env.update({k: v for k, v in values.items() if k.startswith("MDK2_")})
        for key in TEST_KNOBS:
            self.env.pop(key, None)
        for i in range(1, self.slots + 1):
            key = f"MDK2_DIR{i}"
            if not self.env.get(key):
                raise ValueError(f"configure {key} or a resource allocator; see docs/TESTING.md")
        paths = [self.env[f"MDK2_DIR{i}"].rstrip("\\/").lower() for i in range(1, self.slots + 1)]
        if len(set(paths)) != len(paths):
            raise ValueError("test copies must have distinct paths")
        for key in ("MDK2_PORT", "MDK2_PIDS", "MDK2_TOOLS"):
            if not self.env.get(key):
                raise ValueError(f"configure {key}; see docs/TESTING.md")
        self.env["MDK2_BACKGROUND"] = "1"
        self.validated = True
        return self

    def release(self):
        if not self.hook and self.validated:
            # Configured copies have no allocator to clean up after interruption.
            result = subprocess.run(["bash", "-c", "source tools/mdk.sh; mdk_kill"],
                                    cwd=ROOT, env=self.env | {"MDK2_N": str(self.slots)}, timeout=45)
            if result.returncode:
                raise RuntimeError("game cleanup failed")
        if self.acquired:
            self.acquired = False
            result = subprocess.run([str(self.hook), "release"], cwd=ROOT, env=self.env, timeout=60)
            if result.returncode:
                raise RuntimeError("resource release failed")


def phases(gates):
    """Offline, shared-settings, motion and campaign gates form barriers between game pools."""
    pending = []
    for gate in gates:
        if not gate.players or gate.name in EXCLUSIVE:
            if pending:
                yield pending
                pending = []
            yield [gate]
        else:
            pending.append(gate)
    if pending:
        yield pending


def run_gate(gate, env, slots, out):
    gate_env = env | {"MDK2_N": str(gate.players or 2)}
    if gate.players:
        gate_env["MDK2_NO_BUILD"] = "1"
    if gate.shards > 1:
        gate_env["MDK2_SHARDS"] = str(gate.shards)
    if gate.sim:
        gate_env["MDK2_SIM"] = gate.sim
    print(f"RUN {gate.name} ({gate.players} games x {gate.shards} sets)", flush=True)
    start = time.monotonic()
    logfile = out / f"{gate.name}.log"
    code, timed_out = execute(gate.command, gate_env, logfile, gate.timeout)
    result = score(gate, code, logfile.read_text(errors="replace"), timed_out)
    row = {**asdict(gate), "status": result, "exit_code": code,
           "seconds": round(time.monotonic() - start, 1), "log": logfile.name}
    print(f"{result} {gate.name} ({row['seconds']}s)", flush=True)
    if gate.players:
        try:
            collect_games(env, gate.players * gate.shards, out / f"{gate.name}-games")
        finally:
            # Clean only this lane's copies, including clients left by a failed gate.
            if not CANCEL.is_set():
                cleanup_code, _ = execute(("bash", "-c", 'source tools/mdk.sh; mdk_kill'),
                                         env | {"MDK2_N": str(slots)}, out / f"{gate.name}-cleanup.log", 45)
                if cleanup_code:
                    raise RuntimeError("game cleanup failed")
    return row


def run_pool(gates, lanes, hook, env, owner, out, rows):
    assignments = [gates[i::lanes] for i in range(min(lanes, len(gates)))]
    resources = []
    try:
        # Reserve every lane before starting games. Partial allocation is always released.
        for i, assigned in enumerate(assignments):
            resource = Resources(hook, env, f"{owner}-lane{i+1}",
                                 max(g.players * g.shards for g in assigned))
            resources.append(resource)
            resource.acquire(sum(g.timeout for g in gates) + 600)
        # Reject a misconfigured allocator before any lane can start/kill games.
        seen_paths, seen_ports, seen_pids, seen_tools = set(), set(), set(), set()
        for resource in resources:
            e = resource.env
            paths = {e[f"MDK2_DIR{i}"].rstrip("\\/").lower() for i in range(1, resource.slots + 1)}
            ports = set(range(int(e["MDK2_PORT"]), int(e["MDK2_PORT"]) + resource.slots))
            tools, pids = e["MDK2_TOOLS"].rstrip("\\/").lower(), e["MDK2_PIDS"]
            if paths & seen_paths or ports & seen_ports or pids in seen_pids or tools in seen_tools:
                raise ValueError("allocator returned overlapping lane paths, ports, tools or PID files")
            seen_paths.update(paths); seen_ports.update(ports)
            seen_pids.add(pids); seen_tools.add(tools)

        def worker(assigned, resource):
            for gate in assigned:
                if CANCEL.is_set():
                    return
                try:
                    rows.append(run_gate(gate, resource.env, resource.slots, out))
                except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as exc:
                    rows.append({"name": gate.name, "status": "FAIL", "error": str(exc)})
                    # Resource/cleanup failures invalidate the rest of this lane.
                    return

        pool = ThreadPoolExecutor(max_workers=len(resources))
        try:
            futures = [pool.submit(worker, assigned, resource)
                       for assigned, resource in zip(assignments, resources)]
            for future in futures:
                future.result()
        except KeyboardInterrupt:
            CANCEL.set()
            raise
        finally:
            pool.shutdown(wait=True)
    finally:
        # Try every release even if one allocator fails to clean up.
        for resource in reversed(resources):
            try:
                resource.release()
            except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
                rows.append({"name": "cleanup", "status": "FAIL", "error": str(exc)})


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
    parser.add_argument("--lanes", type=int, choices=range(1, 9), default=1,
                        help="concurrent game suites; settings, motion and campaign gates run alone")
    parser.add_argument("--resources", type=Path,
                        help="resource allocator executable (default: MDK2_QA_RESOURCES or local/qa_resources.py)")
    parser.add_argument("--list", action="store_true", help="show gates without running anything")
    parser.add_argument("--gate", action="append", help="run only named gates from the profile (repeatable)")
    parser.add_argument("--campaign-shards", type=int, choices=range(1, 9), default=1,
                        help="parallel two-game sets for the campaign gate; requires 2 slots per shard")
    parser.add_argument("--build", choices=("steam", "overhaul"),
                        help="executable the game copies run (default: whatever the copies hold); overhaul needs "
                             "MDK2_OVERHAUL_EXE, see docs/TESTING.md")
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
    CANCEL.clear()
    out = ROOT / "local" / "regression" / datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    out.mkdir(parents=True)
    env = os.environ.copy()
    for key in TEST_KNOBS:
        env.pop(key, None)
    env["MDK2_BACKGROUND"] = "1"
    env.pop("MDK2_BUILD", None)      # only --build chooses; an inherited value would silently change every gate
    if args.build:
        env["MDK2_BUILD"] = args.build
        if args.build == "overhaul" and not env.get("MDK2_OVERHAUL_EXE"):
            parser.error("--build overhaul needs MDK2_OVERHAUL_EXE (Windows path of that build's mdk2hd.exe)")
    hook = args.resources or os.environ.get("MDK2_QA_RESOURCES")
    if hook is None and (ROOT / "local/qa_resources.py").is_file():
        hook = ROOT / "local/qa_resources.py"
    if hook:
        hook = Path(hook).resolve()
    rows, busy, built = [], False, False
    owner = f"qa-{out.name}-{uuid.uuid4().hex[:6]}"
    try:
        if args.lanes > 1 and any(g.players for g in gates) and not hook:
            raise ValueError("parallel lanes require a resource allocator; see docs/TESTING.md")
        for batch in phases(gates):
            if not batch[0].players:
                row = run_gate(batch[0], env, 0, out)
                rows.append(row)
                if row["status"] != "PASS":
                    break
                built = built or batch[0].name == "build"
            else:
                # Even --gate selections build once before parallel installers copy the DLL.
                if not built:
                    code, timeout = execute(("make", "-C", "bridge"), env, out / "prepare-build.log", 600)
                    if code or timeout:
                        raise RuntimeError("preparatory build failed")
                    built = True
                run_pool(batch, args.lanes, hook, env, owner, out, rows)
    except BusyError as exc:
        busy = True
        rows.append({"name": "runner", "status": "BUSY", "error": str(exc)})
        print(f"BUSY runner: {exc}", flush=True)
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as exc:
        rows.append({"name": "runner", "status": "FAIL", "error": str(exc)})
        print(f"FAIL runner: {exc}", flush=True)
    except KeyboardInterrupt:
        CANCEL.set()
        rows.append({"name": "runner", "status": "INTERRUPTED"})
    finally:
        order = {g.name: i for i, g in enumerate(gates)}
        rows.sort(key=lambda r: order.get(r["name"], len(gates)))
        passed = len(rows) == len(gates) and all(r["status"] == "PASS" for r in rows)
        report = {"profile": args.profile, "lanes": args.lanes,
                  "result": "BUSY" if busy else "PASS" if passed else "FAIL", "gates": rows}
        (out / "results.json").write_text(json.dumps(report, indent=2) + "\n")
        (out / "results.md").write_text("| gate | result | seconds | log |\n|---|---|---|---|\n" + "".join(
            f"| {r['name']} | {r['status']} | {r.get('seconds', '')} | {r.get('log', '')} |\n" for r in rows))
        print(f"RESULT: {report['result']}\nevidence: {out}", flush=True)
    return 3 if busy else 0 if passed else 1


if __name__ == "__main__":
    def terminate(signum, frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, terminate)
    raise SystemExit(main())
