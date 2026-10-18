#!/usr/bin/env python3
"""Archive only this build's unstripped Pine ELF, after matching its APK GNU Build ID."""
import argparse
import hashlib
import json
import pathlib
import re
import shutil
import subprocess
import zipfile

LIBRARY = "lib/arm64-v8a/libskyline.so"

def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()

def inspect_elf(path, readelf):
    with path.open("rb") as source:
        header = source.read(64)
    if len(header) < 64 or header[:6] != b"\x7fELF\x02\x01" or int.from_bytes(header[18:20], "little") != 183:
        raise ValueError("Expected ELF64 little-endian AArch64 library")
    result = subprocess.run([str(readelf), "--notes", "--sections", "--wide", str(path)],
                            check=True, capture_output=True, text=True, timeout=30)
    identifiers = re.findall(r"Build ID:\s*([0-9a-fA-F]+)", result.stdout)
    if len(identifiers) != 1 or not re.fullmatch(r"[0-9a-fA-F]{8,128}", identifiers[0]):
        raise ValueError("Missing or ambiguous GNU Build ID")
    return identifiers[0].lower(), bool(re.search(r"\s\.debug_info\s", result.stdout))

def verified_artifact_name(name, commit):
    if not re.fullmatch(r"[A-Za-z0-9_.-]{1,160}", name) or not name.endswith("-" + commit):
        raise ValueError("Symbol artifact name must be safe and end in this exact source SHA")
    return name

def archive_symbols(apk, commit, roots, output, readelf, inspector=inspect_elf):
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("Expected exact source SHA")
    if output.exists() and any(output.iterdir()):
        raise ValueError("Symbol output must be empty")
    output.mkdir(parents=True, exist_ok=True)
    packaged = output / "packaged-library.tmp"
    try:
        with zipfile.ZipFile(apk) as archive:
            entries = [info for info in archive.infolist() if info.filename == LIBRARY]
            if len(entries) != 1 or entries[0].file_size > 1024**3:
                raise ValueError("Missing, duplicate or oversized packaged Pine ELF")
            with archive.open(entries[0]) as source, packaged.open("wb") as target:
                shutil.copyfileobj(source, target, 1024 * 1024)
        build_id, _ = inspector(packaged, readelf)
        packaged_sha = digest(packaged)
        matches = {}
        examined = set()
        for root in roots:
            if not root.is_dir() or root.is_symlink():
                continue
            canonical_root = root.resolve()
            for candidate in root.rglob("libskyline.so"):
                resolved = candidate.resolve()
                if candidate.is_symlink() or not resolved.is_relative_to(canonical_root) or resolved in examined:
                    continue
                examined.add(resolved)
                if len(examined) > 64 or candidate.stat().st_size > 1024**3:
                    raise ValueError("Native symbol candidate bounds exceeded")
                candidate_id, debug = inspector(candidate, readelf)
                if candidate_id == build_id and debug:
                    matches[digest(candidate)] = candidate
        if len(matches) != 1:
            raise ValueError(f"Expected one byte-unique matching unstripped ELF, found {len(matches)}")
        unstripped_sha, source = next(iter(matches.items()))
        destination = output / "arm64-v8a/libskyline.so"
        destination.parent.mkdir()
        shutil.copyfile(source, destination)
        if digest(destination) != unstripped_sha or inspector(destination, readelf) != (build_id, True):
            raise ValueError("Archived symbol identity changed during copy")
        manifest = {
            "schema": 1, "source_commit": commit, "abi": "arm64-v8a", "library": "libskyline.so",
            "apk_sha256": digest(apk), "packaged_elf_sha256": packaged_sha,
            "unstripped_elf_sha256": unstripped_sha, "elf_build_id": build_id, "debug_info_present": True,
            "symbol_file": "arm64-v8a/libskyline.so",
            "address_scope": "Observed relative PCs for this exact Build ID only; no attribution to older binaries.",
        }
        (output / "native-symbols.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        return manifest
    finally:
        packaged.unlink(missing_ok=True)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("apk", type=pathlib.Path)
    parser.add_argument("commit")
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("--root", action="append", required=True, type=pathlib.Path)
    parser.add_argument("--readelf", required=True, type=pathlib.Path)
    parser.add_argument("--artifact-name", required=True, help="Exact Actions upload-artifact name for these symbols")
    args = parser.parse_args()
    actual = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    if actual != args.commit:
        raise ValueError("Source checkout does not match requested symbols")
    artifact_name = verified_artifact_name(args.artifact_name, actual)
    identity_path = args.apk.with_name("build-identity.json")
    identity = json.loads(identity_path.read_text(encoding="utf-8"))
    if identity["commit"] != actual or identity["sha256"] != digest(args.apk):
        raise ValueError("APK identity does not match the current build")
    manifest = archive_symbols(args.apk, actual, args.root, args.output, args.readelf)
    if identity["skyline_elf_sha256"] != manifest["packaged_elf_sha256"]:
        raise ValueError("Packaged ELF differs from APK verification")
    identity["native_symbols"] = {key: manifest[key] for key in ("elf_build_id", "unstripped_elf_sha256", "debug_info_present")}
    identity["native_symbols"]["artifact"] = artifact_name
    identity_path.write_text(json.dumps(identity, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(manifest, sort_keys=True))

if __name__ == "__main__":
    main()
