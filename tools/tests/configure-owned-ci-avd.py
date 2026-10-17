#!/usr/bin/env python3
"""One explicit test profile on a fresh CI AVD, never a physical/owner device.

Only Digital Wellbeing's per-user enabled state changes. No permission, global
ANR, security, Pine, launcher, document/storage provider or app-data changes.
"""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import subprocess

TARGET = "com.google.android.apps.wellbeing"
PINE = "io.github.pine.emu.diagnostics.preview"


def configure(args):
    args.output.mkdir(parents=True, exist_ok=True)
    report_file = args.output / "profile-report.json"
    commands_file = args.output / "commands.jsonl"
    if report_file.exists() or commands_file.exists():
        raise ValueError("Choose a new profile evidence directory")
    report = {"schema": 1, "profile": "api35-google-apis-without-wellbeing", "status": "blocked",
              "target_package": TARGET, "serial": args.serial, "physical_device": False,
              "permission_changes_requested": False, "app_data_clear_requested": False,
              "global_anr_changes_requested": False, "package_state_change_attempted": False,
              "reason": "Profile not yet verified"}

    def adb(label, *arguments):
        command = [str(args.adb), "-s", args.serial, *arguments]
        row = {"utc": datetime.now(timezone.utc).isoformat(), "label": label, "argv": command}
        try:
            result = subprocess.run(command, capture_output=True, timeout=20)
            row.update(returncode=result.returncode, timed_out=False)
            stdout, stderr = result.stdout, result.stderr
        except subprocess.TimeoutExpired as error:
            row.update(returncode=None, timed_out=True)
            stdout, stderr = error.stdout or b"", error.stderr or b""
        (args.output / (label + ".stdout.txt")).write_bytes(stdout)
        (args.output / (label + ".stderr.txt")).write_bytes(stderr)
        row.update(stdout=stdout.decode(errors="replace")[-3000:], stderr=stderr.decode(errors="replace")[-3000:])
        with commands_file.open("a") as stream:
            stream.write(json.dumps(row) + "\n")
        if row["timed_out"] or row["returncode"] != 0 or stderr.strip():
            raise ValueError("Profile command did not succeed: " + label)
        return stdout.decode(errors="replace").strip()

    try:
        if (not args.owned_ci_avd or os.environ.get("GITHUB_ACTIONS") != "true" or
                args.serial != "emulator-5554"):
            raise ValueError("Only an explicitly owned emulator-5554 in CI may use this profile")
        temporary = Path(os.environ["RUNNER_TEMP"]).resolve()
        avd_home = Path(os.environ["ANDROID_AVD_HOME"]).resolve()
        if (not avd_home.is_relative_to(temporary) or avd_home.name != "pine-diagnostics-avd" or
                not (avd_home / "PineDiagnosticsCI.avd/config.ini").is_file()):
            raise ValueError("Fresh job-owned AVD configuration is not under RUNNER_TEMP")
        report["avd_home"] = str(avd_home)
        if adb("qemu", "shell", "getprop", "ro.kernel.qemu") != "1":
            raise ValueError("Target is not an Android emulator")
        if adb("api", "shell", "getprop", "ro.build.version.sdk") != "35":
            raise ValueError("This explicit profile is only for the observed API35 image")
        if adb("boot", "shell", "getprop", "sys.boot_completed") != "1":
            raise ValueError("Android has not completed boot")
        # A successful list query returns an empty result for absence. In
        # contrast, package path may return an error for absence, which cannot
        # distinguish an uninstalled app from a failed/unknown package service.
        pine = adb("pine-absent", "shell", "cmd", "package", "list", "packages", "-u", PINE)
        if pine:
            raise ValueError("Pine is not demonstrably absent; this is not the required fresh test AVD")
        if adb("system-target", "shell", "cmd", "package", "list", "packages", "-s", TARGET) != "package:" + TARGET:
            raise ValueError("The exact observed Digital Wellbeing system package is not identified")
        before_anr = adb("anr-visibility-before", "shell", "settings", "get", "global", "hide_error_dialogs")
        if before_anr not in ("null", "0"):
            raise ValueError("Global error dialogs are already suppressed; profile refused")
        adb("package-before", "shell", "dumpsys", "package", TARGET)
        report["package_state_change_attempted"] = True
        adb("disable-wellbeing", "shell", "pm", "disable-user", "--user", "0", TARGET)
        if adb("disabled-target", "shell", "cmd", "package", "list", "packages", "-d", TARGET) != "package:" + TARGET:
            raise ValueError("Wellbeing disabled-user state was not confirmed; no fallback")
        adb("package-after", "shell", "dumpsys", "package", TARGET)
        after_anr = adb("anr-visibility-after", "shell", "settings", "get", "global", "hide_error_dialogs")
        if before_anr != after_anr:
            raise ValueError("Global ANR visibility observation changed; profile not verified")
        report.update(status="verified", reason="Only the exact identified Wellbeing package was disabled for user0 on this temporary AVD",
                      global_error_dialog_setting=after_anr)
    except (OSError, KeyError, ValueError) as error:
        report.update(reason=str(error))
    finally:
        report["finished_utc"] = datetime.now(timezone.utc).isoformat()
        report_file.write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report), flush=True)
    return 0 if report["status"] == "verified" else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--adb", required=True, type=Path)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--owned-ci-avd", action="store_true")
    raise SystemExit(configure(parser.parse_args()))


if __name__ == "__main__":
    main()
