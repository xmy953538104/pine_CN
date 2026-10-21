// SPDX-License-Identifier: MPL-2.0
// Isolated test parent. It never opens Pine/game directories or changes system
// configuration. Every child is executed at most once; first failure stops.
#include "fixture_report.h"
#include <array>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

namespace {
std::string Read(const char*name){std::ifstream in(name);return {std::istreambuf_iterator<char>(in),{}};}
bool HexId(const std::string&value){return value.size()==32&&value.find_first_not_of("0123456789abcdef")==std::string::npos;}
struct Outcome{const char*name;int code{-1},signal{};bool timeout{};std::string report;};
void Save(const std::string&id,const char*state,const std::array<Outcome,2>&tests,int exitCode=-1){
    std::ofstream out("android-result.json.tmp");
    out<<"{\"schema\":1,\"source_commit\":\""<<PINE_BOUNDARY_SOURCE_COMMIT<<"\",\"run_id\":\""<<id
       <<"\",\"status\":\""<<state<<"\",\"android_execution\":"<<(pine_fixture::Android?"true":"false")
       <<",\"exit_code\":"<<exitCode<<",\"scope\":\"isolated_fixture_not_pine_app_or_horizon\",\"observer_installation\":\"disabled_in_pine_production\",\"runtime_connected\":false,\"real_game_records\":0,\"tests\":[";
    bool first=true;
    for(const auto&t:tests){if(!first)out<<',';first=false;
        out<<"{\"component\":\""<<t.name<<"\",\"exit_code\":"<<t.code<<",\"signal\":"<<t.signal<<",\"deadline_exceeded\":"<<(t.timeout?"true":"false")<<",\"result\":";
        if(t.report.empty())out<<"null";else out<<t.report;
        out<<'}';
    }
    out<<"]}\n";out.flush();if(!out.good())throw std::runtime_error("result write failed");out.close();
    if(rename("android-result.json.tmp","android-result.json"))throw std::runtime_error("result rename failed");
}
int Run(Outcome&test,const char*executable,const char*directory,const char*argument=nullptr){
    const auto child=fork();if(child<0)throw std::runtime_error("fork failed");
    if(!child){
        const auto output=std::string(test.name)+".stdout.log",error=std::string(test.name)+".stderr.log";
        int out=open(output.c_str(),O_CREAT|O_EXCL|O_WRONLY,0600),err=open(error.c_str(),O_CREAT|O_EXCL|O_WRONLY,0600);
        if(out<0||err<0)_exit(125);
        if(dup2(out,STDOUT_FILENO)<0||dup2(err,STDERR_FILENO)<0)_exit(125);
        close(out);close(err);
        // Deadline belongs to this isolated child only. Never signal another
        // app or retry a crashed child. SIGALRM remains the default disposition.
        signal(SIGALRM,SIG_DFL);alarm(90);
        if(argument)execl(executable,executable,directory,argument,static_cast<char*>(nullptr));
        else execl(executable,executable,directory,static_cast<char*>(nullptr));
        _exit(127);
    }
    int status{};while(waitpid(child,&status,0)<0)if(errno!=EINTR)throw std::runtime_error("waitpid failed");
    if(WIFEXITED(status))test.code=WEXITSTATUS(status);
    else if(WIFSIGNALED(status)){test.signal=WTERMSIG(status);test.code=128+test.signal;test.timeout=test.signal==SIGALRM;}
    else test.code=126;
    test.report=Read((std::string(directory)+"/component-result.json").c_str());
    // Only locally generated test JSON is embedded, never device/user inputs.
    if(!test.report.empty()&&(test.report.front()!='{'||test.report.size()>4096))test.report.clear();
    if(!test.code&&test.report.empty())test.code=124;
    return test.code;
}
}
int main(int argc,char**argv){
    if(argc!=2||!HexId(argv[1])){std::cerr<<"Expected one 32-hex run ID\n";return 64;}
    std::string id=argv[1];std::array<Outcome,2>tests{{{"lifecycle"},{"registers"}}};bool ownDirectory{};
    try{
        char cwd[4096]{};if(!getcwd(cwd,sizeof(cwd)))throw std::runtime_error("getcwd failed");
#ifdef __ANDROID__
        if(std::string(cwd)!="/data/local/tmp/pine-bionic-"+id)throw std::runtime_error("not the unique isolated test directory");
        ownDirectory=true;
        char api[PROP_VALUE_MAX]{};__system_property_get("ro.build.version.sdk",api);
        if(atoi(api)<29)throw std::runtime_error("requires Android API29+");
#endif
        ownDirectory=true;
        umask(0077);Save(id,"RUNNING",tests);
        auto code=Run(tests[0],"./pine-lifecycle","lifecycle-logs");
        if(!code){Save(id,"RUNNING",tests);code=Run(tests[1],"./pine-registers","register-logs","device-once");}
        Save(id,code?"FAIL":"PASS",tests,code);std::cout<<Read("android-result.json");return code;
    }catch(const std::exception&e){
        std::cerr<<e.what()<<'\n';try{if(ownDirectory)Save(id,"SUPERVISOR_FAILED",tests,123);}catch(...){}
        return 123;
    }
}
