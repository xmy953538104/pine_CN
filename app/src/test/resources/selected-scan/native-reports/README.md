# Actual native AES safe report fixtures

These eight files are exact safe reports captured by
`tools/tests/test-selected-scan.py` from the production NCA/KeyStore/AES path
with pinned mbedTLS and independent OpenSSL-generated content-free synthetic
inputs. They were captured in `/tmp/pine-scan-observed-first-aes/native` during
integration development after native production commit `6083527f`.
`sha256.json` records the exact captured bytes; timings are observations from
that run, not constants of the format.

The native harness performs actual scan and Catalog serialization. Its first
three preparation phases are simulated harness observations; these fixtures
do not establish Android JNI preparation timings. The separate real host JNI
lane covers that bridge. Raw Catalog and envelopes were not persisted here.
No commercial content, private keys, owner saves, guest execution or Android
installation is represented. `valid-meta` is a readable CNMT with no Program,
not a runnable title. Digest mismatch remains an observation, not a corruption
or authentication conclusion.
