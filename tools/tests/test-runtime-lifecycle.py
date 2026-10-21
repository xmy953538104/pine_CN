#!/usr/bin/env python3
"""Real POSIX thread/longjmp fences from production bodies; controlled scheduler."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CPP = ROOT / 'app/src/main/cpp/skyline'


def extract(path, start, end):
    source = (CPP / path).read_text()
    return source[source.index(start):source.index(end, source.index(start))]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--aarch64', action='store_true')
    parser.add_argument('--compile-only', action='store_true')
    parser.add_argument('--fortify', action='store_true', help='Enable glibc FORTIFY_SOURCE=3 for the Linux execution boundary')
    parser.add_argument('--cxx')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--qemu-ld-prefix', default=os.environ.get('QEMU_LD_PREFIX', '/usr/aarch64-linux-gnu'))
    args = parser.parse_args()
    out = (args.output or Path(tempfile.mkdtemp(prefix='pine-runtime-lifecycle-'))).resolve()
    out.mkdir(parents=True, exist_ok=True)
    thread = Path('kernel/types/KThread.cpp')
    bodies = {
        'ACTUAL_THREAD_DESTRUCTOR': extract(thread, '    KThread::~KThread()', '    void KThread::ThreadEntrypoint()'),
        'ACTUAL_THREAD_ENTRY': extract(thread, '    void KThread::ThreadEntrypoint()', '    void KThread::Start('),
        'ACTUAL_THREAD_START_KILL': extract(thread, '    void KThread::Start(', '    void KThread::SendSignal('),
        'ACTUAL_THREAD_COMPLETION': extract(thread, '    void KThread::DiagnosticHostReturned()', '    void KNceThread::Init()'),
        'ACTUAL_NCE_INIT': extract(thread, '    void KNceThread::Init()', '    void KNceThread::Run()'),
        'ACTUAL_NCE_RUN': extract(thread, '    void KNceThread::Run()', '    void KJit32Thread::Init()'),
        'ACTUAL_PROCESS_DESTRUCTOR': extract(Path('kernel/types/KProcess.cpp'), '    KProcess::~KProcess()', '    void KProcess::Kill('),
        'ACTUAL_SVC_BREAK': extract(Path('kernel/svc.cpp'), '    void Break(', '    void OutputDebugString('),
        'ACTUAL_NCE_SIGNAL': extract(Path('nce.cpp'), '    void NCE::SignalHandler(', '    void NCE::HostSignalHandler('),
        'ACTUAL_TLS_RESTORER': extract(Path('nce.cpp'), '    void *NceTlsRestorer()', '    NCE::NCE('),
        'ACTUAL_GUEST_SIGNAL_WRAPPER': extract(Path('common/signal.cpp'), '    void __attribute__((no_stack_protector)) GuestSafeSignalHandler(', '    /**\n     * @brief Installs the guest safe signal handler'),
    }
    source = (ROOT / 'tests/runtime_observer/lifecycle.cpp.in').read_text()
    for name, body in bodies.items():
        source = source.replace('/* ' + name + ' */', body)
    generated = out / 'lifecycle.cpp'
    generated.write_text(source)
    cxx = args.cxx or ('aarch64-linux-gnu-g++' if args.aarch64 else os.environ.get('CXX', 'c++'))
    command = [cxx, '-std=c++20', '-O2', '-UNDEBUG', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
               '-Wno-unused-variable', '-Wno-clobbered', '-pthread', '-DPINE_DIAG_RUNTIME_TEST', '-DPINE_DIAG_TEST_HOOKS',
               '-I' + str(ROOT / 'tests/runtime_observer/stubs'), '-I' + str(ROOT / 'tests/runtime_observer'), '-I' + str(CPP),
               str(generated), str(CPP / 'diagnostics/runtime_observer.cpp'), str(CPP / 'diagnostics/nso_image.cpp'),
               str(CPP / 'diagnostics/diagnostics.cpp'), str(CPP / 'nce/observation.cpp')]
    if 'android' in cxx:
        # Existing Pine/Bionic sigevent uses the NDK's nested designator macro.
        command += ['-llog', '-Wno-unknown-warning-option', '-Wno-c99-designator', '-Wno-missing-field-initializers', '-Wl,-z,max-page-size=16384']
    source_sha = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    command.append('-DPINE_BOUNDARY_SOURCE_COMMIT="' + source_sha + '"')
    if args.fortify:
        if 'android' in cxx:
            parser.error('--fortify is a Linux/glibc boundary check, not an Android execution claim')
        command.append('-D_FORTIFY_SOURCE=3')
    if args.aarch64:
        command.append(str(ROOT / 'tests/runtime_observer/lifecycle_spin.S'))
    command += ['-o', str(out / 'lifecycle')]
    metadata = {'source_sha': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                'source_dirty': bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT, text=True).strip()),
                'source_body_sha256': {name: hashlib.sha256(body.encode()).hexdigest() for name, body in bodies.items()},
                'generated_source_sha256': hashlib.sha256(generated.read_bytes()).hexdigest(),
                'compile_command': command, 'android_execution': False, 'real_game_records': 0,
                'dependencies': 'controlled scheduler/JVM/logging; real OS pthreads/signals/timer/setjmp/longjmp/std::thread::join',
                'injected_states': 'legacy consumer handoff/capacity states are injected; actual-writing-interruption executes the exact producer without injected state',
                'fortify_source': 3 if args.fortify else 'toolchain_default',
                'execution': 'not started'}
    metadata['compiler_version'] = subprocess.check_output([cxx, '--version'], text=True).splitlines()[0]
    if args.aarch64 and not args.compile_only:
        metadata['qemu_version'] = subprocess.check_output(['qemu-aarch64', '--version'], text=True).splitlines()[0]
    (out / 'result.json').write_text(json.dumps(metadata, indent=2) + '\n')
    subprocess.run(command, check=True)
    run = (['qemu-aarch64', '-L', args.qemu_ld_prefix] if args.aarch64 else []) + [str(out / 'lifecycle'), 'logs']
    metadata['run_command'] = None if args.compile_only else run
    if not args.compile_only:
        metadata['execution'] = 'actual binary execution started'
        (out / 'result.json').write_text(json.dumps(metadata, indent=2) + '\n')
        try:
            subprocess.run(run, cwd=out, check=True, timeout=60)
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
            metadata['execution'] = 'actual execution failed'
            metadata['execution_error'] = str(error)
            (out / 'result.json').write_text(json.dumps(metadata, indent=2) + '\n')
            raise
        metadata['execution'] = 'binary returned zero; validating writer output'
        (out / 'result.json').write_text(json.dumps(metadata, indent=2) + '\n')
        cases = {}
        for case in ('stop-final-drain', 'launch-rollover'):
            rows = [json.loads(line) for line in (out / 'logs' / case / 'diagnostics-0.jsonl').read_text().splitlines()]
            captures = [row for row in rows if row['event'] == 'NVN_ABORT_CALLSITE_OBSERVED']
            assert len(captures) == 1 and captures[0]['launch_id'] == 'old-launch', case
            assert captures[0]['fields']['runtime_connected'] == 'false', case
            cases[case] = {'consumer_injected_ready_rows': 1, 'record_owner': 'old-launch'}
        rows = [json.loads(line) for line in (out / 'logs/lifetime-capacity/diagnostics-0.jsonl').read_text().splitlines()]
        retirement = [row for row in rows if row['event'] == 'MODULE_RUNTIME_RETIREMENT']
        assert len(retirement) == 1 and retirement[0]['fields']['host_joined_slots'] == '256'
        assert retirement[0]['fields']['interrupted_writing_slots'] == '1'
        assert retirement[0]['fields']['writing_payload_read'] == 'false'
        assert not any(row['event'] == 'NVN_ABORT_CALLSITE_OBSERVED' for row in rows)
        metadata['writer_assertions'] = cases | {'lifetime-capacity': retirement[0]['fields']}
        rows = [json.loads(line) for line in (out / 'logs/stop-drain-timeout/diagnostics-0.jsonl').read_text().splitlines()]
        assert not any(row['event'] == 'NVN_ABORT_CALLSITE_OBSERVED' for row in rows)
        assert any(row['fields'].get('runtime_final_drain_timeouts') == '1' for row in rows)
        metadata['writer_assertions']['stop-drain-timeout'] = {'old_ready_discarded': True, 'reported_timeouts': 1}
        if args.aarch64:
            proof = json.loads((out / 'logs/actual-writing-interruption/producer-proof.json').read_text())
            assert proof['state_injected'] is False and proof['exact_installed_producer'] is True
            assert proof['state_after_host_join'] == 1 and proof['payload_consumed'] is False
            assert proof['altstack_delivery'] is True and proof['asynchronous_nested_sigint'] is True
            metadata['actual_writing_interruption'] = proof
    metadata['execution'] = 'NDK compile only' if args.compile_only else 'QEMU Linux-user AArch64' if args.aarch64 else 'Linux POSIX host'
    metadata['binary_sha256'] = hashlib.sha256((out / 'lifecycle').read_bytes()).hexdigest()
    metadata['writer_jsonl_sha256'] = {str(path.relative_to(out)): hashlib.sha256(path.read_bytes()).hexdigest()
                                       for path in sorted((out / 'logs').rglob('*.jsonl'))}
    (out / 'result.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(json.dumps(metadata))


if __name__ == '__main__':
    main()
