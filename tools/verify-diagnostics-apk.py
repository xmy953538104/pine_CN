#!/usr/bin/env python3
"""Check packaged Debug ABI/identity and write a SHA256 manifest beside the APK."""
import hashlib
import json
import pathlib
import re
import subprocess
import sys
import zipfile
import xml.etree.ElementTree as ET

apk = pathlib.Path(sys.argv[1])
expected = sys.argv[2]
actual = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
assert re.fullmatch(r"[0-9a-f]{40}", expected) and actual == expected, (actual, expected)
badging = apk.with_name("apk-badging.txt").read_text(encoding="utf-8")
assert "name='io.github.pine.emu.diagnostics.preview'" in badging
assert "-diagnostics-v1'" in badging
assert "sdkVersion:'29'" in badging
with zipfile.ZipFile(apk) as archive:
    libs = sorted(p for p in archive.namelist() if p.startswith("lib/") and p.endswith(".so"))
    assert "lib/arm64-v8a/libskyline.so" in libs, libs
    assert all(p.split("/")[1] == "arm64-v8a" for p in libs), libs
    for name in libs:
        elf = archive.read(name)
        assert elf[:6] == b"\x7fELF\x02\x01" and int.from_bytes(elf[18:20], "little") == 183, (name, "not ELF64 AArch64")
    skyline_digest = hashlib.sha256(archive.read("lib/arm64-v8a/libskyline.so")).hexdigest()
    assert any(expected.encode() in archive.read(name) for name in archive.namelist() if re.fullmatch(r"classes\d*\.dex", name)), "BuildConfig commit missing from DEX"
reports = list(pathlib.Path("app/build/test-results/testFullDebugUnitTest").glob("TEST-*.xml"))
suites = [ET.parse(path).getroot() for path in reports]
jvm = {key: sum(int(suite.attrib.get(key, 0)) for suite in suites) for key in ("tests", "failures", "errors", "skipped")}
assert jvm["tests"] >= 126 and not any(jvm[key] for key in ("failures", "errors", "skipped")), jvm
jvm["classes"] = sorted(suite.attrib["name"] for suite in suites)
assert all(any(name.endswith(required) for name in jvm["classes"]) for required in (
    "DiagnosticExportTest", "ContentSelectionTest", "DiagnosticsProcessTest", "EmulationGenerationTest",
    "EmulationActivityCompletionTest", "StartupDiagnosticsTest", "ContentFilesTest", "UriLaunchFailureTest",
    "LaunchIdentityTest", "ProcessAssociationTest", "NativeTraceSummaryTest", "NativeTraceImportTest", "MetadataJniMappingTest", "ContentDiagnosticFieldsTest",
    "DiagnosticsInitializationTest")), jvm
initialization = next(suite for suite in suites if suite.attrib["name"].endswith("DiagnosticsInitializationTest"))
assert int(initialization.attrib["tests"]) >= 15
assert {"preparedReplacementPublishesAfterAnAlreadyOwnedOldDrain",
        "replacementPreferenceReadCrossingOldFinishStillPublishesForItsOwnedLaunch"} <= {
            case.attrib["name"] for case in initialization.findall("testcase")}, "Owned-launch regression tests were not executed"
completion = next(suite for suite in suites if suite.attrib["name"].endswith("EmulationActivityCompletionTest"))
assert {"actualMenuConfirmationRecordsRequestBeforeOwnedDrainAndKill", "staleMenuConfirmationCannotRecordIntoReplacementLaunch",
        "destroyedGenerationCannotRecordANewMenuExitRequest"} <= {case.attrib["name"] for case in completion.findall("testcase")}
exports = next(suite for suite in suites if suite.attrib["name"].endswith("DiagnosticExportTest"))
assert {"realNativeLoggerPrefixCannotExportEncodedPrivateDfxPayload", "productionGuestDfx1RawTextAndDecodedPayloadSurviveRealZipExport",
        "previousSessionCriticalStatesUseFixedNamesAndKeepSourceIdentity", "dfxFragmentCannotReviveAWholeLineCredentialOrOmittedPrivatePathTail",
        "combinedDfxEncodingAndPlainPrivacyRulesComposeInBothFieldOrders"} <= {case.attrib["name"] for case in exports.findall("testcase")}
release_manifests = [*pathlib.Path("app/build/intermediates/merged_manifest/fullRelease").rglob("AndroidManifest.xml"),
                     *pathlib.Path("app/build/intermediates/merged_manifests/fullRelease").rglob("AndroidManifest.xml")]
android_process = "{http://schemas.android.com/apk/res/android}process"
android_name = "{http://schemas.android.com/apk/res/android}name"
assert any(any(activity.attrib.get(android_name, "").endswith(".EmulationActivity") and activity.attrib.get(android_process) == ":emulationProcess"
               for activity in ET.parse(path).getroot().iter("activity")) for path in release_manifests), "Release emulation-process manifest not verified"
metadata = {
    "schema": 1,
    "commit": actual,
    "apk": apk.name,
    "sha256": hashlib.file_digest(apk.open("rb"), "sha256").hexdigest(),
    "application_id": "io.github.pine.emu.diagnostics.preview",
    "abi": ["arm64-v8a"],
    "min_sdk": 29,
    "build_type": "debug",
    "skyline_elf_sha256": skyline_digest,
    "jvm_tests": jvm,
    "release_emulation_process_manifest_verified": True,
    "runtime_validation": "See docs/diagnostics-validation.md; build verification is not device execution.",
}
signer_manifest = apk.with_name("signer-identity.json")
if signer_manifest.is_file():
    signer = json.loads(signer_manifest.read_text(encoding="utf-8"))
    assert signer["source_commit"] == actual and signer["apk_sha256"] == metadata["sha256"]
    assert signer["signature_verified"] is True
    metadata["certificate_sha256"] = signer["certificate_sha256"]
apk.with_name("build-identity.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
print(json.dumps(metadata, sort_keys=True))
