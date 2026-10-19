#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Exercise qualification failures in disposable repositories, not app sources."""
import importlib.util
import json
import os
from pathlib import Path
import py_compile
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("auxiliary_experiment_runner", ROOT / "tools/run-auxiliary-experiment.py")
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)


class RunnerTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="pine-auxiliary-runner-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "repo"
        self.root.mkdir()
        self.evidence = Path(self.temporary.name) / "evidence"
        self.git("init", "-q")
        self.git("config", "user.email", "fixture@example.invalid")
        self.git("config", "user.name", "Synthetic qualification fixture")
        (self.root / "app").mkdir()
        (self.root / "app/ordinary.cpp").write_text("unchanged ordinary source\n")
        (self.root / "app/selected.cpp").write_text("unchanged Selected source\n")
        (self.root / "gradlew").write_text("synthetic launcher\n")
        (self.root / "gradlew").chmod(0o755)
        (self.root / "baseline-link").symlink_to("app/ordinary.cpp")
        (self.root / ".gitignore").write_text("__pycache__/\n")
        self.git("add", ".")
        self.git("commit", "-qm", "synthetic baseline")
        self.baseline = self.git("rev-parse", "HEAD").strip()
        self.tests = self.root / "experiments/selected_compat"
        self.tests.mkdir(parents=True)

    def git(self, *args):
        return subprocess.check_output(["git", "-C", str(self.root), *args], stderr=subprocess.DEVNULL).decode()

    def fixture(self, statement="self.assertTrue(True)"):
        (self.tests / "test_auxiliary.py").write_text(
            "import unittest\nfrom pathlib import Path\n"
            "class ControlledSyntheticRunnerFixture(unittest.TestCase):\n"
            "    def test_controlled(self):\n        " + statement + "\n"
        )

    def qualify(self):
        self.git("add", "experiments")
        if self.git("diff", "--cached", "--name-only").strip():
            self.git("commit", "-qm", "controlled experiment fixture")
        return RUNNER.qualify(self.root, self.evidence, self.baseline)

    def test_pass_binds_exact_baseline_inputs_and_artifacts_without_apk_claim(self):
        self.fixture()
        report = self.qualify()
        self.assertEqual(report["status"], "passed")
        self.assertEqual(report["tests"], dict(discovered=1, run=1, failures=0, errors=0, skipped=0,
                                             expected_failures=0, unexpected_successes=0))
        self.assertEqual(len(report["baseline_guard"]["files_sha256"]), 5)
        self.assertEqual(report["production_baseline"], self.baseline)
        self.assertEqual(report["commit"], self.git("rev-parse", "HEAD").strip())
        self.assertEqual(report["tested_source_state"], "clean_before_and_after")
        self.assertEqual(report["test_execution"], "isolated_python_with_fresh_bytecode_prefix")
        self.assertFalse(report["apk_executed"] or report["apk_built"] or report["runtime_admissible"])
        self.assertEqual(report["production_integration"], "not_implemented")
        self.assertIn("auxiliary-tests.txt", report["artifact_sha256"])
        self.assertIn("experiments/selected_compat/test_auxiliary.py", report["input_sha256"])
        self.assertEqual(json.loads((self.evidence / "manifest.json").read_text()), report)

    def test_ordinary_or_selected_baseline_mutation_cannot_qualify(self):
        for name in ("ordinary.cpp", "selected.cpp"):
            with self.subTest(name=name):
                path = self.root / "app" / name
                original = path.read_bytes()
                path.write_bytes(original + b"changed\n")
                with self.assertRaisesRegex(RUNNER.Rejected, "baseline_file_changed"):
                    RUNNER.production_guard(self.root, self.baseline)
                path.write_bytes(original)

    def test_deleted_or_changed_mode_baseline_is_rejected(self):
        path = self.root / "gradlew"
        path.chmod(0o644)
        with self.assertRaisesRegex(RUNNER.Rejected, "baseline_file_mode_changed"):
            RUNNER.production_guard(self.root, self.baseline)
        path.unlink()
        with self.assertRaisesRegex(RUNNER.Rejected, "baseline_file_missing"):
            RUNNER.production_guard(self.root, self.baseline)

    def test_baseline_symlink_change_is_rejected(self):
        path = self.root / "baseline-link"
        path.unlink()
        path.symlink_to("app/selected.cpp")
        with self.assertRaisesRegex(RUNNER.Rejected, "baseline_file_changed"):
            RUNNER.production_guard(self.root, self.baseline)

    def test_untracked_production_addition_is_rejected(self):
        (self.root / "app/new.cpp").write_text("outside experimental scope\n")
        with self.assertRaisesRegex(RUNNER.Rejected, "production_source_added"):
            RUNNER.production_guard(self.root, self.baseline)

    def test_staged_production_addition_is_rejected(self):
        (self.root / "app/new.cpp").write_text("outside experimental scope\n")
        self.git("add", "app/new.cpp")
        with self.assertRaisesRegex(RUNNER.Rejected, "production_source_added"):
            RUNNER.production_guard(self.root, self.baseline)

    def test_nonempty_evidence_directory_is_preserved(self):
        self.evidence.mkdir()
        saved = self.evidence / "existing.txt"
        saved.write_text("do not overwrite\n")
        with self.assertRaisesRegex(RUNNER.Rejected, "evidence_not_empty_directory"):
            self.qualify()
        self.assertEqual(saved.read_text(), "do not overwrite\n")

    def test_checkout_or_symlink_into_checkout_cannot_receive_evidence(self):
        for path in (self.root / "evidence", Path(self.temporary.name) / "link"):
            if path.name == "link":
                path.symlink_to(self.root, target_is_directory=True)
            with self.assertRaisesRegex(RUNNER.Rejected, "evidence_inside_checkout"):
                RUNNER.evidence_directory(self.root, path)

    def test_missing_tests_are_rejected_without_a_green_manifest(self):
        with self.assertRaisesRegex(RUNNER.Rejected, "auxiliary_tests_missing"):
            self.qualify()
        report = json.loads((self.evidence / "manifest.json").read_text())
        self.assertEqual(report["status"], "rejected")

    def test_empty_suite_is_rejected(self):
        (self.tests / "test_auxiliary.py").write_text("# No tests\n")
        with self.assertRaisesRegex(RUNNER.Rejected, "auxiliary_tests_empty"):
            self.qualify()

    def test_test_failure_or_skip_cannot_pass(self):
        self.fixture("self.fail('controlled synthetic failure')")
        self.assertEqual(self.qualify()["status"], "failed")
        self.evidence = Path(self.temporary.name) / "skip-evidence"
        self.fixture("self.skipTest('controlled synthetic skip')")
        result = self.qualify()
        self.assertEqual(result["status"], "failed")
        self.assertEqual(result["tests"]["skipped"], 1)

    def test_experiment_source_mutation_during_tests_is_rejected(self):
        self.fixture("Path(__file__).write_text('# input changed during synthetic test\\n')")
        with self.assertRaisesRegex(RUNNER.Rejected, "experiment_input_differs_from_head"):
            self.qualify()

    def test_production_mutation_during_tests_is_rejected(self):
        self.fixture("(Path(__file__).parents[2] / 'app/ordinary.cpp').write_text('changed\\n')")
        with self.assertRaisesRegex(RUNNER.Rejected, "baseline_file_changed"):
            self.qualify()

    def test_experiment_source_symlink_is_rejected(self):
        self.fixture()
        (self.tests / "opaque.py").symlink_to(self.root / "app/ordinary.cpp")
        with self.assertRaisesRegex(RUNNER.Rejected, "experiment_input_not_regular"):
            RUNNER.input_snapshot(self.root)

    def test_uncommitted_experiment_cannot_claim_the_head_commit(self):
        self.fixture()
        with self.assertRaisesRegex(RUNNER.Rejected, "source_not_clean"):
            RUNNER.qualify(self.root, self.evidence, self.baseline)

    def test_head_change_is_rejected_even_when_file_bytes_match(self):
        self.git("commit", "--allow-empty", "-qm", "different commit same bytes")
        with self.assertRaisesRegex(RUNNER.Rejected, "tested_commit_changed"):
            RUNNER.clean_source(self.root, self.baseline)

    def test_expected_failure_cannot_qualify(self):
        (self.tests / "test_auxiliary.py").write_text(
            "import unittest\nclass Controlled(unittest.TestCase):\n"
            "    @unittest.expectedFailure\n    def test_controlled(self):\n"
            "        self.fail('synthetic expected failure')\n")
        report = self.qualify()
        self.assertEqual(report["status"], "failed")
        self.assertEqual(report["tests"]["expected_failures"], 1)

    def test_unexpected_success_is_recorded_and_cannot_qualify(self):
        (self.tests / "test_auxiliary.py").write_text(
            "import unittest\nclass Controlled(unittest.TestCase):\n"
            "    @unittest.expectedFailure\n    def test_controlled(self):\n"
            "        self.assertTrue(True)\n")
        report = self.qualify()
        self.assertEqual(report["status"], "failed")
        self.assertEqual(report["tests"]["unexpected_successes"], 1)

    def test_same_size_same_mtime_stale_test_cache_cannot_pass(self):
        self.fixture("self.assertEqual(1, 1)")
        source = self.tests / "test_auxiliary.py"
        info = source.stat()
        py_compile.compile(str(source), doraise=True)
        original = source.read_bytes()
        changed = original.replace(b"assertEqual(1, 1)", b"assertEqual(1, 2)")
        self.assertNotEqual(original, changed)
        self.assertEqual(len(original), len(changed))
        source.write_bytes(changed)
        os.utime(source, ns=(info.st_atime_ns, info.st_mtime_ns))
        report = self.qualify()
        self.assertEqual(report["status"], "failed")
        self.assertEqual(report["tests"]["failures"], 1)

    def test_same_size_same_mtime_stale_helper_cache_cannot_pass(self):
        self.fixture("import auxiliary_fixture_helper; self.assertEqual(auxiliary_fixture_helper.VALUE, 1)")
        source = self.tests / "auxiliary_fixture_helper.py"
        source.write_text("VALUE = 1\n")
        info = source.stat()
        py_compile.compile(str(source), doraise=True)
        source.write_text("VALUE = 2\n")
        os.utime(source, ns=(info.st_atime_ns, info.st_mtime_ns))
        report = self.qualify()
        self.assertEqual(report["status"], "failed")
        self.assertEqual(report["tests"]["failures"], 1)

    def test_linked_helper_directory_cannot_escape_recorded_commit(self):
        outside = Path(self.temporary.name) / "outside_helper"
        outside.mkdir()
        (outside / "value.py").write_text("VALUE = 1\n")
        (self.tests / "linked_helper").symlink_to(outside, target_is_directory=True)
        self.fixture("from linked_helper import value; self.assertEqual(value.VALUE, 1)")
        with self.assertRaisesRegex(RUNNER.Rejected, "experiment_directory_symlink"):
            self.qualify()

    def test_ignored_untracked_helper_cannot_borrow_a_clean_status(self):
        (self.tests / "ignored_helper.py").write_text("VALUE = 1\n")
        (self.root / ".git/info/exclude").write_text("experiments/selected_compat/ignored_helper.py\n")
        self.fixture("import ignored_helper; self.assertEqual(ignored_helper.VALUE, 1)")
        with self.assertRaisesRegex(RUNNER.Rejected, "experiment_input_not_in_head"):
            self.qualify()
        self.assertEqual(self.git("status", "--porcelain"), "")

    def test_input_blob_must_match_head_even_if_git_status_hides_it(self):
        self.fixture("self.assertTrue(True)")
        self.git("add", "experiments")
        self.git("commit", "-qm", "controlled experiment fixture")
        self.git("update-index", "--assume-unchanged", "experiments/selected_compat/test_auxiliary.py")
        self.fixture("self.assertTrue(False)")
        self.assertEqual(self.git("status", "--porcelain"), "")
        with self.assertRaisesRegex(RUNNER.Rejected, "experiment_input_differs_from_head"):
            RUNNER.qualify(self.root, self.evidence, self.baseline)

    def test_experiment_tree_root_cannot_be_a_directory_link(self):
        self.tests.rmdir()
        outside = Path(self.temporary.name) / "outside_tree"
        outside.mkdir()
        self.tests.symlink_to(outside, target_is_directory=True)
        with self.assertRaisesRegex(RUNNER.Rejected, "experiment_directory_symlink"):
            RUNNER.input_snapshot(self.root)

    def test_experiment_parent_directory_link_is_rejected_before_walk(self):
        self.tests.rmdir()
        (self.root / "experiments").rmdir()
        outside = Path(self.temporary.name) / "outside_tree"
        (outside / "selected_compat").mkdir(parents=True)
        (self.root / "experiments").symlink_to(outside, target_is_directory=True)
        with self.assertRaisesRegex(RUNNER.Rejected, "experiment_directory_symlink"):
            RUNNER.input_snapshot(self.root)

    def test_ignored_python_source_in_cache_directory_still_binds_to_head(self):
        cache = self.tests / "__pycache__"
        cache.mkdir()
        (cache / "hidden_source.py").write_text("VALUE = 1\n")
        self.fixture("from __pycache__ import hidden_source; self.assertEqual(hidden_source.VALUE, 1)")
        with self.assertRaisesRegex(RUNNER.Rejected, "experiment_input_not_in_head"):
            self.qualify()


if __name__ == "__main__":
    unittest.main()
