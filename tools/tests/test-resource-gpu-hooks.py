#!/usr/bin/env python3
"""Compile planned/exact production hook fragments against controlled API calls.
Not a Vulkan/device test. Default execution binds fixtures to current source.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CPP = ROOT / 'app/src/main/cpp/skyline'
FIX = ROOT / 'tools/tests/fixtures/resource-gpu'
p = argparse.ArgumentParser()
p.add_argument('--planned-only', action='store_true', help='Only test proposed fragments without production binding')
a = p.parse_args()
fence, submit, vma = ((FIX / (name + '.inc')).read_text() for name in ('fence', 'submit', 'vma'))
if not a.planned_only:
    scheduler = (CPP / 'gpu/command_scheduler.cpp').read_text()
    assert fence in scheduler, 'Fence fixture must match production verbatim'
    assert submit in scheduler, 'Legacy submit fixture must match production verbatim'
    assert submit.replace('vkQueue.submit(', 'vkQueue.submit2(') in scheduler
    memory = (CPP / 'gpu/memory_manager.cpp').read_text()
    assert memory.count(vma) == 4
    assert memory.count('HostVmaAllocationBytes') == 4
    # All four VMA sites must observe only after their original ThrowOnFail.
    for segment in memory.split(vma)[:-1]:
        assert segment.rstrip().endswith('&allocationInfo));')
    assert 'vmaGetHeapBudgets' not in memory and 'vmaCalculateStatistics' not in memory
    channel = (CPP / 'services/nvdrv/devices/nvhost/gpu_channel.cpp').read_text()
    assert channel.index('DriverFifoSubmittedEntries') > channel.index('flags.raw = 0;')
    alloc = channel[channel.index('PosixResult GpuChannel::AllocGpfifoEx2'):]
    assert alloc.index('DriverFifoCapacityEntries') > alloc.index('asCtx->gmmu.Map(')
    fifo = (CPP / 'soc/gm20b/gpfifo.cpp').read_text()
    assert 'Process(gpEntry);\n                if (diagnostics::ResourceObservationEnabled())\n                    diagnostics::ResourceAdd(diagnostics::ResourceMetric::DriverFifoProcessedWords, gpEntry.size);' in fifo
    assert scheduler.index('HostCommandBufferAllocations') > scheduler.index('vk::detail::throwResultException(result, __builtin_FUNCTION());')

# Keep the production snippets verbatim; replace only platform clock/sleep namespaces
# with deterministic fakes. Their calls and arguments are asserted below.
source = r'''
#include "diagnostics/resource_observer.h"
#include <array>
#include <cassert>
#include <chrono>
#include <csignal>
#include <mutex>
#include <stdexcept>
namespace diagnostics = skyline::diagnostics;
namespace skyline::diagnostics {
bool on{};
std::array<uint64_t, static_cast<size_t>(ResourceMetric::Count)> counters{};
bool ResourceObservationEnabled() noexcept { return on; }
void ResourceAdd(ResourceMetric m, uint64_t v) noexcept { if (on) counters[static_cast<size_t>(m)] += v; }
void ResourceSet(ResourceMetric m, uint64_t v) noexcept { if (on) counters[static_cast<size_t>(m)] = v; }
}
namespace controlled_signal { struct SignalException { int signal; }; }
namespace controlled_clock {
int calls{};
struct steady_clock {
 static auto now() { ++calls; return std::chrono::steady_clock::time_point(std::chrono::microseconds(calls * 17)); }
};
}
namespace controlled_thread {
int calls{}; long seconds{};
void sleep_for(std::chrono::seconds duration) { ++calls; seconds = duration.count(); }
}
struct exception : std::runtime_error { using std::runtime_error::runtime_error; };
namespace vk { struct DeviceLostError {}; }
struct Marker {} marker;
struct Cycle {
 int mode{}, calls{}, fence{};
 void Wait(bool destroy) {
  assert(destroy); ++calls;
  if (diagnostics::on) {
   const auto entered = diagnostics::counters[static_cast<size_t>(diagnostics::ResourceMetric::HostFenceWaitEntered)];
   const auto completed = diagnostics::counters[static_cast<size_t>(diagnostics::ResourceMetric::HostFenceWaits)];
   // At the controlled call boundary only: entered is published before the
   // original Wait, completion after return/throw. Not an in-flight metric.
   assert(entered == static_cast<uint64_t>(calls - 1));
   assert(completed == static_cast<uint64_t>(calls - 2));
  }
  if (mode == 1) throw &marker;
  if (mode == 2) throw controlled_signal::SignalException{SIGINT};
  if (mode == 3) throw controlled_signal::SignalException{SIGSEGV};
 }
};
void waitObserved(Cycle *cycle) {
FENCE
}
struct Queue {
 int mode{}, calls{};
 void submit(int, int) { ++calls; if(mode == 1) throw &marker; if(mode == 2) throw vk::DeviceLostError{}; }
};
struct GPU { std::mutex queueMutex; Queue vkQueue; } gpu;
void submitObserved(Cycle *cycle) {
 int submitInfo{};
SUBMIT
}
void allocateObserved(bool success, uint64_t bytes) {
 if (!success) throw &marker; // The existing ThrowOnFail boundary.
 struct { uint64_t size; } allocationInfo{bytes};
VMA
}
uint64_t count(diagnostics::ResourceMetric m) { return diagnostics::counters[static_cast<size_t>(m)]; }
int main() {
 using enum diagnostics::ResourceMetric;
 Cycle cycle;
 waitObserved(&cycle);
 assert(cycle.calls == 1 && controlled_clock::calls == 0 && count(HostFenceWaits) == 0 && count(HostFenceWaitEntered) == 0);
 diagnostics::on = true;
 waitObserved(&cycle);
 assert(cycle.calls == 2 && count(HostFenceWaits) == 1 && count(HostFenceWaitEntered) == 1 && count(HostFenceWaitUs) == 17);
 cycle.mode = 1;
 try { waitObserved(&cycle); assert(false); } catch (Marker *e) { assert(e == &marker); }
 assert(cycle.calls == 3 && count(HostFenceErrors) == 1 && count(HostFenceWaitUs) == 34);
 cycle.mode = 2;
 try { waitObserved(&cycle); assert(false); } catch (controlled_signal::SignalException &e) { assert(e.signal == SIGINT); }
 assert(cycle.calls == 4 && count(HostFenceErrors) == 1 && count(HostFenceWaitEntered) == 3 && count(HostFenceWaits) == 3);
 cycle.mode = 3;
 try { waitObserved(&cycle); assert(false); } catch (controlled_signal::SignalException &e) { assert(e.signal == SIGSEGV); }
 assert(cycle.calls == 5 && count(HostFenceErrors) == 2 && count(HostFenceWaitUs) == 68);
 assert(count(HostFenceWaitEntered) == 4 && count(HostFenceWaits) == 4);
 submitObserved(&cycle);
 assert(gpu.vkQueue.calls == 1 && count(HostSubmitAccepted) == 1);
 gpu.vkQueue.mode = 1;
 try { submitObserved(&cycle); assert(false); } catch (Marker *e) { assert(e == &marker); }
 assert(count(HostSubmitErrors) == 1 && count(HostSubmitAccepted) == 1 && controlled_thread::calls == 0);
 gpu.vkQueue.mode = 2;
 try { submitObserved(&cycle); assert(false); } catch (const exception &e) { assert(std::string(e.what()) == "Vulkan device lost!"); }
 assert(count(HostSubmitErrors) == 2 && gpu.vkQueue.calls == 3);
 assert(controlled_thread::calls == 1 && controlled_thread::seconds == 5);
 allocateObserved(true, 4096);
 try { allocateObserved(false, 999); assert(false); } catch (Marker *e) { assert(e == &marker); }
 assert(count(HostVmaAllocations) == 1 && count(HostVmaAllocationBytes) == 4096);
 diagnostics::on = false;
 allocateObserved(true, 2048);
 gpu.vkQueue.mode = 0; submitObserved(&cycle);
 assert(count(HostSubmitAccepted) == 1 && count(HostVmaAllocationBytes) == 4096);
}
'''
source = source.replace('FENCE', fence.replace('std::chrono::steady_clock::now()', 'controlled_clock::steady_clock::now()').replace('signal::SignalException', 'controlled_signal::SignalException'))
source = source.replace('SUBMIT', submit.replace('std::this_thread::sleep_for', 'controlled_thread::sleep_for')).replace('VMA', vma)
with tempfile.TemporaryDirectory(prefix='pine-resource-gpu-') as tmp:
    f = Path(tmp) / 'test.cpp'
    f.write_text(source)
    exe = Path(tmp) / 'test'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra', '-Werror', '-I' + str(CPP), str(f), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('GPU hook controlled-callable tests passed' + ('; production fragments bound' if not a.planned_only else '; planned fragments only, production not yet changed'))
