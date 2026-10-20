#!/usr/bin/env python3
"""Compile the exact production Break body with controlled host dependencies.

Uses real host setjmp/longjmp, not Android execution or the real stack formatter,
process Kill, logger or diagnostics sink. No production source is rewritten.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
CPP = ROOT / "app/src/main/cpp/skyline"
SVC = CPP / "kernel/svc.cpp"
BASELINE_COMMIT = "cd7966c40a87b470e4feebf1ec5bb6a6b4c6540e"
BASELINE_TOKEN_SHA256 = "d5777b73e66839a0043fa859e574b617c0256b45afa07e0834d6f94d878ae8c9"

# Copied independently from `git show <BASELINE_COMMIT>:<svc.cpp path>`, before
# removing anything from current source. Retain a literal to work in shallow CI
# checkouts too; the hash is pinned to that real frozen Git baseline, not derived
# by stripping the current function. Both literal and stripped source must match.
BASELINE_BREAK = r'''    void Break(const DeviceState &state, SvcContext &ctx) {
        auto reason{ctx.x0};
        if (reason & (1ULL << 31)) {
            LOGD("Debugger is being engaged ({})", reason);
        } else {
            LOGE("Exit Stack Trace ({}){}", reason, state.loader->GetStackTrace());
            if (state.thread->id)
                state.process->Kill(false);
            std::longjmp(state.thread->originalCtx, true);
        }
    }
'''

OBSERVER_BLOCK = r'''        if (diagnostics::Enabled()) {
            try {
                diagnostics::Emit("guest", "GUEST_BREAK_REQUEST", {{"break_reason", std::to_string(reason)},
                    {"notification_only", reason & (1ULL << 31) ? "true" : "false"},
                    {"location", "kernel::svc::Break"},
                    {"observation", "guest_svc_request_before_stack_formatting_not_host_signal_or_memory_diagnosis"}},
                    reason & (1ULL << 31) ? "INFO" : "WARN");
            } catch (...) {} // Only observation preparation; original guest flow follows.
        }
'''


def token_hash(text):
    tokens = "".join(re.findall(r'"(?:\\.|[^"\\])*"|[A-Za-z0-9_]+|[^\s]', text))
    return hashlib.sha256(tokens.encode()).hexdigest()


def extract_break(text):
    signature = "    void Break(const DeviceState &state, SvcContext &ctx) {"
    assert text.count(signature) == 1, "Expected exactly one production Break definition"
    start = text.index(signature)
    end = text.index("\n    void OutputDebugString(", start)
    return text[start:end].rstrip() + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    current = extract_break(SVC.read_text())
    assert token_hash(BASELINE_BREAK) == BASELINE_TOKEN_SHA256, "Frozen baseline literal changed"
    assert current.count(OBSERVER_BLOCK) == 1, "Production observer block must match verbatim exactly once"
    restored = current.replace(OBSERVER_BLOCK, "", 1)
    assert token_hash(restored) == BASELINE_TOKEN_SHA256, "Original Break flow changed beyond the exact observer block"

    program = r'''
#include "diagnostics/diagnostics.h"
#include <array>
#include <cassert>
#include <csetjmp>
#include <cstdio>
#include <cstdint>
#include <string_view>
#ifdef NDEBUG
#error These controlled checks require assertions.
#endif
namespace diagnostics = skyline::diagnostics;
enum class Step { Enabled, Emit, Format, ErrorLog, DebugLog, Kill, Jump };
enum class Outcome { Returned, Jumped, FormatException };
struct FormatterFailure {} formatterFailure;
std::array<Step, 16> trace{};
size_t traceSize{};
unsigned enabledCalls{}, emits{}, formats{}, errors{}, debugs{}, kills{}, jumps{}, caught{};
bool on{}, formatterThrows{};
uint64_t expectedReason{};
char expectedReasonText[32]{};
void Record(Step step) { assert(traceSize < trace.size()); trace[traceSize++] = step; }
namespace skyline::diagnostics {
// Exact production declarations, including noexcept and string_view fields.
// This validates the observer call, not sink admission or file persistence.
bool Enabled() noexcept { ++enabledCalls; Record(Step::Enabled); return on; }
void Emit(std::string_view category, std::string_view event,
          std::initializer_list<std::pair<std::string_view, std::string_view>> fields,
          std::string_view level) noexcept {
    assert(on && formats == 0 && kills == 0 && jumps == 0 && emits == 0);
    assert(category == "guest" && event == "GUEST_BREAK_REQUEST");
    const bool notification = expectedReason & (1ULL << 31);
    assert(level == (notification ? "INFO" : "WARN"));
    constexpr std::array<std::string_view, 4> keys{
        "break_reason", "notification_only", "location", "observation"};
    const std::array<std::string_view, 4> values{
        expectedReasonText, notification ? "true" : "false", "kernel::svc::Break",
        "guest_svc_request_before_stack_formatting_not_host_signal_or_memory_diagnosis"};
    assert(fields.size() == keys.size());
    size_t index{};
    for (const auto &field : fields) {
        assert(field.first == keys[index] && field.second == values[index]); ++index;
    }
    ++emits; Record(Step::Emit);
}
}
struct Loader {
    // Trivial return avoids skipping a nontrivial local's destructor on longjmp.
    // The actual formatter is deliberately not compiled or claimed as tested.
    int GetStackTrace() const {
        ++formats; Record(Step::Format);
        if (formatterThrows) throw &formatterFailure;
        return 71;
    }
} loader;
struct Process {
    void Kill(bool join) const { assert(!join && !kills && formats == 1 && errors == 1); ++kills; Record(Step::Kill); }
} process;
struct Thread { uint64_t id{}; std::jmp_buf originalCtx; } thread;
struct DeviceState { Loader *loader; Process *process; Thread *thread; } state{&loader, &process, &thread};
struct SvcContext { uint64_t x0{}, untouched{}; } context;
void ErrorLog(std::string_view message, uint64_t reason, int formatted) {
    assert(message == "Exit Stack Trace ({}){}" && reason == expectedReason && formatted == 71);
    assert(formats == 1); ++errors; Record(Step::ErrorLog);
}
void DebugLog(std::string_view message, uint64_t reason) {
    assert(message == "Debugger is being engaged ({})" && reason == expectedReason);
    ++debugs; Record(Step::DebugLog);
}
#define LOGE(...) ErrorLog(__VA_ARGS__)
#define LOGD(...) DebugLog(__VA_ARGS__)
namespace current_source {
CURRENT_BODY
}
namespace frozen_baseline {
BASELINE_BODY
}
using Entry = void (*)(const DeviceState &, SvcContext &);
Outcome Invoke(Entry entry) {
    // No nontrivial automatic object crosses this real longjmp boundary. The
    // observer's temporary strings have already been destroyed inside Break.
    if (setjmp(thread.originalCtx) == 0) {
        try { entry(state, context); return Outcome::Returned; }
        catch (FormatterFailure *failure) {
            assert(failure == &formatterFailure); ++caught; return Outcome::FormatException;
        }
    }
    ++jumps; Record(Step::Jump); return Outcome::Jumped;
}
void Case(Entry entry, bool observedSource, bool enabled, uint64_t tid, uint64_t reason, bool throwFormatter) {
    traceSize = enabledCalls = emits = formats = errors = debugs = kills = jumps = caught = 0;
    on = enabled; formatterThrows = throwFormatter; expectedReason = reason;
    assert(std::snprintf(expectedReasonText, sizeof(expectedReasonText), "%llu", static_cast<unsigned long long>(reason)) > 0);
    thread.id = tid; context.x0 = reason; context.untouched = 0x123456789abcdef0ULL;
    const auto result = Invoke(entry);
    const bool notification = reason & (1ULL << 31);
    assert(context.x0 == reason && context.untouched == 0x123456789abcdef0ULL);
    assert(enabledCalls == (observedSource ? 1U : 0U));
    assert(emits == (observedSource && enabled ? 1U : 0U));
    size_t at{};
    const auto expect = [&](Step step) { assert(at < traceSize && trace[at++] == step); };
    if (observedSource) {
        expect(Step::Enabled);
        if (enabled) expect(Step::Emit);
    }
    if (notification) {
        assert(result == Outcome::Returned && debugs == 1);
        assert(!formats && !errors && !kills && !jumps && !caught);
        expect(Step::DebugLog);
    } else {
        assert(formats == 1 && !debugs); expect(Step::Format);
        if (throwFormatter) {
            assert(result == Outcome::FormatException && caught == 1);
            assert(!errors && !kills && !jumps);
        } else {
            assert(result == Outcome::Jumped && jumps == 1 && errors == 1 && !caught);
            expect(Step::ErrorLog);
            assert(kills == (tid ? 1U : 0U));
            if (tid) expect(Step::Kill);
            expect(Step::Jump);
        }
    }
    assert(at == traceSize);
}
int main() {
    unsigned currentCases{}, baselineCases{};
    for (const bool enabled : {false, true})
        for (const uint64_t tid : {0ULL, 47ULL})
            for (const bool notification : {false, true})
                for (const bool throws : {false, true}) {
                    // Bit 63 proves reason stays an unsigned full-width value;
                    // bit 31 is independently varied and never inferred by role.
                    const uint64_t reason = (1ULL << 63) | (notification ? 1ULL << 31 : 0) | 37;
                    Case(current_source::Break, true, enabled, tid, reason, throws); ++currentCases;
                    Case(frozen_baseline::Break, false, enabled, tid, reason, throws); ++baselineCases;
                }
    Case(current_source::Break, true, true, 0, 0, false); ++currentCases;
    Case(frozen_baseline::Break, false, true, 0, 0, false); ++baselineCases;
    Case(current_source::Break, true, true, 47, UINT64_MAX, true); ++currentCases;
    Case(frozen_baseline::Break, false, true, 47, UINT64_MAX, true); ++baselineCases;
    std::printf("PASS %u production + %u frozen-baseline controlled cases: event before formatter, original exception identity/no Kill/no jump, notification bit31, main/other Kill(false)/real longjmp, disabled no Emit; no Android execution\n", currentCases, baselineCases);
}
'''.replace("CURRENT_BODY", current).replace("BASELINE_BODY", BASELINE_BREAK)

    flags = ["-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror"]
    sanitized = args.sanitize or os.environ.get("PINE_DIAG_SANITIZE") == "1"
    if sanitized:
        flags += ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    with tempfile.TemporaryDirectory(prefix="pine-svc-break-") as temporary:
        directory = Path(temporary)
        source, executable = directory / "test.cpp", directory / "test"
        source.write_text(program)
        subprocess.run([os.environ.get("CXX", "c++"), *flags, "-I" + str(CPP), str(source), "-o", str(executable)], check=True, timeout=60)
        subprocess.run([str(executable)], check=True, timeout=10)
    assert extract_break(SVC.read_text()) == current, "Production Break changed during this run"
    print(f"PASS exact observer removal matches git baseline {BASELINE_COMMIT}; token_sha256={BASELINE_TOKEN_SHA256}; sanitizers={sanitized}")


if __name__ == "__main__":
    main()
