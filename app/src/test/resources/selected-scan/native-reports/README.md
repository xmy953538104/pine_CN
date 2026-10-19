# Actual native AES safe report fixtures

These eight files are exact safe reports captured by
`tools/run-selected-scan-observed.py` from the production NCA/KeyStore/AES path
with pinned mbedTLS and independent OpenSSL-generated content-free synthetic
inputs. They are a development capture in
`/tmp/pine-scan-observed-first-aes/native` from production report sources later
committed as `6083527f`. That run started at `0f6cbab8`; commits made during the
run intentionally caused its final source-binding check to fail. These bytes
are not qualification of a clean `6083527f` run. Final clean-head qualification
is separate CI evidence. `sha256.json` records the exact captured bytes;
timings are observations from that run, not constants of the format.

The native harness performs actual scan and Catalog serialization. Its first
three preparation phases are simulated harness observations; these fixtures
do not establish Android JNI preparation timings. The separate real host JNI
lane covers that bridge. Raw Catalog and envelopes were not persisted here.
No commercial content, private keys, owner saves, guest execution or Android
installation is represented. `valid-meta` is a readable CNMT with no Program,
not a runnable title. Digest mismatch remains an observation, not a corruption
or authentication conclusion.
