// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <filesystem>
#include <fstream>
#include <cassert>
#include <unistd.h>
#ifndef PINE_BOUNDARY_SOURCE_COMMIT
#define PINE_BOUNDARY_SOURCE_COMMIT "unrecorded"
#endif
namespace pine_fixture {
inline constexpr bool Android =
#if defined(__ANDROID__) && defined(__aarch64__)
true;
#else
false;
#endif
inline void ValidateOutput(const char*value,const char*expected){
#ifdef __ANDROID__
    assert(std::string(value)==expected);char cwd[4096]{};assert(getcwd(cwd,sizeof(cwd)));
    const std::string directory=cwd,prefix="/data/local/tmp/pine-bionic-";
    assert(directory.starts_with(prefix));const auto id=directory.substr(prefix.size());
    assert(id.size()==32&&id.find_first_not_of("0123456789abcdef")==std::string::npos);
#else
    (void)value;(void)expected;
#endif
}
// Ordinary test threads ONLY. No signal handler calls these formatting/IO APIs.
inline void Checkpoint(const std::filesystem::path&root,const char*name){
    std::filesystem::create_directories(root);
    std::ofstream out(root/"checks.jsonl",std::ios::app);
    out<<"{\"check\":\""<<name<<"\",\"status\":\"PASS\",\"android_execution\":"<<(Android?"true":"false")<<"}\n";
    out.flush();assert(out.good());
}
inline void Complete(const std::filesystem::path&root,const char*component){
    std::ofstream out(root/"component-result.json");
    out<<"{\"schema\":1,\"component\":\""<<component<<"\",\"status\":\"PASS\",\"source_commit\":\""<<PINE_BOUNDARY_SOURCE_COMMIT
       <<"\",\"android_execution\":"<<(Android?"true":"false")<<",\"scope\":\"isolated_fixture_not_pine_app_or_horizon\",\"runtime_connected\":false,\"real_game_records\":0}\n";
    out.flush();assert(out.good());
}
}
