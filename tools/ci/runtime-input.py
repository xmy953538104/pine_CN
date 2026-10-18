#!/usr/bin/env python3
"""Bind a preexisting signed APK to a separately versioned observer; never build/sign."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[2]
PRODUCERS = {'.github/workflows/diagnostics-performance.yml', '.github/workflows/diagnostics.yml'}


def validate_request(value):
    if not re.fullmatch('[0-9a-f]{40}', str(value.get('apk_source_commit', ''))): raise ValueError('Expected full APK source SHA')
    if not re.fullmatch('[0-9a-f]{64}', str(value.get('apk_sha256', ''))): raise ValueError('Expected exact APK SHA256')
    for key in ('run_id', 'artifact_id'):
        if not re.fullmatch('[1-9][0-9]*', str(value.get(key, ''))): raise ValueError('Invalid ' + key)


def bind(value, run, artifact, repository, observer):
    validate_request(value)
    source = value['apk_source_commit']
    if run.get('id') != int(value['run_id']) or run.get('head_sha') != source or run.get('conclusion') != 'success' or run.get('status') != 'completed': raise ValueError('Producer run/source is not successful and exact')
    if run.get('path') not in PRODUCERS or run.get('head_repository', {}).get('full_name') != repository: raise ValueError('Untrusted producer workflow/repository')
    if run.get('event') not in ('push', 'pull_request', 'workflow_dispatch'): raise ValueError('Untrusted producer event')
    expected = {'pine-diagnostics-O2-debug-candidate-' + source, 'pine-diagnostics-debug-' + source}
    if artifact.get('id') != int(value['artifact_id']) or artifact.get('name') not in expected or artifact.get('expired') is not False: raise ValueError('Artifact ID/name/expiry mismatch')
    provenance = artifact.get('workflow_run', {})
    if provenance.get('id') != int(value['run_id']) or provenance.get('head_sha') != source: raise ValueError('Artifact is not from that exact source/run')
    if not re.fullmatch('[0-9a-f]{40}', observer): raise ValueError('Invalid observer source SHA')
    return dict(schema=1, apk_source_commit=source, observer_source_commit=observer,
                producer_run_id=int(value['run_id']), artifact_id=int(value['artifact_id']), artifact_name=artifact['name'],
                github_reported_artifact_zip_digest=artifact.get('digest'), expected_apk_sha256=value['apk_sha256'],
                apk_built=False, apk_resigned=False, runtime_execution_claim='none_input_qualification_only',
                scope='input qualification only; no Settings/log/export/game execution pass')


def inspect_apk(directory, binding):
    apk = directory / 'app-full-debug.apk'
    metadata = directory / 'build-identity.json'
    if not apk.is_file() or apk.is_symlink() or not metadata.is_file() or metadata.is_symlink(): raise ValueError('Expected regular APK/identity files')
    manifest = json.loads(metadata.read_text())
    with apk.open('rb') as stream: digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    if digest != binding['expected_apk_sha256'] or manifest.get('sha256') != digest or manifest.get('commit') != binding['apk_source_commit']: raise ValueError('Downloaded APK/source/manifest mismatch')
    with zipfile.ZipFile(apk) as archive:
        libs = [name for name in archive.namelist() if name.startswith('lib/') and name.endswith('.so')]
        if 'lib/arm64-v8a/libskyline.so' not in libs or any(name.split('/')[1] != 'arm64-v8a' for name in libs): raise ValueError('APK is not ARM64-only')
        for name in libs:
            header = archive.read(name)[:20]
            if header[:6] != b'\x7fELF\x02\x01' or header[18:20] != b'\xb7\x00': raise ValueError('Wrong native ELF architecture')
        elf = hashlib.sha256(archive.read('lib/arm64-v8a/libskyline.so')).hexdigest()
        if manifest.get('skyline_elf_sha256') != elf: raise ValueError('Packaged ELF identity mismatch')
        dex = [name for name in archive.namelist() if re.fullmatch(r'classes[0-9]*\.dex', name)]
        if not any(binding['apk_source_commit'].encode() in archive.read(name) for name in dex): raise ValueError('APK DEX source identity absent')
    return apk, digest, elf


def api(path):
    request = urllib.request.Request('https://api.github.com/repos/' + os.environ['GITHUB_REPOSITORY'] + '/' + path,
        headers={'Authorization': 'Bearer ' + os.environ['GH_TOKEN'], 'Accept': 'application/vnd.github+json'})
    with urllib.request.urlopen(request, timeout=30) as response: return json.load(response)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    metadata = sub.add_parser('metadata')
    metadata.add_argument('--request', type=Path)
    metadata.add_argument('--output', type=Path, required=True)
    metadata.add_argument('--github-output', type=Path, required=True)
    verify = sub.add_parser('verify')
    verify.add_argument('--directory', type=Path, required=True); verify.add_argument('--binding', type=Path, required=True)
    verify.add_argument('--apksigner', type=Path, required=True); verify.add_argument('--aapt', type=Path, required=True)
    args = parser.parse_args()
    if args.command == 'metadata':
        value = json.loads(args.request.read_text()) if args.request else {
            key: os.environ[env] for key, env in (('apk_source_commit', 'APK_SOURCE_COMMIT'), ('apk_sha256', 'APK_SHA256'), ('run_id', 'APK_RUN_ID'), ('artifact_id', 'APK_ARTIFACT_ID'))}
        validate_request(value)
        run = api(f'actions/runs/{value["run_id"]}')
        artifacts = api(f'actions/runs/{value["run_id"]}/artifacts?per_page=100')['artifacts']
        artifact = next((item for item in artifacts if item['id'] == int(value['artifact_id'])), {})
        observer = subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip()
        result = bind(value, run, artifact, os.environ['GITHUB_REPOSITORY'], observer)
        args.output.parent.mkdir(parents=True, exist_ok=True); args.output.write_text(json.dumps(result, indent=2) + '\n')
        with args.github_output.open('a') as stream:
            print('artifact_name=' + result['artifact_name'], file=stream)
            print('artifact_id=' + str(result['artifact_id']), file=stream)
            print('run_id=' + str(result['producer_run_id']), file=stream)
            print('apk_source_commit=' + result['apk_source_commit'], file=stream)
    else:
        result = json.loads(args.binding.read_text()); apk, digest, elf = inspect_apk(args.directory, result)
        spec = importlib.util.spec_from_file_location('signer', ROOT / 'tools/verify-apk-signer.py')
        signer = importlib.util.module_from_spec(spec); spec.loader.exec_module(signer)
        pin = json.loads((ROOT / 'tools/diagnostics-ci-signer.json').read_text())
        if pin.get('schema') != 1: raise ValueError('Unknown signer pin schema')
        cert = signer.verify_certificate(apk, args.apksigner, pin['certificate_sha256'])
        badging = subprocess.check_output([str(args.aapt), 'dump', 'badging', str(apk)], text=True, timeout=30)
        if "name='io.github.pine.emu.diagnostics.preview'" not in badging or "sdkVersion:'29'" not in badging or 'application-debuggable' not in badging: raise ValueError('APK package/SDK/Debug mismatch')
        result.update(apk_sha256=digest, skyline_elf_sha256=elf, certificate_sha256=cert, signature_verified=True,
                      apk_input_verified=True, original_build_manifest_preserved=True)
        original = args.directory / 'build-identity.json'
        result['original_build_manifest_sha256'] = hashlib.sha256(original.read_bytes()).hexdigest()
        result['producer_reported_native_symbols'] = json.loads(original.read_text()).get('native_symbols')
        args.binding.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__': main()
