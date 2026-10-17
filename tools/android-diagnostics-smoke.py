#!/usr/bin/env python3
"""Exercise a fresh Pine Diagnostics APK through official adb/UIAutomator.

Requires an already authorized, booted Android device with ARM64 support. This
runner never roots a device, changes Android security settings, clears logcat,
uninstalls an app, or wipes app/device data. A pre-existing target package blocks
the run. Only the app's ordinary settings UI changes its diagnostics settings;
run-as reads the debuggable app's fixed diagnostics files for evidence.

Exit 0: every required check passed. Exit 1: a check failed. Exit 2: a check was
blocked, including a document picker requiring manual completion. summary.json
is the source of truth; install/build/JVM success alone is never a runtime pass.
"""

import argparse
import hashlib
import io
import json
import re
import subprocess
import sys
import time
import traceback
import uuid
import xml.etree.ElementTree as ET
import zipfile
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path


PACKAGE = "io.github.pine.emu.diagnostics.preview"
APP_LABEL = "Pine Diagnostics Preview"
SETTINGS = PACKAGE + "/emu.skyline.settings.SettingsActivity"
UUID_RE = re.compile(r"[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}")
LOG_NAME = re.compile(r"(?:(?:main|emulation)/)?diagnostics-[0-7]\.jsonl")
ALLOWED = {"session-metadata.txt", "emulation.log"} | {
    prefix + "diagnostics-" + str(index) + ".jsonl"
    for prefix in ("", "main/", "emulation/") for index in range(8)
}
REQUIRED = (
    "apk_identity", "device_ready", "fresh_install", "installed_apk_identity",
    "settings_entry", "default_off", "enable_and_session", "native_jsonl",
    "logcat_jsonl", "verbose_setting", "retention_setting", "category_setting",
    "export_preview", "export_snapshot", "saf_save", "zip_verify", "disable_stop",
    "app_crash_free",
)


class Blocked(Exception):
    """An environmental limitation or missing observation, never a pass."""


class AppAnr(AssertionError):
    """An observed app ANR that ordinary UI cleanup must not mask."""


class SettingsBackBlocked(Blocked):
    """A bounded Settings recovery that generic cleanup must not repeat."""


class PlatformCrashBlocked(Blocked):
    """A bounded platform-dialog action that cleanup must not repeat."""


def digest(data):
    return hashlib.sha256(data).hexdigest()


def file_digest(path):
    hasher = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            hasher.update(chunk)
    return hasher.hexdigest()


def decode(data):
    return data.decode("utf-8", errors="replace")


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def parse_records(data, source):
    records = []
    for number, line in enumerate(data.splitlines(), 1):
        if not line.strip() or line.startswith(b"---------"):
            continue
        try:
            row = json.loads(line.decode("utf-8"))
        except (ValueError, UnicodeError) as error:
            raise AssertionError(f"{source}:{number}: invalid UTF-8/JSON: {error}") from error
        require(isinstance(row, dict) and row.get("schema") == 1,
                f"{source}:{number}: expected schema-1 JSON object")
        require(UUID_RE.fullmatch(str(row.get("session_id", ""))) is not None,
                f"{source}:{number}: invalid native session ID")
        require(isinstance(row.get("sequence"), int) and row["sequence"] > 0,
                f"{source}:{number}: invalid sequence")
        require(isinstance(row.get("fields"), dict), f"{source}:{number}: fields is not an object")
        records.append(row)
    return records


@dataclass
class Screen:
    root: ET.Element
    xml: Path
    number: int

    def text(self):
        return "\n".join(node.get("text", "") for node in self.root.iter("node"))

    @staticmethod
    def bounds(node):
        match = re.fullmatch(r"\[(\d+),(\d+)\]\[(\d+),(\d+)\]", node.get("bounds", ""))
        if not match:
            return None
        left, top, right, bottom = map(int, match.groups())
        return (left, top, right, bottom) if right > left and bottom > top else None

    def find(self, text=None, *, exact=False, resource=None, class_name=None):
        for node in self.root.iter("node"):
            if not self.bounds(node) or node.get("enabled") == "false":
                continue
            if resource and not node.get("resource-id", "").endswith(resource):
                continue
            if class_name and node.get("class") != class_name:
                continue
            if text is not None:
                values = (node.get("text", ""), node.get("content-desc", ""))
                if not any(value == text if exact else value.startswith(text) for value in values):
                    continue
            return node
        return None

    def find_button(self, text, *, exact=False):
        # Android's Button transformation may render these known app actions
        # in all caps. Keep generic text, platform dialogs and data checks exact.
        expected = text.casefold()
        for node in self.root.iter("node"):
            if (not self.bounds(node) or node.get("enabled") != "true" or
                    node.get("package") != PACKAGE or node.get("class") != "android.widget.Button"):
                continue
            actual = node.get("text", "").casefold()
            if actual == expected if exact else actual.startswith(expected):
                return node
        return None


class Smoke:
    def __init__(self, args):
        self.args = args
        self.output = args.output.resolve()
        self.output.mkdir(parents=True, exist_ok=True)
        if (self.output / "summary.json").exists() or (self.output / "commands.jsonl").exists():
            raise Blocked("Output contains a previous run; choose a new --output directory")
        self.command_number = 0
        self.screen_number = 0
        # UIAutomator runs as the authorized shell. Keep its temporary hierarchy
        # on shell-owned storage: first-boot FUSE failures can make /sdcard
        # unreadable even after dump reports success. SAF still uses Downloads.
        self.remote_xml = "/data/local/tmp/pine-diag-smoke-" + uuid.uuid4().hex + ".xml"
        self.session = None
        self.sessions = set()
        self.ever_enabled = False
        self.installed = False
        self.preview_entries = {}
        self.snapshot_bytes = {}
        self.snapshot_id = None
        self.saved_name = None  # Set only from the exact observed current-session SAF filename.
        self.logger = None
        self.log_stream = None
        self.results = {}
        self.summary = {
            "schema": 1, "started_utc": datetime.now(timezone.utc).isoformat(),
            "application_id": PACKAGE, "serial": args.serial,
            "expected_commit": args.expected_commit, "apk_path": str(args.apk.resolve()),
            "required_checks": list(REQUIRED), "checks": self.results,
            "limitations": [
                "A translated ARM64 APK run is not physical ARM64 or NCE/game validation.",
                "No game content, keys, tickets, or private saves are supplied.",
                "Retention/category checks verify configuration and native session restart, not load or rotation stress.",
            ],
            "platform_anr_wait_enabled": bool(getattr(args, "wait_known_platform_anr", False)),
            "platform_anr_wait_limit": 2,
            "platform_anr_waits": [],
            "platform_crash_dismiss_enabled": bool(getattr(args, "dismiss_known_platform_crash", False)),
            "platform_crash_dismiss_limit": 1,
            "platform_crash_dismissals": [],
            "settings_back_recovery_limit": 1,
            "settings_back_recoveries": [],
            "storage_readiness_required": bool(getattr(args, "require_ready_storage", False)),
            "system_last_anr_traces_enabled": bool(getattr(args, "collect_system_last_anr_traces", False)),
            "system_last_anr_traces_scope": "Latest system-wide ANR trace file and all retained data_app_anr DropBox entries; not filtered to Pine; entries may be old or truncated and include other processes.",
        }
        if self.summary["platform_crash_dismiss_enabled"]:
            self.summary["limitations"].append("One ordinary Bluetooth crash-dialog close is permitted during settings observation; it does not establish platform stability.")
        runner_source = Path(__file__).read_bytes()
        self.summary["runner_sha256"] = digest(runner_source)
        (self.output / "runner-source.py").write_bytes(runner_source)
        self.write_summary()

    def write_summary(self):
        states = [self.results.get(name, {}).get("status") for name in REQUIRED]
        self.summary["overall"] = (
            "fail" if "fail" in states else "pass" if all(s == "pass" for s in states) else "blocked"
        )
        self.summary["sessions_observed"] = sorted(self.sessions)
        temporary = self.output / "summary.json.tmp"
        temporary.write_text(json.dumps(self.summary, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        temporary.replace(self.output / "summary.json")
        status = {"pass": "verified", "fail": "failed", "blocked": "environment_blocked"}[self.summary["overall"]]
        report = {
            "schema": 1, "status": status,
            "finished": "finished_utc" in self.summary,
            "commit": self.args.expected_commit,
            "apk_sha256": self.summary.get("apk_sha256"),
            "application_id": PACKAGE, "serial": self.args.serial,
            "runner_sha256": self.summary["runner_sha256"],
            "physical_arm64_verified": False,
            "checks": self.results,
            "blockers": {name: row.get("reason", "") for name, row in self.results.items() if row["status"] == "blocked"},
            "failed_checks": {name: row.get("reason", "") for name, row in self.results.items() if row["status"] == "fail"},
            "device": self.summary.get("device", {}),
            "android_readiness": self.summary.get("android_readiness", {}),
            "capture_warnings": self.summary.get("capture_warnings", []),
            "platform_anr_wait_enabled": self.summary["platform_anr_wait_enabled"],
            "platform_anr_wait_limit": self.summary["platform_anr_wait_limit"],
            "platform_anr_waits": self.summary["platform_anr_waits"],
            "platform_crash_dismiss_enabled": self.summary["platform_crash_dismiss_enabled"],
            "platform_crash_dismiss_limit": self.summary["platform_crash_dismiss_limit"],
            "platform_crash_dismissals": self.summary["platform_crash_dismissals"],
            "platform_crash_dismiss_blocker": self.summary.get("platform_crash_dismiss_blocker"),
            "settings_back_recovery_limit": self.summary["settings_back_recovery_limit"],
            "settings_back_recoveries": self.summary["settings_back_recoveries"],
            "settings_back_recovery_blocker": self.summary.get("settings_back_recovery_blocker"),
            "storage_readiness_required": self.summary["storage_readiness_required"],
            "system_last_anr_traces_enabled": self.summary["system_last_anr_traces_enabled"],
            "system_last_anr_traces_scope": self.summary["system_last_anr_traces_scope"],
            "possible_platform_cause": self.summary.get("possible_platform_cause", []),
            "limitations": self.summary["limitations"],
            "evidence": sorted(p.name for p in self.output.iterdir() if p.is_file() and not p.name.endswith(".tmp")),
        }
        temporary = self.output / "runtime-report.json.tmp"
        temporary.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        temporary.replace(self.output / "runtime-report.json")

    def adb(self, *arguments, timeout=None, check=True, binary=False, save=None, save_stderr=None):
        command = [self.args.adb, "-s", self.args.serial, *map(str, arguments)]
        self.command_number += 1
        started = time.monotonic()
        expired = False
        try:
            result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    timeout=self.args.command_timeout if timeout is None else timeout, check=False)
            stdout, stderr, code = result.stdout, result.stderr, result.returncode
        except subprocess.TimeoutExpired as error:
            stdout, stderr, code = error.stdout or b"", error.stderr or b"", None
            expired = True
        except OSError as error:
            stdout, stderr, code = b"", str(error).encode(), None
        record = {
            "number": self.command_number, "utc": datetime.now(timezone.utc).isoformat(),
            "argv": command, "seconds": round(time.monotonic() - started, 3),
            "returncode": code, "timed_out": expired, "stdout_bytes": len(stdout),
            "stderr": decode(stderr)[-6000:],
        }
        record["stdout_sha256" if binary else "stdout"] = digest(stdout) if binary else decode(stdout)[-10000:]
        if save:
            (self.output / save).write_bytes(stdout)
            record["saved_stdout"] = save
        if save_stderr and stderr:
            (self.output / save_stderr).write_bytes(stderr)
            record["saved_stderr"] = save_stderr
        with (self.output / "commands.jsonl").open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(record, ensure_ascii=False) + "\n")
        if check and code != 0:
            reason = "timed out" if expired else f"returned {code}"
            raise Blocked(f"adb {' '.join(map(str, arguments))} {reason}; command {self.command_number}: {decode(stderr or stdout)[-1200:]}")
        return stdout

    def step(self, name, function, dependencies=()):
        missing = [d for d in dependencies if self.results.get(d, {}).get("status") != "pass"]
        before = {p.name for p in self.output.iterdir()}
        first_command = self.command_number + 1
        started = time.monotonic()
        try:
            if missing:
                raise Blocked("Required preceding checks did not pass: " + ", ".join(missing))
            detail = function() or {}
            result = {"status": "pass", **detail}
        except Blocked as error:
            result = {"status": "blocked", "reason": str(error)}
        except AssertionError as error:
            result = {"status": "fail", "reason": str(error)}
        except Exception as error:
            with (self.output / "runner-errors.txt").open("a", encoding="utf-8") as stream:
                traceback.print_exc(file=stream)
            result = {"status": "fail", "reason": f"Runner error: {type(error).__name__}: {error}"}
        result["seconds"] = round(time.monotonic() - started, 3)
        result["command_numbers"] = [first_command, self.command_number]
        result["evidence"] = sorted({p.name for p in self.output.iterdir()} - before)
        self.results[name] = result
        self.write_summary()
        print(f"{name}: {result['status']}" + (": " + result["reason"] if "reason" in result else ""), flush=True)

    def budget(self, deadline, observation):
        if deadline is None:
            return self.args.command_timeout
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise Blocked(f"{observation} exceeded its observation deadline")
        return min(self.args.command_timeout, remaining)

    @staticmethod
    def pause(deadline, seconds=1):
        time.sleep(min(seconds, max(0, deadline - time.monotonic())))

    def screenshot(self, label, deadline=None):
        # Captures are diagnostic only. They cannot extend an observation's
        # deadline or replace its original success/blocker with a capture error.
        timeout = min(5, self.args.command_timeout)
        if deadline is not None:
            timeout = min(timeout, max(0, deadline - time.monotonic()))
        command_number = None
        reason = "Screenshot skipped after the observation deadline"
        if timeout > 0:
            try:
                data = self.adb("exec-out", "screencap", "-p", binary=True, check=False,
                                timeout=timeout, save=f"{self.screen_number:03d}-{label}.png")
                command_number = self.command_number
                if data.startswith(b"\x89PNG\r\n\x1a\n"):
                    return
                reason = "screencap did not produce a PNG; UI XML remains the UI observation"
            except (Blocked, OSError) as error:
                reason = f"Screenshot capture failed: {error}; UI XML remains the UI observation"
        self.summary.setdefault("capture_warnings", []).append({
            "label": label, "command_number": command_number, "reason": reason,
        })
        try:
            (self.output / f"{self.screen_number:03d}-{label}-screenshot-error.txt").write_text(
                reason + "; see commands.jsonl\n", encoding="utf-8")
        except OSError:
            pass

    def screen(self, label, deadline=None):
        observation = f"UI observation '{label}'"
        self.screen_number += 1
        self.adb("shell", "uiautomator", "dump", "--compressed", self.remote_xml,
                 timeout=self.budget(deadline, observation))
        path = self.output / f"{self.screen_number:03d}-{label}.xml"
        data = self.adb("exec-out", "cat", self.remote_xml, save=path.name,
                        timeout=self.budget(deadline, observation))
        try:
            screen = Screen(ET.fromstring(data), path, self.screen_number)
        except ET.ParseError as error:
            raise Blocked(f"UIAutomator did not provide parseable XML: {path.name}: {error}") from error
        self.budget(deadline, observation)
        return screen

    def wait_ui(self, label, predicate):
        deadline = time.monotonic() + self.args.ui_timeout
        last = None
        last_error = None
        while time.monotonic() < deadline:
            try:
                last = self.screen(label, deadline=deadline)
                self.reject_app_anr(last)
                if self.wait_known_platform_dialog(last, deadline):
                    self.pause(deadline)
                    continue
                if self.dismiss_known_platform_crash(label, last, deadline):
                    self.pause(deadline)
                    continue
                if self.recover_known_search_locations(label, last, deadline):
                    self.pause(deadline)
                    continue
                if predicate(last):
                    self.budget(deadline, f"UI observation '{label}'")
                    self.screenshot(label, deadline=deadline)
                    return last
            except (SettingsBackBlocked, PlatformCrashBlocked):
                raise
            except Blocked as error:
                last_error = str(error)
            self.pause(deadline)
        self.screenshot(label + "-blocked", deadline=deadline)
        evidence = last.xml.name if last else last_error or "no UI XML"
        raise Blocked(f"UI observation '{label}' not reached within {self.args.ui_timeout}s; last evidence: {evidence}")

    @staticmethod
    def reject_app_anr(screen):
        # ANR is a separate app failure from the Java/native crash gate. Match
        # the actual Android dialog identity, not arbitrary application text.
        if any(node.get("package") == "android" and
               node.get("resource-id") == "android:id/alertTitle" and
               node.get("text") == APP_LABEL + " isn't responding"
               for node in screen.root.iter("node")):
            raise AppAnr(f"Pine application ANR observed: {APP_LABEL} isn't responding; see {screen.xml.name}")

    def dismiss_known_platform_crash(self, label, screen, deadline):
        """One opt-in Close app on the exact observed Bluetooth system dialog.

        This is ordinary UI recovery on a fresh CI AVD, not evidence of recovery
        or platform stability. Subsequent fresh predicates and all checks remain.
        """
        if label != "settings" or not self.summary["platform_crash_dismiss_enabled"]:
            return False
        titles = [node for node in screen.root.iter("node")
                  if node.get("resource-id") == "android:id/alertTitle"]
        if len(titles) != 1:
            return False
        title = titles[0]
        if (title.get("package") != "android" or title.get("class") != "android.widget.TextView" or
                title.get("enabled") != "true" or title.get("text") != "Bluetooth keeps stopping" or
                not Screen.bounds(title)):
            return False
        container = next((node for node in screen.root.iter() if title in list(node)), None)
        if container is None or container.get("package") != "android" or not Screen.bounds(container):
            return False
        buttons = [node for node in container if node.get("resource-id") == "android:id/aerr_close"]
        if len(buttons) != 1:
            return False
        button = buttons[0]
        if (button.get("package") != "android" or button.get("class") != "android.widget.Button" or
                button.get("enabled") != "true" or button.get("clickable") != "true" or
                button.get("text") != "Close app" or not Screen.bounds(button)):
            return False
        attempts = self.summary["platform_crash_dismissals"]
        if len(attempts) >= self.summary["platform_crash_dismiss_limit"]:
            reason = "Known Bluetooth crash dialog persists after the single ordinary Close app attempt"
            self.summary["platform_crash_dismiss_blocker"] = {"source_xml": screen.xml.name, "reason": reason}
            self.write_summary()
            raise PlatformCrashBlocked(reason)
        self.budget(deadline, "Known platform crash Close app")
        row = {"source_xml": screen.xml.name, "title": title.get("text"),
               "button_resource": button.get("resource-id"), "bounds": button.get("bounds"),
               "action": "Close app", "command_number": self.command_number + 1, "status": "attempted"}
        attempts.append(row)
        try:
            self.tap(button, deadline=deadline)
            self.budget(deadline, "Known platform crash Close app")
            row["status"] = "close_sent"
        except Blocked as error:
            row.update(status="blocked", reason=str(error))
            raise PlatformCrashBlocked(str(error)) from error
        finally:
            self.write_summary()
        return True

    def recover_known_search_locations(self, label, screen, deadline):
        if label != "settings":
            return False

        def own(nodes, **attributes):
            return next((node for node in nodes if Screen.bounds(node) and
                         node.get("package") == PACKAGE and node.get("enabled") == "true" and
                         all(node.get(key) == value for key, value in attributes.items())), None)

        toolbar = own(screen.root.iter("node"), **{"resource-id": PACKAGE + ":id/toolbar_layout",
                                                  "content-desc": "Search Locations"})
        if toolbar is None:
            return False
        locations = own(screen.root.iter("node"), **{"resource-id": PACKAGE + ":id/locations_list"})
        add = own(screen.root.iter("node"), **{"resource-id": PACKAGE + ":id/add_location_button",
                                              "class": "android.widget.Button"})
        navigate = own(toolbar.iter("node"), **{"class": "android.widget.ImageButton", "content-desc": "Navigate up",
                                                "clickable": "true"})
        if any(node is None for node in (locations, add, navigate)):
            return False
        attempts = self.summary["settings_back_recoveries"]
        if len(attempts) >= self.summary["settings_back_recovery_limit"]:
            reason = "Known Search Locations page persists after the single Settings Back attempt"
            self.summary["settings_back_recovery_blocker"] = {"source_xml": screen.xml.name, "reason": reason}
            self.write_summary()
            raise SettingsBackBlocked(reason)
        self.budget(deadline, "Settings Back recovery")
        row = {"source_xml": screen.xml.name, "action": "KEYCODE_BACK",
               "command_number": self.command_number + 1, "status": "attempted"}
        attempts.append(row)
        try:
            self.adb("shell", "input", "keyevent", "KEYCODE_BACK", timeout=self.budget(deadline, "Settings Back recovery"))
            self.budget(deadline, "Settings Back recovery")
            row["status"] = "back_sent"
        except Blocked as error:
            row.update(status="blocked", reason=str(error))
            raise SettingsBackBlocked(str(error)) from error
        finally:
            self.write_summary()
        return True

    def wait_known_platform_dialog(self, screen, deadline):
        """Opt-in, bounded ordinary Wait on two observed platform titles only.

        A sent tap never proves recovery. Only a subsequent fresh hierarchy and
        the original predicate can satisfy the surrounding UI observation.
        """
        if not self.summary["platform_anr_wait_enabled"]:
            return False
        title = screen.find(resource="android:id/alertTitle")
        if (title is None or title.get("package") != "android" or title.get("resource-id") != "android:id/alertTitle" or
                title.get("text") not in ("System UI isn't responding", "Process system isn't responding")):
            return False
        button = screen.find(resource="android:id/aerr_wait", class_name="android.widget.Button")
        if (button is None or button.get("package") != "android" or button.get("resource-id") != "android:id/aerr_wait" or
                button.get("enabled") != "true" or
                button.get("clickable") != "true" or button.get("text", "").casefold() != "wait"):
            return False
        attempts = self.summary["platform_anr_waits"]
        if len(attempts) >= self.summary["platform_anr_wait_limit"]:
            raise Blocked("Known platform ANR persists after the bounded ordinary Wait attempts; no further action")
        self.budget(deadline, "Known platform ANR Wait")
        row = {"source_xml": screen.xml.name, "title": title.get("text"),
               "button_resource": button.get("resource-id"), "bounds": button.get("bounds"),
               "action": "Wait", "command_number": self.command_number + 1, "status": "attempted"}
        attempts.append(row)
        try:
            self.tap(button, deadline=deadline)
            self.budget(deadline, "Known platform ANR Wait")
            row["status"] = "tap_sent"
        except Blocked as error:
            row.update(status="blocked", reason=str(error))
            raise
        finally:
            self.write_summary()
        return True

    def tap(self, node, deadline=None):
        bounds = Screen.bounds(node)
        if not bounds:
            raise Blocked("Cannot tap a UI node without visible bounds")
        left, top, right, bottom = bounds
        self.adb("shell", "input", "tap", (left + right) // 2, (top + bottom) // 2,
                 timeout=self.budget(deadline, "UI tap"))

    def seek(self, text, label, direction="down", exact=False, button=False):
        deadline = time.monotonic() + self.args.ui_timeout
        previous = None
        screen = None
        for _ in range(10):
            screen = self.screen(label, deadline=deadline)
            self.reject_app_anr(screen)
            if self.wait_known_platform_dialog(screen, deadline):
                self.pause(deadline)
                continue
            node = screen.find_button(text, exact=exact) if button else screen.find(text, exact=exact)
            self.budget(deadline, f"Seeking '{text}'")
            if node is not None:
                self.screenshot(label, deadline=deadline)
                return screen, node
            signature = ET.tostring(screen.root)
            if signature == previous:
                break
            previous = signature
            scrollable = next((n for n in screen.root.iter("node")
                               if n.get("scrollable") == "true" and Screen.bounds(n)), None)
            bounds = Screen.bounds(scrollable) if scrollable is not None else None
            if not bounds:
                break
            left, top, right, bottom = bounds
            x = (left + right) // 2
            low, high = top + (bottom - top) * 4 // 5, top + (bottom - top) // 5
            start, finish = (low, high) if direction == "down" else (high, low)
            self.adb("shell", "input", "swipe", x, start, x, finish, 350,
                     timeout=self.budget(deadline, f"Seeking '{text}'"))
            self.budget(deadline, f"Seeking '{text}'")
            self.pause(deadline, .5)
        self.screenshot(label + "-missing", deadline=deadline)
        evidence = screen.xml.name if screen else "no UI XML"
        raise Blocked(f"Visible control '{text}' unavailable after bounded scrolling; see {evidence}")

    def prefs(self):
        data = self.adb("exec-out", "run-as", PACKAGE, "cat", "shared_prefs/diagnostics.xml")
        try:
            root = ET.fromstring(data)
        except ET.ParseError as error:
            raise Blocked("Diagnostics preferences could not be read as XML") from error
        return {n.get("name"): n.get("value", n.text or "") for n in root}

    def marker(self, previous=None):
        deadline = time.monotonic() + self.args.ui_timeout
        while time.monotonic() < deadline:
            raw = self.adb("exec-out", "run-as", PACKAGE, "cat",
                           "files/diagnostics/main/session-id.txt", check=False,
                           timeout=self.budget(deadline, "Native session marker"))
            session = decode(raw).strip()
            self.budget(deadline, "Native session marker")
            if UUID_RE.fullmatch(session) and session != previous:
                self.session = session
                self.sessions.add(session)
                return session
            self.pause(deadline)
        raise Blocked("No new native-session marker observed after the UI settings change")

    def native_records(self, label, deadline=None):
        observation = f"Native records '{label}'"
        listing = decode(self.adb("exec-out", "run-as", PACKAGE, "ls", "-1", "files/diagnostics/main",
                                  timeout=self.budget(deadline, observation)))
        names = sorted(name for name in listing.splitlines() if re.fullmatch(r"diagnostics-[0-7]\.jsonl", name))
        rows = []
        for name in names:
            data = self.adb("exec-out", "run-as", PACKAGE, "cat", "files/diagnostics/main/" + name,
                            binary=True, save=label + "-main-" + name,
                            timeout=self.budget(deadline, observation))
            require(len(data) <= 2 * 1024 * 1024, f"Native {name} exceeds configured 2 MiB file bound")
            rows.extend(parse_records(data, name))
            self.budget(deadline, observation)
        self.budget(deadline, observation)
        return rows, names

    def wait_native(self, label, event, session):
        deadline = time.monotonic() + self.args.ui_timeout
        while time.monotonic() < deadline:
            rows, names = self.native_records(label, deadline=deadline)
            matching = [r for r in rows if r.get("session_id") == session and r.get("event") == event]
            self.budget(deadline, f"Native {event} observation")
            if matching:
                return rows, names, matching
            self.pause(deadline)
        raise Blocked(f"No native {event} record observed for session {session}")

    def apk_identity(self):
        if not self.args.apk.is_file():
            raise Blocked("APK file does not exist: " + str(self.args.apk))
        self.summary["apk_sha256"] = file_digest(self.args.apk)
        with zipfile.ZipFile(self.args.apk) as archive:
            libraries = sorted(n for n in archive.namelist() if n.startswith("lib/") and n.endswith(".so"))
            require("lib/arm64-v8a/libskyline.so" in libraries, "APK lacks ARM64 libskyline.so")
            require(all(n.startswith("lib/arm64-v8a/") for n in libraries), "APK contains an unexpected native ABI")
            for name in libraries:
                with archive.open(name) as stream:
                    header = stream.read(20)
                require(header[:6] == b"\x7fELF\x02\x01" and int.from_bytes(header[18:20], "little") == 183,
                        name + " is not ELF64 AArch64")
            dex = [n for n in archive.namelist() if re.fullmatch(r"classes\d*\.dex", n)]
            require(any(self.args.expected_commit.encode("ascii") in archive.read(n) for n in dex),
                    "Expected commit string absent from packaged DEX")
        return {"sha256": self.summary["apk_sha256"], "native_libraries": libraries,
                "commit_check": "packaged DEX presence; native SESSION_METADATA and exporter metadata are checked separately"}

    def device_ready(self):
        deadline = time.monotonic() + self.args.boot_timeout
        observations = []
        storage_required = self.summary["storage_readiness_required"]
        readiness = {"preinstall_ui_required": False, "continuity_window_seconds": 2,
                     "storage_readiness_required": storage_required,
                     "observations": observations}
        if storage_required:
            readiness["storage_probe_scope"] = "The internal private volume is a mount-service stub; mounted does not prove FUSE or installation readiness."
        self.summary["android_readiness"] = readiness
        sampled_multiple = False
        last_reason = "No readiness observation completed"
        # The boot flag can survive a framework restart. Missing/multiple PIDs,
        # a failed package query or a changed PID are retryable observations;
        # none permits choosing a candidate or skipping the continuity window.
        while time.monotonic() < deadline:
            started = time.monotonic()
            first_command = self.command_number + 1
            attempt = len(observations) + 1
            prefix = f"readiness-{attempt:03d}"
            row = {"attempt": attempt, "started_utc": datetime.now(timezone.utc).isoformat(),
                   "status": "error", "reason": "Readiness observation did not complete"}
            files, error_files = {}, {}
            accepted = None

            def observe(name, *arguments):
                row["stage"] = name
                files[name] = f"{prefix}-{name}.txt"
                error_files[name] = f"{prefix}-{name}.stderr.txt"
                data = self.adb(*arguments, timeout=self.budget(deadline, "Android readiness"),
                                save=files[name], save_stderr=error_files[name])
                self.budget(deadline, "Android readiness")
                return decode(data)

            def sample_candidates(pids):
                # Diagnostic only: never use /proc identity to select a PID.
                samples = row.setdefault("pid_status_samples", [])
                for index, pid in enumerate(pids[:4], 1):
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        break
                    name = f"candidate-{index}-{pid}-status"
                    files[name] = f"{prefix}-{name}.txt"
                    error_files[name] = f"{prefix}-{name}.stderr.txt"
                    self.adb("shell", "cat", f"/proc/{pid}/status", check=False,
                             timeout=min(2, self.args.command_timeout, remaining),
                             save=files[name], save_stderr=error_files[name])
                    samples.append({"pid": pid, "command_number": self.command_number,
                                    "stdout": files[name],
                                    "stderr": error_files[name] if (self.output / error_files[name]).is_file() else None})

            def single_pid(raw, position):
                nonlocal sampled_multiple
                pids = raw.split()
                if len(pids) != 1 or not re.fullmatch(r"[0-9]+", pids[0]):
                    if (not sampled_multiple and len(pids) > 1 and
                            all(re.fullmatch(r"[0-9]+", pid) for pid in pids)):
                        sampled_multiple = True
                        sample_candidates(pids)
                    raise Blocked(f"Expected one Android system_server {position}; observed PID list: {pids!r}")
                return pids[0]

            try:
                state = observe("adb-state", "get-state").strip()
                if state != "device":
                    raise Blocked("ADB device is not authorized/online: " + state)
                data = observe("properties", "shell", "getprop")
                properties = dict(re.findall(r"^\[([^]]+)\]: \[([^]]*)\]$", data, re.MULTILINE))
                row["boot_completed"] = properties.get("sys.boot_completed", "")
                self.summary["device"] = {key: properties.get(key, "") for key in (
                    "ro.build.fingerprint", "ro.build.version.sdk", "ro.product.cpu.abi",
                    "ro.product.cpu.abilist", "ro.dalvik.vm.native.bridge", "ro.kernel.qemu", "sys.boot_completed")}
                if row["boot_completed"] != "1":
                    raise Blocked("sys.boot_completed is not 1; Android boot is not complete")
                if "arm64-v8a" not in properties.get("ro.product.cpu.abilist", "").split(","):
                    raise Blocked("Guest does not advertise the APK's arm64-v8a ABI")
                before = single_pid(observe("system-server-before", "shell", "pidof", "system_server"), "before the package query")
                continuity_started = time.monotonic()
                package = observe("package-query", "shell", "cmd", "package", "path", "android")
                if not any(re.fullmatch(r"package:/\S+\.apk", line) for line in package.splitlines()):
                    raise Blocked("Android package manager did not answer its real package query with an APK path")
                row["package_query_answered"] = True
                if storage_required:
                    volumes = observe("storage-volumes", "shell", "sm", "list-volumes", "all")
                    mounted = next((line for line in volumes.splitlines()
                                    if re.fullmatch(r"private[ \t]+mounted[ \t]+\S+", line)), None)
                    row["storage_internal_mounted"] = mounted is not None
                    row["storage_internal_volume_line"] = mounted
                    if mounted is None:
                        raise Blocked("Storage service did not report the internal private volume mounted")
                if deadline - time.monotonic() <= 2:
                    raise Blocked("Readiness budget cannot fit the full two-second continuity window")
                time.sleep(2)
                after = single_pid(observe("system-server-after", "shell", "pidof", "system_server"), "after the continuity window")
                row["continuity_seconds"] = round(time.monotonic() - continuity_started, 3)
                self.budget(deadline, "Android readiness")
                if before != after:
                    raise Blocked(f"Android system_server changed during package/PID observation: {before} -> {after}")
                accepted = {"system_server_before": before, "system_server_after": after,
                            "package_query_answered": True, "preinstall_ui_required": False}
                if storage_required:
                    accepted["storage_internal_mounted"] = True
                row.update(status="pass", reason="One identical PID across the package query and full two-second window")
            except Blocked as error:
                last_reason = str(error)
                row.update(status="retry" if time.monotonic() < deadline else "blocked", reason=last_reason)
            except Exception as error:
                row.update(status="error", reason=f"{type(error).__name__}: {error}")
                raise
            finally:
                storage_file = self.output / files["storage-volumes"] if "storage-volumes" in files else None
                row["storage_volumes_raw"] = (storage_file.read_text(encoding="utf-8", errors="replace")
                                               if storage_file and storage_file.is_file() else None)
                for name, position in (("system-server-before", "before"), ("system-server-after", "after")):
                    path = self.output / files[name] if name in files else None
                    raw = path.read_text(encoding="utf-8", errors="replace") if path and path.is_file() else None
                    row[f"system_server_{position}_raw"] = raw
                    row[f"system_server_{position}_pids"] = raw.split() if raw is not None else None
                row["command_errors"] = {name: (self.output / path).read_text(encoding="utf-8", errors="replace")
                                         for name, path in error_files.items() if (self.output / path).is_file()}
                row["seconds"] = round(time.monotonic() - started, 3)
                row["command_numbers"] = [first_command, self.command_number]
                row["evidence"] = sorted(path for path in (*files.values(), *error_files.values())
                                         if (self.output / path).is_file())
                observations.append(row)
                with (self.output / "readiness-observations.jsonl").open("a", encoding="utf-8") as stream:
                    stream.write(json.dumps(row, ensure_ascii=False) + "\n")
                if accepted is not None:
                    readiness.update(accepted)
                    readiness["accepted_attempt"] = attempt
                    readiness["status"] = "pass"
                self.write_summary()
            if accepted is not None:
                return {**self.summary["device"], **accepted, "readiness_attempts": attempt,
                        "readiness_evidence": "readiness-observations.jsonl"}
            self.pause(deadline, 1 if row.get("boot_completed") == "1" else 5)
        readiness.update(status="blocked", reason=last_reason)
        self.write_summary()
        raise Blocked("Bounded Android boot/readiness gate expired before installation; last observation: " + last_reason)

    def fresh_install(self):
        existing = decode(self.adb("shell", "cmd", "package", "path", PACKAGE, check=False)).strip()
        if any(line.startswith("package:") for line in existing.splitlines()):
            raise Blocked("Target package already installed; fresh-default check requires a fresh target package. Nothing was uninstalled or cleared.")
        result = self.adb("install", str(self.args.apk.resolve()), timeout=self.args.install_timeout,
                          save="install.txt")
        require("Success" in decode(result).splitlines(), "adb install did not report Success")
        self.installed = True
        return {"fresh": True, "data_clear_or_uninstall": False}

    def installed_identity(self):
        result = decode(self.adb("shell", "cmd", "package", "path", PACKAGE, save="installed-package-path.txt"))
        paths = [line[len("package:"):] for line in result.splitlines() if line.startswith("package:")]
        require(len(paths) == 1 and paths[0].startswith("/data/app/") and paths[0].endswith("/base.apk"),
                "Expected exactly one installed base APK for the Diagnostics application ID")
        hashed = decode(self.adb("shell", "sha256sum", paths[0], save="installed-apk-sha256.txt")).split()
        require(hashed and hashed[0] == self.summary["apk_sha256"], "Installed APK SHA256 differs from supplied APK")
        return {"installed_sha256": hashed[0], "package_path": paths[0]}

    def start_logger(self):
        self.log_stream = (self.output / "runtime-logcat.txt").open("wb")
        command = [self.args.adb, "-s", self.args.serial, "logcat", "-T", "1", "-b", "main", "-b", "system", "-b", "crash",
                   "-v", "threadtime", "PineDiag:V", "AndroidRuntime:V", "libc:F", "DEBUG:V", "Watchdog:W",
                   "ndk_translation:V", "NativeBridge:V", "*:S"]
        self.logger = subprocess.Popen(command, stdout=self.log_stream, stderr=subprocess.STDOUT)
        self.summary["logcat_capture_argv"] = command

    @staticmethod
    def own_diagnostics_controls(screen):
        """Cleanup-only recognition: visible own switch plus own live status, no foreign dialog."""
        visible = [n for n in screen.root.iter("node") if Screen.bounds(n) and n.get("enabled") == "true"]
        if any(n.get("package") not in (PACKAGE, "com.android.systemui") for n in visible):
            return None
        switch = next((n for n in visible if n.get("package") == PACKAGE and
                       n.get("class") == "android.widget.Switch" and n.get("checkable") == "true" and
                       n.get("checked") in ("true", "false") and
                       n.get("text") == "Enable diagnostics / 启用诊断 (default off)"), None)
        status = next((n for n in visible if n.get("package") == PACKAGE and
                       n.get("class") == "android.widget.TextView" and
                       "Status process: main\n" in n.get("text", "") and
                       re.search(r"(?:^|\n)Session: [0-9a-f-]{36}\nSink active: (?:true|false)\nWritten: ", n.get("text", ""))), None)
        return switch if status is not None else None

    def open_settings(self, *, cleanup=False):
        result = decode(self.adb("shell", "am", "start", "-W", "-n", SETTINGS,
                                 save="settings-launch.txt"))
        require("Error:" not in result and "Exception" not in result, "Settings activity launch failed: " + result[-1500:])
        def preference(screen):
            node = screen.find("Developer diagnostics")
            return node if node is not None and (not cleanup or node.get("package") == PACKAGE) else None
        screen = self.wait_ui("settings", lambda screen: preference(screen) is not None or
                              (cleanup and self.own_diagnostics_controls(screen) is not None))
        if cleanup and self.own_diagnostics_controls(screen) is not None:
            return {"component": SETTINGS, "entry": "cleanup: already visible own Diagnostics controls and status",
                    "evidence": screen.xml.name}
        if cleanup:
            screen = self.wait_ui("settings-entry", lambda s: preference(s) is not None)
            node = preference(screen)
        else:
            _, node = self.seek("Developer diagnostics", "settings-entry")
        self.tap(node)
        self.wait_ui("diagnostics", lambda screen: screen.find("Enable diagnostics") is not None)
        return {"component": SETTINGS, "entry": "GlobalSettingsFragment developer_diagnostics preference"}

    def default_off(self):
        screen = self.wait_ui("default-off", lambda s: "Sink active:" in s.text())
        node = screen.find("Enable diagnostics")
        require(node is not None and node.get("checked") == "false", "Fresh Diagnostics enable switch was not off")
        require("Sink active: false" in screen.text(), "Fresh native sink is active or unavailable")
        return {"switch_checked": False, "native_sink_active": False}

    def enable(self):
        _, node = self.seek("Enable diagnostics", "enable", direction="up")
        require(node.get("checked") == "false", "Enable switch unexpectedly already checked")
        self.tap(node)
        self.ever_enabled = True
        session = self.marker()
        screen = self.wait_ui("enabled-status", lambda s: "Sink active: true" in s.text())
        require(screen.find("Enable diagnostics").get("checked") == "true", "Enable switch did not remain checked")
        require(self.prefs().get("enabled") == "true", "Enabled preference was not persisted")
        return {"session_id": session, "native_sink_active": True}

    def verify_native(self):
        rows, names, metadata = self.wait_native("enabled", "SESSION_METADATA", self.session)
        require(any(self.args.expected_commit in json.dumps(row) for row in metadata), "Native SESSION_METADATA lacks expected commit")
        return {"session_id": self.session, "records": len(rows), "files": names, "metadata_records": len(metadata)}

    def verify_logcat(self):
        data = self.adb("logcat", "-d", "-v", "raw", "PineDiag:V", "*:S", save="PineDiag-logcat.jsonl")
        rows = parse_records(data, "PineDiag logcat")
        current = [r for r in rows if r.get("session_id") == self.session]
        require(any(r.get("event") == "SESSION_METADATA" and self.args.expected_commit in json.dumps(r) for r in current),
                "No current-session native SESSION_METADATA with expected commit in actual PineDiag logcat")
        return {"current_session_records": len(current), "session_id": self.session}

    def verbose(self):
        previous = self.session
        _, node = self.seek("Detailed tracing", "verbose", direction="up")
        require(node.get("checked") == "false", "Fresh detailed tracing was not off")
        self.tap(node)
        self.marker(previous)
        require(self.prefs().get("verbose") == "true", "Detailed-tracing preference was not persisted")
        screen = self.wait_ui("verbose-on", lambda s: s.find("Detailed tracing") is not None and s.find("Detailed tracing").get("checked") == "true")
        self.wait_native("verbose", "SESSION_METADATA", self.session)
        return {"verbose": True, "new_session_id": self.session, "ui": screen.xml.name}

    def retention(self):
        previous = self.session
        _, node = self.seek("Retention /", "retention", button=True)
        self.tap(node)
        screen = self.wait_ui("retention-dialog", lambda s: s.find("2 files / 4 MiB", exact=True) is not None)
        self.tap(screen.find("2 files / 4 MiB", exact=True))
        self.marker(previous)
        require(self.prefs().get("files") == "2", "Retention preference was not persisted as two files")
        rows, names, _ = self.wait_native("retention", "SESSION_METADATA", self.session)
        require(len(names) <= 2, "Native retention change left more than two fixed log files")
        self.wait_ui("retention-status", lambda s: "Limit per process: 2 × 2 MiB" in s.text())
        return {"files_per_process": 2, "native_files_observed": names, "new_session_id": self.session, "records": len(rows)}

    def categories(self):
        previous = self.session
        _, node = self.seek("Diagnostic categories", "categories", button=True)
        self.tap(node)
        screen = self.wait_ui("category-dialog", lambda s: s.find("guest", exact=True) is not None)
        guest = screen.find("guest", exact=True)
        require(guest.get("checked") == "true", "Fresh guest category was not selected")
        self.tap(guest)
        screen = self.wait_ui("category-unchecked", lambda s: s.find("guest", exact=True) is not None and s.find("guest", exact=True).get("checked") == "false")
        positive = screen.find(resource="android:id/button1")
        if positive is None:
            raise Blocked("Category dialog has no visible Android positive button")
        self.tap(positive)
        self.marker(previous)
        selected = self.prefs().get("categories", "")
        require(selected == "content,storage,exception", "Unexpected persisted categories: " + selected)
        self.wait_native("categories", "SESSION_METADATA", self.session)
        return {"categories": selected, "new_session_id": self.session}

    def export_preview(self):
        _, node = self.seek("Preview and export", "export-button", button=True)
        self.tap(node)
        screen = self.wait_ui("export-preview", lambda s: s.find("Export snapshot /") is not None)
        entries = dict((name, int(size)) for name, size in re.findall(
            r"(?m)^([^\n]+) \((\d+) bytes\)$", screen.text()))
        require("session-metadata.txt" in entries, "Actual export preview lacks metadata entry")
        require(set(entries) <= ALLOWED, "Actual preview contains a non-allowlisted filename")
        require(any(LOG_NAME.fullmatch(name) for name in entries), "Actual preview contains no structured native logs")
        self.preview_entries = entries
        return {"entries": entries, "screen": screen.xml.name}

    def export_snapshot(self):
        listing = decode(self.adb("exec-out", "run-as", PACKAGE, "ls", "-1", "cache/diagnostics-exports"))
        candidates = [name for name in listing.splitlines() if re.fullmatch(r"snapshot-" + UUID_RE.pattern, name)]
        require(len(candidates) == 1, "Fresh run did not produce exactly one private export snapshot")
        self.snapshot_id = candidates[0]
        for name, expected_size in self.preview_entries.items():
            data = self.adb("exec-out", "run-as", PACKAGE, "cat",
                            "cache/diagnostics-exports/" + self.snapshot_id + "/" + name,
                            binary=True, save="snapshot-" + name.replace("/", "-"))
            require(len(data) == expected_size, "Preview byte count differs from captured snapshot: " + name)
            self.snapshot_bytes[name] = data
            if name.endswith(".jsonl"):
                parse_records(data, "snapshot/" + name)
        metadata = decode(self.snapshot_bytes["session-metadata.txt"])
        require("exporter_commit=" + self.args.expected_commit + "\n" in metadata,
                "Actual exporter metadata does not identify expected commit")
        require("configured_enabled=true\n" in metadata and "sink_active=true\n" in metadata,
                "Prepared snapshot does not report enabled native diagnostics")
        return {"snapshot_id": self.snapshot_id,
                "entry_sha256": {name: digest(data) for name, data in self.snapshot_bytes.items()}}

    def saf_save(self):
        screen = self.screen("export-confirm")
        self.reject_app_anr(screen)
        button = screen.find_button("Export", exact=True)
        if button is None:
            raise Blocked("Prepared preview Export action is unavailable; use retained preview XML/screenshot for manual handoff")
        self.tap(button)
        providers = ("com.google.android.documentsui", "com.android.documentsui")
        def control(screen, *, labels=None, resource=None, class_name=None, package=None):
            return next((n for n in screen.root.iter("node") if Screen.bounds(n) and n.get("enabled") == "true" and
                         n.get("package") in ((package,) if package else providers) and
                         (labels is None or n.get("text") in labels or n.get("content-desc") in labels) and
                         (resource is None or n.get("resource-id") == resource) and
                         (class_name is None or n.get("class") == class_name)), None)
        picker = self.wait_ui("saf-picker", lambda s: control(s) is not None)
        download_labels = ("Downloads", "Download", "下载")
        # Pick an explicit local Downloads root. Do not guess a provider or grant permissions.
        if control(picker, labels=download_labels) is None:
            drawer = control(picker, labels=("Show roots", "Open navigation drawer", "显示根目录", "显示位置", "打开导航抽屉"))
            if drawer is None:
                raise Blocked("SAF picker does not expose a recognized local Downloads root; complete export manually using saf-picker evidence")
            self.tap(drawer)
            picker = self.wait_ui("saf-roots", lambda s: control(s, labels=download_labels) is not None)
        root = control(picker, labels=download_labels)
        if root is not None:
            self.tap(root)
        picker = self.wait_ui("saf-downloads", lambda s: control(s, resource="android:id/title", class_name="android.widget.EditText") is not None)
        field = control(picker, resource="android:id/title", class_name="android.widget.EditText")
        if not self.session or not UUID_RE.fullmatch(self.session):
            raise Blocked("No verified current session for SAF filename validation")
        expected_name = "pine-diag-" + self.session + ".zip"
        if field.get("text") != expected_name:
            raise Blocked("SAF filename does not exactly match the current-session app default; no filename editing attempted")
        self.saved_name = field.get("text")
        save = control(picker, labels=("SAVE", "Save", "保存", "CREATE", "Create", "创建"),
                       resource="android:id/button1", class_name="android.widget.Button", package=field.get("package"))
        if save is None or save.get("clickable") != "true":
            raise Blocked("SAF Save control unavailable; complete the picker manually using saf-downloads evidence")
        self.tap(save)
        self.wait_ui("saf-return", lambda s: any(n.get("package") == PACKAGE for n in s.root.iter("node")))
        remote = "/sdcard/Download/" + self.saved_name
        deadline = time.monotonic() + self.args.ui_timeout
        while time.monotonic() < deadline:
            data = self.adb("exec-out", "cat", remote, check=False, binary=True,
                            save="exported-diagnostics.zip",
                            timeout=self.budget(deadline, "SAF ZIP observation"))
            try:
                with zipfile.ZipFile(io.BytesIO(data)) as archive:
                    if archive.testzip() is None:
                        self.budget(deadline, "SAF ZIP observation")
                        return {"destination": remote, "observed_filename": self.saved_name, "zip_bytes": len(data), "sha256": digest(data)}
            except (zipfile.BadZipFile, OSError):
                pass
            self.pause(deadline)
        raise Blocked("SAF did not produce a readable ZIP in the selected Downloads root; see picker evidence and commands.jsonl")

    def verify_zip(self):
        with zipfile.ZipFile(self.output / "exported-diagnostics.zip") as archive:
            names = archive.namelist()
            require(len(names) == len(set(names)), "Saved ZIP contains duplicate entry names")
            require(set(names) == set(self.preview_entries), "Saved ZIP entries differ from the real preview")
            require(set(names) <= ALLOWED, "Saved ZIP contains non-allowlisted entries")
            require(all(item.file_size <= (1024 * 1024 if item.filename == "session-metadata.txt" else 8 * 1024 * 1024)
                        for item in archive.infolist()), "Saved ZIP exceeds a per-entry export bound")
            require(sum(item.file_size for item in archive.infolist() if item.filename != "session-metadata.txt") <= 40 * 1024 * 1024,
                    "Saved ZIP exceeds the 40 MiB log-snapshot bound")
            require(archive.testzip() is None, "Saved ZIP CRC check failed")
            records = []
            for name in names:
                data = archive.read(name)
                require(data == self.snapshot_bytes[name], "Saved ZIP differs from prepared snapshot: " + name)
                if name.endswith(".jsonl"):
                    records.extend(parse_records(data, "ZIP/" + name))
            require(any(r.get("session_id") == self.session and r.get("event") == "SESSION_METADATA"
                        and self.args.expected_commit in json.dumps(r) for r in records),
                    "Saved ZIP lacks current-session native metadata for expected commit")
        return {"entries": names, "valid_jsonl_records": len(records), "preview_bytes_match": True}

    def disable(self):
        if not self.ever_enabled:
            raise Blocked("Diagnostics was never successfully enabled; no native Stop summary can be claimed")
        # Recover through the normal exported Settings activity if a picker/dialog blocked later checks.
        try:
            self.open_settings(cleanup=True)
        except (AppAnr, SettingsBackBlocked, PlatformCrashBlocked):
            raise
        except (Blocked, AssertionError):
            if self.summary["settings_back_recoveries"] or self.summary["platform_crash_dismissals"]:
                raise
            self.adb("shell", "input", "keyevent", "KEYCODE_BACK", check=False)
            self.open_settings(cleanup=True)
        screen = self.wait_ui("disable-controls", lambda s: self.own_diagnostics_controls(s) is not None)
        node = self.own_diagnostics_controls(screen)
        require(node.get("checked") == "true", "Diagnostics unexpectedly disabled before cleanup")
        ending_session = self.marker()
        self.tap(node)
        screen = self.wait_ui("disabled-status", lambda s: self.own_diagnostics_controls(s) is not None and "Sink active: false" in s.text())
        require(self.own_diagnostics_controls(screen).get("checked") == "false", "Disable switch remained checked")
        require(self.prefs().get("enabled") == "false", "Disabled preference did not persist")
        rows, names, summaries = self.wait_native("disabled", "SESSION_SUMMARY", ending_session)
        require(all(str(r["fields"].get("disk_errors")) == "0" for r in summaries), "Native Stop summary reports disk errors")
        self.verify_logcat()
        return {"sink_active": False, "session_id": ending_session,
                "stop_event": "SESSION_SUMMARY", "summaries": summaries, "native_files": names, "records": len(rows)}

    def app_crash_free(self):
        if self.logger is None:
            raise Blocked("App runtime logcat capture did not start")
        if self.logger.poll() is not None:
            raise Blocked("App runtime logcat capture exited before validation completed")
        self.log_stream.flush()
        text = (self.output / "runtime-logcat.txt").read_text(encoding="utf-8", errors="replace")
        patterns = (
            r"Process: " + re.escape(PACKAGE) + r"(?:[:,\s]|$)",
            r">>> " + re.escape(PACKAGE) + r"(?::[^ ]+)? <<<",
            r"Cmdline: " + re.escape(PACKAGE) + r"(?:[:\s]|$)",
        )
        app_crash = any(re.search(pattern, text) for pattern in patterns)
        platform_hints = [label for needle, label in (
            ("WATCHDOG KILLING SYSTEM PROCESS", "Android framework watchdog kill observed"),
            ("DeadSystemException", "DeadSystemException occurs somewhere in the captured log; process/block attribution requires review"),
        ) if needle in text]
        if app_crash and platform_hints:
            # Do not attribute another process's platform error to Pine or let it
            # mask a Pine native/Java crash. All observed Pine crashes fail this
            # conservative gate; the retained evidence permits manual attribution.
            self.summary["possible_platform_cause"] = platform_hints
        require(not app_crash, "An app Java/native crash was observed in runtime-logcat.txt")
        if "WATCHDOG KILLING SYSTEM PROCESS" in text:
            raise Blocked("Android framework watchdog restarted the system during the app test; runtime-logcat.txt retains platform failure evidence")
        return {"app_crash_observed": False, "capture": "runtime-logcat.txt",
                "scope": "Logcat tail started before first application launch; captures PineDiag and Java/native crash tags"}

    def capture_failure_process_evidence(self):
        # Read-only, best-effort diagnostics for this fresh Debug application.
        # A refused or timed-out probe cannot replace any functional result.
        def capture(name, timeout, *arguments):
            try:
                return self.adb(*arguments, check=False, timeout=min(timeout, self.args.command_timeout),
                                save=name + ".txt", save_stderr=name + ".stderr.txt")
            except (Blocked, OSError) as error:
                self.summary.setdefault("capture_warnings", []).append({
                    "label": name, "command_number": self.command_number, "reason": str(error),
                })
                return b""

        if self.summary["system_last_anr_traces_enabled"]:
            # This reads the system-wide latest file, without package filtering.
            # Only fresh, job-owned AVDs should opt in to this broader evidence.
            capture("last-anr-traces", 15, "shell", "dumpsys", "activity", "lastanr-traces")
            capture("dropbox-data-app-anr", 15, "shell", "dumpsys", "dropbox", "--print", "data_app_anr")
        capture("last-anr", 15, "shell", "dumpsys", "activity", "lastanr")
        raw = capture("pine-process-ids", 5, "shell", "pidof", PACKAGE)
        pids = decode(raw).split()
        if not all(re.fullmatch(r"[1-9][0-9]*", pid) for pid in pids):
            return
        for pid in list(dict.fromkeys(pids))[:2]:
            capture("pine-maps-" + pid, 5, "exec-out", "run-as", PACKAGE, "cat", f"/proc/{pid}/maps")
            capture("pine-native-backtrace-" + pid, 15, "exec-out", "run-as", PACKAGE,
                    "/system/bin/debuggerd", "-b", pid)

    def finish(self):
        if not self.installed and self.summary.get("device"):
            try:
                self.adb("logcat", "-b", "system", "-b", "crash", "-d", "-t", "1000", "-v", "threadtime",
                         "Watchdog:V", "SystemServer:V", "ActivityManager:E", "AndroidRuntime:E", "*:S",
                         save="android-framework-logcat.txt", timeout=20)
                self.adb("shell", "rm", "-f", self.remote_xml, check=False, timeout=10)
            except Blocked as error:
                self.summary["final_capture_blocker"] = str(error)
        if self.installed:
            if self.summary["overall"] != "pass":
                self.capture_failure_process_evidence()
            try:
                self.adb("logcat", "-b", "crash", "-d", "-v", "threadtime", save="crash-buffer.txt")
                self.adb("shell", "dumpsys", "activity", "activities", save="final-activities.txt")
                self.adb("shell", "rm", "-f", self.remote_xml, check=False)
            except Blocked as error:
                self.summary["final_capture_blocker"] = str(error)
        if self.logger is not None:
            self.logger.terminate()
            try:
                self.logger.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.logger.kill()
                self.logger.wait(timeout=5)
            self.log_stream.close()
        self.summary["finished_utc"] = datetime.now(timezone.utc).isoformat()
        self.write_summary()

    def run(self):
        try:
            self.step("apk_identity", self.apk_identity)
            self.step("device_ready", self.device_ready, ("apk_identity",))
            self.step("fresh_install", self.fresh_install, ("device_ready",))
            self.step("installed_apk_identity", self.installed_identity, ("fresh_install",))
            if self.results["installed_apk_identity"]["status"] == "pass":
                self.start_logger()
            self.step("settings_entry", self.open_settings, ("installed_apk_identity",))
            self.step("default_off", self.default_off, ("settings_entry",))
            self.step("enable_and_session", self.enable, ("default_off",))
            self.step("native_jsonl", self.verify_native, ("enable_and_session",))
            self.step("logcat_jsonl", self.verify_logcat, ("native_jsonl",))
            self.step("verbose_setting", self.verbose, ("enable_and_session",))
            self.step("retention_setting", self.retention, ("enable_and_session",))
            self.step("category_setting", self.categories, ("enable_and_session",))
            self.step("export_preview", self.export_preview, ("native_jsonl",))
            self.step("export_snapshot", self.export_snapshot, ("export_preview",))
            self.step("saf_save", self.saf_save, ("export_snapshot",))
            self.step("zip_verify", self.verify_zip, ("saf_save",))
        finally:
            # Also attempt ordinary UI cleanup when enabling was tapped but its
            # observation timed out. The failed enable check still blocks the gate.
            self.step("disable_stop", self.disable)
            # This must also run when an application crash prevented the Settings
            # entry observation; a UI timeout alone must not hide that failure.
            self.step("app_crash_free", self.app_crash_free, ("installed_apk_identity",))
            self.finish()
        print(json.dumps({"overall": self.summary["overall"], "summary": str(self.output / "summary.json"),
                          "runtime_report": str(self.output / "runtime-report.json")}), flush=True)
        return 0 if self.summary["overall"] == "pass" else 1 if self.summary["overall"] == "fail" else 2


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--adb", default="adb", help="Official SDK platform-tools adb executable")
    parser.add_argument("--serial", required=True, help="An already authorized Android device/emulator serial")
    parser.add_argument("--apk", required=True, type=Path, help="Fresh-install Debug Diagnostics ARM64 APK")
    parser.add_argument("--expected-commit", required=True, help="Full 40-character source commit embedded in the APK")
    parser.add_argument("--output", required=True, type=Path, help="New evidence directory, preserved even on failure")
    parser.add_argument("--command-timeout", type=int, default=60)
    parser.add_argument("--ui-timeout", type=int, default=120)
    parser.add_argument("--boot-timeout", type=int, default=120, help="Total pre-install Android boot/readiness gate budget")
    parser.add_argument("--install-timeout", type=int, default=300)
    parser.add_argument("--wait-known-platform-anr", action="store_true",
                        help="Fresh owned test devices only: at most two normal Wait taps on exact observed System UI/process-system ANR dialogs; never Close app or a Pine/unknown title")
    parser.add_argument("--dismiss-known-platform-crash", action="store_true",
                        help="Fresh job-owned CI AVDs only: during the initial settings observation, at most one ordinary Close app on the exact Android Bluetooth keeps stopping dialog; never Pine, unknown titles, ANRs or App info; does not prove platform stability")
    parser.add_argument("--collect-system-last-anr-traces", action="store_true",
                        help="Fresh job-owned AVDs only: on an unsuccessful installed run, read the latest system-wide ANR trace and retained data_app_anr DropBox entries; not filtered to Pine; entries may be old or truncated")
    parser.add_argument("--require-ready-storage", action="store_true",
                        help="Fresh job-owned AVDs only: require sm list-volumes all to report internal private mounted within the original readiness deadline; this does not prove FUSE or install readiness")
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9a-f]{40}", args.expected_commit):
        parser.error("--expected-commit must be a full lowercase 40-character Git commit")
    if min(args.command_timeout, args.ui_timeout, args.boot_timeout, args.install_timeout) < 1:
        parser.error("Timeout values must be positive")
    try:
        return Smoke(args).run()
    except Blocked as error:
        print("blocked: " + str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
