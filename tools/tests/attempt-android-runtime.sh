#!/usr/bin/env bash
# Official SDK tools only; never change KVM permissions, guest security or APK ABI.
set -euo pipefail
# Python keeps these noninteractive probes portable without GNU timeout/realpath.
# Command output is inherited live. A deadline returns 124, with at most two
# additional seconds for TERM/KILL and bounded reaping of the owned process group.
run_bounded() {
  python3 - "$@" <<'PY'
import os, signal, subprocess, sys
seconds = float(sys.argv[1])
try:
    process = subprocess.Popen(sys.argv[2:], stdin=subprocess.DEVNULL, start_new_session=True)
except OSError as error:
    print(error, file=sys.stderr)
    raise SystemExit(127)
def signal_group(signum):
    try:
        os.killpg(process.pid, signum)
    except ProcessLookupError:
        pass
def interrupted(signum, frame):
    signal_group(signal.SIGKILL)
    raise SystemExit(128 + signum)
signal.signal(signal.SIGTERM, interrupted)
signal.signal(signal.SIGINT, interrupted)
try:
    try:
        result = process.wait(timeout=seconds)
    except subprocess.TimeoutExpired:
        print(f'Command exceeded {seconds:g}s deadline; terminating its process group.', file=sys.stderr)
        result = 124
finally:
    signal_group(signal.SIGTERM)
    try:
        process.wait(timeout=1)
    except subprocess.TimeoutExpired:
        pass
    signal_group(signal.SIGKILL)
    try:
        process.wait(timeout=1)
    except subprocess.TimeoutExpired:
        pass
raise SystemExit(result if result >= 0 else 128 - result)
PY
}
apk=$1
commit=$2
out=$3
mkdir -p "$out"
out=$(python3 - "$out" <<'PY'
import pathlib, sys
print(pathlib.Path(sys.argv[1]).resolve())
PY
)
adb="$ANDROID_HOME/platform-tools/adb"
emulator="$ANDROID_HOME/emulator/emulator"
export ANDROID_AVD_HOME="$RUNNER_TEMP/pine-diagnostics-avd"
mkdir -p "$ANDROID_AVD_HOME"
accel_check_status=0
run_bounded 20 "$emulator" -accel-check > "$out/accel-check.txt" 2>&1 && accel=on || { accel_check_status=$?; accel=off; }
printf '\naccel_check_exit_status=%s\n' "$accel_check_status" >> "$out/accel-check.txt"
uname -a > "$out/host.txt"
if [[ $(uname -s) == Linux ]]; then ls -l /dev/kvm /dev/dri >> "$out/host.txt" 2>&1 || true; fi
python3 - <<'PY' >> "$out/host.txt"
import json, os, pathlib, platform, subprocess
try:
    affinity = sorted(os.sched_getaffinity(0)) if hasattr(os, 'sched_getaffinity') else None
except OSError:
    affinity = None
print(json.dumps({'system': platform.system(), 'machine': platform.machine(), 'cpu_count': os.cpu_count(),
                  'affinity': affinity, 'guest_cores': 4, 'guest_memory_mib': 6144}))
if platform.system() == 'Linux':
    for name in ('/sys/fs/cgroup/cpu.max', '/sys/fs/cgroup/memory.max', '/proc/meminfo'):
        try:
            text = pathlib.Path(name).read_text().strip()
        except OSError:
            continue
        if name == '/proc/meminfo':
            text = next((line for line in text.splitlines() if line.startswith('MemTotal:')), 'MemTotal unavailable')
        print(name + ': ' + text)
elif platform.system() == 'Darwin':
    for name in ('hw.memsize', 'hw.logicalcpu', 'kern.hv_support', 'kern.osrelease'):
        try:
            probe = subprocess.run(['sysctl', '-n', name], capture_output=True, text=True, timeout=2)
            print(json.dumps({'sysctl': name, 'returncode': probe.returncode,
                              'stdout': probe.stdout.strip(), 'stderr': probe.stderr.strip()}))
        except (OSError, subprocess.TimeoutExpired) as error:
            print(json.dumps({'sysctl': name, 'unavailable': str(error)}))
PY
# This is a fresh, job-local AVD, never a user's existing device or disk.
printf 'no\n' | avdmanager create avd --name PineDiagnosticsCI --package 'system-images;android-35;google_apis;x86_64' --path "$ANDROID_AVD_HOME/PineDiagnosticsCI.avd" > "$out/avd-create.txt" 2>&1
# API35 attempts also observed system-app and Pine ANRs with two cores / 4 GiB.
# Retry with four cores / 6 GiB inside the same standard runner, retaining the
# original deadlines and failure probes. This is an empirical capacity retry,
# not an established cause or a game-performance test. No paid runner is used.
"$emulator" -avd PineDiagnosticsCI -port 5554 -no-window -no-audio -no-snapshot -no-boot-anim -gpu swiftshader -accel "$accel" -cores 4 -memory 6144 -skin 480x800 -verbose > "$out/emulator.log" 2>&1 &
emulator_pid=$!
framework_logger_pid=''
cleanup() {
  run_bounded 20 "$adb" -s emulator-5554 logcat -d -t 2000 -v threadtime > "$out/android-logcat.txt" 2>&1 || true
  if [[ -n "$framework_logger_pid" ]]; then kill -TERM "$framework_logger_pid" 2>/dev/null || true; fi
  kill -TERM "$emulator_pid" 2>/dev/null || true
}
trap cleanup EXIT
ready=false
started=$SECONDS
# An observed official API34 first boot spent over 18 minutes in system package
# initialization under software acceleration, without a framework restart.
while (( SECONDS - started < 1800 )); do
  if ! kill -0 "$emulator_pid" 2>/dev/null; then break; fi
  boot=$(run_bounded 12 "$adb" -s emulator-5554 shell getprop sys.boot_completed 2>/dev/null | tr -d '\r') || boot=''
  if [[ "$boot" == 1 ]]; then ready=true; break; fi
  sleep 5
done
if [[ "$ready" != true ]]; then
  python3 - "$apk" "$commit" "$out" "$accel" <<'PY'
import hashlib,json,pathlib,sys
apk,commit,out,accel=sys.argv[1:]
hasher=hashlib.sha256()
with open(apk,'rb') as f:
    for chunk in iter(lambda: f.read(1024*1024), b''): hasher.update(chunk)
digest=hasher.hexdigest()
report={'schema':1,'commit':commit,'apk_sha256':digest,'status':'environment_blocked','physical_arm64_verified':False,
        'reason':'Official Android emulator did not complete boot within 1800s. No Pine installation or UI success is claimed.',
        'acceleration':accel,'evidence':['emulator.log','host.txt','accel-check.txt','android-logcat.txt']}
pathlib.Path(out,'runtime-report.json').write_text(json.dumps(report,indent=2)+'\n')
PY
  if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
    echo 'Android runtime attempt: **environment blocked**. Boot did not complete; APK UI was not tested. See runtime-report.json and emulator logs.' >> "$GITHUB_STEP_SUMMARY"
  fi
  exit 0
fi
# Retain the whole install interval: a bounded tail after a slow installation
# can otherwise lose the framework event that first stalled the package service.
"$adb" -s emulator-5554 logcat -T 1 -b all -v threadtime > "$out/android-install-logcat.txt" 2>&1 &
framework_logger_pid=$!
# The first API36 software-accelerated attempt completed boot but exhausted the
# normal 300s install budget. This larger, finite CI-only budget changes neither
# device security nor the APK/guest behavior; all unobserved checks stay blocked.
# This flag is scoped to this newly created, job-owned AVD. Ordinary users of
# the smoke runner do not automatically interact with another app's ANR dialog.
if python3 tools/android-diagnostics-smoke.py --adb "$adb" --serial emulator-5554 --apk "$apk" --expected-commit "$commit" --output "$out" --install-timeout 900 --wait-known-platform-anr; then
  smoke_result=0
else
  smoke_result=$?
fi
case "$smoke_result" in
  0)
    if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
      echo 'Android Diagnostics runtime: **verified** for the exact APK SHA256 and source commit in runtime-report.json. This is translated emulator UI validation, not physical ARM64 or NCE/game validation.' >> "$GITHUB_STEP_SUMMARY"
    fi
    ;;
  2)
    if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
      echo 'Android Diagnostics runtime: **environment blocked**. Individual UI, native-log, export and cleanup observations are recorded in runtime-report.json; unobserved checks are not passes. Retain the screenshots, UI XML and command log for manual handoff.' >> "$GITHUB_STEP_SUMMARY"
    fi
    # Preserve the environmental limitation without presenting it as an APK bug.
    exit 0
    ;;
  *)
    if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
      echo 'Android Diagnostics runtime: **failed**. See failed_checks and exact APK identity in runtime-report.json, plus screenshots and Android crash evidence.' >> "$GITHUB_STEP_SUMMARY"
    fi
    exit "$smoke_result"
    ;;
esac
