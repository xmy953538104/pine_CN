# Bounded auxiliary structure experiment

This is an offline Python prototype over public synthetic bytes. It has no
production consumer, file-import CLI, key-import API or Android entry point.
Every result, including recognition, has `authentication=not_verified` and
`runtime_admissible=false`.

**Structure cannot authenticate a Ticket, detect arbitrary corruption in opaque
fields, or distinguish a genuine Ticket from a deliberately constructed mimic.**
The tests demonstrate this limitation by changing opaque signature and body
bytes while retaining an untrusted recognition result. Recognition does not
justify a production auxiliary exemption, removing `otherEntries`, suppressing
a missing-key failure or changing selection policy.

## Fixed source evidence and limits of that evidence

The only Ticket layout source used is Pine at
`b25e9e411dde6921b4fb1fda3c3cb7842930c2f3`:

| Source | Fact used | SHA-256 of local source bytes |
| --- | --- | --- |
| [`vfs/ticket.cpp`, lines 10–40](https://github.com/xmy953538104/pine_CN/blob/b25e9e411dde6921b4fb1fda3c3cb7842930c2f3/app/src/main/cpp/skyline/vfs/ticket.cpp#L10-L40) | Six signature declarations and the corresponding body offsets | `bc85aabe06a469a37f4be20a5057ac33c00d492cdee32c0b7a97e0d8cd8de93b` |
| [`vfs/ticket.h`, lines 14–37](https://github.com/xmy953538104/pine_CN/blob/b25e9e411dde6921b4fb1fda3c3cb7842930c2f3/app/src/main/cpp/skyline/vfs/ticket.h#L14-L37) | Body size `0x180`, issuer extent `0x40`, Common/Personal declarations and their position in the body | `46ec65361220bb14307cfcd5ce5c7f9295ae2b9bc3eb5fb529522ecb8a915e5f` |

The existing Pine reader selects an offset and reads that fixed body. Those
files do not establish signature validation, exact file-end validation,
certificate structure, an issuer grammar, optional records or complete modern
Ticket-format validation. This prototype does not claim those properties.
No hactool Ticket/certificate layout or other external format was used to fill
the gaps. Restricted external lookup actions were stopped; no alternate fetch
was used. Certificate classification remains explicitly unsupported because
fixed primary evidence was not established within the permitted source scope.

The C++ Ticket reader is not compiled or invoked by this experiment. The tests
exercise the actual new Python classifier against independently written literal
fixture layouts. They are not native Ticket, cryptographic or runtime tests.

## Deliberately narrow local profile

`experiments/selected_compat/auxiliary.py` reads the signature declaration as
little endian, matching the target layout used by this synthetic profile.

| Signature declaration | Body offset | Required total extent |
| --- | --- | --- |
| `0x010000`, `0x010003` | `0x240` | `0x3c0` |
| `0x010001`, `0x010004` | `0x140` | `0x2c0` |
| `0x010002`, `0x010005` | `0x80` | `0x200` |

Only the exact fixed extent is eligible. Trailers, truncation and an unknown
signature declaration cannot produce recognition. The body type at `+0x141`
must be Common (`0`); Personal (`1`) is unsupported, and other declarations are
rejected. The issuer's 64-byte array must contain 1–63 printable ASCII bytes,
a NUL and only zero padding afterward. This issuer restriction and the exact
file end are conservative local profile choices, not normative validation
inferred from Pine. They can reject otherwise valid files outside this profile.

Signature bytes, the title-key block, all identifiers, revision and padding
fields remain opaque. They are not extracted, hashed, retained in results,
authenticated or imported. In particular, fields named as padding by the pinned
Pine source are not reinterpreted using unpinned format assumptions. Arbitrary
damage to these opaque fields is undetectable here and can remain recognized.

All fixtures are generated in memory from explicit public constants and zero
opaque bodies. A repeated public byte pattern stands in for a signature; no
fixture is signed or usable as a credential. Privacy tests insert public canary
strings into opaque regions solely to check that reports omit them.

## API, budgets and output

The API is `classify(reader, budget, hint=Hint.NONE) -> Result`. A Reader has only
`size()` and `read(offset, count)`; it has no filename, path or key API. The caller
must retain one `Budget` for the entire batch. `Limits` can only lower these
immutable hard caps:

| Limit | Hard cap |
| --- | --- |
| One member's declared extent | 960 bytes (`0x3c0`) |
| Member attempts | 32 |
| Aggregate requested read bytes | 32 KiB |
| Read callback attempts | 32 |

A valid attempt charges the member before invoking `size()`. An eligible
member's entire declared extent and a read attempt are reserved before a single
`read(0, declared_size)`, including reads that fail or return too few bytes.
There is no refund or reset API. Budget snapshots and limit views are copies.
Invalid hints still consume a member attempt. An invalid Budget cannot invoke
the reader. Failed global count, aggregate or read limits are sticky: later
attempts invoke no reader callback. A per-member oversize declaration is rejected
before reading; it consumes the member attempt but does not reserve a forbidden
read. In a successful batch, 32 maximum-sized members request 30,720 bytes,
which is below the aggregate hard cap; callers can lower the aggregate cap.

Each attempted member makes at most two size callbacks and one read callback.
Short reads, oversized/non-byte returns, callback exceptions, invalid sizes or
a changed second size reject without retries. The size recheck is not an atomic
snapshot, TOCTOU defense or byte-identity proof: same-size mutation can go
undetected, as an explicit test demonstrates. Limits bound classifier requests,
not execution time or allocations inside a supplied Reader. Python callers
that deliberately rewrite private object state are outside this API contract.

Hints are only `none`, `ticket`, `certificate`, `nca` and `other`, passed as enum
values. No extension is an allowlist. A Ticket layout with a conflicting hint
rejects. A certificate hint without a Ticket conflict remains unsupported.
An NCA hint rejects without opening the reader. Visible `NCA0`–`NCA3` sentinels
at offset `0` or `0x200` reject with `routing=nca_verifier_required`; routing is
only a fixed result, not an invocation or success of any verifier. A complete
ordinary NCA header is outside the 960-byte profile. Encrypted NCA detection is
not performed, and a crafted small mimic can only receive untrusted, non-admitting
recognition. There is no production route from this result.

Classification strings are `recognized_untrusted_ticket_structure`, `unknown`,
`rejected` and `unsupported`. None grants eligibility. Result codes, hints,
profile and routing are explicit enums. A safe report is at most 1 KiB and has
only these fields:

```json
{
  "schema_version": 1,
  "classification": "recognized_untrusted_ticket_structure",
  "code": "ticket_structure",
  "hint": "ticket",
  "profile": "pine_b25_common_ticket_fixed_extent",
  "routing": "none",
  "authentication": "not_verified",
  "runtime_admissible": false,
  "member_ordinal": 0,
  "declared_size": 704,
  "read_bytes": 704,
  "read_complete": true
}
```

An ordinal is `0..31` or null. A reported member/read size is `0..960` or null;
oversized declarations and oversized returned counts are not echoed. Budget
counts stay within their hard caps. `read_complete` means only that the one
requested read returned exactly its length and the two sizes agreed. It is not
a validation, authentication or source-identity status. Raw names, paths,
issuer/body bytes, private identifiers, hashes and exception text are absent
from the API's safe output. `authentication` and `runtime_admissible` are fixed
by the serializer and cannot be supplied as result parameters.

## Reproduction and remaining blockers

Use Python 3.11 or 3.12 from this worktree:

```sh
python3 -m unittest discover -s experiments/selected_compat -p test_auxiliary.py -v
python3 tools/run-auxiliary-experiment.py --evidence-dir /tmp/pine-auxiliary-evidence
```

The second command requires the final committed, clean tree and a new empty
evidence directory. Its baseline guard freezes every b25 tracked file before
and after testing, separately from experiment input hashes. No existing b25
tracked file is changed by this prototype. The tests cover all six literal
layouts, opaque-mutation limits, malformed extents/type/issuer, hints and NCA
sentinels, fake-reader short reads/exceptions/size changes, count/read/aggregate
boundaries and error charging, hard-cap configuration, bounded reports and
privacy canaries. Runner evidence records exact counts rather than treating
individual parameter combinations as separate test methods.

Certificate parsing, signature/authenticity checks, complete Ticket-format
validation, actual stored keys, existing NCA verification, native loading,
guest/device behavior and APK execution are untested and unimplemented here.
The existing missing matching-key failure is untouched; this module has no
key-match input or credential operation that could alter it. Opaque corruption
and malicious mimics remain a blocker to a blanket production exemption.
See [the parent integration conditions](selected-compat-integration-conditions.md)
for the independent unfinished components and gates. No CNMT policy component
is implemented or substituted by this experiment.
