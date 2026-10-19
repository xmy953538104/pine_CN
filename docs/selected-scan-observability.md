# Selected content scan reports

This change is based on the opt-in Selected content candidate `40419e0e` (PR #14).
It diagnoses the existing scan and selection decisions. It does not change
ordinary Play, key provisioning, Ticket import, content identity, CNMT digest
policy, BKTR support or selection eligibility.

## What the message means

The ordinary app-details version fallback can show `metadata_missing` even when
ordinary Play works. Its diagnostic event records the actual resource name and
`app_details_version` stage; it is not evidence that a Selected scan happened.

The Selected picker previously used `selected_no_candidates` for an empty list
of primary rows and `selected_scan_failed` for an exception. A completed native
scan is now distinguishable from these selection outcomes:

| Outcome | Observation |
| --- | --- |
| `no_groups` | No CNMT groups were retained. Member issue codes can still explain a missing Meta key or an unreadable member. |
| `no_primary_role` | Groups exist, but none are Application/Patch primary rows. |
| `primary_rows_all_rejected` | Primary rows exist and remain visible, with their actual first rejection predicates. |
| `primary_rows_available` | At least one primary row passed the existing strict rules. This is not a launch/runtime success. |
| `scan_failed` | FD/JNI/envelope/catalog processing failed. The fixed failure code and completed/failed native phases distinguish where. |

The first native issue and the first UI rejection predicate are separate
observations. A retained issue is not automatically the cause of a later guest
failure. Ordinary Play imports Tickets through its existing Loader; the Selected
inspector does not, so ordinary Play success does not prove that the inspector
has the same key prerequisites.

## Safe report boundary

The new `inspectObserved` JNI entry returns the existing transient Catalog plus
a separately constructed report. The old `inspect` API and Catalog JSON schema
remain unchanged. The five native stages are `profile_validation`,
`keystore_load`, `fd_duplicate`, `catalog_scan` and `catalog_serialize`.
Completion is recorded only after an operation returns. Durations use a
monotonic clock; they are not guest CPU progress measurements.

Native reporting copies only explicit enums, bounded counts/timings, real
allowlisted issue codes, and validated parsed public identifiers. It never
copies Catalog member names, arbitrary IDs, URI/path, exception text, keys,
Rights IDs, Ticket bodies, raw metadata or commercial bytes. Kotlin rebuilds
the report from its own allowlist before persistence and again at export.
The native report is limited to 16 KiB and 32 combined group/member rows.
An omitted count makes truncation explicit.

Each UI attempt has an independent UUID, build/session association and exactly
one terminal result: completed, failed or cancelled. Closing the picker discards
late JNI results; it does not close an FD while synchronous native inspection
still owns it. The report uses `report_sequence`, independently of the native
diagnostic sink's `sequence`.

Diagnostics remains default OFF. Persistent reports are restricted to
`main/selected-scan-0.jsonl` and `main/selected-scan-1.jsonl`, at most 128 KiB each
and 32 KiB per complete record. Writes use a bounded serial queue and recheck
the captured active session without holding the global diagnostics monitor over
disk I/O. Queued revoked attempts are discarded; a write already in progress
may finish with its captured old-session label. Drop counts are retained in the
next safe report and bounded live summary. These exact names are part of the export allowlist;
arbitrary directories, backups and save files are not added.

## Interpreting digest observations

`cnmt_digest_mismatch` records the existing recomputation observation. It does
not assert that a production CNMT is corrupt: the documented production format
can retain a digest from development metadata. The existing strict selection
rule is preserved in this patch. Any future policy change needs independent
format tests and review; do not edit commercial CNMTs or remove Tickets to make
inspection pass. See the pinned [Switchbrew CNMT digest description](https://switchbrew.org/w/index.php?title=CNMT&oldid=13421#Digest).

## Owner check after offline qualification

1. Install the new PR's exact Debug APK; retain the already working PR #14 APK
   as a separate baseline. Confirm the new build commit in diagnostics settings.
2. Enable Diagnostics, note the Session ID, then open the existing NSP through
   the Selected picker once for the required profile. A freeze reproduction is
   not required for this scan check.
3. Record the actual displayed message/attempt reference. Cancel normally if
   no selectable row is present; do not repeatedly attempt a rejected launch.
4. Preview and export the diagnostic ZIP. The two fixed report files, when
   present, contain sanitized scan attempts. Check the matching Session/attempt,
   phase status, native issue codes, role counts and UI rejection predicate.
5. Turn Diagnostics OFF. Further scans must not append persistent reports.

Automated evidence must identify constructed Catalog/JVM cases separately from
actual AES-backed native scans and host-JVM JNI calls. None is an Android APK
runtime or commercial-game pass. Cloud Android runtime results remain separate
and must state any environment blocker explicitly.
