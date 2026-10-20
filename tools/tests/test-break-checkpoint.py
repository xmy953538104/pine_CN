#!/usr/bin/env python3
"""Actual writer checkpoint, identity, priority, async gate and loss boundaries."""
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
with tempfile.TemporaryDirectory(prefix='pine-break-checkpoint-') as tmp:
    directory = Path(tmp)
    executable = directory / 'checkpoint'
    subprocess.run([os.environ.get('CXX', 'c++'), *flags, '-I' + str(CORE), str(CORE / 'diagnostics.cpp'),
                    str(ROOT / 'tests/diagnostics/break_checkpoint_test.cpp'), '-o', str(executable)], check=True, timeout=90)
    subprocess.run([str(executable), str(directory / 'logs')], check=True, timeout=25)
    enabled = directory / 'logs/enabled'
    ordinary = [json.loads(row) for row in (enabled / 'diagnostics-0.jsonl').read_bytes().splitlines()]
    capabilities = next(r for r in ordinary if r['event'] == 'RESOURCE_CAPABILITIES')['fields']
    for name in ['nvn_command_capacity', 'nvn_control_capacity', 'nvn_remaining', 'nvn_growth_request', 'nvn_growth_callback']:
        assert capabilities[name] == 'unavailable_no_user_api_hook'
    def rows(name):
        path = enabled / name
        data = path.read_bytes()
        assert data.endswith(b'\n')
        if 'window' in name: assert len(data) <= 131072
        return [json.loads(row) for row in data.splitlines()]
    checkpoint = rows('break-window.jsonl')
    critical = rows('break-critical.jsonl')
    request = next(r for r in checkpoint[1:] if r.get('event') == 'GUEST_BREAK_REQUEST')
    state = next(r for r in critical if r['snapshot_metadata']['observed_event'] == 'GUEST_BREAK_REQUEST')
    for key in ['sequence', 'wall_clock_ms', 'monotonic_us', 'session_id', 'process_run_id', 'launch_id']:
        assert request[key] == state[key], (key, request[key], state[key])
    assert (request['session_id'], request['process_run_id'], request['launch_id']) == ('break-session', 'break-process', 'break-launch')
    assert request['fields']['notification_only'] == 'false'
    assert state['fields']['notification_only'] == 'false'
    assert checkpoint[0]['reason'] == 'guest_break_observed'
    dense = rows('dense-window.jsonl')
    assert any(r == request for r in dense[1:]), 'Critical request lost under dense ordinary samples'
    assert 0 < sum(r.get('event') == 'CONTROLLED_DENSE_SAMPLE' for r in dense[1:]) < 120
    assert [r['sequence'] for r in dense[1:]] == sorted(r['sequence'] for r in dense[1:])
    assert all(r['event'] != 'PREVIOUS_NATIVE_TRACE' for r in checkpoint[1:] + dense[1:])
    fault = directory / 'logs/checkpoint-fault'
    events = [json.loads(row) for row in (fault / 'diagnostics-0.jsonl').read_bytes().splitlines()]
    failed = next(r for r in events if r['event'] == 'RESOURCE_WINDOW_PERSIST_FAILED')
    assert failed['fields']['checkpoint_trigger'] == 'guest_break_observed'
    assert failed['fields']['failure_reason'] == 'policy_rejected'
    assert any(r['event'] == 'RESOURCE_WINDOW_PERSIST_RECOVERED' for r in events)
    summary = next(r for r in events if r['event'] == 'SESSION_SUMMARY')['fields']
    assert summary['disk_errors'] == '0' and summary['resource_window_checkpoint_status'] == 'ok'
    assert summary['resource_window_persist_errors'] == '1'
print('PASS actual async Break checkpoint, original identity/time, critical retention, notification/off/overflow boundaries; no APK/crash-atomic claim')
