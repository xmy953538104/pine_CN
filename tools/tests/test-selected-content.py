#!/usr/bin/env python3
"""Selected-content host structural tests; no commercial inputs or device launch.

Default: controlled cipher and rejecting LZ4 stand-ins, real selected factory,
NCA/CTR/CNMT/NPDM/NACP/RomFS/PatchManager/diagnostics sources. --real-aes:
actual Pine AES wrapper + exact pinned mbedTLS, independent OpenSSL encryption,
and pinned production LZ4. Only OS::ExecuteLoadedContent (platform/process tail)
and NcaLoader::LoadExeFs (guest mapping/execution) are host stubs.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading


def digest(data):
    return hashlib.sha256(data).hexdigest()


def default_path_checks(root):
    expected = json.loads((root / "tests/selected/fixtures/default-path.expected.json").read_text())
    source = (root / "app/src/main/cpp/skyline/os.cpp").read_text()
    start = "    void OS::Execute("
    tail = "        state.gpu->Initialise();"
    end = "    std::shared_ptr<loader::Loader> OS::GetLoader"
    prefix = source[source.index(start):source.index("    void OS::ExecuteLoadedContent")]
    suffix = "        ExecuteLoadedContent();\n    }\n\n"
    assert prefix.endswith(suffix), "ordinary Execute no longer calls only its shared tail"
    assert digest(prefix[:-len(suffix)].encode()) == expected["execute_prefix_sha256"]
    restored_tail = source[source.index(tail):source.index(end)].replace(
        '{"source", metadataSource}', '{"source", "base_control"}')
    assert digest(restored_tail.encode()) == expected["execute_tail_sha256"]
    assert digest((root / "app/src/main/cpp/skyline/loader/nsp.cpp").read_bytes()) == expected["nsp_loader_sha256"]
    assert digest((root / "app/src/main/cpp/skyline/services/aocsrv/IAddOnContentManager.cpp").read_bytes()) == expected["aoc_manager_sha256"]
    assert digest((root / "app/src/main/cpp/skyline/services/fssrv/IFileSystemProxy.cpp").read_bytes()) == expected["fs_proxy_sha256"]
    cmake = (root / "app/CMakeLists.txt").read_text()
    assert 'option(PINE_ENABLE_SELECTED_CONTENT "Compile the explicit selected native content entry" OFF)' in cmake
    assert 'if(NOT PINE_ENABLE_NSP_CATALOG)' in cmake
    assert 'metadataSource = "base_control"' in (root / "app/src/main/cpp/skyline/os.h").read_text()
    print("PASS frozen ordinary Execute prefix/tail, NspLoader, AOC/fssrv implementations and default-OFF configuration", flush=True)
    return expected


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--real-aes", action="store_true")
    parser.add_argument("--mbedtls-root", type=Path)
    parser.add_argument("--lz4-root", type=Path)
    parser.add_argument("--schema-check", action="store_true")
    parser.add_argument("--evidence-dir", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    compiler = os.environ.get("CXX", "c++")
    c_compiler = os.environ.get("CC", "cc")

    def git(*arguments):
        return subprocess.check_output(["git", *arguments], cwd=root, text=True).strip()

    baseline = default_path_checks(root)
    flags = ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread",
             "-Wno-unused-parameter", "-Wno-sign-compare", "-Wno-reorder",
             "-DPINE_ENABLE_SELECTED_CONTENT=1", "-Itests/selected/stubs", "-Itests/catalog/native_stubs",
             "-isystem", "app/src/main/cpp/skyline"]
    sanitizer = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitize else []
    flags += sanitizer
    sources = ["catalog/wire.cpp", "catalog/catalog.cpp", "catalog/json.cpp", "catalog/native_adapter.cpp",
               "loader/selected_validation.cpp", "loader/selected_content.cpp", "loader/selected_execute.cpp",
               "vfs/nca.cpp", "vfs/partition_filesystem.cpp", "vfs/ctr_encrypted_backing.cpp", "vfs/bktr.cpp",
               "vfs/rom_filesystem.cpp", "vfs/cnmt.cpp", "vfs/nacp.cpp", "vfs/npdm.cpp", "vfs/patch_manager.cpp",
               "diagnostics/diagnostics.cpp"]
    cipher_root = lz4_root = None
    pins = {}
    if args.real_aes:
        def pinned(path, supplied):
            folder = (supplied or root / path).resolve()
            pin = git("ls-tree", "HEAD", str(path)).split()[2]
            actual = git("-C", str(folder), "rev-parse", "HEAD")
            dirty = git("-C", str(folder), "status", "--porcelain", "--untracked-files=no")
            if actual != pin or dirty:
                parser.error(f"--real-aes requires clean exact pinned {path} sources; dependency skips are forbidden")
            print(f"Pinned {path}: {pin}", flush=True)
            pins[str(path)] = {"commit": pin, "source_directory": str(folder), "tracked_status": dirty}
            return folder
        cipher_root = pinned(Path("app/libraries/mbedtls"), args.mbedtls_root)
        lz4_root = pinned(Path("app/libraries/lz4"), args.lz4_root)
        if '#define MBEDTLS_VERSION_STRING         "2.16.11"' not in (cipher_root / "include/mbedtls/version.h").read_text():
            parser.error("--real-aes requires the pinned mbedTLS 2.16.11")
        flags = [flags[0], "-DPINE_CATALOG_TEST_REAL_AES", '-DMBEDTLS_CONFIG_FILE="native_aes_config.h"',
                 "-Itests/catalog/native_real_stubs", "-Itests/catalog", f"-I{cipher_root / 'include'}",
                 f"-I{lz4_root / 'lib'}", *flags[1:]]
        sources += ["crypto/aes_cipher.cpp"]

    # The input set is synthetic source/test/schema content only. No filesystem
    # scans for games, keys, saves, user containers or upload destinations occur.
    inputs = set()
    for pattern in ["app/src/main/cpp/skyline/catalog/*", "app/src/main/cpp/skyline/loader/selected*",
                    "app/src/main/cpp/skyline/vfs/*", "app/src/main/cpp/skyline/diagnostics/*",
                    "app/src/main/cpp/skyline/crypto/*", "app/src/main/cpp/skyline/common/**/*", "app/src/main/cpp/skyline/common.h",
                    "app/src/main/cpp/skyline/services/aocsrv/*", "tests/selected/**/*",
                    "tests/catalog/native_stubs/**/*", "tests/catalog/native_real_stubs/**/*"]:
        inputs.update(path for path in root.glob(pattern) if path.is_file() and "__pycache__" not in path.parts)
    inputs.update(root / path for path in ["tools/tests/test-selected-content.py", "tests/catalog/native_aes_config.h",
                                          "app/src/main/cpp/skyline/loader/loader.h", "app/src/main/cpp/skyline/loader/nca.h",
                                          "app/src/main/cpp/skyline/loader/executable.h", "app/src/main/cpp/skyline/os.cpp",
                                          "app/src/main/cpp/skyline/os.h", "app/CMakeLists.txt", "docs/nsp-catalog-v1.schema.json"])
    inputs.add(root / "app/src/main/cpp/skyline/services/fssrv/IFileSystemProxy.cpp")
    input_hashes = {str(path.relative_to(root)): digest(path.read_bytes()) for path in sorted(inputs)}
    manifest = {
        "manifest_version": 1, "source_commit": git("rev-parse", "HEAD"),
        "source_status": git("status", "--porcelain", "--untracked-files=all"),
        "synthetic_inputs_only": True, "cipher_lane": "production_aes_independent_openssl" if args.real_aes else "controlled_not_aes",
        "lz4_lane": "pinned_production" if args.real_aes else "rejecting_stand_in", "sanitizers": bool(args.sanitize),
        "signature_verification": "not_verified", "ivfc_tree_verification": "not_performed", "guest_execution": "not_performed",
        "host_stubs": ["Android/kernel platform types", "OS::ExecuteLoadedContent", "NcaLoader::LoadExeFs"],
        "dependency_pins": pins, "default_path_baseline": baseline, "input_sha256": input_hashes,
        "compiler_version": subprocess.check_output([compiler, "--version"], text=True).splitlines()[0],
        "commands": [], "status": "running",
    }
    with tempfile.TemporaryDirectory(prefix="pine-selected-content-") as folder:
        output = Path(folder)
        evidence = args.evidence_dir.resolve() if args.evidence_dir else output / "evidence"
        evidence.mkdir(parents=True, exist_ok=True)
        manifest_path = evidence / "manifest.json"
        command_lock = threading.Lock()

        def save_manifest(status):
            manifest["status"] = status
            manifest["artifact_sha256"] = {str(path.relative_to(evidence)): digest(path.read_bytes())
                                           for path in sorted(evidence.rglob("*")) if path.is_file() and path != manifest_path}
            manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")

        def run(command, **kwargs):
            with command_lock:
                manifest["commands"].append(command)
                # Preserve the exact arguments even when a later compiler fails.
                manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
            return subprocess.run(command, cwd=root, check=True, **kwargs)

        try:
            objects = []
            if args.real_aes:
                cflags = ["-O1", "-g", *sanitizer]
                for name in ["aes", "cipher", "cipher_wrap", "platform_util"]:
                    obj = output / f"mbedtls-{name}.o"
                    run([c_compiler, "-std=c99", *cflags, '-DMBEDTLS_CONFIG_FILE="native_aes_config.h"',
                         "-Itests/catalog", f"-I{cipher_root / 'include'}", "-c", str(cipher_root / "library" / f"{name}.c"), "-o", str(obj)])
                    objects.append(str(obj))
                obj = output / "lz4.o"
                run([c_compiler, *cflags, "-c", str(lz4_root / "lib/lz4.c"), "-o", str(obj)])
                objects.append(str(obj))

            def compile_source(index_source):
                index, source = index_source
                path = source if source.startswith("tests/") else "app/src/main/cpp/skyline/" + source
                obj = output / f"{index}.o"
                legacy = ["-Wno-missing-field-initializers"] if source.startswith("vfs/") else []
                if source == "vfs/nca.cpp":
                    legacy += ["-Wno-return-type"]
                if source == "vfs/npdm.cpp":
                    # Existing GCC packed-union warning; preserve real parser
                    # source and all static layout assertions. NDK Clang passes.
                    legacy += ["-Wno-error"]
                run([compiler, *flags, *legacy, "-c", path, "-o", str(obj)])
                return str(obj)

            with ThreadPoolExecutor(max_workers=4) as pool:
                objects += list(pool.map(compile_source, enumerate(sources + ["tests/selected/selected_content_test.cpp"])))
            exe = output / "selected-test"
            run([compiler, *flags, *objects, *(["-lcrypto"] if args.real_aes else []), "-o", str(exe)])
            env = os.environ.copy()
            if args.sanitize:
                env["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1"
                env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
            result = run([str(exe), str(evidence)], env=env, text=True, capture_output=True)
            print(result.stdout, end="")
            print(result.stderr, end="")
            (evidence / "scenarios.txt").write_text(result.stdout + result.stderr)
            catalog = json.loads((evidence / "selected-catalog.json").read_text())
            assert catalog["inspector"] == "pine_existing_nca" and catalog["bounded_scan_complete"] and catalog["all_metadata_enumerated"]
            assert len(catalog["groups"]) == 4 and catalog["verification"]["signature_verification"] == "not_verified"
            runtime = json.loads((evidence / "selected-runtime.json").read_text())
            assert runtime["primary_cnmt_role"] == "application" and runtime["update_cnmt_role"] == "patch" and runtime["aoc_count"] == 2
            assert runtime["bktr_supported"] is False and runtime["guest_execution"] == "not_performed"
            events = [json.loads(line) for path in sorted((evidence / "diagnostics").glob("diagnostics-*.jsonl"))
                      for line in path.read_text().splitlines()]
            patches = [event["fields"] for event in events if event.get("event") == "PATCH_SELECTED"]
            assert len(patches) == 2 and {event["source"] for event in patches} == {"update", "standalone_patch"}
            for event in patches:
                assert event["cnmt_type"] == "patch" and event["status"] == "selected_not_executed"
                assert event["parent_title_id"] == runtime["application_id"]
                assert len(event["program_id"]) == 16 and len(event["program_content_id"]) == 32
            manifest["patch_selection_contract_checked"] = True
            manifest["scenario_count"] = result.stdout.count("PASS ")
            manifest["schema_checked"] = bool(args.schema_check)
            if args.schema_check:
                import jsonschema
                schema = json.loads((root / "docs/nsp-catalog-v1.schema.json").read_text())
                jsonschema.Draft202012Validator(schema).validate(catalog)
            manifest["source_commit_after"] = git("rev-parse", "HEAD")
            manifest["source_status_after"] = git("status", "--porcelain", "--untracked-files=all")
            assert manifest["source_commit_after"] == manifest["source_commit"], "HEAD changed during this evidence run"
            assert manifest["source_status_after"] == manifest["source_status"], "worktree changed during this evidence run"
            assert all(digest((root / path).read_bytes()) == value for path, value in input_hashes.items()), "test input bytes changed during execution"
            save_manifest("passed")
            print("PASS selected actual native fields and Catalog JSON contract; guest/Android execution not performed")
        except BaseException as error:
            if isinstance(error, subprocess.CalledProcessError) and error.stdout is not None:
                print(error.stdout, end="")
                print(error.stderr or "", end="")
                (evidence / "failed-scenarios.txt").write_text(error.stdout + (error.stderr or ""))
            save_manifest("failed")
            raise


if __name__ == "__main__":
    main()
