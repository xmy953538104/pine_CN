#!/usr/bin/env python3
"""Execute the verbatim production GetPatchData with controlled host constants.

Actual instruction and ThreadContext headers are compiled. This is not an
ARM64 register-preservation test, Guest execution, debugger or runtime hook.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CPP = ROOT / "app/src/main/cpp/skyline"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    text = (CPP / "nce.cpp").read_text()
    start = text.index("    NCE::PatchData NCE::GetPatchData(")
    end = text.index("\n    void NCE::PatchCode(", start)
    production = text[start:end]
    names = ("TrampolineSize", "RescaleClockSize", "TpidrEl0", "TpidrroEl0", "CntfrqEl0", "CntpctEl0", "TegraX1Freq")
    constants = []
    for name in names:
        matches = re.findall(r"    constexpr (?:size_t|u32) " + name + r"\{[^}]+\};", text)
        assert len(matches) == 1, name
        constants.append(matches[0])
    utils = (CPP / "common/utils.h").read_text()
    start = utils.index("    template<typename Type>\n    requires std::is_integral_v<Type>\n    constexpr Type MakeMagic(")
    magic = utils[start:utils.index("\n    constexpr u8 HexDigitToNibble(", start)]
    shim = '''#pragma once
#include <array>
#include <vector>
#include <string_view>
#include <type_traits>
#include <common/base.h>
namespace skyline::util {
inline u64 ClockFrequency{19200000};
inline size_t AlignUp(size_t value, size_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }
MAGIC
}
'''.replace("MAGIC", magic)
    program = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <nce/instructions.h>
#include <nce/guest.h>
#ifdef NDEBUG
#error Assertions are required.
#endif
namespace skyline::nce {
class NCE { public:
    struct PatchData { size_t size; std::vector<size_t> offsets; };
    static PatchData GetPatchData(const std::vector<u8> &text);
};
CONSTANTS
PRODUCTION
}
template<class T> concept HasX19 = requires(T value) { value.x19; };
template<class T> concept HasX20 = requires(T value) { value.x20; };
template<class T> concept HasPc = requires(T value) { value.pc; };
template<class T> concept HasSp = requires(T value) { value.sp; };
template<class T> concept HasLr = requires(T value) { value.lr; };
static_assert(sizeof(skyline::nce::GpRegisters) == 19 * 8);
static_assert(!HasX19<skyline::nce::GpRegisters> && !HasX20<skyline::nce::GpRegisters>);
static_assert(!HasPc<skyline::nce::ThreadContext> && !HasSp<skyline::nce::ThreadContext> && !HasLr<skyline::nce::ThreadContext>);
int main() {
    using namespace skyline;
    struct Case { std::vector<u32> words; std::vector<size_t> normal, rescaled; };
    const std::array<Case, 10> cases{{
        {{}, {}, {}},
        {{0xD503201F}, {}, {}},
        {{0x94044CC0}, {}, {}}, // Owner-supplied BL word, no independently obtained game bytes.
        {{0xD4200000}, {}, {}},
        {{0x14000000}, {}, {}},
        {{0xD4000021}, {0}, {0}},
        {{0xD53BD040, 0xD51BD040}, {0, 1}, {0, 1}},
        {{0xD53BE020, 0xD53BE000}, {0}, {0, 1}},
        {{0xD53BE040}, {}, {}},
        {{0x94044CC0, 0xD4000021, 0x94044CC0, 0xD53BD040}, {1, 3}, {1, 3}},
    }};
    unsigned controls{};
    for (bool rescale : {false, true}) {
        util::ClockFrequency = rescale ? 1000000000 : 19200000;
        for (const auto &test : cases) {
            std::vector<u8> bytes(test.words.size() * 4);
            if (!bytes.empty()) std::memcpy(bytes.data(), test.words.data(), bytes.size());
            const auto before = bytes;
            const auto result = nce::NCE::GetPatchData(bytes);
            assert(result.offsets == (rescale ? test.rescaled : test.normal));
            assert(bytes == before && result.size >= 4096 && result.size % constant::PageSize == 0);
            ++controls;
        }
    }
    nce::instructions::BL branch{0}; branch.raw = 0x94044CC0;
    assert(branch.Verify() && branch.Offset() == 0x113300);
    assert(0x2D64E0 + branch.Offset() == 0x3E97E0);
    std::printf("PASS %u production GetPatchData controls; supplied BL is not selected; real headers expose X0-X18 only; no Guest/ARM64 execution\n", controls);
}
'''.replace("CONSTANTS", "\n".join(constants)).replace("PRODUCTION", production)
    with tempfile.TemporaryDirectory(prefix="pine-nvn-pc-capability-") as directory:
        temp = Path(directory)
        (temp / "common.h").write_text(shim)
        (temp / "probe.cpp").write_text(program)
        executable = temp / "probe"
        command = [os.environ.get("CXX", "c++"), "-std=c++20", "-g", "-O1", "-I", str(temp), "-I", str(CPP), str(temp / "probe.cpp"), "-o", str(executable)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=60)
        environment = dict(os.environ)
        if args.sanitize:
            environment["ASAN_OPTIONS"] = "detect_leaks=0"  # Explicitly not a LeakSanitizer qualification.
        subprocess.run([str(executable)], check=True, env=environment, timeout=10)
    files = ("nce.cpp", "nce/instructions.h", "nce/guest.h", "nce/guest.S", "common/base.h", "common/wregister.h")
    print(json.dumps({"scope": "controlled host production function and headers, no runtime capture",
                      "production_function_sha256": hashlib.sha256(production.encode()).hexdigest(),
                      "source_sha256": {name: hashlib.sha256((CPP / name).read_bytes()).hexdigest() for name in files},
                      "asan_ubsan": args.sanitize, "leak_sanitizer_qualified": False}, sort_keys=True))


if __name__ == "__main__":
    main()
