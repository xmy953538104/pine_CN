#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Run the synthetic auxiliary experiment with a frozen production baseline.

This runner does not open an NSP, import credentials, compile an APK, or invoke
the production Loader. Its evidence is a Python experiment result only.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import time


BASELINE = "b25e9e411dde6921b4fb1fda3c3cb7842930c2f3"
INPUT_GLOBS = (
    "experiments/selected_compat/**/*.py",
    "tools/run-auxiliary-experiment.py",
    "tools/tests/test-auxiliary-experiment-runner.py",
    ".github/workflows/selected-auxiliary-experiment.yml",
    "docs/selected-auxiliary-classification.md",
    "docs/selected-compat-integration-conditions.md",
)
PRODUCTION_ROOTS = (
    "app", "gradle", ".gitmodules", "build.gradle", "settings.gradle",
    "gradle.properties", "gradlew", "gradlew.bat",
)


class Rejected(ValueError):
    """A fixed, non-sensitive qualification failure code."""


def git(root, *args):
    return subprocess.check_output(["git", "-C", str(root), *args], stderr=subprocess.DEVNULL)


def production_guard(root, baseline=BASELINE):
    """Check every baseline tracked blob, including ordinary and Selected code.

    Gitlinks are recorded but their dependency checkouts are not exercised by
    this stdlib experiment. SHA-1 below compares Git blob identities; SHA-256
    separately binds each actual input in the evidence.
    """
    entries = git(root, "ls-tree", "-r", "-z", baseline).split(b"\0")
    files, gitlinks = {}, {}
    for entry in entries:
        if not entry:
            continue
        metadata, raw_path = entry.split(b"\t", 1)
        mode, kind, oid = metadata.decode("ascii").split()
        name = os.fsdecode(raw_path)
        path = root / name
        if kind == "commit" and mode == "160000":
            gitlinks[name] = oid
            continue
        if kind != "blob" or mode not in ("100644", "100755", "120000"):
            raise Rejected("baseline_entry_unsupported")
        try:
            info = path.lstat()
        except OSError:
            raise Rejected("baseline_file_missing") from None
        if mode == "120000":
            if not stat.S_ISLNK(info.st_mode):
                raise Rejected("baseline_file_mode_changed")
            data = os.fsencode(os.readlink(path))
            sha = hashlib.sha256(data).hexdigest()
            actual = hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()
        else:
            if not stat.S_ISREG(info.st_mode) or bool(info.st_mode & 0o111) != (mode == "100755"):
                raise Rejected("baseline_file_mode_changed")
            blob = hashlib.sha1(b"blob " + str(info.st_size).encode() + b"\0")
            sha256 = hashlib.sha256()
            with path.open("rb") as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b""):
                    blob.update(block)
                    sha256.update(block)
            actual, sha = blob.hexdigest(), sha256.hexdigest()
        if actual != oid:
            raise Rejected("baseline_file_changed")
        files[name] = sha
    # Additions under production roots are outside this experimental scope.
    # Check both staged additions and untracked, non-ignored source files.
    additions = git(root, "diff", "--name-only", "--diff-filter=A", "-z", baseline, "--", *PRODUCTION_ROOTS)
    untracked = git(root, "ls-files", "--others", "--exclude-standard", "-z", "--", *PRODUCTION_ROOTS)
    if additions or untracked:
        raise Rejected("production_source_added")
    changed_links = git(root, "diff", "--ignore-submodules=dirty", "--name-only", "-z", baseline, "--", *gitlinks)
    if gitlinks and changed_links:
        raise Rejected("baseline_gitlink_changed")
    return {"files_sha256": files, "gitlinks_not_exercised": gitlinks}


def input_snapshot(root):
    files = sorted({path for pattern in INPUT_GLOBS for path in root.glob(pattern)})
    result = {}
    for path in files:
        if path.is_symlink() or not path.is_file():
            raise Rejected("experiment_input_not_regular")
        result[path.relative_to(root).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    return result


def evidence_directory(root, requested):
    target = requested.resolve()
    if target == root or root in target.parents:
        raise Rejected("evidence_inside_checkout")
    if target.exists() and (not target.is_dir() or any(target.iterdir())):
        raise Rejected("evidence_not_empty_directory")
    target.mkdir(parents=True, exist_ok=True)
    return target


def clean_source(root, commit):
    if git(root, "rev-parse", "HEAD").decode().strip() != commit:
        raise Rejected("tested_commit_changed")
    if git(root, "status", "--porcelain", "-z", "--untracked-files=all", "--ignore-submodules=dirty"):
        raise Rejected("source_not_clean")


WORKER = r'''
import json
import sys
import unittest

directory, output, summary = sys.argv[1:]
suite = unittest.TestLoader().discover(directory, pattern="test_auxiliary.py")
count = suite.countTestCases()
with open(output, "w", encoding="utf-8") as stream:
    result = unittest.TextTestRunner(stream=stream, verbosity=2).run(suite)
report = {
    "discovered": count, "run": result.testsRun,
    "failures": len(result.failures), "errors": len(result.errors),
    "skipped": len(result.skipped), "expected_failures": len(result.expectedFailures),
    "unexpected_successes": len(result.unexpectedSuccesses),
}
with open(summary, "w", encoding="utf-8") as stream:
    json.dump(report, stream, sort_keys=True)
    stream.write("\n")
'''


def run_tests(root, target):
    # -B alone still READS existing bytecode. A unique empty prefix prevents
    # timestamp/size-valid stale caches for both test modules and their helpers.
    # -I also excludes inherited PYTHONPATH and the user's site packages.
    with tempfile.TemporaryDirectory(prefix="fresh-bytecode-", dir=target) as cache:
        try:
            result = subprocess.run(
                [sys.executable, "-I", "-B", "-X", "pycache_prefix=" + cache,
                 "-c", WORKER, str(root / "experiments/selected_compat"),
                 str(target / "auxiliary-tests.txt"), str(target / "test-counts.json")],
                cwd=root, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                timeout=45, check=False,
            )
        except subprocess.TimeoutExpired:
            raise Rejected("auxiliary_worker_timed_out") from None
    if result.returncode != 0:
        raise Rejected("auxiliary_worker_failed")
    try:
        counts = json.loads((target / "test-counts.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        raise Rejected("auxiliary_worker_result_invalid") from None
    fields = {"discovered", "run", "failures", "errors", "skipped", "expected_failures", "unexpected_successes"}
    if (type(counts) is not dict or set(counts) != fields or
            any(type(value) is not int or not 0 <= value <= 100000 for value in counts.values())):
        raise Rejected("auxiliary_worker_result_invalid")
    if not counts["discovered"]:
        raise Rejected("auxiliary_tests_empty")
    return counts


def qualify(root, requested, baseline=BASELINE):
    root = root.resolve()
    if Path(os.fsdecode(git(root, "rev-parse", "--show-toplevel")).strip()).resolve() != root:
        raise Rejected("repository_root_required")
    target = evidence_directory(root, requested)
    report = {
        "schema": 1, "kind": "synthetic_auxiliary_structure_experiment",
        "commit": git(root, "rev-parse", "HEAD").decode().strip(),
        "production_baseline": baseline, "status": "rejected",
        "python": list(sys.version_info[:3]),
        "apk_built": False, "apk_executed": False, "commercial_input_used": False,
        "cnmt_policy_experiment": "not_run_platform_security_screening",
        "production_integration": "not_implemented", "runtime_admissible": False,
    }
    start = time.monotonic()
    try:
        before = production_guard(root, baseline)
        inputs = input_snapshot(root)
        if not inputs or not (root / "experiments/selected_compat/test_auxiliary.py").is_file():
            raise Rejected("auxiliary_tests_missing")
        clean_source(root, report["commit"])
        report["input_sha256"] = inputs
        report["tests"] = run_tests(root, target)
        report["test_execution"] = "isolated_python_with_fresh_bytecode_prefix"
        if input_snapshot(root) != inputs:
            raise Rejected("experiment_input_changed_during_tests")
        after = production_guard(root, baseline)
        if before != after:
            raise Rejected("baseline_changed_during_tests")
        clean_source(root, report["commit"])
        report["tested_source_state"] = "clean_before_and_after"
        report["baseline_guard"] = {"before_and_after": "identical", **after}
        counts = report["tests"]
        passed = counts["run"] == counts["discovered"] and not any(
            counts[name] for name in ("failures", "errors", "skipped", "expected_failures", "unexpected_successes"))
        report["status"] = "passed" if passed else "failed"
    except Rejected as error:
        report["error_code"] = str(error)
        raise
    finally:
        report["duration_seconds"] = round(time.monotonic() - start, 6)
        report["artifact_sha256"] = {
            path.name: hashlib.sha256(path.read_bytes()).hexdigest()
            for path in target.iterdir() if path.is_file() and path.name != "manifest.json"
        }
        (target / "manifest.json").write_text(json.dumps(report, sort_keys=True, indent=2) + "\n", encoding="utf-8")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence-dir", required=True, type=Path)
    args = parser.parse_args()
    try:
        report = qualify(Path(__file__).resolve().parents[1], args.evidence_dir)
    except Rejected as error:
        print("Experiment qualification rejected: " + str(error), file=sys.stderr)
        return 2
    print(json.dumps({"status": report["status"], "commit": report["commit"], "tests": report["tests"],
                      "baseline_files": len(report["baseline_guard"]["files_sha256"]), "runtime_admissible": False}))
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    sys.exit(main())
