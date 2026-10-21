#!/usr/bin/env python3
"""Build/run owned fixtures. QEMU execution is not an Android/game run."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def run(command, log, timeout=180):
    with log.open('wb') as stream:
        result = subprocess.run(command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f'command failed ({result.returncode}): {log}\n'+log.read_text(errors='replace')[-10000:])


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--host-only', action='store_true')
    p.add_argument('--require-aarch64', action='store_true')
    p.add_argument('--cross-cxx', default='aarch64-linux-gnu-g++')
    p.add_argument('--qemu', default='qemu-aarch64')
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    source = ROOT/'tests/abort_entry/prototype.cpp'
    flags = ['-std=c++20', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
             '-I'+str(ROOT/'tests/abort_entry'), '-I'+str(ROOT/'app/src/main/cpp/skyline/diagnostics')]
    host = a.output/'host-prototype'
    run([os.environ.get('CXX', 'c++'), *flags, str(source), '-o', str(host)], a.output/'host-build.log')
    run([str(host)], a.output/'host-run.log')
    report = {'schema': 1, 'source_sha': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
              'scope': 'Owned content-free fixtures only. No NSO/game/Android execution or production hook.',
              'host_controls': 'passed', 'aarch64_execution': 'not_run', 'sdk_authenticated': False,
              'production_connected': False}
    if not a.host_only:
        compiler, qemu = shutil.which(a.cross_cxx), shutil.which(a.qemu)
        if not compiler or not qemu:
            if a.require_aarch64:
                raise RuntimeError('required official cross compiler/QEMU unavailable; no AArch64 execution claimed')
        else:
            executable = a.output/'aarch64-prototype'
            run([compiler, *flags, '-static', str(source), str(ROOT/'tests/abort_entry/fixture.S'),
                 str(ROOT/'app/src/main/cpp/skyline/diagnostics/diagnostics.cpp'), '-o', str(executable)], a.output/'aarch64-build.log')
            run([qemu, '-cpu', 'max', str(executable), str(a.output/'aarch64')], a.output/'aarch64-run.log')
            log = (a.output/'aarch64-run.log').read_text()
            objects = [json.loads(line) for line in log.splitlines() if line.startswith('{')]
            assert objects[-1]['architecture'] == 'aarch64' and objects[-1]['production_connected'] is False
            folder = a.output/'aarch64/diagnostics'
            records = [json.loads(line) for path in folder.glob('diagnostics-*.jsonl') for line in path.read_text().splitlines()]
            observed = [row for row in records if row['event'] == 'SYNTHETIC_ABORT_ENTRY']
            assert len(observed) == 1 and observed[0]['fields']['entry_x0'] == '0xfedcba9876543210'
            assert observed[0]['fields']['observation_scope'] == 'executed_aarch64_fixture_not_game'
            assert observed[0]['session_id'] == 'synthetic-entry-session' and observed[0]['launch_id'] == 'synthetic-launch'
            window = [json.loads(line) for line in (folder/'resource-window.jsonl').read_text().splitlines()]
            assert any(row.get('event') == 'SYNTHETIC_ABORT_ENTRY' for row in window)
            report.update(aarch64_execution='passed_under_qemu_linux_user_not_android', native_result=objects[-1],
                          native_diagnostics_and_window='actual production writer files parsed',
                          raw_entry_event=observed[0], executable_sha256=hashlib.sha256(executable.read_bytes()).hexdigest())
            run([qemu, '--version'], a.output/'qemu-version.log')
            run([compiler, '--version'], a.output/'cross-compiler-version.log')
    report['source_sha256'] = {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                               for path in [source, ROOT/'tests/abort_entry/fixture.S', ROOT/'tests/abort_entry/tap_plan.h', ROOT/'app/src/main/cpp/skyline/diagnostics/diagnostics.cpp']}
    (a.output/'result.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps({key: report[key] for key in ('source_sha', 'host_controls', 'aarch64_execution', 'production_connected')}))


if __name__ == '__main__':
    main()
