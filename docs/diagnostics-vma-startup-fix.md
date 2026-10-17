# Debug startup abort: mapped VMA allocation contract

The startup failure identified by `VmaAllocator_T::CalcAllocationParams` is a
VMA parameter-contract violation, before game loading: `AllocateBuffer` creates
the first `MegaBufferChunk` while constructing GPU. The same invalid pattern
also exists in `AllocateStagingBuffer` and must be repaired there as well.

Both main `f665512f` and preview `e9de48c1` use the same `memory_manager.cpp`
blob (`4b6556a06974305cfdce0ac00859cb40a9dc779b`) and VMA revision
`1d8f600fd424278486eade7ed3e877c99f0846b1`. AUTO memory usage plus persistent
mapping requires a HOST_ACCESS declaration. Debug VMA asserts this contract;
the Release build defines NDEBUG. This preview exposed the existing invalid
parameters; it is not evidence that diagnostic event formatting changed them.
The old installed APK's actual build configuration is not known.

The fix retains AUTO_PREFER_DEVICE/AUTO_PREFER_HOST and existing required/
preferred memory properties. It adds HOST_ACCESS_RANDOM_BIT to both mapped
buffer policies. Sequential-write-only would be incorrect for their shared
APIs: `Buffer::SynchronizeHost` writes the backing while
`Buffer::SynchronizeGuest` reads it, and the same staging API handles texture
upload and GPU-to-CPU download. Debug checkpoints also read CPU-mapped memory.
ALLOW_TRANSFER_INSTEAD is not set because callers require a valid CPU mapping.
Mapped images use non-AUTO UNKNOWN with explicit host-visible properties and
do not have this missing-AUTO-declaration problem.

Run `bash tools/tests/run-mapped-vma.sh` after initializing the pinned vkma,
vkhpp and vkhpp/Vulkan-Headers submodules. The test calls the real pinned VMA
implementation with controlled Vulkan callbacks and assertions enabled:

- Two exec'd baseline children use the original mapped-only flags and must
  die by SIGABRT with the exact VMA contract assertion.
- Both production allocation policies must create a persistent mapping that
  supports CPU writes and reads.
- Unavailable host-visible memory must return VK_ERROR_FEATURE_NOT_PRESENT,
  without falling back to an unmapped allocation or suppressing assertions.

This is a VMA contract regression test with a synthetic Vulkan driver, not a
physical GPU, Android or game run. A newly built exact-source APK still needs
device startup verification. The unrelated missing SAF file/provider exception
requires its own fix. No private crash report is used as a repository fixture.
