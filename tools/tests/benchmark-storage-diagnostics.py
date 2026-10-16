#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Optional production-storage host comparison; records observations without timing thresholds."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import statistics
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path,
                        help='fresh output directory; defaults to a retained temporary directory')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    if args.output is None:
        base = Path(tempfile.mkdtemp(prefix='pine-storage-benchmark-'))
    else:
        base = args.output.expanduser().resolve()
        try:
            base.mkdir(parents=True, exist_ok=False)
        except OSError as error:
            parser.error(f'--output must be a fresh writable directory: {error}')
    print(f'Benchmark evidence: {base}', flush=True)
    binary = base / 'storage-read-benchmark'
    compiler = shlex.split(os.environ.get('CXX', 'c++'))
    command = [*compiler, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-parameter', '-Wno-sign-compare', '-pthread',
               '-Itests/diagnostics/storage_stubs', '-Iapp/src/main/cpp/skyline',
               'app/src/main/cpp/skyline/services/fssrv/IStorage.cpp',
               'app/src/main/cpp/skyline/diagnostics/diagnostics.cpp',
               'tests/diagnostics/storage_benchmark.cpp', '-o', str(binary)]
    subprocess.run(command, cwd=repo, check=True)
    trials = []
    reads = 1000000
    for trial in range(5):
        # Rotate order; each mode gets a fresh process and sink with the same backing workload.
        modes = ['off', 'normal', 'detailed']
        modes = modes[trial % 3:] + modes[:trial % 3]
        for mode in modes:
            logs = base / f'round-{trial}-{mode}'
            run = [str(binary), mode, str(reads), str(logs), str(trial)]
            row = json.loads(subprocess.check_output(run, text=True, cwd=repo))
            assert row['reads'] == row['backing_calls'] == reads
            assert row['accepted'] + row['dropped'] == row['expected_attempts']
            assert row['accepted'] == row['written'] and row['disk_errors'] == 0
            assert row['checksum'] == 39500000
            row.update(trial=trial, ns_per_call=row['calls_ns'] / reads)
            retained = 0
            for path in sorted(logs.glob('*.jsonl')):
                for line in path.read_text(encoding='utf-8').splitlines():
                    record = json.loads(line)
                    if record['event'] == 'DLC_DATA_READ':
                        fields = record['fields']
                        assert int(fields['sampling_period']) == row['sampling_period']
                        assert (int(fields['read_ordinal']) - 1) % row['sampling_period'] == 0
                        assert fields['actual_bytes'] == fields['buffer_size'] == '64'
                        retained += 1
            assert not row['accepted'] or retained
            row['retained_sample_records_verified'] = retained
            trials.append(row)
    summary = {}
    for mode in ['off', 'normal', 'detailed']:
        cases = [row for row in trials if row['mode'] == mode]
        times = [row['ns_per_call'] for row in cases]
        summary[mode] = {
            'median_ns_per_call': statistics.median(times),
            'min_ns_per_call': min(times), 'max_ns_per_call': max(times),
            'accepted_total': sum(row['accepted'] for row in cases),
            'written_total': sum(row['written'] for row in cases),
            'dropped_total': sum(row['dropped'] for row in cases),
            'expected_attempts_per_trial': cases[0]['expected_attempts'],
            'sampling_period': cases[0]['sampling_period'],
        }
    source_paths = [
        'app/src/main/cpp/skyline/services/fssrv/IStorage.cpp',
        'app/src/main/cpp/skyline/vfs/backing.h',
        'app/src/main/cpp/skyline/diagnostics/diagnostics.cpp',
        'tests/diagnostics/storage_benchmark.cpp',
        'tools/tests/benchmark-storage-diagnostics.py',
    ]
    result = {
        'scope': 'Host comparison of production IStorage.cpp, Backing and diagnostics.cpp; existing IPC/service/platform stand-ins. Does not measure Android game FPS, audio, device storage or ARM64 performance.',
        'commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True, cwd=repo).strip(),
        'host': platform.platform(),
        'compiler': subprocess.check_output([*compiler, '--version'], text=True).splitlines()[0],
        'workload': {
            'reads_per_trial': reads, 'bytes_per_read': 64,
            'backing': 'same deterministic in-memory copy', 'trials_per_mode': 5,
            'affinity': sorted(os.sched_getaffinity(0)) if hasattr(os, 'sched_getaffinity') else None,
            'nice': os.getpriority(os.PRIO_PROCESS, 0) if hasattr(os, 'getpriority') else None,
            'sink': 'real bounded async sink; default 1024 queue / 2MiB * 4 files; complete Stop drain outside timed calls',
            'limits': 'Tight loop can saturate the writer. Shared-host scheduling and contention affect timings and drops; no performance pass/fail threshold.',
        },
        'build_command': command, 'summary': summary, 'trials': trials,
        'source_sha256': {p: hashlib.sha256((repo / p).read_bytes()).hexdigest() for p in source_paths},
    }
    (base / 'result.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
