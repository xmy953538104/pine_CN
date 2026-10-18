#!/usr/bin/env python3
"""Compile and run the isolated native bridge test; no external keys or ROMs.

Production NCA, CTR, PFS, RegionBacking and OsBacking sources are compiled.
Only Android/common facilities, loader exception declarations and crypto are
controlled stand-ins. The default cipher is NOT AES. --real-aes instead builds
Pine's production AES wrapper and pinned mbedTLS against independent OpenSSL
fixture encryption. Neither mode validates commercial ROMs or Android builds.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--real-aes", action="store_true")
    parser.add_argument("--mbedtls-root", type=Path)
    parser.add_argument("--evidence-dir", type=Path, help="retain native JSON outputs instead of deleting temporary evidence")
    parser.add_argument("--schema-check", action="store_true", help="require jsonschema and validate every native JSON output")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    compiler = os.environ.get("CXX", "c++")
    # Legacy VFS headers contain unused virtual parameters and signed compares.
    common = ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread",
              "-Wno-unused-parameter", "-Wno-sign-compare",
              "-Itests/catalog/native_stubs", "-Iapp/src/main/cpp/skyline"]
    if args.sanitize:
        common += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    production = "app/src/main/cpp/skyline/"
    sources = ["catalog/wire.cpp", "catalog/catalog.cpp", "catalog/json.cpp", "catalog/native_adapter.cpp",
               "vfs/nca.cpp", "vfs/partition_filesystem.cpp", "vfs/ctr_encrypted_backing.cpp",
               "vfs/bktr.cpp", "vfs/os_backing.cpp"]
    cipher_root = None
    if args.real_aes:
        cipher_root = (args.mbedtls_root or root / "app/libraries/mbedtls").resolve()
        version = cipher_root / "include/mbedtls/version.h"
        if not version.is_file() or '#define MBEDTLS_VERSION_STRING         "2.16.11"' not in version.read_text():
            parser.error("--real-aes requires this repository's initialized, pinned mbedTLS 2.16.11 sources; no dependency skip is allowed")
        pin = subprocess.run(["git", "ls-tree", "HEAD", "app/libraries/mbedtls"], cwd=root,
                             text=True, capture_output=True, check=True).stdout.split()[2]
        actual = subprocess.run(["git", "-C", str(cipher_root), "rev-parse", "HEAD"],
                                text=True, capture_output=True, check=True).stdout.strip()
        dirty = subprocess.run(["git", "-C", str(cipher_root), "status", "--porcelain", "--untracked-files=no"],
                               text=True, capture_output=True, check=True).stdout
        if actual != pin or dirty:
            parser.error("--real-aes requires clean mbedTLS sources at this repository's exact submodule commit")
        print(f"Using pinned mbedTLS 2.16.11 at {pin}; independent OpenSSL fixture encryption", flush=True)
        common = [*common[:1], "-DPINE_CATALOG_TEST_REAL_AES", '-DMBEDTLS_CONFIG_FILE="native_aes_config.h"',
                  "-Itests/catalog/native_real_stubs", "-Itests/catalog", f"-I{cipher_root / 'include'}", *common[1:]]
        sources += ["crypto/aes_cipher.cpp"]
    with tempfile.TemporaryDirectory(prefix="pine-native-catalog-") as folder:
        output = Path(folder)
        objects = []
        if cipher_root:
            c_compiler = os.environ.get("CC", "cc")
            for source in ["aes", "cipher", "cipher_wrap", "platform_util"]:
                obj = output / f"mbedtls-{source}.o"
                flags = ["-std=c99", "-O1", "-g", '-DMBEDTLS_CONFIG_FILE="native_aes_config.h"',
                         "-Itests/catalog", f"-I{cipher_root / 'include'}"]
                if args.sanitize:
                    flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
                subprocess.run([c_compiler, *flags, "-c", str(cipher_root / "library" / f"{source}.c"), "-o", str(obj)], cwd=root, check=True)
                objects.append(str(obj))
        for index, source in enumerate(sources + ["tests/catalog/native_adapter_test.cpp"]):
            path = source if source.startswith("tests/") else production + source
            flags = common.copy()
            if source.startswith("vfs/"):
                # Existing backend warnings are not changes in this adapter.
                flags += ["-Wno-unused-parameter", "-Wno-reorder", "-Wno-return-type"]
            obj = output / f"{index}.o"
            subprocess.run([compiler, *flags, "-c", path, "-o", str(obj)], cwd=root, check=True)
            objects.append(str(obj))
        executable = output / "native-adapter-test"
        libraries = ["-lcrypto"] if args.real_aes else []
        subprocess.run([compiler, *common, *objects, *libraries, "-o", str(executable)], cwd=root, check=True)
        evidence = args.evidence_dir.resolve() if args.evidence_dir else output / "json"
        subprocess.run([str(executable), str(evidence)], cwd=root, check=True)
        expected = {"native-complete.json": (2, 2, True, True), "native-missing-keys.json": (2, 0, False, True),
                    "native-partial.json": (2, 1, False, True), "native-independent-golden.json": (1, 1, True, True),
                    "native-pfs-budget.json": (2, 1, False, False), "native-header-budget.json": (1, 0, False, False),
                    "native-nonzero-header.json": (1, 1, True, True)}
        for name, (ncas, groups, complete, bounded_complete) in expected.items():
            data = json.loads((evidence / name).read_text())
            assert data["schema_version"] == 1 and data["inspector"] == "pine_existing_nca"
            assert len(data["ncas"]) == ncas and len(data["groups"]) == groups
            assert data["all_metadata_enumerated"] is complete
            assert data["bounded_scan_complete"] is bounded_complete
            assert data["verification"]["signature_verification"] == "not_verified"
            if name == "native-nonzero-header.json":
                header = data["groups"][0]["packaged_header"]
                assert header["storage_id"] == 5 and header["install_type"] == 1
                assert header["committed_raw"] == 1 and header["committed"] is True
                assert header["required_download_system_version"] == 0x04030201
            if args.schema_check:
                import jsonschema
                schema = json.loads((root / "docs/nsp-catalog-v1.schema.json").read_text())
                jsonschema.Draft202012Validator(schema).validate(data)
        print("PASS: native Catalog JSON contract, partial/missing-key completeness, independent CNMT tables and raw ciphertext hash scope")


if __name__ == "__main__":
    main()
