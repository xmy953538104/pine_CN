#!/usr/bin/env python3
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('reuse', ROOT / 'tools/ci/runtime-input.py')
reuse = importlib.util.module_from_spec(spec); spec.loader.exec_module(reuse)
SOURCE, OBSERVER, REPO = 'a' * 40, 'b' * 40, 'owner/repo'


class ExistingApkTest(unittest.TestCase):
    def fixture(self):
        request = dict(apk_source_commit=SOURCE, apk_sha256='c' * 64, run_id=42, artifact_id=84)
        run = dict(id=42, head_sha=SOURCE, conclusion='success', status='completed', path='.github/workflows/diagnostics-performance.yml',
                   head_repository=dict(full_name=REPO), event='pull_request')
        artifact = dict(id=84, name='pine-diagnostics-O2-debug-candidate-' + SOURCE, expired=False,
                        workflow_run=dict(id=42, head_sha=SOURCE), digest='sha256:' + 'd' * 64)
        return request, run, artifact

    def test_independent_observer_keeps_original_apk_source_and_provenance(self):
        request, run, artifact = self.fixture()
        binding = reuse.bind(request, run, artifact, REPO, OBSERVER)
        self.assertNotEqual(binding['apk_source_commit'], binding['observer_source_commit'])
        self.assertEqual(binding['artifact_id'], 84)
        self.assertFalse(binding['apk_built']); self.assertFalse(binding['apk_resigned'])
        self.assertEqual(binding['runtime_execution_claim'], 'none_input_qualification_only')

    def test_wrong_workflow_repository_run_source_name_id_and_expiry_reject(self):
        request, run, artifact = self.fixture()
        for change in (dict(path='.github/workflows/unrelated.yml'), dict(head_sha=OBSERVER), dict(conclusion='failure'),
                       dict(id=43), dict(head_repository=dict(full_name='fork/repo'))):
            with self.assertRaises(ValueError): reuse.bind(request, dict(run, **change), artifact, REPO, OBSERVER)
        for change in (dict(id=85), dict(name='pine-diagnostics-O2-debug-candidate-' + OBSERVER), dict(expired=True),
                       dict(workflow_run=dict(id=43, head_sha=SOURCE))):
            with self.assertRaises(ValueError): reuse.bind(request, run, dict(artifact, **change), REPO, OBSERVER)

    def test_downloaded_apk_hash_dex_elf_binding_and_original_manifest_are_checked(self):
        request, run, artifact = self.fixture()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); apk = root / 'app-full-debug.apk'
            elf = b'\x7fELF\x02\x01' + b'\0' * 12 + b'\xb7\0' + b'controlled'
            with zipfile.ZipFile(apk, 'w') as archive:
                archive.writestr('lib/arm64-v8a/libskyline.so', elf)
                archive.writestr('classes.dex', b'controlled host fixture ' + SOURCE.encode())
            request['apk_sha256'] = hashlib.sha256(apk.read_bytes()).hexdigest()
            manifest = dict(commit=SOURCE, sha256=request['apk_sha256'], skyline_elf_sha256=hashlib.sha256(elf).hexdigest())
            original = json.dumps(manifest)
            (root / 'build-identity.json').write_text(original)
            binding = reuse.bind(request, run, artifact, REPO, OBSERVER)
            self.assertEqual(reuse.inspect_apk(root, binding)[1], request['apk_sha256'])
            self.assertEqual((root / 'build-identity.json').read_text(), original)
            with self.assertRaises(ValueError): reuse.inspect_apk(root, dict(binding, expected_apk_sha256='0' * 64))
            manifest['commit'] = OBSERVER
            (root / 'build-identity.json').write_text(json.dumps(manifest))
            with self.assertRaises(ValueError): reuse.inspect_apk(root, binding)

    def test_workflow_never_builds_or_loads_signing_secrets_and_runtime_is_explicit(self):
        workflow = (ROOT / '.github/workflows/diagnostics-runtime-input.yml').read_text()
        self.assertNotIn('gradlew', workflow); self.assertNotIn('secrets.', workflow)
        self.assertNotIn('submodule update', workflow)
        self.assertIn("github.event_name == 'workflow_dispatch' && inputs.run_runtime", workflow)
        self.assertIn('PINE_CI_REQUIRE_ACCELERATION', workflow)


if __name__ == '__main__': unittest.main()
