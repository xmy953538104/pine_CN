# Selected compatibility experiment: integration conditions

This branch starts at `b25e9e411dde6921b4fb1fda3c3cb7842930c2f3`
([PR #17](https://github.com/xmy953538104/pine_CN/pull/17)). It adds a
synthetic Python auxiliary-structure experiment, a qualification runner and
host CI. It does not change any existing b25 tracked file, production selection
policy, normal Loader, KeyStore, BKTR or watchdog. No new APK is requested.

## Evidence boundaries

The parent task supplied a verified summary of three b25 device scans: the two
key files were present, their reads completed, loading returned, counts were
not partial, and the inspected Base Program had no matching entry in the
already loaded title-key map. The first UI rejection was `catalog_incomplete`;
a CNMT trailer mismatch was another observation. Those statements came from
the parent task's device evidence, not from this experiment. The two original
JSON files were not supplied to this worktree and were not independently read
here. This branch does not establish which input member caused every part of
catalog incompleteness. It does not attribute a missing map match to an import
failure or establish any normal-runtime key source. No new game launch was
reported for those scans.

The source audit below is independent of those device observations and is
reproducible against the fixed b25 files. Line numbers refer to that baseline.

| Existing stage | Source | Actual boundary |
| --- | --- | --- |
| Observed scan key loading | `selected_scan_jni.cpp`, `SelectedKeyStoreFactory::Load` | Uses the fixed existing private key directory and reports existing parser/map outcomes. No container Ticket import occurs in this entry. |
| Loaded-map probe | `catalog/native_adapter.cpp`, `InspectionValidator::ValidateHeader` | Tests membership using the decoded header transiently. It exports only a fixed lookup outcome, not the lookup identifier or any key. A match alone proves no decryption or authorization. |
| Member enumeration | `catalog/catalog.cpp`, `Scan`, lines 260–302 | A `.nca` hint, large enough extent or visible NCA header causes NCA inspection. Other entries remain unclassified. A filename suffix does not establish a harmless auxiliary file. |
| Metadata completeness | `Catalog::AllMetadataEnumerated`, line 498 | Any unclassified outer entry, unknown NCA type, unreadable Meta content or unparsed group prevents completeness. Work-budget completeness is a separate field. |
| First UI decision | `loader/SelectedContentCatalog.kt`, lines 46 and 244 | Completeness combines bounded work, metadata enumeration and group/member conditions. It short-circuits to `catalog_incomplete` before the per-group digest predicate. A trailer observation alone does not identify the first predicate's cause. |
| Native admission | `loader/selected_content.cpp`, `ValidateAndCreateSelectedContent` | Rescans the exact source with full raw-NCA hashes, requires bounded work and complete metadata, and checks references and identities again. The current per-group trailer check also remains unchanged. |
| Self-contained Program | `selected_content.cpp`, `Open` and `make` | Requires independently complete ExeFS/RomFS, NPDM identity, Control/NACP and source bindings; declared Patch role is retained. This is not a conversion to Application. |
| AOC association | `selected_content.cpp`, lines 215–225 | Requires the true AddOnContent parent, unique Data ID, PublicData type/header identity and agreement with the NACP/service Data ID mapping. |
| Missing map entry | `vfs/nca.cpp`, `NCA::GetTitleKey`, line 213 | Continues to raise the existing missing-entry failure. No fallback key, generated entry, credential import or bypass is provided by this branch. |

Ordinary `NspLoader` has its own `ExtractTickets` call before NCA enumeration
(`loader/nsp.cpp`, lines 13–28). This read-only observation explains why a
Selected scan cannot establish the ordinary runtime's complete key provenance.
It is not a proposal to copy that operation into the experiment or modify the
ordinary path. No credential values or container inputs are accepted here.

## Isolated auxiliary experiment

See [the supported structural profile and explicit limitations](selected-auxiliary-classification.md).
The prototype has no production consumer. Every result has
`authentication=not_verified` and `runtime_admissible=false`. Recognition of a
bounded, Pine-compatible layout does not certify a genuine credential,
detect arbitrary opaque-body corruption, authorize content or justify removing
an entry from `otherEntries`. Certificate structures remain unsupported.

The shared budget accounts for attempted members and each permitted read's
requested extent, including failed reads. Oversized declarations reject before
reading. Reads have hard bounds, short reads and exceptions reject, and an unknown,
conflicting or unsupported member cannot produce runtime eligibility. Synthetic
data is built in memory; this experiment has no file-import or key-import CLI.
Its report excludes raw bodies, private identifiers, names, paths and exception
messages.

## CNMT policy component is blocked

The delegated CNMT research/policy-model turn was stopped by the platform's
agent security screening with: "This content was flagged for possible
cybersecurity risk." No rephrasing, alternate agent or alternate tool is used
to perform that stopped component. There is no new CNMT policy model or
independent implementation comparison in this branch.

Before that screening, one public
[pinned CNMT format page](https://switchbrew.org/w/index.php?oldid=13421&title=CNMT#Digest)
was read. Its development/production trailer note is a format observation. It
does not authenticate the Owner's data or validate a production policy change.
The requested independent evidence and synthetic policy matrix are unfinished;
no general mismatch exception has been implemented.

## Conditions before any production proposal can be accepted

The current result supports review of a bounded offline structural classifier
only. Production Selected integration is not ready. A later, separately
reviewable proposal would still need all of the following:

1. Resolve the blocked CNMT component through a permitted process. Establish
   policy evidence and tests that keep structural/reference completeness,
   actual size/hash checks, trailer observations and authenticity separate.
   Neither the pinned note nor this branch authorizes ignoring mismatches.
2. Independently establish a sufficiently complete auxiliary classification
   and authentication boundary. Unknown, malformed, oversized, disguised and
   certificate members cannot become harmless merely from extensions or this
   prototype's recognition result. The opaque-field limitation remains a
   blocker to any blanket exemption.
3. Keep the current missing matching title entry failure. Existing map counts
   do not prove a target match; this work does not request, generate, replace
   or repair Owner credentials. A legal usable input must satisfy the existing
   mechanism before a content-opening test can succeed.
4. Preserve explicit single-container selection and true Application/Patch/AOC
   identities; verify complete CTR-only content, every selected reference,
   parent/Program/Data ID, hash/source binding, and TOCTOU recheck. Unverified
   BKTR, sparse, compressed or base-dependent content stays unsupported.
5. Run the real native/JNI/JVM/service suites for any proposed app change,
   preserve ordinary-path behavior and the Selected OFF gate, then qualify a
   new exact-commit Debug APK and actual device behavior. None of those runtime
   results can be inferred from the Python experiment.

The next production change must be small enough to review against these
conditions and must not reinterpret this experiment as permission to launch.

## Reproducing this branch's host evidence

Use Python 3.11 or 3.12 and Git. No Android SDK, keys, tickets, ROMs, third-party
Python dependencies or emulator are needed. Start from this branch's exact
commit and use a new, empty directory outside the checkout:

```sh
python3 tools/tests/test-auxiliary-experiment-runner.py
python3 tools/run-auxiliary-experiment.py --evidence-dir /tmp/pine-auxiliary-evidence
```

`manifest.json` records the tested commit, experiment input SHA-256 values,
every b25 tracked blob's actual SHA-256, Gitlink identities, test counts and
artifact hashes. The runner compares baseline Git blob identities and modes
before and after tests, requires a clean source tree and rejects a changed HEAD.
Submodule checkouts are not exercised; their recorded
Gitlinks are not evidence of native dependency execution. Source drift, a
production-source addition, a missing/empty suite, any skipped or expected-failure
test, unexpected success, failure or exception cannot yield a passing
qualification. Tests execute in a fresh isolated interpreter with an empty
bytecode-cache prefix so same-size/same-mtime stale caches, including helper
caches, cannot substitute old execution for the recorded source. The experiment
tree rejects linked files/directories and binds every non-cache input to a
regular HEAD Git blob, so ignored helpers cannot borrow a clean Git status or
escape the recorded commit. Only conventional tagged CPython cache files are
excluded; ordinary sourceless bytecode helpers are rejected, including inside
`__pycache__`. This checks the declared experiment input tree;
it is not a sandbox for arbitrary hostile Python test code. The CI matrix runs
these same commands on Python 3.11 and 3.12 and uploads host evidence only.
