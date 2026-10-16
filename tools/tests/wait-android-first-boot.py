#!/usr/bin/env python3
"""Observe a fresh CI AVD's user-0 BOOT_COMPLETED callback using read-only adb.

Exit 0 means the exact log marker was observed for one unchanged system_server
PID. Exit 2 means it was not observed within the bounded budget. This does not
claim system idle, APK validation, or any Android Diagnostics runtime check.
Only the caller can establish that the selected AVD is fresh and job-owned.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess
import sys
import time


MARKER = "Finished processing BOOT_COMPLETED for u0"
MARKER_LINE = re.compile(
    r"\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2}\.\d{3,}\s+"
    r"(?P<pid>[1-9][0-9]*)\s+[1-9][0-9]*\s+I\s+ActivityManager\s*:\s*"
    + re.escape(MARKER) + r"\s*"
)


class ObservationBlocked(Exception):
    """A missing or failed observation, never an application failure."""


class FirstBootObserver:
    def __init__(self, args):
        self.args = args
        self.started = time.monotonic()
        self.deadline = self.started + args.timeout
        self.output = args.output.resolve()
        self.output.mkdir(parents=True, exist_ok=True)
        self.result_path = self.output / "first-boot-result.json"
        self.observations_path = self.output / "observations.jsonl"
        if self.result_path.exists() or self.observations_path.exists():
            raise ObservationBlocked("Output contains a previous first-boot observation; choose a new --output directory")
        source = Path(__file__).read_bytes()
        (self.output / "observer-source.py").write_bytes(source)
        self.observations_path.touch()
        self.command_number = 0
        self.result = {
            "schema": 1, "status": "blocked", "finished": False,
            "started_utc": datetime.now(timezone.utc).isoformat(), "serial": args.serial,
            "timeout_seconds": args.timeout, "command_timeout_seconds": args.command_timeout,
            "observer_sha256": hashlib.sha256(source).hexdigest(), "source_file": "observer-source.py",
            "observations_file": self.observations_path.name, "observation_count": 0,
            "reason": "Current system_server BOOT_COMPLETED callback has not been observed",
            "system_idle_claimed": False, "apk_validation_performed": False,
            "limitations": [
                "Only for a fresh, job-owned CI AVD; freshness is established by the caller.",
                "The callback marker does not establish CPU, storage, background-job or UI readiness.",
                "A missing or rotated log marker remains blocked; no broadcasts are flushed or accelerated.",
                "This result does not satisfy any Android Diagnostics runtime acceptance check.",
            ],
        }
        self.write_result()

    def write_result(self):
        temporary = self.output / "first-boot-result.json.tmp"
        temporary.write_text(json.dumps(self.result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        temporary.replace(self.result_path)

    def budget(self):
        remaining = self.deadline - time.monotonic()
        if remaining <= 0:
            raise ObservationBlocked("First-boot observation deadline expired")
        return min(self.args.command_timeout, remaining)

    def command(self, row, label, *arguments):
        timeout = self.budget()
        self.command_number += 1
        argv = [self.args.adb, "-s", self.args.serial, *arguments]
        prefix = f"{row['attempt']:03d}-{label}"
        stdout_name, stderr_name = prefix + ".txt", prefix + ".stderr.txt"
        started = time.monotonic()
        expired = False
        try:
            completed = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                       timeout=timeout, check=False)
            stdout, stderr, code = completed.stdout, completed.stderr, completed.returncode
        except subprocess.TimeoutExpired as error:
            stdout, stderr, code = error.stdout or b"", error.stderr or b"", None
            expired = True
        except OSError as error:
            stdout, stderr, code = b"", str(error).encode(), None
        (self.output / stdout_name).write_bytes(stdout)
        (self.output / stderr_name).write_bytes(stderr)
        row["commands"].append({
            "number": self.command_number, "argv": argv, "returncode": code, "timed_out": expired,
            "timeout_seconds": timeout, "seconds": round(time.monotonic() - started, 3),
            "stdout": stdout_name, "stderr": stderr_name,
            "stdout_sha256": hashlib.sha256(stdout).hexdigest(),
            "stderr_sha256": hashlib.sha256(stderr).hexdigest(),
        })
        if code != 0:
            reason = "timed out" if expired else f"returned {code}"
            raise ObservationBlocked(f"{label} {reason}; see {stdout_name} and {stderr_name}")
        self.budget()
        return stdout.decode("utf-8", errors="replace")

    @staticmethod
    def single_pid(raw):
        pids = raw.split()
        if len(pids) != 1 or not re.fullmatch(r"[1-9][0-9]*", pids[0]):
            raise ObservationBlocked(f"Expected one system_server PID; observed {pids!r}")
        return pids[0]

    def run(self):
        while time.monotonic() < self.deadline:
            started = time.monotonic()
            attempt = self.result["observation_count"] + 1
            row = {"attempt": attempt, "started_utc": datetime.now(timezone.utc).isoformat(),
                   "status": "blocked", "commands": []}
            try:
                before = self.single_pid(self.command(row, "pid-before", "shell", "pidof", "system_server"))
                row["system_server_before"] = before
                logs = self.command(row, "system-logcat", "logcat", "-b", "system", "-d", "-v", "threadtime",
                                    "ActivityManager:I", "*:S")
                after = self.single_pid(self.command(row, "pid-after", "shell", "pidof", "system_server"))
                row["system_server_after"] = after
                if before != after:
                    raise ObservationBlocked(f"system_server PID changed during the observation: {before} -> {after}")
                matched = next((line for line in logs.splitlines()
                                if (match := MARKER_LINE.fullmatch(line)) and match.group("pid") == before), None)
                self.budget()
                if matched is None:
                    raise ObservationBlocked("Exact user-0 BOOT_COMPLETED marker absent for current system_server PID " + before)
                row.update(status="observed", marker_line=matched)
                self.result.update(status="observed", system_server_pid=before, marker_line=matched,
                                   accepted_attempt=attempt, reason="Exact callback marker observed with one unchanged system_server PID")
            except ObservationBlocked as error:
                row["reason"] = str(error)
                self.result["reason"] = str(error)
            finally:
                row["seconds"] = round(time.monotonic() - started, 3)
                with self.observations_path.open("a", encoding="utf-8") as stream:
                    stream.write(json.dumps(row, ensure_ascii=False) + "\n")
                self.result["observation_count"] = attempt
                self.write_result()
            if self.result["status"] == "observed":
                break
            time.sleep(min(2, max(0, self.deadline - time.monotonic())))
        self.result.update(finished=True, finished_utc=datetime.now(timezone.utc).isoformat(),
                           seconds=round(time.monotonic() - self.started, 3))
        self.write_result()
        print(json.dumps({"status": self.result["status"], "result": str(self.result_path)}), flush=True)
        return 0 if self.result["status"] == "observed" else 2


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--adb", default="adb", help="Official SDK platform-tools adb executable")
    parser.add_argument("--serial", required=True, help="Fresh, job-owned CI AVD serial")
    parser.add_argument("--output", required=True, type=Path, help="New directory for this observer's evidence only")
    parser.add_argument("--timeout", type=float, default=600, help="Total observation budget in seconds")
    parser.add_argument("--command-timeout", type=float, default=10, help="Per-command cap in seconds, at most 10")
    args = parser.parse_args()
    if not all(math.isfinite(value) and value > 0 for value in (args.timeout, args.command_timeout)):
        parser.error("Timeout values must be finite and positive")
    if args.command_timeout > 10:
        parser.error("--command-timeout must not exceed 10 seconds")
    try:
        return FirstBootObserver(args).run()
    except (ObservationBlocked, OSError) as error:
        print("blocked: " + str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
