// SPDX-License-Identifier: MPL-2.0
#include "diagnostics/diagnostics.h"
#include "diagnostics/guest.h"
#include "diagnostics/legacy_error.h"
#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <time.h>
#include <vector>
using namespace skyline;
using Clock = std::chrono::steady_clock;
uint64_t cpu(clockid_t which) { timespec ts{}; clock_gettime(which,&ts); return uint64_t(ts.tv_sec)*1000000000+ts.tv_nsec; }
uint64_t ioWrites() { std::ifstream f("/proc/self/io"); std::string key; uint64_t n; while(f>>key>>n) if(key=="syscw:") return n; return 0; }
int main(int argc,char**argv) {
 if(argc!=7)return 2;
 std::string kind=argv[1],mode=argv[2],directory=argv[3];
 unsigned threads=std::stoul(argv[4]),count=std::stoul(argv[5]),paceUs=std::stoul(argv[6]);
 if(mode!="off") { diagnostics::Config c; c.directory=directory;c.sessionId="controlled-"+kind+"-"+mode;c.verbose=mode=="detailed";if(!diagnostics::Start(c))return 3; }
 diagnostics::SetLaunchId("synthetic-launch",true);
 diagnostics::SetContentIdentity("0100000000000000","0100000000000000","0100000000000000","0123456789abcdef0123456789abcdef","controlled");
 std::atomic<uint64_t> producerCpu{};
 std::vector<std::vector<uint64_t>> latencies(threads);
 std::barrier gate(threads+1);
 std::vector<std::thread> jobs;
 std::string message(128,'x');
 auto beforeIo=ioWrites(),beforeCpu=cpu(CLOCK_PROCESS_CPUTIME_ID);
 for(unsigned t=0;t<threads;t++)jobs.emplace_back([&,t]{
  latencies[t].reserve(count);gate.arrive_and_wait();auto cbegin=cpu(CLOCK_THREAD_CPUTIME_ID);auto start=Clock::now();
  for(unsigned i=0;i<count;i++) {
   if(paceUs)std::this_thread::sleep_until(start+std::chrono::microseconds(uint64_t(i)*paceUs));
   auto begin=Clock::now();
   if(kind=="guest")diagnostics::GuestMessage(message,"controlled-fixture");
   else if(kind=="legacy")diagnostics::EmitLegacyError("audio_core",message);
   else diagnostics::Emit(kind=="critical"?"exception":"storage",kind=="critical"?"CONTROLLED_ERROR":"DLC_DATA_READ",
    {{"source","controlled"},{"message",message},{"actual_bytes","64"},{"status","ok"}},kind=="critical"?"ERROR":"INFO");
   latencies[t].push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-begin).count());
  }
  producerCpu.fetch_add(cpu(CLOCK_THREAD_CPUTIME_ID)-cbegin);
 });
 auto begin=Clock::now();gate.arrive_and_wait();for(auto &job:jobs)job.join();auto end=Clock::now();
 diagnostics::Stop();auto drained=Clock::now();auto totalCpu=cpu(CLOCK_PROCESS_CPUTIME_ID)-beforeCpu;
 auto stats=diagnostics::Stats();if(stats.accepted!=stats.written||stats.diskErrors)return 4;
 std::vector<uint64_t> all;for(auto&v:latencies)all.insert(all.end(),v.begin(),v.end());std::sort(all.begin(),all.end());
 uint64_t bytes{};if(std::filesystem::exists(directory))for(auto&f:std::filesystem::directory_iterator(directory))if(f.is_regular_file())bytes+=f.file_size();
 std::cout<<"{\"kind\":\""<<kind<<"\",\"mode\":\""<<mode<<"\",\"threads\":"<<threads<<",\"calls\":"<<all.size()
 <<",\"pace_us\":"<<paceUs<<",\"wall_ns\":"<<std::chrono::duration_cast<std::chrono::nanoseconds>(end-begin).count()
 <<",\"producer_cpu_ns\":"<<producerCpu<<",\"process_cpu_ns\":"<<totalCpu
 <<",\"drain_ns\":"<<std::chrono::duration_cast<std::chrono::nanoseconds>(drained-end).count()
 <<",\"p50_ns\":"<<all[all.size()/2]<<",\"p95_ns\":"<<all[all.size()*95/100]<<",\"p99_ns\":"<<all[all.size()*99/100]<<",\"max_ns\":"<<all.back()
 <<",\"accepted\":"<<stats.accepted<<",\"written\":"<<stats.written<<",\"contention\":"<<stats.droppedContention<<",\"queue_full\":"<<stats.droppedFull
 <<",\"critical_dropped\":"<<stats.criticalDropped<<",\"write_syscalls\":"<<ioWrites()-beforeIo<<",\"retained_bytes\":"<<bytes<<"}\n";
}
