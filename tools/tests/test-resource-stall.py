#!/usr/bin/env python3
"""Actual diagnostics.cpp writer integration with only resource sampling clock injected.
No APK/game/Vulkan execution and no claim of a real-time 30-second stall.
"""
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
with tempfile.TemporaryDirectory(prefix='pine-resource-stall-') as tmp:
    directory = Path(tmp)
    binary = directory / 'stall'
    subprocess.run([os.environ.get('CXX', 'c++'), *flags, '-I' + str(CORE), str(CORE / 'diagnostics.cpp'), str(ROOT / 'tests/diagnostics/resource_stall_test.cpp'), '-o', str(binary)], check=True, timeout=90)
    logs = directory / 'logs'
    subprocess.run([str(binary), str(logs)], check=True, timeout=30)
    rows = [json.loads(line) for path in logs.glob('diagnostics-*.jsonl') for line in path.read_text().splitlines()]
    samples = [r for r in rows if r['event'] == 'RESOURCE_GPU_SAMPLE']
    assert len(samples) == 97, len(samples)
    assert all(r['fields']['sampling_over_budget'] == '0' for r in samples)
    assert all(r['fields']['host_submit_accepted'] == '0' for r in samples)
    assert samples[1]['fields']['observation_state'] == 'observing'
    assert samples[1]['fields']['observed_quiet_ms'] == '0'
    assert [int(r['fields']['observed_quiet_ms']) for r in samples[1:32]] == list(range(0, 30001, 1000))
    stall = [r for r in rows if r['event'] == 'RESOURCE_POST_PROGRESS_NO_NEW_PRESENT']
    recovery = [r for r in rows if r['event'] == 'RESOURCE_PRESENT_RECOVERED']
    assert len(stall) == len(recovery) == 1
    assert stall[0]['fields']['observed_quiet_ms'] == '30000'
    assert stall[0]['fields']['observed_accepted_delta'] == '3'
    assert all(r['fields']['observation_state'] == 'presentation_paused' and r['fields']['observed_quiet_ms'] == '0' for r in samples[33:65])
    assert all(r['fields']['observation_state'] == 'guest_exit_process_requested' and r['fields']['observed_quiet_ms'] == '0' for r in samples[65:])
    for filename, reason, event in [('stall-window.jsonl', 'post_progress_no_new_present', 'RESOURCE_POST_PROGRESS_NO_NEW_PRESENT'), ('recovered-window.jsonl', 'recovered', 'RESOURCE_PRESENT_RECOVERED')]:
        data = (logs / filename).read_bytes()
        assert len(data) <= 131072
        snapshot = [json.loads(line) for line in data.splitlines()]
        assert snapshot[0]['reason'] == reason
        assert snapshot[0]['time_scope'] == 'writer_ingestion_monotonic_not_event_execution'
        assert snapshot[0]['retained_records'] == len(snapshot) - 1
        assert any(r['event'] == event for r in snapshot[1:])
        assert snapshot[0]['session_id'] == 'resource-stall-session'
    assert all(r['session_id'] == 'resource-stall-session' and r['launch_id'] == 'resource-stall-launch' for r in samples)
    assert samples[0]['monotonic_us'] > 1000 * 1000, 'Do not inject resource clock into event timestamps'
    assert samples[-1]['monotonic_us'] - samples[0]['monotonic_us'] < 30_000_000
    barrier = next(r for r in rows if r['event'] == 'RESOURCE_OFF_BARRIER')
    assert not any(r['category'] == 'resource' and r['sequence'] > barrier['sequence'] for r in rows)
print('PASS real sink JSONL and bounded resource-window persistence; synthetic resource clock, unchanged real event/ingestion clocks')
