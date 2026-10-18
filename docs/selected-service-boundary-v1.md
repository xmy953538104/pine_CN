# Selected content: positive real IPC service boundary

This host test connects the opt-in selected native objects to the existing AOC
and filesystem service methods through actual IPC request bytes and responses.
It does not change any production implementation or the default loader path.
All inputs are generated public synthetic bytes. No game, save, private key,
Android package, installation, guest execution or device is involved. All BKTR
profiles remain rejected by the selected factory.

Run with Clang, OpenSSL development headers/library and the exact repository
submodule pins:

```sh
git submodule update --init --depth 1 app/libraries/boost app/libraries/fmt app/libraries/frozen app/libraries/xxhash app/libraries/mbedtls app/libraries/lz4
git -C app/libraries/boost submodule update --init --depth 1 libs/assert libs/config libs/container libs/intrusive libs/move libs/static_assert
CXX=clang++ CC=clang python3 tools/run-selected-service-boundary.py --sanitize --evidence-dir /tmp/pine-selected-service-evidence
```

The evidence directory must be empty. `--deps-root /path/to/app/libraries` may
reuse clean local copies only at exactly the parent and nested gitlink pins.
Missing dependencies fail; no test is silently skipped. The dedicated workflow
runs the positive suite with ASan/UBSan and read-only permissions, without APK
build steps. The runner stays outside the default fast test glob because these
real dependencies are mandatory.

Leak checking remains enabled. A local unsuppressed positive run observed six
32,816-byte directory allocations leaked after the six unmatched DataID calls.
`OsFileSystem::OpenDirectoryImpl` calls `opendir` to check the directory and
returns a new OsFileSystemDirectory without closing or transferring that DIR
pointer. This existing implementation is unchanged. Its unsuppressed failure is
retained as `tests/selected-services/known-failures/opendir-lsan.txt`, not counted
as a passing test.

The positive ASan/UBSan/LSan lane uses slow stack unwinding plus llvm-symbolizer
and one LSan suppression for the exact qualified OpenDirectoryImpl function.
The runner guards the entire production source SHA-256 and exact method-body
SHA-256 against the a901 baseline before enabling that suppression; a source
change invalidates it. No libc/opendir wildcard is used. The suppression file,
source guard and actual suppression summary are retained. Each diagnostics-off
and diagnostics-on process must report precisely six 32,816-byte allocations
from this function, totaling exactly 196,896 bytes. The fixed expected JSON
records this observed Linux x86_64/glibc allocation profile; a different size
fails rather than inheriting approval from the function name. A stdlib-only
regression suite calls the actual runner parser and rejects missing, duplicated,
misplaced or extra table rows and incorrect counts/bytes, including 6×40,000.
The runner requires one complete LSan table containing only that exact rule.
Other leak stacks remain failures. This is
not a leak-clean claim. `--sanitize --unsuppressed-leaks` removes the suppression
and is expected to fail on that known legacy leak, never treated as a pass.

`CXXLD` may explicitly select another host link driver when an existing Clang
distribution has no Linux sanitizer runtime. For example, `CXXLD=c++` links the
Clang-instrumented host objects with the installed GNU sanitizer runtimes. The
manifest records both drivers; the dedicated Ubuntu lane uses native Clang for
both compilation and linking. Android sanitizer runtimes are never used for
the host executable. Sanitizer runs require llvm-symbolizer; an explicit
`ASAN_SYMBOLIZER_PATH` may select its existing local binary.

The client writes bounded HIPC/CMIF bytes independently, including full literal
Count request/response goldens and a fixed Type-C request template with only the
six dynamic address bytes patched from a real allocated output buffer. These
are hand-derived layout fixtures checked against the primary builders, not
claimed output from executing a libnx producer. The
request layout follows the independently checked full builders in pinned libnx
[`cmif.h`](https://github.com/switchbrew/libnx/blob/20cbcfd90567f7bf84df7725f27e345e2164fa6b/nx/include/switch/sf/cmif.h#L89-L148)
and [`hipc.h`](https://github.com/switchbrew/libnx/blob/20cbcfd90567f7bf84df7725f27e345e2164fa6b/nx/include/switch/sf/hipc.h#L149-L245).
`wire-profile.expected.json` records that commit, complete source-file hashes,
verified function ranges/hashes and exact fixture layout expectations. No libnx
source is substituted for Pine's production decoder.

For these non-domain, no-PID/no-handle requests, the raw word count includes
16 bytes of combined front/rear padding, a 16-byte CMIF input header and the
word-rounded argument bytes. The no-argument Count request is 40 bytes total
(8 HIPC header + 32 raw bytes); its SFCI header starts at aligned offset 16 and
its trailing bytes 32–39 are zero. The production Count response has nine raw
words and is checked across all 44 bytes, including zero rear padding 36–43.
One fixed Type-C output pointer uses receive-static mode 3 and has no output
pointer-size table. A 16-argument-byte request has 12 raw words, zero rear
padding 48–55 and its 8-byte C descriptor at offset 56, totaling 64 bytes.
All pointer values come from real host buffers and fit the 48-bit field.

Before dispatch, the actual IpcRequest decoder must preserve the aligned CMIF
header, original argument pointer and each output span's exact original address
and size. Each diagnostics mode performs 60 such request checks and 27 output
span checks, retained in `wire-layout.json`. Historical raw-size-six Count and
eight-byte-early C-descriptor copies are rejected by a test-side checker only;
those invalid copies are never passed to the production decoder or dispatched.
The same artifact retains the actual complete Count request/response hex and
the fixed 56-byte Read(offset=0,size=80) prefix before the real address. The
Read request's full 64-byte literal also checks its two size bytes and patches
only the six real-buffer address bytes.

Commits 019ed133 and e9ae0541 omitted the rear request padding and checked only
a prefix of the Count response. Their preserved manifests demonstrate that
the production implementation executed the synthetic service operations; they
do not demonstrate canonical positive request framing. The corrected profile
has version 2 and requires fresh exact-commit evidence.

It uses ordinary non-domain sessions. Actual `kernel/ipc.cpp`
decodes them; actual `BaseService::HandleRequest` and frozen `SERVICE_DECL`
tables select the methods; actual `IpcResponse::WriteResponse` writes replies.
No handwritten Pop/Push, callback dispatcher or service-algorithm substitute is
used. Each application, full-CTR-update and standalone-rebuilt-Patch fixture
runs with diagnostics disabled and enabled, checking the same results:

- Actual AOC Count=2, List=[1,2], safe pagination, and GetBase from the primary NACP.
- Actual FSP SetCurrentProcess response and exact DataID matching for both DLCs,
  each with its own true AOC CNMT and PublicData NCA.
- Actual service registration returns an IStorage move handle. Actual GetSize and
  Read return each DLC's distinct `aoc1`/`aoc2` marker from production AES-backed
  RomFS bytes, and current-process storage returns `base` or `patch` as selected.
- The standalone primary retains its true Patch CNMT. The inherited FSP
  diagnostic event still names its no-update slot `BASE_STORAGE_OPENED`; that
  label is not a claim that its metadata became Application.
- Unmatched DataID fallback executes real OsFileSystem and KeyStore code against
  newly created empty temporary system-archive and key roots, then a controlled
  fixture asset backend. An existing public marker succeeds. A missing asset
  makes the actual FileSystem::OpenFile wrapper throw normally and registers no
  storage handle. It does not demonstrate a null-storage fallback bug.
- Actual diagnostics record two matched DLC opens and sampled reads per fixture.
  `actual_bytes=80` comes from reliable production `ReadUnchecked` operation
  counts, not the historic `Backing::Read` total-size return. The wire read size
  and output size agree in these positive tests.

The existing fixture writer is reused, with independent OpenSSL AES-XTS/ECB/CTR
encryption and the actual Pine AES wrapper compiled against pinned mbedTLS
2.16.11; production pinned LZ4 is linked. The only fixture transformation changes
its KeyStore construction to the real constructor under an explicit empty test
root. No key files are populated. Signatures are not verified and the IVFC hash
tree is not verified; the selected structural profile and whole-NCA hashes still
run. This is not proof that a commercial title runs on hardware.

## Exact production boundary and substitutions

The runner stages byte-identical production sources because quote includes in
IPC and service headers otherwise bind to Android/kernel platform types. The
manifest records each original and staged SHA-256. The actual common/span.h is
retained, including its size/cast checks. Ordinary common/utils.h helpers remain
unchanged; only its Android property/ARM clock block is replaced with a host
clock, with original and generated hashes recorded.

The actual ServiceManager class declaration is retained. `RegisterService`,
`CloseSession` and `SyncRequestHandler` are bounded verbatim slices from the
original translation unit, with exact byte offsets, sizes and hashes. The global
manager constructor is replaced by state assignment only. Global time/font/NV
initialization and CreateOrGetService's unrelated factories are not compiled or
tested. Initial AOC/FSP objects are explicitly created by the test; their real
service methods register the later IStorage objects through the verbatim manager
method. Only the normal non-domain request route is executed; the included
CloseSession/domain/TIPC branches are not exercised.

Platform substitutions are DeviceState/OS fields, a host KProcess handle map,
KThread TLS pointer, KSyncObject event base, kernel scheduler/memory type
declarations, logging/trace/stack unwinding, and the clock. Actual KSession,
KObject and KEvent declarations and actual filesystem/KeyStore implementations
are used. `NcaLoader::LoadExeFs` is an explicit throwing stub and must remain
unexecuted; OS execution tail, guest mapping/execution, service-manager global
initialization, kernel handle ownership/transfer, scheduler/TLS isolation and
full Android service acquisition are not tested. A successful host move-handle
response is not a kernel transfer proof.

`manifest.json` records exact source commit/status before and after, all input
hashes, dependency pins, full C/C++ commands, binary hash, artifact hashes,
scenario count and diagnostic counts. `source-staging.json`, generated manager
slices, generated platform utils and generated synthetic fixtures make the
bounded source transforms inspectable. Each diagnostics-on/off fixture writes
the same result JSON and a scenario log; diagnostics-on also retains real JSONL
events. Evidence is local/CI artifacts only, with no upload destination added by
the test runner.

## Remaining limits

Only bounded positive wire inputs and the normal missing-fixture exception are
executed. AOC offsets past the installed count, IStorage short reads or unequal
wire/output sizes, null storage, malformed IPC, domain/TIPC behavior, real system
archive contents and concurrent kernel use are unexecuted legacy cases. This
suite neither fixes nor validates those paths. In particular, source inspection
of ListAddOnContent shows its out-of-range offset is unchecked; no unsafe-offset
probe is included here. No default implementation is changed to make tests pass.
