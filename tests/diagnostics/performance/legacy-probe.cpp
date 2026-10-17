// SPDX-License-Identifier: MPL-2.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>
#include "logger/logger.h"
#include "diagnostics/diagnostics.h"
namespace AudioCore::Log { void Error(const std::string&); }
using Clock=std::chrono::steady_clock;
std::atomic<unsigned> calls{};
std::atomic<bool> releaseFirst{},finishing{};
unsigned expected{};
extern "C" int __android_log_write(int,const char*,const char*) {
 auto ordinal=++calls;
 if(ordinal==1)while(!releaseFirst.load())std::this_thread::sleep_for(std::chrono::milliseconds(1));
 if(ordinal==expected)while(!finishing.load())std::this_thread::sleep_for(std::chrono::milliseconds(1));
 return 1;
}
int main(int argc,char**argv) {
 if(argc!=4)return 2;
 bool adapter=std::string(argv[1])=="adapter",diag=std::string(argv[2])=="on";
 std::filesystem::path dir=argv[3];std::filesystem::create_directories(dir);
 constexpr unsigned count=4096;expected=adapter?count+1:1;
 if(diag){skyline::diagnostics::Config c;c.directory=(dir/"diag").string();c.sessionId="controlled-legacy";if(!skyline::diagnostics::Start(c))return 3;}
 skyline::AsyncLogger::Initialize(skyline::AsyncLogger::LogLevel::Disabled,dir/"emulation.log");
 if(skyline::AsyncLogger::CheckLogLevel(skyline::AsyncLogger::LogLevel::Error))return 4;
 std::thread release([&]{std::this_thread::sleep_for(std::chrono::milliseconds(200));releaseFirst=true;});
 std::vector<uint64_t> latency;auto begin=Clock::now();
 for(unsigned i=0;i<count;i++){
  auto start=Clock::now();
  if(adapter){auto message=fmt::format("Audio Core controlled message {}",i);AudioCore::Log::Error(message);}
  else LOGE("controlled macro {}",i);
  latency.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count());
 }
 auto producerEnd=Clock::now();
 skyline::AsyncLogger::LogAsync(skyline::AsyncLogger::LogLevel::Info,std::string("controlled shutdown sentinel"));
 // Release the final callback only after Finalize can signal the real writer.
 std::thread ending([&]{std::this_thread::sleep_for(std::chrono::milliseconds(20));finishing=true;});
 skyline::AsyncLogger::Finalize(true);release.join();ending.join();skyline::diagnostics::Stop();
 if(calls!=expected)return 5;
 std::sort(latency.begin(),latency.end());
 auto stats=skyline::diagnostics::Stats();
 std::cout<<"{\"path\":\""<<(adapter?"actual-audio-adapter":"guarded-LOGE")<<"\",\"diagnostics\":\""<<(diag?"on":"off")<<"\",\"text_level\":\"Disabled\",\"calls\":"<<count
 <<",\"unfiltered_legacy_writes\":"<<calls-1<<",\"producer_wall_ns\":"<<std::chrono::duration_cast<std::chrono::nanoseconds>(producerEnd-begin).count()
 <<",\"max_call_ns\":"<<latency.back()<<",\"p95_call_ns\":"<<latency[latency.size()*95/100]<<",\"diag_accepted\":"<<stats.accepted<<",\"diag_dropped\":"<<stats.dropped
 <<",\"scope\":\"Production logger/queue and extracted unchanged audio adapter; host mutex and controlled200ms logcat stand-ins, not Android latency\"}\n";
 // Finalize destroys the placement-new singleton. Production exits by killProcess,
 // so avoid a second C++ global destruction in this standalone host executable.
 std::cout.flush();std::_Exit(0);
}
