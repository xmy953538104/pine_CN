#!/usr/bin/env python3
"""Actual host JVM/JNI bounded transfer + default-OFF production bridge gate."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[2]
    baseline = json.loads((root / "tests/selected/fixtures/jni-default-path.expected.json").read_text())
    source = (root / "app/src/main/cpp/emu_jni.cpp").read_text()
    start = source.index("    skyline::signal::ScopedStackBlocker stackBlocker;", source.index("static void ExecuteApplication("))
    end = source.index('extern "C" JNIEXPORT void Java_emu_skyline_EmulationActivity_executeApplication(', start)
    body = source[start:end].replace(
        "        if (selected)\n            skyline::ExecuteSelectedFromJava(env, *os, romFd, selectionFields, programIndices);\n        else\n            ",
        "        ")
    assert hashlib.sha256(body.encode()).hexdigest() == baseline["legacy_jni_body_sha256"], "ordinary JNI lifetime/body changed"
    print("PASS ordinary JNI lifecycle/signal/cleanup body matches reviewed a901 baseline after the explicit dispatch branch", flush=True)
    javac = shutil.which("javac")
    if not javac:
        raise SystemExit("Java17+ JDK required; JNI transfer tests may not be skipped")
    java_home = Path(os.environ.get("JAVA_HOME", Path(javac).resolve().parents[1]))
    platform = "darwin" if os.uname().sysname == "Darwin" else "linux"
    sources = ["tests/selected/jni_fixture.cpp", "app/src/main/cpp/skyline/catalog/wire.cpp",
               "app/src/main/cpp/skyline/loader/selected_launch.cpp", "app/src/main/cpp/selected_jni.cpp",
               "app/src/main/cpp/selected_scan_jni.cpp"]
    inputs = sources + ["tests/selected/SelectedJniFixture.java", "app/src/main/cpp/selected_jni.h",
                       "app/src/main/cpp/skyline/loader/selected_launch_jni.h",
                       "app/src/main/cpp/skyline/loader/selected_launch.h", "tools/tests/test-selected-jni.py",
                       "app/src/main/cpp/emu_jni.cpp", "tests/selected/fixtures/jni-default-path.expected.json"]
    with tempfile.TemporaryDirectory(prefix="pine-selected-jni-") as temporary:
        build = Path(temporary)
        library = build / "libselected-jni.so"
        commands = [[os.environ.get("CXX", "c++"), "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                     "-Wno-unused-parameter", "-shared", "-fPIC", "-Iapp/src/main/cpp/skyline",
                     "-I" + str(java_home / "include"), "-I" + str(java_home / "include" / platform),
                     *sources, "-o", str(library)],
                    [str(javac), "-d", str(build), "tests/selected/SelectedJniFixture.java"],
                    [str(java_home / "bin/java"), "-Xcheck:jni", "-cp", str(build),
                     "emu.skyline.loader.SelectedJniFixture", str(library)]]
        for command in commands:
            result = subprocess.run(command, cwd=root, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=90)
            print(result.stdout, end="", flush=True)
            if result.returncode:
                raise SystemExit(result.returncode)
            if "WARNING in native method" in result.stdout or "FATAL ERROR in native method" in result.stdout:
                raise SystemExit("JNI check detected invalid production transfer")
        evidence = os.environ.get("PINE_SELECTED_JNI_EVIDENCE")
        if evidence:
            path = Path(evidence)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(json.dumps({
                "source_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
                "source_status": subprocess.check_output(["git", "status", "--porcelain"], cwd=root, text=True),
                "commands": commands, "result": "passed", "android_execution": False, "guest_execution": False,
                "input_sha256": {name: hashlib.sha256((root / name).read_bytes()).hexdigest() for name in inputs},
            }, indent=2) + "\n")


if __name__ == "__main__":
    main()
