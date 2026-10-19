# Selected scan key and content observations

This supplement starts at `ae10d4405e9904c4be0778211b544787bec67124`
(PR #16). It adds observations to the opt-in Selected inspector. Ordinary Play,
Ticket import, key precedence, strict selection predicates, CNMT digest policy
and BKTR support keep their existing behavior. It does not repair or replace
the key importer.

## Reading the report

The native report uses version 2; historical version 1 reports remain readable
and exportable. The enclosing persisted scan-record schema stays version 1.
All observations describe the one container opened by this scan. Associated
Update or DLC files discovered by ordinary app details are not inspected by
this operation and must not be inferred from its rows.

The two fixed `title_keys` and `prod_keys` entries report file presence, actual
read completion and load outcome separately. They do not expose filenames,
paths, key material, Rights IDs, key hashes or rejected input lines. A present
file may be empty, unreadable or malformed. A completed read is not necessarily
a successful parse. Counts come from the strict native parser's resulting map
and recognized optional slots, rather than counting input lines. Duplicate
title entries retain the existing first-wins rule. Counts are marked partial
when loading fails; a title-file failure can leave the prod-file read unstarted.

`title_key_lookup` is a probe of the already loaded title-key map using the
existing decrypted NCA header. Its values are `not_observed`, `not_required`,
`matched` and `not_matched`. A match establishes only that an entry is present;
it is not proof of successful decryption, authorization, integrity or launch.
A header that could not be inspected cannot establish a missing target entry.
The first actual UI rejection remains separate from later key, digest and
member observations. This supplement never overrides selection eligibility.

Parsed CNMT version is recorded when available. Program/PublicData rows may
include a validated public `content_id_hint` with `entry_name_hint` provenance.
This hint comes from a canonical NCA member name; it is not a full content hash
or a verified CNMT reference association. Member size describes the container
extent. Scope is `single_container`, external lookup is `not_performed`, and
the preview's full-member hash verification is `not_performed`. No additional
runtime hashing or external storage search is introduced.

Reports retain the 16 KiB native limit, 32 combined group/member rows and
explicit omitted counts. Kotlin independently rebuilds fixed fields before
persisting and again before export. Existing cancellation, diagnostics OFF,
session revocation, record/rotation limits and exact export allowlist apply.
See [scan report boundaries](selected-scan-observability.md).

## Importer characterization

`KeyReaderObservationTest` invokes the unchanged Kotlin importer with synthetic
public strings in a Robolectric app sandbox. Only the Android document-authority
gate is substituted. These are importer tests, not an APK or device-permission
test. The seven tests cover these existing behaviors:

| Synthetic case | Current importer outcome |
| --- | --- |
| Empty or comments-only file | Success; installed file is empty. |
| Hex pairs of widths 0, 1, 31, 32, 33 and 64 | Success, including invalid native widths. |
| Nonhex pair with exactly 32 characters per side | Success. |
| Nonhex 31-character key | InvalidKeys; previous installed file survives. |
| Duplicate title rows | Both rows preserved in input order. |
| Whitespace and unterminated final line | Trimmed and terminated with a newline. |
| Missing or repeated delimiter | InvalidKeys; previous file survives. |

The title importer uses `length != 32 && !isHexString(...)`, which rejects only
inputs failing both checks. The native title parser independently requires
exactly 32 hex characters per side. Therefore an import-success message alone
cannot establish a valid matching native title-key entry. This finding does
not establish the contents or cause of any Owner file. Do not request keys,
repeat installation as a diagnosis, remove Tickets, or modify commercial
metadata based on it. A separate scoped change is needed to alter validation.

## Reproduction after qualification

1. Install this PR's exact qualified Debug APK and confirm its commit.
2. Enable diagnostics, note the session, and perform one Selected scan of the
   existing container. No key reinstallation or game freeze is required.
3. Read the original first-rejection message and open scan observation details
   separately. Compare the file states, strict entry counts and map probe.
4. Preview/export the diagnostic ZIP. Use its session and attempt to associate
   version, member size and public ID hints with this single scan; do not join
   unrelated ordinary app-details events as if they were scanned members.
5. Disable diagnostics; further scans must not append persistent reports.

Automated native, JNI and JVM tests are synthetic offline evidence. Real APK
execution and commercial-game acceptance must be reported separately. Earlier
PR #16 device scan/export success does not qualify this new APK automatically.
