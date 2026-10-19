# Native v2 report regression bytes

These are exact safe report bytes captured from the real AES/native factory and
ON JNI development run in `/tmp/pine-key-state-first-aes/host-jvm`, using
`tools/run-selected-scan-observed.py`. The host JVM ran with `-Xcheck:jni`.
The four files cover a parsed CNMT, a missing Meta key area, a missing header key,
and a failed key load. They do not contain a raw Catalog or envelope.

The source parent was `a70ad001464434d14e9bdea653790af0255c07be`, with uncommitted
v2 production changes. `capture-provenance.json` pins the relevant source input
hashes from that development manifest. Its 8 native, 4 model and 16 JNI assertions
passed, but the overall manifest intentionally failed the changing-worktree
qualification guard. These parser regression bytes are **not** clean-build,
APK, device, commercial-content, signature or guest-execution evidence. The
final clean qualification is recorded separately by the exact committed head.

`sha256.json` pins each unedited report. The original eight v1 report resources
in the sibling `native-reports` directory remain unchanged and are still tested.
Constructed boundary cases in `SelectedKeyStateTest` are labeled separately.
