# Explicit NSP selection UI, version 1

This entry is disabled by default. A debug build explicitly configured with
`-PpineSelectedContent=true` enables the Kotlin flag and native selected entry.
The NSP app dialog shows **Choose NSP content** only when both the build flag
and native `isAvailable()` are true. Ordinary Play keeps its existing route.

The earlier [PR 11](https://github.com/xmy953538104/pine_CN/pull/11), frozen at
[`a901a470`](https://github.com/xmy953538104/pine_CN/tree/a901a470feddb55f6ff2e847931707b7aaa46c81),
provided an internal native entry with no UI/JNI integration. Its
[native profile](https://github.com/xmy953538104/pine_CN/blob/a901a470feddb55f6ff2e847931707b7aaa46c81/docs/selected-content-v1.md)
is the bounded backend consumed by this later entry; this UI does not expand
its supported storage or execution profile.

## Explicit choices and preview limits

The user chooses `modern-40` or `legacy-48` before scanning. The CNMT title
version and filenames do not select a wire layout. The chooser then requires
an explicit Application or standalone rebuilt Patch group and exact Program
(Content ID, CNMT index and NCA Program ID). An Application can separately
choose a matching complete CTR Update or **No separate Update**. Standalone
Patch mode forbids a separate Update. Each AOC group is a separate unchecked
choice; continuing without AOCs is explicit. No first, highest-version or
Title-ID-mask choice is made.

The preview calls production native Catalog with `verifyHashes=false`. It
does not authenticate signatures, verify whole-NCA hashes, validate the guest
executable, or prove that a game will run. Filename-hint resolutions remain
unverified observations. Arbitrarily renamed members may remain unresolved
until hashing and are unavailable in this preview; the UI does not guess a
replacement association. Incomplete/unreadable metadata, ambiguous references,
unknown identities and known unsupported content disable selection. BKTR and
external-Base forms are disabled with a reason. Native launch preparation
rescans and validates the selected raw bytes with full hashes before installing
the bounded supported profile. Ticket-bearing containers, unsupported layouts
and arbitrary DLC compatibility are not claimed.

The preview is limited to 4 MiB of JSON, 512 groups, 2048 NCAs, 256 entries in
each group table and 8192 total references. Native scan/output budgets apply
independently. All group IDs and Program fields passed to native come from the
selected Catalog rows; the review screen shows those exact IDs and indices.

## Intent and file ownership

`emu.skyline.SELECTED_CONTENT_V1` carries strict JSON shaped as
`[stringFields, programIndices]`. Field zero is version `"1"`; the remaining
fields follow the native selected wire contract:

| Index | String field |
| --- | --- |
| 1 | `application` or `standalone_patch` |
| 2 | `modern-40` or `legacy-48` |
| 3 | explicit Application ID, 16 hex digits |
| 4–6 | primary `cnmt-sha256:` group, Content ID, Program ID |
| 7–9 | separate Update group, Content ID, Program ID; all empty when absent |
| 10 onward | distinct AOC group IDs, at most 32 |

The integer array has exactly two elements: primary index 0–255 and Update
index 0–255, or `-1` when absent. Content/Program/Application IDs must be
nonzero, roles are disjoint, and standalone mode forbids an Update. The codec
checks types, lengths, enums, nesting, group duplication and partial Updates
before any launch FD opens. Present malformed or unavailable selection never
falls back to ordinary Play. A changed selection for the same URI is rechecked.

Preview opens one caller-owned `ParcelFileDescriptor` in `.use`, borrows its
FD for native inspection, and closes it on return or failure. Native owns a
duplicate during inspection. Scanning runs off the UI thread. Cancellation or
view destruction suppresses later dialogs and launch callbacks; it does not
interrupt synchronous native I/O. The descriptor stays owned until that call
returns, then closes. No native result is published after cancellation.

The selected launch opens only the one base NSP. Saved ordinary Update/DLC
associations are not consulted. Kotlin detaches that single FD into the common
native lifecycle, which owns it through return/failure. The selected JNI and
native preparation independently reject malformed choices and changed sources.

An old library `AppItem` can describe another Application inside the same NSP.
It is used only to check the URI and NSP format. Selected launch builds a fresh
validation-pending placeholder without old name, version, author, save owner,
Update or DLC association. Settings use the explicit Application ID, including
the in-game settings and GPU driver preference chain. That identity is user
intent until native validation succeeds; it is not an authenticated title or a
save owner. No association is saved by this chooser.
The selected settings key is the normalized lowercase CNMT Application ID;
ordinary entries can use their NACP save-owner ID or URI key instead. This
entry does not guess that identity, migrate or silently reuse those preferences.

## JVM evidence and limits

The resource `app/src/test/resources/selected-content/native-metadata-preview.json`
is an exact production `ScanNative` metadata-only result over content-free,
public-key synthetic AES/CTR inputs. Its SHA-256 is
`3aefa8e5c4c16f70af5aa05c081bb6c6965d38e4f692c767c8cc1bd67a9068d7`.
The generator source is `tests/selected/selected_content_test.cpp`. This fixture
uses its mutable Source, not an Android provider FD. JVM tests consume the
actual unverified fields, require explicit selections, reject unsupported and
unresolved rows, and exercise strict wire validation and selected-only dispatch.

Robolectric tests exercise the actual ContentResolver/PFD open/use/close chain,
Activity rejection before opening, settings identity propagation, and the
single-base-FD plan. One test-class shadow substitutes only the FD integer
passed to its injected preview callback because Robolectric 4.13's `getFd`
reflects into JDK 17 internals. It keeps normal shadow open/use/close behavior;
it does not test a kernel FD or call Android JNI. Native POSIX/JNI ownership,
actual AES validation, and NDK syntax checks have separate native tests.
Picker tests additionally substitute the native preview result to hold a
synchronous inspection in flight. They verify profile choice has no default,
cancel/view destruction cannot publish a late result or launch, and the PFD
remains open until that inspection returns. They do not call Android JNI.

Run the host JVM suite, without an APK or native Android build:

```sh
source /workspace/android-build/env.sh
SIGNING_STORE_PATH=unused.jks IS_BUILD_SIGNED=false ./gradlew --no-daemon --offline \
  -I /workspace/android-build/local-dependencies.init.gradle \
  -PpineNativeDebugOptimization=2 testFullDebugUnitTest
```

Repeat with `-PpineSelectedContent=true` to compile and test the opt-in flag.
These are host synthetic checks. No device, owner ROM, private save, guest
execution or gameplay success is established by the UI tests.
