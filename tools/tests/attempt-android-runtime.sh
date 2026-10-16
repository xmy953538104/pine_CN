#!/usr/bin/env bash
# Official SDK tools only; never change KVM permissions, guest security or APK ABI.
set -euo pipefail
apk=$1
commit=$2
out=$3
mkdir -p "$out"
out=$(realpath "$out")
adb="$ANDROID_HOME/platform-tools/adb"
emulator="$ANDROID_HOME/emulator/emulator"
export ANDROID_AVD_HOME="$RUNNER_TEMP/pine-diagnostics-avd"
mkdir -p "$ANDROID_AVD_HOME"
"$emulator" -accel-check > "$out/accel-check.txt" 2>&1 && accel=on || accel=off
uname -a > "$out/host.txt"
ls -l /dev/kvm /dev/dri >> "$out/host.txt" 2>&1 || true
python3 - <<'PY' >> "$out/host.txt"
import json, os, pathlib
print(json.dumps({'cpu_count': os.cpu_count(), 'affinity': sorted(os.sched_getaffinity(0)), 'guest_memory_mib': 3072}))
for name in ('/sys/fs/cgroup/cpu.max', '/sys/fs/cgroup/memory.max'):
    path = pathlib.Path(name)
    if path.is_file():
        print(name + ': ' + path.read_text().strip())
print(next(line for line in pathlib.Path('/proc/meminfo').read_text().splitlines() if line.startswith('MemTotal:')))
PY
# This is a fresh, job-local AVD, never a user's existing device or disk.
printf 'no\n' | avdmanager create avd --name PineDiagnosticsCI --package 'system-images;android-36;google_apis;x86_64' --path "$ANDROID_AVD_HOME/PineDiagnosticsCI.avd" > "$out/avd-create.txt" 2>&1
"$emulator" -avd PineDiagnosticsCI -port 5554 -no-window -no-audio -no-snapshot -no-boot-anim -gpu swiftshader -accel "$accel" -cores 2 -memory 3072 -skin 480x800 -verbose > "$out/emulator.log" 2>&1 &
emulator_pid=$!
cleanup() {
  timeout 20 "$adb" -s emulator-5554 logcat -d -t 2000 -v threadtime > "$out/android-logcat.txt" 2>&1 || true
  kill -TERM "$emulator_pid" 2>/dev/null || true
}
trap cleanup EXIT
ready=false
started=$SECONDS
# An observed official API34 first boot spent over 18 minutes in system package
# initialization under software acceleration, without a framework restart.
while (( SECONDS - started < 1800 )); do
  if ! kill -0 "$emulator_pid" 2>/dev/null; then break; fi
  boot=$(timeout 12 "$adb" -s emulator-5554 shell getprop sys.boot_completed 2>/dev/null | tr -d '\r') || boot=''
  if [[ "$boot" == 1 ]]; then ready=true; break; fi
  sleep 5
done
if [[ "$ready" != true ]]; then
  python3 - "$apk" "$commit" "$out" "$accel" <<'PY'
import hashlib,json,pathlib,sys
apk,commit,out,accel=sys.argv[1:]
with open(apk,'rb') as f: digest=hashlib.file_digest(f,'sha256').hexdigest()
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
if python3 tools/android-diagnostics-smoke.py --adb "$adb" --serial emulator-5554 --apk "$apk" --expected-commit "$commit" --output "$out"; then
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
