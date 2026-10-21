#!/usr/bin/env python3
"""Run the actual exporter/test on writer files without Android/JitPack dependencies.

This explicit runner is outside the argument-free test-*.py CI discovery glob.
The caller supplies Kotlin compiler and JVM dependency classpaths. This script
does not download dependencies. Only the Android selected-scan writer is omitted;
its portable record parser is extracted verbatim, with its real dependencies.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
MAIN = ROOT / 'app/src/main/java/emu/skyline'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler-classpath', required=True)
    parser.add_argument('--classpath', required=True)
    parser.add_argument('--capture', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    capture = args.capture.resolve()
    inputs = {name: sha(capture / name) for name in ('diagnostics-0.jsonl', 'resource-window.jsonl')}

    reports_path = MAIN / 'diagnostics/SelectedScanReports.kt'
    reports = reports_path.read_text()
    fragments = {
        'SelectedScanSession': reports.split('internal data class SelectedScanSession', 1)[1].split('\n', 1)[0],
        'SelectedScanRecord': reports[reports.index('internal object SelectedScanRecord'):reports.index('/** A single bounded worker')],
        'record_extensions': reports[reports.index('private fun JsonObject.recordText'):],
    }
    fragments['SelectedScanSession'] = 'internal data class SelectedScanSession' + fragments['SelectedScanSession']
    extracted = out / 'SelectedScanRecord.kt'
    extracted.write_text('''package emu.skyline.diagnostics
import kotlinx.serialization.json.*
import emu.skyline.loader.SelectedNativeScanReport
import emu.skyline.loader.SelectedRejection
import emu.skyline.loader.checkJsonEnvelope
import emu.skyline.loader.selectedScanPredicates
''' + '\n'.join(fragments.values()))
    production = [MAIN / name for name in (
        'diagnostics/DiagnosticExport.kt', 'diagnostics/HistoricalExitAliases.kt',
        'loader/SelectedContentSelection.kt', 'loader/SelectedContentCatalog.kt',
        'loader/SelectedObservedScan.kt', 'loader/SelectedScanPredicates.kt')]
    test = ROOT / 'app/src/test/java/emu/skyline/diagnostics/RuntimePipelineExportTest.kt'
    classes = out / 'classes'
    compile_command = ['java', '-cp', args.compiler_classpath,
        'org.jetbrains.kotlin.cli.jvm.K2JVMCompiler', '-no-stdlib', '-no-reflect',
        '-jvm-target', '17', '-classpath', args.classpath, '-d', str(classes),
        *map(str, production), str(extracted), str(test)]
    with (out / 'compile.log').open('w') as log:
        subprocess.run(compile_command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=120)
    environment = dict(os.environ, PINE_RUNTIME_PIPELINE_CAPTURE=str(capture),
        PINE_RUNTIME_PIPELINE_REPORT=str(out / 'export.json'))
    run_command = ['java', '-cp', str(classes) + os.pathsep + args.classpath,
        'org.junit.runner.JUnitCore', 'emu.skyline.diagnostics.RuntimePipelineExportTest']
    with (out / 'junit.log').open('w') as log:
        subprocess.run(run_command, env=environment, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=90)
    assert inputs == {name: sha(capture / name) for name in inputs}, 'Writer inputs changed'
    report = {
        'source_sha': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
        'source_dirty': bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT, text=True).strip()),
        'production_source_sha256': {str(path.relative_to(ROOT)): sha(path) for path in [*production, reports_path, test]},
        'extracted_body_sha256': {name: hashlib.sha256(body.encode()).hexdigest() for name, body in fragments.items()},
        'input_directory': str(capture), 'input_sha256': inputs,
        'zip_sha256': sha(out / 'runtime-export.zip'), 'compile_command': compile_command, 'run_command': run_command,
        'scope': 'production DiagnosticExport and original JUnit test on executed pipeline writer files; JVM only',
        'android_writer_excluded': 'SelectedScanReports Android worker; actual portable SelectedScanRecord parser retained',
        'dependency_stubs': False, 'android_app_run': False, 'game_observation': False,
    }
    (out / 'exporter-source.json').write_text(json.dumps(report, indent=2) + '\n')
    print((out / 'junit.log').read_text())


if __name__ == '__main__':
    main()
