#!/usr/bin/env python3
"""Bind the production JIT32 SVC boundary to controlled host calls, not a guest run."""
import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CPP = ROOT / 'app/src/main/cpp/skyline'
source = (CPP / 'jit/jit_core_32.cpp').read_text()
body = source[source.index('    void JitCore32::SvcHandler('):source.index('    template<typename T>', source.index('    void JitCore32::SvcHandler('))]
# Remove only this observer's exact additions before comparing original behavior.
original = re.sub(r'        if \(diagnostics::ResourceObservationEnabled\(\)\) \{\n(?:            diagnostics::Resource(?:Set|Add)\([^;]+;\n){3}        \}\n', '', body)
original = re.sub(r'^            diagnostics::ResourceAdd\(diagnostics::SvcReturnedMetric\(state.thread->id\)\);\n', '', original, flags=re.M)
tokens = ''.join(re.findall(r'"(?:\\.|[^"\\])*"|[A-Za-z0-9_]+|[^\s]', original))
EXPECTED = '0ff7b676d406a8ba5df3fa6829783807ca40dd90af9c93623c3b0ab1be60596e'
assert hashlib.sha256(tokens.encode()).hexdigest() == EXPECTED, 'JIT32 original SVC behavior changed'
program = r'''
#include "diagnostics/resource_observer.h"
#include <array>
#include <cassert>
#include <stdexcept>
using u32 = uint32_t;
namespace diagnostics = skyline::diagnostics;
namespace skyline::diagnostics {
bool on{};
std::array<uint64_t, static_cast<size_t>(ResourceMetric::Count)> counters{};
bool ResourceObservationEnabled() noexcept { return on; }
void ResourceAdd(ResourceMetric metric, uint64_t value) noexcept { if (on) counters[static_cast<size_t>(metric)] += value; }
void ResourceSet(ResourceMetric metric, uint64_t value) noexcept { if (on) counters[static_cast<size_t>(metric)] = value; }
}
struct Thread { uint64_t id; } mainThread{1}, otherThread{2};
struct State { Thread *thread{&mainThread}; };
struct Context { int value{}; };
struct Marker {} marker;
struct exception : std::runtime_error {
 exception(const char *message, u32) : std::runtime_error(message) {}
};
int calls{}, mode{};
void callable(const State &, Context &context) {
 ++calls;
 if (mode) throw &marker;
 context.value = 42;
}
namespace kernel::svc {
struct Entry {
 void (*function)(const State &, Context &);
 const char *name;
 explicit operator bool() const { return function; }
};
std::array<Entry, 256> SvcTable{};
}
#define TRACE_EVENT(...) ((void)0)
class JitCore32 {
public:
 State state;
 int made{}, applied{}, result{};
 Context MakeSvcContext() { ++made; return {}; }
 void ApplySvcContext(const Context &context) { ++applied; result = context.value; }
 void SvcHandler(u32 swi);
};
BOUNDARY
uint64_t count(diagnostics::ResourceMetric metric) { return diagnostics::counters[static_cast<size_t>(metric)]; }
int main() {
 using enum diagnostics::ResourceMetric;
 kernel::svc::SvcTable[5] = {callable, "controlled"};
 JitCore32 jit;
 jit.SvcHandler(5);
 assert(calls == 1 && jit.made == 1 && jit.applied == 1 && jit.result == 42);
 for (auto value : diagnostics::counters) assert(value == 0);
 diagnostics::on = true;
 jit.SvcHandler(5);
 assert(count(GuestMainSvcEntered) == 1 && count(GuestMainSvcReturned) == 1);
 assert(count(GuestLastSvc) == 5 && count(GuestLastThread) == 1);
 jit.state.thread = &otherThread;
 jit.SvcHandler(5);
 assert(count(GuestOtherSvcEntered) == 1 && count(GuestOtherSvcReturned) == 1);
 mode = 1;
 try { jit.SvcHandler(5); assert(false); } catch (Marker *caught) { assert(caught == &marker); }
 assert(count(GuestOtherSvcEntered) == 2 && count(GuestOtherSvcReturned) == 1);
 assert(jit.made == 4 && jit.applied == 3);
 try { jit.SvcHandler(6); assert(false); } catch (const exception &) {}
 assert(count(GuestOtherSvcEntered) == 3 && count(GuestOtherSvcReturned) == 1);
 assert(count(GuestLastSvc) == 6 && calls == 4);
 diagnostics::on = false;
 mode = 0;
 jit.SvcHandler(5);
 assert(count(GuestOtherSvcEntered) == 3 && count(GuestOtherSvcReturned) == 1);
 assert(calls == 5 && jit.applied == 4);
}
'''.replace('BOUNDARY', body)
with tempfile.TemporaryDirectory(prefix='pine-resource-jit32-') as temp:
    test = Path(temp) / 'test.cpp'
    test.write_text(program)
    exe = Path(temp) / 'test'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra', '-Werror', '-I' + str(CPP), str(test), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('PASS production-bound JIT32 SVC entry/return, main/other, disabled and unchanged exception propagation')
