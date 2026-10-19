#!/usr/bin/env python3
"""Real AES/KeyStore scan reports + actual host JVM observed JNI; synthetic only.

Exact pinned dependencies are mandatory. --sanitize instruments the native/model
executables with ASan/UBSan/LSan, without suppression. The separate actual JVM
lane is not sanitizer instrumented. No APK, Android, guest or private data used.
"""
import argparse
import base64
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import threading
from selected_key_store_compat import key_store_functions


def sha(value):
    return hashlib.sha256(value).hexdigest()


def validate_report(path, contract):
    data = path.read_bytes()
    assert len(data) <= contract["max_report_bytes"] and data.isascii(), path
    assert b"PRIVATE" not in data, path
    report = json.loads(data)
    assert set(report) == {"schema_version", "status", "failed_stage", "error_code", "elapsed_us", "phases", "catalog_summary", "key_state"}, path
    assert report["schema_version"] == 2 and report["status"] in ["completed", "failed"]
    codes = contract["code_allowlist"]
    stages = contract["phase_stages"]
    assert report["failed_stage"] is None or report["failed_stage"] in stages
    assert report["error_code"] is None or report["error_code"] in codes
    assert type(report["elapsed_us"]) is int and 0 <= report["elapsed_us"] <= contract["max_time_us"]
    phases = report["phases"]
    assert len(phases) <= 5 and [p["stage"] for p in phases] == stages[:len(phases)]
    for phase in phases:
        assert set(phase) == {"stage", "start_us", "duration_us", "status"}
        assert phase["status"] in contract["phase_statuses"]
        assert all(type(phase[k]) is int and 0 <= phase[k] <= contract["max_time_us"] for k in ["start_us", "duration_us"])
    if report["status"] == "completed":
        assert len(phases) == 5 and all(p["status"] == "completed" for p in phases)
        assert report["failed_stage"] is None and report["error_code"] is None
    else:
        assert phases and phases[-1]["status"] == "failed" and report["failed_stage"] == phases[-1]["stage"]
        assert all(p["status"] == "completed" for p in phases[:-1])
    keys = report["key_state"]
    assert set(keys) == {"status", "title_keys", "prod_keys", "loaded_unique_title_key_count", "recognized_prod_slot_count", "counts_partial"}
    assert keys["status"] in contract["key_load_statuses"] and type(keys["counts_partial"]) is bool
    for name in ["title_keys", "prod_keys"]:
        value = keys[name]
        assert set(value) == {"presence", "read", "load"}
        assert value["presence"] in contract["file_presence_statuses"] and value["read"] in contract["file_read_statuses"] and value["load"] in contract["file_load_statuses"]
    for name, maximum in [("loaded_unique_title_key_count", contract["max_count"]), ("recognized_prod_slot_count", 81)]:
        assert keys[name] is None or type(keys[name]) is int and 0 <= keys[name] <= maximum
    if keys["status"] != "loaded" or any(keys[name]["presence"] == "check_failed" or keys[name]["read"] in ["short", "failed", "unknown"] or keys[name]["load"] == "failed" for name in ["title_keys", "prod_keys"]):
        assert keys["counts_partial"] is True
    if keys["status"] == "not_started":
        assert keys["loaded_unique_title_key_count"] is None and keys["recognized_prod_slot_count"] is None
    summary = report["catalog_summary"]
    if summary is None:
        return report
    assert set(summary) == {"bounded_scan_complete", "all_metadata_enumerated", "nca_count", "group_count", "other_entry_count", "meta_nca_count", "unknown_nca_count", "unreadable_nca_count", "role_counts", "issue_codes", "groups", "members", "groups_omitted", "members_omitted", "container_scope", "includes_external_associations", "container_size_bytes", "full_content_hash"}
    assert all(type(summary[k]) is bool for k in ["bounded_scan_complete", "all_metadata_enumerated"])
    def size(value):
        assert type(value) is str and re.fullmatch(r"0|[1-9][0-9]{0,19}", value) and int(value) <= 18446744073709551615
    size(summary["container_size_bytes"])
    assert summary["container_scope"] == "single_container" and summary["includes_external_associations"] is False
    assert summary["full_content_hash"] in contract["full_hash_statuses"]
    count_keys = ["nca_count", "group_count", "other_entry_count", "meta_nca_count", "unknown_nca_count", "unreadable_nca_count", "groups_omitted", "members_omitted"]
    assert all(type(summary[k]) is int and 0 <= summary[k] <= contract["max_count"] for k in count_keys)
    assert set(summary["role_counts"]) == set(contract["roles"])
    assert all(type(v) is int and 0 <= v <= contract["max_count"] for v in summary["role_counts"].values())
    def issues(values):
        assert len(values) <= contract["max_issue_codes_per_array"] and len(set(values)) == len(values)
        assert all(value in codes for value in values)
    issues(summary["issue_codes"])
    groups, members = summary["groups"], summary["members"]
    assert len(groups) + len(members) <= contract["max_combined_groups_members"]
    assert len(groups) + summary["groups_omitted"] == summary["group_count"]
    assert len(members) + summary["members_omitted"] == summary["nca_count"]
    for index, group in enumerate(groups):
        assert {"ordinal", "role", "parsing", "digest_observation", "issue_codes"} <= set(group)
        assert set(group) <= {"ordinal", "role", "parsing", "digest_observation", "issue_codes", "title_id", "application_id", "cnmt_group_id", "version"}
        assert group["ordinal"] == index and group["role"] in contract["roles"]
        assert group["parsing"] in contract["parsing"] and group["digest_observation"] in contract["digest_observation"]
        issues(group["issue_codes"])
        if "version" in group:
            assert group["parsing"] == "parsed" and type(group["version"]) is int and 0 <= group["version"] <= 4294967295
        for key in ["title_id", "application_id", "cnmt_group_id"]:
            if key in group:
                assert group["parsing"] == "parsed"
                assert re.fullmatch(r"cnmt-sha256:[0-9a-f]{64}" if key == "cnmt_group_id" else r"[0-9a-f]{16}", group[key])
    for index, member in enumerate(members):
        required = {"ordinal", "nca_type", "readability", "support", "issue_codes", "size_bytes", "size_source", "container_scope", "external_lookup", "full_hash_verification", "title_key_lookup", "title_key_lookup_evidence"}
        assert required <= set(member) <= required | {"content_id_hint", "content_id_source"}
        size(member["size_bytes"])
        assert member["size_source"] == "container_member_extent" and member["container_scope"] == "single_container" and member["external_lookup"] == "not_performed"
        assert member["full_hash_verification"] in contract["full_hash_statuses"]
        assert member["title_key_lookup"] in contract["title_key_lookup_statuses"]
        assert member["title_key_lookup_evidence"] == ("not_performed" if member["title_key_lookup"] == "not_observed" else "read_only_loaded_map_probe")
        assert ("content_id_hint" in member) == ("content_id_source" in member)
        if "content_id_hint" in member:
            assert member["nca_type"] in [0, 5] and re.fullmatch(r"[0-9a-f]{32}", member["content_id_hint"]) and member["content_id_source"] == "entry_name_hint"
        assert member["ordinal"] == index
        assert member["nca_type"] is None or type(member["nca_type"]) is int and 0 <= member["nca_type"] <= 255
        assert member["readability"] in contract["readability"] and member["support"] in contract["support"]
        issues(member["issue_codes"])
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--deps-root", type=Path)
    parser.add_argument("--evidence-dir", type=Path, required=True)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    src = root / "app/src/main/cpp/skyline"
    evidence = args.evidence_dir.resolve()
    evidence.mkdir(parents=True, exist_ok=True)
    if any(evidence.iterdir()):
        parser.error("--evidence-dir must be empty; earlier evidence is preserved")
    def git(*command, cwd=root):
        return subprocess.check_output(["git", *command], cwd=cwd, text=True).strip()
    compiler, cc = os.environ.get("CXX", "clang++"), os.environ.get("CC", "cc")
    linker = os.environ.get("CXXLD", compiler)
    version = subprocess.check_output([compiler, "--version"], text=True).splitlines()[0]
    if "clang" not in version.lower():
        parser.error("production legacy designated initializers require Clang; source rewriting is forbidden")
    javac = shutil.which("javac")
    if not javac:
        parser.error("Java17+ JDK is mandatory; JNI cases may not be skipped")
    java = Path(os.environ.get("JAVA_HOME", Path(javac).resolve().parents[1]))
    libraries, dependencies = (args.deps_root or root / "app/libraries").resolve(), {}
    def pinned(name, parent=root, relative=None):
        relative = relative or f"app/libraries/{name}"
        path = libraries / name if parent == root else parent / relative
        expected = git("ls-tree", "HEAD", relative, cwd=parent).split()[2]
        actual, status = git("rev-parse", "HEAD", cwd=path), git("status", "--porcelain", "--untracked-files=no", cwd=path)
        if expected != actual or status:
            parser.error(f"mandatory dependency {relative} missing, dirty or wrong pin")
        dependencies[str(path)] = {"pin": expected, "tracked_status": status}
        return path
    deps = {name: pinned(name) for name in ["boost", "fmt", "frozen", "xxhash", "mbedtls", "lz4"]}
    boost = [pinned(name, deps["boost"], f"libs/{name}") for name in ["assert", "config", "container", "intrusive", "move", "static_assert"]]
    assert '#define MBEDTLS_VERSION_STRING         "2.16.11"' in (deps["mbedtls"] / "include/mbedtls/version.h").read_text()
    inputs = set()
    for pattern in ["app/src/main/cpp/skyline/catalog/*", "app/src/main/cpp/skyline/vfs/*", "app/src/main/cpp/skyline/crypto/*", "app/src/main/cpp/skyline/common/**/*", "tests/selected-scan/**/*", "tests/selected-services/platform/common.h", "tests/selected-services/platform/common/signal.h"]:
        inputs.update(p for p in root.glob(pattern) if p.is_file() and "__pycache__" not in p.parts)
    extras = ["common.h", "loader/loader.h", "loader/executable.h", "loader/selected_launch.h", "loader/selected_launch_jni.h", "diagnostics/utf16.h"]
    inputs.update(src / name for name in extras)
    inputs.update(root / name for name in ["app/src/main/cpp/selected_scan_jni.cpp", "app/src/main/cpp/selected_jni.cpp", "app/CMakeLists.txt", "tests/selected/fixtures.h", "tests/catalog/native_aes_config.h", "tests/catalog/native_stubs/diagnostics/content.h", "tools/run-selected-scan-observed.py", "tools/selected_key_store_compat.py"])
    hashes = {str(p.relative_to(root)): sha(p.read_bytes()) for p in sorted(inputs)}
    compatibility = json.loads((root / "tests/selected-scan/compatibility.expected-v2.json").read_text())
    assert all(sha((root / name).read_bytes()) == expected for name, expected in compatibility["unchanged_sha256"].items()), "compatibility inspect, Catalog/schema, keys or selection policy changed"
    manifest = {"manifest_version": 1, "source_commit": git("rev-parse", "HEAD"), "source_status": git("status", "--porcelain", "--untracked-files=all"), "input_sha256": hashes, "dependency_pins": dependencies, "compiler_version": version, "linker_version": subprocess.check_output([linker, "--version"], text=True).splitlines()[0], "commands": [], "status": "running", "synthetic_inputs_only": True, "cipher_lane": "production_pinned_mbedtls_independent_openssl_fixture", "native_sanitizers": args.sanitize, "leak_check": "enabled_no_suppression" if args.sanitize else "not_requested", "host_jvm_sanitizers": "not_instrumented_separate_lane", "production_keystore": "real_app_private_policy_public_synthetic_tmp_root", "signature_verification": "not_verified", "ivfc_verification": "not_performed", "apk_build": "not_performed", "android_execution": False, "guest_execution": False, "substitutions": ["DeviceState declarations only", "no-op native diagnostic logging macros", "stack unwinding returns empty", "ARM/Android clock block only"]}
    manifest["compatibility_guard"] = compatibility
    manifest["native_setup_phases"] = "first_three_mark_prepared_test_inputs_only; scan_and_serialize_are_real"
    manifest["host_jvm_phases"] = "all_five_actual_production_JNI_operations"
    manifest["unexecuted_cases"] = ["Android VM/APK runtime", "NewStringUTF allocation failure injection", "commercial inputs", "guest execution"]
    manifest_path, lock = evidence / "manifest.json", threading.Lock()
    def save(status):
        manifest["status"] = status
        manifest["artifact_sha256"] = {str(p.relative_to(evidence)): sha(p.read_bytes()) for p in sorted(evidence.rglob("*")) if p.is_file() and p != manifest_path}
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    def run(command, **kwargs):
        with lock:
            manifest["commands"].append([str(s) for s in command]); manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
        return subprocess.run(command, cwd=root, check=True, **kwargs)
    with tempfile.TemporaryDirectory(prefix="pine-selected-observed-") as tmp:
        out = Path(tmp); stage = out / "skyline"; stage.mkdir()
        copies = {}
        def copy(name):
            data = (src / name).read_bytes(); target = stage / name
            target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes(data); copies[name] = sha(data)
        try:
            for directory in ["catalog", "vfs", "crypto"]:
                for p in sorted((src / directory).iterdir()):
                    if p.suffix in [".h", ".cpp", ".inc"]: copy(str(p.relative_to(src)))
            for name in [n for n in extras if n != "common.h"] + ["common/base.h", "common/exception.h", "common/format.h", "common/span.h", "common/language.h", "common/macros.h", "common/result.h"]: copy(name)
            original = (src / "common/utils.h").read_text()
            assert original.count("#include <sys/system_properties.h>") == 1
            utils = original.replace("#include <sys/system_properties.h>", "#include <chrono>")
            begin = utils.index("    namespace detail {"); end = utils.index("    /**\n     * @brief A way to implicitly", begin)
            utils = utils[:begin] + """    inline constexpr u64 ClockFrequency{1000000000};
    inline i64 GetTimeNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    inline u64 GetTimeTicks() { return static_cast<u64>(GetTimeNs()); }
""" + utils[end:]
            (stage / "common/utils.h").write_text(utils); (evidence / "platform-utils.h").write_text(utils)
            overlays = {}
            for name, source in {"common.h": "tests/selected-services/platform/common.h", "common/signal.h": "tests/selected-services/platform/common/signal.h", "diagnostics/content.h": "tests/catalog/native_stubs/diagnostics/content.h"}.items():
                assert name not in copies
                data = (root / source).read_bytes(); target = stage / name; target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes(data); overlays[name] = {"source": source, "sha256": sha(data)}
            fixture = (root / "tests/selected/fixtures.h").read_text(); old = "std::make_shared<crypto::KeyStore>()"
            assert fixture.count(old) == 1
            fixture = '#include <string>\nnamespace fixture { inline std::string keyRoot; }\n' + fixture.replace(old, "std::make_shared<crypto::KeyStore>(keyRoot)")
            (out / "fixtures.h").write_text(fixture); (evidence / "synthetic-fixtures.h").write_text(fixture)
            jni = (root / "app/src/main/cpp/selected_scan_jni.cpp").read_bytes(); (out / "selected_scan_jni.cpp").write_bytes(jni)
            (evidence / "source-staging.json").write_text(json.dumps({"byte_identical_sha256": copies, "jni_byte_identical_sha256": sha(jni), "platform_overlay": overlays, "utils_original_sha256": sha(original.encode()), "utils_platform_sha256": sha(utils.encode()), "fixture_original_sha256": sha((root / "tests/selected/fixtures.h").read_bytes()), "fixture_generated_sha256": sha(fixture.encode()), "fixture_transform": "one KeyStore constructor and explicit synthetic root declaration; cipher and format writers unchanged"}, indent=2) + "\n")
            includes = [f"-I{stage}", f"-I{out}", f"-I{root / 'tests/catalog'}", f"-I{deps['mbedtls'] / 'include'}", f"-I{deps['lz4'] / 'lib'}", f"-I{deps['frozen'] / 'include'}", f"-I{deps['fmt'] / 'include'}", f"-I{deps['xxhash']}", f"-I{java / 'include'}", f"-I{java / 'include/linux'}"]
            for folder in boost: includes += ["-isystem", str(folder / "include")]
            flags = ["-include", "cstdint", "-std=c++20", "-O1", "-g", "-fPIC", "-ffunction-sections", "-fdata-sections", "-DFMT_HEADER_ONLY=1", "-DPINE_CATALOG_TEST_REAL_AES", "-DPINE_ENABLE_SELECTED_CONTENT=1", '-DMBEDTLS_CONFIG_FILE="native_aes_config.h"', "-Wno-reorder-init-list", "-Wno-c99-designator", *includes]
            baseline = out / "ordinary-baseline"
            (baseline / "crypto").mkdir(parents=True)
            header_guard = compatibility["key_store_header_abi_baseline"]
            assert header_guard["encoding"] == "base64"
            baseline_header = base64.b64decode(b"".join((root / header_guard["fixture"]).read_bytes().splitlines()), validate=True)
            assert sha(baseline_header) == header_guard["sha256"]
            (baseline / "crypto/key_store.h").write_bytes(baseline_header)
            (baseline / "crypto/key_store.cpp").write_bytes((src / "crypto/key_store.cpp").read_bytes())
            ordinary = {}
            for name, folder in [("baseline", baseline), ("current", stage)]:
                obj = out / (name + "-ordinary-key-store.o")
                run([compiler, f"-I{folder}", *flags, "-O2", "-g0", "-c", str(folder / "crypto/key_store.cpp"), "-o", str(obj)])
                ordinary[name] = key_store_functions(obj)
                size_source = out / (name + "-size.cpp")
                size_source.write_text('#include <crypto/key_store.h>\n#include <cstdio>\nint main() { std::printf("%zu\\n", sizeof(skyline::crypto::KeyStore)); }\n')
                size_exe = out / (name + "-size")
                run([compiler, f"-I{folder}", *flags, "-O2", "-g0", str(size_source), "-o", str(size_exe)])
                result = run([str(size_exe)], text=True, capture_output=True)
                ordinary[name + "_sizeof"] = int(result.stdout.strip())
            ordinary["function_bytes_and_relocations_equal"] = ordinary["baseline"] == ordinary["current"]
            ordinary["sizeof_equal"] = ordinary["baseline_sizeof"] == ordinary["current_sizeof"]
            (evidence / "ordinary-key-store-compatibility.json").write_text(json.dumps(ordinary, indent=2) + "\n")
            assert ordinary["function_bytes_and_relocations_equal"] and ordinary["sizeof_equal"], "ordinary KeyStore compiled functions or layout changed"
            manifest["ordinary_key_store_binary_compatibility"] = {"methods": len(ordinary["current"]), "function_bytes_and_relocations_equal": True, "sizeof": ordinary["current_sizeof"]}
            print("PASS ordinary KeyStore constructor/parser/populate function bytes, relocations and sizeof unchanged", flush=True)
            sources = ["catalog/wire.cpp", "catalog/catalog.cpp", "catalog/json.cpp", "catalog/native_adapter.cpp", "catalog/scan_report.cpp", "vfs/nca.cpp", "vfs/partition_filesystem.cpp", "vfs/ctr_encrypted_backing.cpp", "vfs/bktr.cpp", "vfs/os_filesystem.cpp", "vfs/os_backing.cpp", "crypto/aes_cipher.cpp", "crypto/key_store.cpp", "crypto/selected_key_store.cpp"]
            manifest["compiled_production_sources"] = sources + ["selected_scan_jni.cpp"]
            def build(lane, sanitize):
                folder = out / lane; folder.mkdir(); san = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if sanitize else []
                objects = []
                for name in ["aes", "cipher", "cipher_wrap", "platform_util"]:
                    obj = folder / (name + ".o")
                    run([cc, "-std=c99", "-O1", "-g", "-fPIC", *san, '-DMBEDTLS_CONFIG_FILE="native_aes_config.h"', f"-I{root / 'tests/catalog'}", f"-I{deps['mbedtls'] / 'include'}", "-c", str(deps['mbedtls'] / 'library' / (name + '.c')), "-o", str(obj)])
                    objects.append(str(obj))
                obj = folder / "lz4.o"; run([cc, "-O1", "-g", "-fPIC", *san, "-c", str(deps['lz4'] / 'lib/lz4.c'), "-o", str(obj)]); objects.append(str(obj))
                paths = [stage / s for s in sources] + [out / "selected_scan_jni.cpp", root / "tests/selected-scan/native_observed_test.cpp", root / "tests/selected-scan/key_store_observation_test.cpp"]
                def compile_one(value):
                    i, path = value; obj = folder / (str(i) + ".o")
                    run([compiler, *flags, *san, *(["-DPINE_OBSERVED_JNI_LIBRARY=1"] if lane == "jvm" else []), "-c", str(path), "-o", str(obj)])
                    return str(obj)
                with ThreadPoolExecutor(max_workers=4) as pool: objects += list(pool.map(compile_one, enumerate(paths)))
                binary = folder / ("libobserved.so" if lane == "jvm" else "observed-test")
                run([linker, *san, *objects, *(["-shared"] if lane == "jvm" else []), "-Wl,--gc-sections", "-pthread", "-lcrypto", "-o", str(binary)])
                manifest.setdefault("binary_sha256", {})[lane] = sha(binary.read_bytes())
                return binary
            env = os.environ.copy()
            if args.sanitize:
                env["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1"
                env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
                env.pop("LSAN_OPTIONS", None)
                manifest["sanitizer_environment"] = {key: env[key] for key in ["ASAN_OPTIONS", "UBSAN_OPTIONS"]}
            native = build("native", args.sanitize)
            native_out = evidence / "native"; native_out.mkdir()
            result = run([str(native), str(out / "native-roots"), str(native_out)], text=True, capture_output=True, env=env)
            (native_out / "result.txt").write_text(result.stdout + result.stderr); print(result.stdout, end="", flush=True)
            assert result.stdout.count("PASS native ") == 8
            assert result.stdout.count("PASS key-state ") == 17
            model = out / "model-test"; san = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitize else []
            run([linker, "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", *san, f"-I{src}", *[str(src / name) for name in ["catalog/wire.cpp", "catalog/catalog.cpp", "catalog/scan_report.cpp"]], str(root / "tests/selected-scan/report_model_test.cpp"), "-o", str(model)])
            model_out = evidence / "model"; model_out.mkdir()
            result = run([str(model), str(model_out)], text=True, capture_output=True, env=env)
            (model_out / "result.txt").write_text(result.stdout + result.stderr); print(result.stdout, end="", flush=True)
            library = build("jvm", False)
            run([javac, "-d", str(out), str(root / "tests/selected-scan/SelectedScanObservedFixture.java")])
            jvm_out = evidence / "host-jvm"; jvm_out.mkdir()
            result = run([str(java / "bin/java"), "-Xcheck:jni", "-cp", str(out), "emu.skyline.loader.SelectedScanObservedFixture", str(library), str(out / "jvm-roots"), str(jvm_out)], text=True, capture_output=True)
            (jvm_out / "result.txt").write_text(result.stdout + result.stderr); print(result.stdout + result.stderr, end="", flush=True)
            assert "WARNING in native method" not in result.stdout + result.stderr and "FATAL ERROR in native method" not in result.stdout + result.stderr
            assert "PASS 22 actual host-JVM" in result.stdout
            contract = json.loads((root / "tests/selected-scan/report-contract-v2.json").read_text())
            assert json.loads((model_out / "native-code-allowlist.json").read_text()) == contract["code_allowlist"]
            count = 0
            for path in sorted(evidence.glob("*/*.json")):
                if path.name == "native-code-allowlist.json": continue
                validate_report(path, contract); count += 1
            # Pure parser regressions, distinct from the real native scenarios:
            # mutate only the partial flag on actual produced safe reports.
            partial_cases = ["short-read-loaded-prefix", "short-read-failed-prefix", "unreliable-read-marked-unknown",
                             "controlled-read-failure", "controlled-open-failure", "controlled-probe-failure",
                             "failed-title-prefix", "failed-prod-prefix"]
            for name in partial_cases:
                candidate = json.loads((evidence / "key-state" / (name + ".json")).read_text())
                assert candidate["key_state"]["counts_partial"] is True
                candidate["key_state"]["counts_partial"] = False
                temporary = out / "invalid-partial-model.json"
                temporary.write_text(json.dumps(candidate))
                try: validate_report(temporary, contract)
                except AssertionError: pass
                else: raise AssertionError("false partial flag accepted: " + name)
            (evidence / "partial-flag-parser-regressions.txt").write_text("PASS 8 constructed false-partial mutations rejected; source reports came from actual native cases\n")
            manifest["constructed_partial_flag_regressions"] = len(partial_cases)
            manifest["validated_safe_report_count"] = count
            manifest["native_assertions"] = 8; manifest["model_assertions"] = 4; manifest["host_jvm_assertions"] = 22
            manifest["key_state_assertions"] = 17
            manifest["raw_catalog_persisted"] = False
            manifest["source_commit_after"] = git("rev-parse", "HEAD"); manifest["source_status_after"] = git("status", "--porcelain", "--untracked-files=all")
            assert manifest["source_commit_after"] == manifest["source_commit"]
            assert manifest["source_status_after"] == manifest["source_status"]
            assert all(sha((root / name).read_bytes()) == value for name, value in hashes.items()), "owned input changed during run"
            save("passed"); print(f"PASS {count} safe report contracts; true AES native + actual host JNI + model provenance")
        except BaseException as error:
            if isinstance(error, subprocess.CalledProcessError):
                (evidence / "failed-process.txt").write_text((error.stdout or "") + (error.stderr or "")); print(error.stdout or "", end=""); print(error.stderr or "", end="")
            save("failed"); raise


if __name__ == "__main__":
    main()
