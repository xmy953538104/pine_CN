#!/usr/bin/env python3
"""Build actual descriptor/wire primitives with stdlib/C++20 only; synthetic inputs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--evidence-dir", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    sources = ["app/src/main/cpp/skyline/catalog/wire.cpp", "app/src/main/cpp/skyline/catalog/catalog.cpp",
               "app/src/main/cpp/skyline/catalog/json.cpp", "app/src/main/cpp/skyline/loader/selected_launch.cpp",
               "tests/selected/selected_launch_primitives_test.cpp"]
    inputs = sources + ["app/src/main/cpp/skyline/catalog/wire.h", "app/src/main/cpp/skyline/catalog/catalog.h",
                        "app/src/main/cpp/skyline/loader/selected_launch.h", "docs/nsp-catalog-v1.schema.json",
                        "tools/tests/test-selected-launch-primitives.py"]
    hashes = {path: digest(root / path) for path in inputs}
    git = lambda *arguments: subprocess.check_output(["git", *arguments], cwd=root, text=True).strip()
    with tempfile.TemporaryDirectory(prefix="pine-launch-primitives-") as folder:
        build = Path(folder)
        evidence = args.evidence_dir.resolve() if args.evidence_dir else build / "evidence"
        evidence.mkdir(parents=True, exist_ok=True)
        command = [os.environ.get("CXX", "c++"), "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                   "-Iapp/src/main/cpp/skyline", *sources, "-Wl,--wrap=pread", "-o", str(build / "primitives")]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie"]
        manifest = {"source_commit": git("rev-parse", "HEAD"), "source_status": git("status", "--porcelain"),
                    "synthetic_only": True, "sanitizers": bool(args.sanitize), "input_sha256": hashes,
                    "commands": [command], "status": "running", "game_execution": "not_performed"}
        path = evidence / "manifest.json"
        path.write_text(json.dumps(manifest, indent=2) + "\n")
        try:
            compilation = subprocess.run(command, cwd=root, capture_output=True, text=True)
            (evidence / "compile.txt").write_text(compilation.stdout + compilation.stderr)
            compilation.check_returncode()
            run_command = [str(build / "primitives"), str(evidence)]
            manifest["commands"].append(run_command)
            env = os.environ.copy()
            if args.sanitize:
                env["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1"
                env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
            run = subprocess.run(run_command, cwd=root, capture_output=True, text=True, timeout=20, env=env)
            (evidence / "scenarios.txt").write_text(run.stdout + run.stderr)
            print(run.stdout, end="")
            run.check_returncode()
            catalog = json.loads((evidence / "descriptor-catalog.json").read_text())
            assert catalog["container"]["identity_kind"] == "duplicated_fd_stat_snapshot"
            assert "no_named_path_check" in catalog["container"]["identity_scope"]
            schema = json.loads((root / "docs/nsp-catalog-v1.schema.json").read_text())
            assert catalog["container"]["identity_kind"] in schema["$defs"]["catalog"]["properties"]["container"]["properties"]["identity_kind"]["enum"]
            assert all(digest(root / name) == value for name, value in hashes.items()), "primitive inputs changed during test"
            manifest["scenario_count"] = run.stdout.count("PASS ")
            manifest["status"] = "passed"
        finally:
            manifest["source_commit_after"] = git("rev-parse", "HEAD")
            manifest["source_status_after"] = git("status", "--porcelain")
            manifest["artifact_sha256"] = {item.name: digest(item) for item in evidence.iterdir() if item.is_file() and item != path}
            path.write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
