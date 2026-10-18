#!/usr/bin/env python3
"""Positive real IPC/service tests with public synthetic bytes; no APK/device run.

Required: Clang host C++ compiler, OpenSSL development headers/library, and exact
pinned Boost (six nested modules), fmt, frozen, xxhash, mbedTLS and LZ4 sources.
No dependency/test skips. --sanitize instruments only positive scenarios.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import threading


def sha(data):
    return hashlib.sha256(data).hexdigest()


def parse_lsan_summary(stderr, expected):
    """Accept only the complete one-rule table for the fixed legacy allocation.

    A matching row elsewhere in stderr is not evidence of a suppression table.
    The count and byte size are both fixed by the guarded host fixture profile.
    """
    count, size, allocation = (expected[key] for key in
                               ["expected_allocations", "expected_bytes", "expected_allocation_bytes"])
    if any(type(value) is not int or value <= 0 for value in [count, size, allocation]) or count * allocation != size:
        raise ValueError("inconsistent fixed legacy allocation profile")
    lines = stderr.splitlines()
    headings = [index for index, line in enumerate(lines) if line.strip() == "Suppressions used:"]
    if len(headings) != 1:
        raise ValueError("expected exactly one LSan suppression table")
    start = headings[0] + 1
    if start >= len(lines) or not re.fullmatch(r"\s*count\s+bytes\s+template\s*", lines[start]):
        raise ValueError("invalid LSan suppression table header")
    rows, row_indices = [], []
    for index in range(start + 1, len(lines)):
        line = lines[index].strip()
        if re.fullmatch(r"-{3,}", line):
            break
        match = re.fullmatch(r"\s*(\d+)\s+(\d+)\s+(.+?)\s*", lines[index])
        if not match:
            raise ValueError("invalid LSan suppression table row")
        rows.append((int(match[1]), int(match[2]), match[3]))
        row_indices.append(index)
    else:
        raise ValueError("unterminated LSan suppression table")
    expected_row = (count, size, "^" + expected["suppressed_function"])
    if rows != [expected_row]:
        raise ValueError("unexpected LSan rule, allocation count or exact byte size")
    if any(index not in row_indices and re.fullmatch(r"\s*\d+\s+\d+\s+.+?\s*", line)
           for index, line in enumerate(lines)):
        raise ValueError("suppression-like row outside the sole LSan table")
    return {"allocations": count, "bytes": size, "allocation_bytes": allocation}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--deps-root", type=Path, help="alternate app/libraries directory at the same exact pins")
    parser.add_argument("--evidence-dir", type=Path, required=True)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--unsuppressed-leaks", action="store_true", help="retain the known legacy OsFileSystem LSan failure; not a passing lane")
    args = parser.parse_args()
    if args.unsuppressed_leaks and not args.sanitize:
        parser.error("--unsuppressed-leaks requires --sanitize")
    root = Path(__file__).resolve().parents[1]
    src = root / "app/src/main/cpp/skyline"
    compiler, cc = os.environ.get("CXX", "clang++"), os.environ.get("CC", "cc")
    linker = os.environ.get("CXXLD", compiler)
    evidence = args.evidence_dir.resolve()
    evidence.mkdir(parents=True, exist_ok=True)
    if any(evidence.iterdir()):
        parser.error("--evidence-dir must be empty, preserving earlier run evidence")

    def git(*command, cwd=root):
        return subprocess.check_output(["git", *command], cwd=cwd, text=True).strip()

    version = subprocess.check_output([compiler, "--version"], text=True).splitlines()[0]
    if "clang" not in version.lower():
        parser.error("actual legacy designated initializers require Clang; no source rewriting is performed")
    symbolizer = None
    if args.sanitize:
        supplied = os.environ.get("ASAN_SYMBOLIZER_PATH") or shutil.which("llvm-symbolizer")
        symbolizer = Path(supplied) if supplied else Path(shutil.which(compiler) or compiler).resolve().parent / "llvm-symbolizer"
        if not symbolizer.is_file():
            parser.error("--sanitize requires llvm-symbolizer for the exact qualified-method LSan suppression")
    libraries = (args.deps_root or root / "app/libraries").resolve()
    dependencies = {}

    def pinned(name, parent=root, relative=None):
        relative = relative or f"app/libraries/{name}"
        folder = libraries / name
        if parent != root:
            folder = parent / relative
        expected = git("ls-tree", "HEAD", relative, cwd=parent).split()[2]
        actual = git("rev-parse", "HEAD", cwd=folder)
        status = git("status", "--porcelain", "--untracked-files=no", cwd=folder)
        if expected != actual or status:
            parser.error(f"required dependency {relative} is missing, dirty or at the wrong commit; skips forbidden")
        dependencies[str(folder)] = {"pin": expected, "tracked_status": status, "pin_parent": str(parent), "pin_path": relative}
        return folder

    deps = {name: pinned(name) for name in ["boost", "fmt", "frozen", "xxhash", "mbedtls", "lz4"]}
    boost = [pinned(name, deps["boost"], f"libs/{name}") for name in
             ["assert", "config", "container", "intrusive", "move", "static_assert"]]
    if '#define MBEDTLS_VERSION_STRING         "2.16.11"' not in (deps["mbedtls"] / "include/mbedtls/version.h").read_text():
        parser.error("pinned production mbedTLS must be 2.16.11")
    inputs = set()
    for pattern in ["app/src/main/cpp/skyline/catalog/*", "app/src/main/cpp/skyline/vfs/*",
                    "app/src/main/cpp/skyline/crypto/*", "app/src/main/cpp/skyline/services/aocsrv/*",
                    "app/src/main/cpp/skyline/services/fssrv/*", "app/src/main/cpp/skyline/services/account/*",
                    "app/src/main/cpp/skyline/common/**/*", "app/src/main/cpp/skyline/diagnostics/*",
                    "tests/selected-services/**/*"]:
        inputs.update(p for p in root.glob(pattern) if p.is_file() and "__pycache__" not in p.parts)
    extra = ["kernel/ipc.h", "kernel/ipc.cpp", "kernel/types/KSession.h", "kernel/types/KEvent.h", "kernel/types/KObject.h",
             "services/base_service.h", "services/base_service.cpp", "services/serviceman.h", "services/serviceman.cpp",
             "common.h", "loader/loader.h", "loader/nca.h", "loader/executable.h", "loader/selected_content.h",
             "loader/selected_content.cpp", "loader/selected_validation.h", "loader/selected_validation.cpp"]
    inputs.update(src / name for name in extra)
    inputs.update(root / name for name in ["tests/selected/fixtures.h", "tests/catalog/native_aes_config.h",
                                          "tools/tests/test-selected-service-runner.py",
                                          "tools/run-selected-service-boundary.py", ".github/workflows/selected-service-boundary.yml",
                                          "docs/selected-service-boundary-v1.md"])
    input_hashes = {str(path.relative_to(root)): sha(path.read_bytes()) for path in sorted(inputs)}
    wire_profile = json.loads((root / "tests/selected-services/wire-profile.expected.json").read_text())
    manifest = {"manifest_version": 1, "source_commit": git("rev-parse", "HEAD"),
                "source_status": git("status", "--porcelain", "--untracked-files=all"),
                "input_sha256": input_hashes, "dependency_pins": dependencies, "compiler_version": version,
                "wire_profile": wire_profile,
                "linker_version": subprocess.check_output([linker, "--version"], text=True).splitlines()[0],
                "sanitizer_link_driver": linker if args.sanitize else "not_requested",
                "cipher_lane": "production_pinned_mbedtls_independent_openssl_fixture",
                "lz4_lane": "production_pinned", "sanitizers": args.sanitize,
                "leak_check": ("enabled_unsuppressed_known_failure_expected" if args.unsuppressed_leaks else
                               "enabled_with_one_source_guarded_legacy_method_suppression") if args.sanitize else "not_requested",
                "synthetic_inputs_only": True,
                "tested_boundary": "real IPC request decode / verbatim manager route / BaseService dispatch / service bodies / response encode",
                "full_service_manager": "not_tested_global_constructor_and_factory_excluded",
                "platform_substitutions": ["DeviceState/OS platform fields", "KProcess handles and NPDM field", "KThread TLS pointer",
                                           "KSyncObject event base", "kernel memory/scheduler type declarations", "logging/trace/stack unwind", "ARM/Android clock"],
                "guest_mapping_stub": "NcaLoader::LoadExeFs throws if invoked; not executed",
                "guest_execution": "not_performed", "apk_build": "not_performed", "bktr_support": "rejected_by_selected_factory",
                "signature_verification": "not_verified", "ivfc_tree_verification": "not_performed",
                "unexecuted_legacy_cases": ["AOC offset past count", "IStorage short reads and unequal wire/output sizes", "null storage", "malformed IPC"],
                "commands": [], "status": "running"}
    manifest_path = evidence / "manifest.json"
    lock = threading.Lock()

    def save(status):
        manifest["status"] = status
        manifest["artifact_sha256"] = {str(p.relative_to(evidence)): sha(p.read_bytes()) for p in sorted(evidence.rglob("*"))
                                       if p.is_file() and p != manifest_path}
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")

    def run(command, **kwargs):
        with lock:
            manifest["commands"].append([str(item) for item in command])
            manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
        return subprocess.run(command, cwd=root, check=True, **kwargs)

    with tempfile.TemporaryDirectory(prefix="pine-selected-services-") as tmp:
        output = Path(tmp)
        stage = output / "stage"
        stage.mkdir()
        copies = {}

        def copy(name):
            data = (src / name).read_bytes()
            target = stage / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            copies[name] = sha(data)

        try:
            parser_tests = run([sys.executable, str(root / "tools/tests/test-selected-service-runner.py")],
                               text=True, capture_output=True)
            (evidence / "runner-parser-tests.txt").write_text(parser_tests.stdout + parser_tests.stderr)
            print(parser_tests.stdout + parser_tests.stderr, end="", flush=True)
            manifest["runner_parser_regressions"] = "passed_stdlib_only_actual_parser"
            guard = json.loads((root / "tests/selected-services/legacy-opendir.expected.json").read_text())
            guarded_source = (root / guard["source"]).read_bytes()
            assert sha(guarded_source) == guard["source_sha256"], "legacy suppression invalid: source bytes changed"
            at, count = guard["method_byte_offset"], guard["method_byte_size"]
            assert sha(guarded_source[at:at + count]) == guard["method_sha256"], "legacy suppression invalid: method body changed"
            suppression = (root / "tests/selected-services/legacy-opendir.lsan").read_bytes()
            expected_rule = "leak:^" + guard["suppressed_function"]
            assert [line for line in suppression.decode().splitlines() if line and not line.startswith("#")] == [expected_rule]
            suppression_path = evidence / "legacy-opendir.lsan"
            suppression_path.write_bytes(suppression)
            manifest["legacy_lsan_suppression"] = {**guard, "suppression_sha256": sha(suppression), "symbolizer": str(symbolizer),
                                                 "scope": "only unchanged qualified OpenDirectoryImpl stack; not leak-clean"}
            for directory in ["catalog", "vfs", "crypto", "services/aocsrv", "services/fssrv", "services/account", "diagnostics"]:
                for path in sorted((src / directory).iterdir()):
                    if path.suffix in (".h", ".cpp"):
                        copy(str(path.relative_to(src)))
            for name in [p for p in extra if p not in ["common.h", "services/serviceman.cpp"]] + [
                    "common/base.h", "common/exception.h", "common/format.h", "common/span.h", "common/language.h",
                    "common/macros.h", "common/result.h", "diagnostics/content.h", "diagnostics/diagnostics.h", "diagnostics/diagnostics.cpp"]:
                copy(name)
            # The real span and ordinary utils remain intact. Only the platform
            # clock/sysproperty block is replaced, with exact provenance below.
            original_utils = (src / "common/utils.h").read_text()
            assert original_utils.count("#include <sys/system_properties.h>") == 1
            utils = original_utils.replace("#include <sys/system_properties.h>", "#include <chrono>")
            start = utils.index("    namespace detail {")
            end = utils.index("    /**\n     * @brief A way to implicitly", start)
            utils = utils[:start] + """    inline constexpr u64 ClockFrequency{1000000000};
    inline i64 GetTimeNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    inline u64 GetTimeTicks() { return static_cast<u64>(GetTimeNs()); }
""" + utils[end:]
            (stage / "common/utils.h").write_text(utils)
            (evidence / "platform-utils.h").write_text(utils)
            overlays = {}
            for path in sorted((root / "tests/selected-services/platform").rglob("*")):
                if not path.is_file():
                    continue
                relative = path.relative_to(root / "tests/selected-services/platform")
                assert str(relative) not in copies, "platform overlay must never replace a promised byte-identical source"
                dest = stage / relative
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(path.read_bytes())
                overlays[str(relative)] = sha(path.read_bytes())
            manager = (src / "services/serviceman.cpp").read_text()
            methods = {}
            markers = [("RegisterService", "    void ServiceManager::RegisterService(", "    void ServiceManager::CloseSession("),
                       ("CloseSession", "    void ServiceManager::CloseSession(", "    void ServiceManager::SyncRequestHandler("),
                       ("SyncRequestHandler", "    void ServiceManager::SyncRequestHandler(", None)]
            for name, a, b in markers:
                at = manager.index(a)
                until = manager.index(b, at) if b else manager.rfind("\n}")
                text = manager[at:until]
                assert sum(line.startswith("    void ServiceManager::") for line in text.splitlines()) == 1
                methods[name] = {"source_byte_offset": len(manager[:at].encode()), "source_byte_size": len(text.encode()), "sha256": sha(text.encode())}
                methods[name]["_text"] = text
            generated = """#include <services/serviceman.h>
#include <common/trace.h>
namespace skyline::service {
// Host boundary only: exclude unrelated global time/font/NV service initialization.
ServiceManager::ServiceManager(const DeviceState &state) : state(state) {}
""" + "".join(entry["_text"] for entry in methods.values()) + "\n}\n"
            (output / "manager_slices.cpp").write_text(generated)
            (evidence / "manager_slices.cpp").write_text(generated)
            fixture = (root / "tests/selected/fixtures.h").read_text()
            old = "std::make_shared<crypto::KeyStore>()"
            assert fixture.count(old) == 1
            fixture = '#include <string>\nnamespace fixture { inline std::string keyRoot; }\n' + fixture.replace(old, "std::make_shared<crypto::KeyStore>(keyRoot)")
            (output / "fixtures.h").write_text(fixture)
            (evidence / "synthetic-fixtures.h").write_text(fixture)
            staging = {"byte_identical_sha256": copies, "platform_overlay_sha256": overlays,
                       "manager_original_sha256": sha(manager.encode()),
                       "manager_verbatim_slices": {name: {k: v for k, v in entry.items() if k != "_text"} for name, entry in methods.items()},
                       "manager_generated_sha256": sha(generated.encode()),
                       "utils_original_sha256": sha(original_utils.encode()), "utils_platform_sha256": sha(utils.encode()),
                       "fixture_original_sha256": sha((root / "tests/selected/fixtures.h").read_bytes()),
                       "fixture_generated_sha256": sha(fixture.encode()),
                       "fixture_transform": "one KeyStore constructor plus declared explicit synthetic root; no cipher/format writer edits"}
            (evidence / "source-staging.json").write_text(json.dumps(staging, indent=2) + "\n")
            manifest["source_staging_sha256"] = sha((evidence / "source-staging.json").read_bytes())
            for name, expected in copies.items():
                assert sha((stage / name).read_bytes()) == expected
            sanitizer = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitize else []
            flags = ["-include", "cstdint", "-std=c++20", "-O1", "-g", "-ffunction-sections", "-fdata-sections",
                     "-fno-omit-frame-pointer", "-DFMT_HEADER_ONLY=1", "-DPINE_CATALOG_TEST_REAL_AES", "-DPINE_ENABLE_SELECTED_CONTENT=1",
                     '-DMBEDTLS_CONFIG_FILE="native_aes_config.h"', "-Wno-reorder-init-list", "-Wno-c99-designator",
                     "-Wno-c++20-extensions", "-Wno-unknown-warning-option", *sanitizer,
                     f"-I{stage}", f"-I{output}", f"-I{root / 'tests/catalog'}",
                     f"-I{deps['mbedtls'] / 'include'}", f"-I{deps['lz4'] / 'lib'}", f"-I{deps['frozen'] / 'include'}",
                     f"-I{deps['fmt'] / 'include'}", f"-I{deps['xxhash']}"]
            for folder in boost:
                flags += ["-isystem", str(folder / "include")]
            objects = []
            for name in ["aes", "cipher", "cipher_wrap", "platform_util"]:
                obj = output / ("mbedtls-" + name + ".o")
                run([cc, "-std=c99", "-O1", "-g", *sanitizer, '-DMBEDTLS_CONFIG_FILE="native_aes_config.h"',
                     f"-I{root / 'tests/catalog'}", f"-I{deps['mbedtls'] / 'include'}", "-c", str(deps["mbedtls"] / "library" / (name + ".c")), "-o", str(obj)])
                objects.append(str(obj))
            obj = output / "lz4.o"
            run([cc, "-O1", "-g", *sanitizer, "-c", str(deps["lz4"] / "lib/lz4.c"), "-o", str(obj)])
            objects.append(str(obj))
            sources = ["kernel/ipc.cpp", "services/base_service.cpp", "services/aocsrv/IAddOnContentManager.cpp",
                       "services/aocsrv/IPurchaseEventManager.cpp", "services/fssrv/IFileSystemProxy.cpp", "services/fssrv/IStorage.cpp",
                       "services/fssrv/IFileSystem.cpp", "services/fssrv/IFile.cpp", "services/fssrv/IDirectory.cpp",
                       "services/fssrv/ISaveDataInfoReader.cpp", "services/fssrv/IMultiCommitManager.cpp",
                       "catalog/wire.cpp", "catalog/catalog.cpp", "catalog/json.cpp", "catalog/native_adapter.cpp",
                       "loader/selected_content.cpp", "loader/selected_validation.cpp", "vfs/nca.cpp", "vfs/partition_filesystem.cpp",
                       "vfs/ctr_encrypted_backing.cpp", "vfs/bktr.cpp", "vfs/rom_filesystem.cpp", "vfs/cnmt.cpp", "vfs/nacp.cpp",
                       "vfs/npdm.cpp", "vfs/patch_manager.cpp", "vfs/os_filesystem.cpp", "vfs/os_backing.cpp",
                       "crypto/aes_cipher.cpp", "crypto/key_store.cpp", "diagnostics/diagnostics.cpp"]
            manifest["compiled_production_sources"] = sources
            paths = [stage / source for source in sources] + [output / "manager_slices.cpp", root / "tests/selected-services/service_boundary_test.cpp"]

            def compile_one(index_path):
                i, path = index_path
                obj = output / f"cxx-{i}.o"
                run([compiler, *flags, "-c", str(path), "-o", str(obj)])
                return str(obj)

            with ThreadPoolExecutor(max_workers=4) as pool:
                objects += list(pool.map(compile_one, enumerate(paths)))
            exe = output / "service-test"
            run([linker, *sanitizer, *objects, "-Wl,--gc-sections", "-pthread", "-lcrypto", "-o", str(exe)])
            manifest["binary_sha256"] = sha(exe.read_bytes())
            env = os.environ.copy()
            if args.sanitize:
                env["ASAN_OPTIONS"] = f"detect_leaks=1:halt_on_error=1:fast_unwind_on_malloc=0:symbolize=1:external_symbolizer_path={symbolizer}"
                env["ASAN_SYMBOLIZER_PATH"] = str(symbolizer)
                env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
                env["LSAN_OPTIONS"] = "print_suppressions=1" + ("" if args.unsuppressed_leaks else f":suppressions={suppression_path}")
                manifest["sanitizer_environment"] = {name: env[name] for name in ["ASAN_OPTIONS", "ASAN_SYMBOLIZER_PATH", "UBSAN_OPTIONS", "LSAN_OPTIONS"]}
            for mode in ["off", "on"]:
                dest = evidence / mode
                dest.mkdir()
                result = run([str(exe), str(output / ("roots-" + mode)), str(dest), mode], env=env, text=True, capture_output=True)
                (dest / "scenarios.txt").write_text(result.stdout + result.stderr)
                print(result.stdout, end="", flush=True)
                print(result.stderr, end="", flush=True)
                assert result.stdout.count("PASS ") == 30
                wire = json.loads((dest / "wire-layout.json").read_text())
                assert wire == wire_profile["expected_runtime_wire_layout"], "literal CMIF frame and actual output span contract differs"
                manifest.setdefault("verified_wire_layout", {})[mode] = wire
                if args.sanitize and not args.unsuppressed_leaks:
                    item = parse_lsan_summary(result.stderr, guard)
                    if mode == "on":
                        assert item == manifest["legacy_suppressed_leaks"]["off"], "diagnostics changed legacy directory allocation count/size"
                    manifest.setdefault("legacy_suppressed_leaks", {})[mode] = item
            for name in ["application", "update", "standalone"]:
                assert (evidence / "off" / (name + ".json")).read_bytes() == (evidence / "on" / (name + ".json")).read_bytes(), "diagnostics changed actual results"
                events = [json.loads(line) for path in sorted((evidence / "on/diagnostics" / name).glob("diagnostics-*.jsonl")) for line in path.read_text().splitlines()]
                counts = {}
                for event in events:
                    counts[event["event"]] = counts.get(event["event"], 0) + 1
                expected = {"DLC_AOC_COUNT": 1, "DLC_AOC_LIST": 2, "DLC_AOC_BASE_ID": 1,
                            "DLC_DATA_ID_MATCHED": 2, "DLC_STORAGE_OPENED": 2, "DLC_DATA_READ": 2,
                            "DATA_STORAGE_REQUEST": 4, "DLC_DATA_ID_UNMATCHED": 2, "ASSET_STORAGE_OPENED": 1,
                            "PATCH_STORAGE_OPENED" if name == "update" else "BASE_STORAGE_OPENED": 1}
                assert all(counts.get(k, 0) == v for k, v in expected.items()), (name, counts)
                reads = [e["fields"] for e in events if e["event"] == "DLC_DATA_READ"]
                assert {item["title_id"] for item in reads} == {"0100000000002001", "0100000000002002"}
                for item in reads:
                    assert item["actual_bytes"] == item["buffer_size"] == item["request_size"] == "80"
                    assert item["byte_count_evidence"] == "reliable_backing_result" and item["status"] == "ok"
                    assert item["offset"] == "0" and item["read_ordinal"] == "1"
                assets = [event["fields"]["data_id"] for event in events if event["event"] == "ASSET_STORAGE_OPENED"]
                assert assets == ["01000000000FF001"], "only the existing fixture asset may emit open success; missing asset must not"
                unmatched = [event["fields"]["data_id"] for event in events if event["event"] == "DLC_DATA_ID_UNMATCHED"]
                assert unmatched == ["01000000000FF001", "01000000000FF002"]
                assert not any("ERROR" == e.get("level") for e in events), "positive/missing-file exception path unexpectedly emitted runtime error"
                manifest.setdefault("diagnostic_event_counts", {})[name] = counts
            manifest["diagnostics_same_results"] = True
            manifest["missing_asset_contract"] = "normal FileSystem::OpenFile exception; no new handle; no ASSET_STORAGE_OPENED for exact missing ID"
            manifest["diagnostic_count_evidence"] = "actual ReadUnchecked operation count 80, not legacy Read total backing size"
            manifest["scenario_count"] = 60
            manifest["source_commit_after"] = git("rev-parse", "HEAD")
            manifest["source_status_after"] = git("status", "--porcelain", "--untracked-files=all")
            assert manifest["source_commit_after"] == manifest["source_commit"]
            assert manifest["source_status_after"] == manifest["source_status"]
            assert all(sha((root / name).read_bytes()) == expected for name, expected in input_hashes.items()), "source changed during test run"
            save("passed")
            print("PASS 60 actual positive IPC/service scenarios; diagnostic counts/results and source provenance verified")
        except BaseException as error:
            if isinstance(error, subprocess.CalledProcessError):
                (evidence / "failed-process.txt").write_text((error.stdout or "") + (error.stderr or ""))
                print(error.stdout or "", end="")
                print(error.stderr or "", end="")
            save("failed")
            raise


if __name__ == "__main__":
    main()
