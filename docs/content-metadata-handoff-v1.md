# Minimal content metadata INPUT v1

This is a small input contract for the Don't Starve game line to hand Pine
already-authorized **metadata observations**. It is independent of the much larger
Pine Catalog output. No container/NCA/ROM bytes, keys, tickets, saves, credentials,
filenames, host paths, URIs, notes or logs belong in it. Unknown facts may be omitted.

Start with [unknown-template.json](../tools/content-handoff/unknown-template.json),
use the [schema](../tools/content-handoff/metadata-v1.schema.json), and consult the
entirely invented [synthetic example](../tools/content-handoff/example.json).
`data_kind: synthetic_example` marks invented values, including invented hashes.
The unknown template invents no metadata: `container-1` is only a local report label.

```sh
python3 -m pip install 'jsonschema==4.25.1'
python3 tools/validate-content-handoff.py tools/content-handoff/unknown-template.json
python3 tools/validate-content-handoff.py /local/authorized-metadata.json > /local/validation.json
python3 tools/tests/test-content-handoff.py
```

Use Python 3.10+ on a POSIX host with `O_NOFOLLOW`; Linux is tested. The validator
opens only the explicit input and its bundled trusted schema, read-only and
nonblocking, rejects final symlinks/nonregular files, caps each JSON file at 1 MiB,
rejects duplicate JSON keys/invalid UTF-8/nonfinite numbers, and never opens any
metadata-specified path or network resource. Caller-selected parent directories
are trusted. Missing `jsonschema` is an error, never a skipped green check.

Exit `0`: schema and supplied links have no known contradiction, even when facts
are unknown. Exit `1`: known supplied facts/links contradict one another. Exit `2`:
invalid/unreadable/oversized input or missing dependency. All errors use fixed
codes; no untrusted values, paths or library validation messages are echoed.

| Field | Required / unknown rules |
| --- | --- |
| Root | `schema`, `data_kind`, `container`, `observations`, `ncas`, `cnmts` required. One report covers one container. |
| `report_id`, `container_report_id` | Local structural labels, not filenames or cryptographic IDs. Every NCA/CNMT/content record binds to the root container ID. Labels are at most 64 restricted characters. |
| Every fact | Optional. When supplied: `{state, value, observation_id}`. `observed`/`reported` require a nonnull typed value and observation ID. `unknown` requires null value and omitted/null observation ID. Never use zero, an empty string or a guessed ID for unknown. |
| Observations | Bounded IDs plus subject/container, scope, method, claimed state, source tool and revision. Unknown tool/revision are null. An observed fact needs an identified source tool. Each fact binds to the same subject/container and scope. |
| Container | Optional whole raw-container `raw_sha256`, `raw_size`, `enumeration`, `unreadable_metadata`, `runtime_report`. Enumeration is `complete`/`partial`, or unknown. Empty arrays mean nothing listed; they do not prove completeness. |
| NCA | Optional discovery/readability, actual `nca_type`/`program_id`, raw size/full raw SHA, filename Content ID hint, `requires_base`, `section_crypto`, ExeFS/RomFS presence. A filename hint stays unverified and never resolves a link. |
| CNMT | Optional readability, source Meta NCA report link, role/title/version, explicit application parent and optional parent report link, Application `patch_id`, DataPatch `data_id`; optional raw storage/install/committed bytes and decoded committed boolean. |
| Content references | Optional explicit NCA report link, actual CNMT content type/Content ID/expected full raw-NCA SHA/size, six raw size bytes, `legacy-48`/`modern-40` profile, modern attributes, actual `id_offset`, supplied Program index and `cnmt_id_offset` basis. Missing/unreadable links use an unknown fact with null value. |

`observed` and `reported` describe the **sender's claim**, never receiver
authentication. Observation scope is `raw_container`, `raw_nca`, `cnmt`,
`filename_hint` or `runtime`. An observed raw hash must identify
`method: full_raw_sha256` at the correct raw-container/raw-NCA scope; a partial,
decrypted-section or CNMT digest is not that measurement. Full container SHA/size
are optional identity evidence; without them the report ID binds records only.

Raw NCA type codes are Program=0, Meta=1, Control=2, Data=4, PublicData=5.
CNMT content codes are Meta=0, Program=1, Data=2, Control=3, DeltaFragment=6.
These spaces differ. Data can refer to an actual Data or PublicData NCA; neither
proves registration or runtime support. Unknown enum codes can be retained as
byte facts; unsupported CNMT roles stay unknown rather than receiving a guess.

Each CNMT links its source Meta NCA. Each content reference names an NCA report
ID or remains unknown; no hash prefix, filename, ID mask, parent arithmetic,
first/highest candidate or content order chooses a relationship. CNMT Content ID
need not equal a SHA prefix. Application `patch_id` is not its parent. Parent
title/role contradictions are checked only when both supplied values are known.
Known Application/system roles cannot carry an application-parent title or report
link. Known self-parent links and cycles between existing CNMT report IDs are
contradictions even when roles/titles are unknown. Missing/unknown links create no
graph edges and remain unknown.

`legacy-48` decodes all six size bytes little-endian; `modern-40` decodes five
size bytes plus one attributes byte. With an unknown profile the raw six bytes
remain usable evidence and no size is guessed. A supplied Program index is only
compared with `id_offset` when the explicit `cnmt_id_offset` basis is supplied.
Multiple Programs remain ambiguous; automatic selection is never performed.

Keep discovery, readability, reference consistency, declared measured/reported
hashes, dependency and runtime reports separate. A `complete` enumeration claim
with unknown/unreadable/unlisted Meta metadata is a contradiction; use partial or
unknown instead. CTR, ExeFS presence, matching report hashes, or `requires_base:
false` never establishes a complete or runnable Program. BKTR/base dependency,
unsupported and unknown states remain explicit; no reconstruction is attempted.

The validator reads metadata JSON only. Its result always says
`schema_and_reported_links_only`, `provenance: not_authenticated`, zero container/
NCA bytes read, signatures/runtime unverified. Hash/size comparisons compare
supplied values; they do not compute a Catalog's full-content verification or
authenticate a package. A `runtime_report` records a claim and never becomes
verified launch/AOC success. The synthetic example does not simulate a game run.

Ordinary encrypted NSPs are outside the plaintext host CLI's complete-inspection
support. Supply observations from an already-authorized legal metadata workflow
with already-provided keys, or leave unavailable fields unknown. Do not run a
plaintext-only CLI expecting a complete encrypted-package catalog or send keys.

Limits: 512 NCAs, 256 CNMTs, 4,096 observations, 1,024 references per CNMT and
2,048 references across the report. All objects reject unexpected properties.
CI validates the schema, synthetic example, unknown template and semantic tests;
only those noncommercial inputs/reports/logs are artifact candidates.

Source boundaries: [Owner-tested 79a CNMT types](https://github.com/xmy953538104/pine_CN/blob/79a4d1a2250585ec64233befab934f0e5ed27ab7/app/src/main/cpp/skyline/vfs/cnmt.h)
and the separate P0 catalog's `docs/nsp-catalog-v1.md`/format-source notes. P0
Catalog JSON is an output format; this contract does not require its runtime,
section, parser or verification objects and changes no P0/P1 behavior.
