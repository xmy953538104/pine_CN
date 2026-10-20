#!/usr/bin/env python3
"""Real writer/checkpoint files and DAC failure/recovery. No APK/game execution."""
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CORE = ROOT / 'app/src/main/cpp/skyline/diagnostics'
flags = ['-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread', '-DPINE_DIAG_TEST_HOOKS']
if os.environ.get('PINE_DIAG_SANITIZE') == '1':
    flags += ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
with tempfile.TemporaryDirectory(prefix='pine-resource-persist-') as tmp:
    directory = Path(tmp)
    executable = directory / 'persist'
    subprocess.run([os.environ.get('CXX', 'c++'), *flags, '-I' + str(CORE), str(CORE / 'diagnostics.cpp'),
                    str(ROOT / 'tests/diagnostics/resource_persist_test.cpp'), '-o', str(executable)], check=True, timeout=90)
    logs = directory / 'logs'
    logs.mkdir()
    drop = None
    if os.geteuid() == 0:
        # Root would bypass the permission fault; run only this child unprivileged.
        directory.chmod(0o755)
        os.chown(logs, 65534, 65534)
        def drop():
            os.setgroups([])
            os.setgid(65534)
            os.setuid(65534)
    subprocess.run([str(executable), str(logs)], check=True, timeout=20, preexec_fn=drop)
    rows = [json.loads(line) for path in logs.glob('diagnostics-*.jsonl') for line in path.read_text().splitlines()]
    failed = [r for r in rows if r['event'] == 'RESOURCE_WINDOW_PERSIST_FAILED']
    recovered = [r for r in rows if r['event'] == 'RESOURCE_WINDOW_PERSIST_RECOVERED']
    assert len(failed) == len(recovered) == 1, (len(failed), len(recovered))
    detail = failed[0]['fields']
    assert detail['failure_stage'] == 'open_temporary' and detail['failure_reason'] == 'syscall_error', detail
    assert detail['error_code'] == 'EACCES' and detail['errno'] == '13' and detail['window_persist_errors'] == '1'
    assert detail['checkpoint_trigger'] == 'exception_observed'
    assert detail['failure_scope'] == 'current_writer_diagnostic_checkpoint_not_guest_storage'
    detail = recovered[0]['fields']
    assert detail['previous_failure_stage'] == 'open_temporary' and detail['previous_failure_reason'] == 'syscall_error'
    assert detail['previous_error_code'] == 'EACCES' and detail['window_persist_errors'] == '8', detail
    assert detail['recovery_scope'] == 'completed_checkpoint_not_game_io_or_power_loss_guarantee'
    assert all(r['session_id'] == 'persist-session' and r['launch_id'] == 'persist-launch' for r in failed + recovered)
    assert all('/' not in value and 'pine-resource-persist-' not in value for r in failed + recovered for value in r['fields'].values())
    summary = next(r for r in rows if r['event'] == 'SESSION_SUMMARY')['fields']
    assert summary['disk_errors'] == '0' and int(summary['snapshot_errors']) >= 8
    window = (logs / 'resource-window.jsonl').read_bytes()
    assert len(window) <= 131072 and window.endswith(b'\n')
    records = [json.loads(line) for line in window.splitlines()]
    assert records[0]['record_kind'] == 'resource_window_metadata' and records[0]['reason'] == 'session_end'
    assert records[0]['retained_records'] == len(records) - 1
    assert any(r.get('event') == 'CONTROLLED_CHECKPOINT_RECOVERY' for r in records[1:])
    off = [json.loads(line) for path in (logs / 'resource-off').glob('diagnostics-*.jsonl') for line in path.read_text().splitlines()]
    assert not any(r['event'].startswith('RESOURCE_WINDOW_') for r in off)
    assert not (logs / 'resource-off/resource-window.jsonl').exists()
print('PASS fixed stage/reason/error, first failure rate limit, exact counter, real recovery, independent summary counters and resource-off')
