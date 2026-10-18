#!/usr/bin/env python3
"""Generate a real synthetic NSP, run the native CLI, and retain exact evidence."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/tests'))
import nsp_catalog_fixture as fixture


def generate(output, binary):
    output.mkdir(parents=True, exist_ok=True)
    nsp = output / 'synthetic-multi-title.nsp'
    nsp.write_bytes(fixture.pfs(fixture.multi_title()))
    command = [str(binary.resolve()), '--profile', 'modern-40', '--verify-hashes', str(nsp.resolve())]
    run = subprocess.run(command, capture_output=True, text=True, check=True, timeout=30)
    catalog = json.loads(run.stdout)
    (output / 'catalog.json').write_text(json.dumps(catalog, indent=2, sort_keys=True) + '\n')
    for name in ('nsp-catalog-v1.schema.json', 'nsp-catalog-format-sources.md'):
        shutil.copyfile(ROOT / 'docs' / name, output / name)
    evidence_files = [path for path in output.iterdir() if path.is_file() and path.name != 'manifest.json']
    for lane in ('native-controlled', 'native-aes'):
        evidence_files.extend((output / lane).glob('native-*.json'))
    files = {path.relative_to(output).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
             for path in sorted(evidence_files) if path.is_file() and not path.is_symlink()}
    commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    tracked_diff = subprocess.check_output(['git', 'diff', '--binary', 'HEAD'], cwd=ROOT)
    untracked = subprocess.check_output(['git', 'ls-files', '--others', '--exclude-standard', '-z'], cwd=ROOT).decode().strip('\0').split('\0')
    untracked_source = [name for name in untracked if name and not (ROOT / name).resolve().is_relative_to(output.resolve())]
    manifest = {
        'source_commit': commit,
        'source_dirty': bool(tracked_diff or untracked_source),
        'tracked_source_diff_sha256': hashlib.sha256(tracked_diff).hexdigest() if tracked_diff else None,
        'untracked_source_files': untracked_source,
        'command': command,
        'synthetic_only': True,
        'groups': len(catalog['groups']), 'ncas': len(catalog['ncas']),
        'signature_verification': 'not_verified', 'runtime_integration': 'not_implemented',
        'sha256': files,
    }
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')
    return catalog


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--binary', type=Path)
    parser.add_argument('--write-example', type=Path, help='Also save this real CLI result as the documented example.')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='pine-catalog-example-build-') as temporary:
        binary = args.binary
        if binary is None:
            binary = Path(temporary) / 'nsp-catalog'
            subprocess.run(['bash', 'tools/build-nsp-catalog.sh', str(binary)], cwd=ROOT, check=True)
        catalog = generate(args.output_dir, binary)
        if args.write_example:
            args.write_example.write_text(json.dumps(catalog, indent=2, sort_keys=True) + '\n')
        print(f'Generated {len(catalog["groups"])} synthetic groups and {len(catalog["ncas"])} NCA members in {args.output_dir}')


if __name__ == '__main__':
    main()
