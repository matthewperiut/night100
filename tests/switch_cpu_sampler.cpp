#include "switch.h"
#include "platform/switch/cpu_sampler.h"
#include <cstdlib>
#include <iostream>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {
constexpr uint64_t hz=19200000;
Thread thread{1};
bool hinted=true,paused=false;
uint64_t tick=1;
unsigned clocks=0,pauses=0,resumes=0,contexts=0;
unsigned pauseFailure=0,contextFailure=0,resumeFailures=0;
unsigned checks=0;
void require(bool condition,const char* reason){++checks;if(!condition)throw std::runtime_error(reason);}
void reset() {
    require(!paused,"test peer was left paused");
    thread.handle=1;hinted=true;tick=1;clocks=pauses=resumes=contexts=0;
    pauseFailure=contextFailure=resumeFailures=0;
    SwitchCpuSampler::configure(false);
}
void enable() {
    SwitchCpuSampler::configure(true);thread.handle=2;
    SwitchCpuSampler::producerThreadReady();thread.handle=1;
}
struct Capture {
    std::ostringstream text;
    std::streambuf* original=std::cout.rdbuf(text.rdbuf());
    ~Capture(){std::cout.rdbuf(original);}
};
}
// An allocation while the synthetic peer is paused is a test failure, even if
// normal iostream output would happen to have sufficient preallocated capacity.
void* operator new(std::size_t count) {
    if(paused)std::_Exit(99);
    if(void* p=std::malloc(count?count:1))return p;
    throw std::bad_alloc{};
}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
Thread* threadGetSelf(){return &thread;}
bool envIsSyscallHinted(unsigned svc){return hinted && (svc==0x32 || svc==0x33);}
uint64_t armGetSystemTick(){++clocks;return tick;}
Result svcSetThreadActivity(Handle handle,ThreadActivity activity) {
    if(handle!=2)std::_Exit(98);
    if(activity==ThreadActivity_Paused) {
        ++pauses;
        if(paused)std::_Exit(97);
        if(pauseFailure)return pauseFailure;
        paused=true;return 0;
    }
    ++resumes;
    if(!paused)std::_Exit(96);
    if(resumeFailures){--resumeFailures;return 7;}
    paused=false;return 0;
}
Result svcGetThreadContext3(ThreadContext* context,Handle handle) {
    if(!paused || handle!=2)std::_Exit(95);
    ++contexts;
    if(contextFailure)return contextFailure;
    context->pc.x=0x7123456780;context->lr=0x7123456704;context->sp=0x7090000010;
    return 0;
}
[[noreturn]] void svcExitProcess(){std::_Exit(77);}

int main(){try {
    {
        Capture capture;reset();SwitchCpuSampler::poll(true);SwitchCpuSampler::dump();
        require(clocks==0 && pauses==0 && capture.text.str().empty(),"disabled sampler performs work");
    }
    {
        Capture capture;reset();hinted=false;enable();SwitchCpuSampler::poll(true);
        require(pauses==0 && capture.text.str().find("context_svcs_not_hinted")!=std::string::npos,"missing SVC permissions were not rejected before pause");
        const auto size=capture.text.str().size();SwitchCpuSampler::poll(true);SwitchCpuSampler::dump();
        require(capture.text.str().size()==size,"failure report emitted repeatedly");
    }
    {
        Capture capture;reset();SwitchCpuSampler::configure(true);SwitchCpuSampler::producerThreadReady();SwitchCpuSampler::poll(true);
        require(pauses==0 && capture.text.str().find("refused_current_thread")!=std::string::npos,"sampler attempted to pause itself");
    }
    {
        Capture capture;reset();enable();tick=hz*1000;SwitchCpuSampler::poll(false);
        require(pauses==0 && clocks==0,"ineligible startup starts diagnostic deadline or samples");
        SwitchCpuSampler::poll(true);
        require(pauses==1 && contexts==1 && resumes==1 && !paused,"first admitted context was not captured/resumed exactly once");
        tick+=hz/40;SwitchCpuSampler::poll(true);
        require(pauses==1,"sampler exceeds20Hz cadence");
        tick+=hz/40;SwitchCpuSampler::poll(true);
        require(pauses==2 && resumes==2,"eligible second observation was lost");
        tick+=hz*60;SwitchCpuSampler::poll(false);
        const auto text=capture.text.str();
        require(text.find("reason=time_limit samples=2")!=std::string::npos && text.find("7123456780,7123456704,7090000010")!=std::string::npos,"time limit did not emit captured PC/LR/SP");
        require(text.find("anchor=0x")!=std::string::npos && text.find("[cpu-native] end")!=std::string::npos,"symbolization anchor or record delimiter missing");
        const auto size=text.size();SwitchCpuSampler::dump();SwitchCpuSampler::poll(true);
        require(capture.text.str().size()==size,"completed sampler did not remain stopped");
    }
    {
        Capture capture;reset();enable();pauseFailure=13;SwitchCpuSampler::poll(true);
        require(pauses==1 && contexts==0 && resumes==0 && !paused,"failed pause attempted context/resume");
        require(capture.text.str().find("pause_failures=1")!=std::string::npos,"pause failure result was discarded");
    }
    {
        Capture capture;reset();enable();contextFailure=17;SwitchCpuSampler::poll(true);
        require(pauses==1 && contexts==1 && resumes==1 && !paused,"failed context read did not resume peer");
        require(capture.text.str().find("reason=context_failed samples=0")!=std::string::npos,"failed context emitted an invalid observation");
    }
    {
        Capture capture;reset();enable();resumeFailures=1;SwitchCpuSampler::poll(true);SwitchCpuSampler::dump();
        require(resumes==2 && !paused,"first resume failure was not retried");
        require(capture.text.str().find("resume_retries=1")!=std::string::npos,"successful resume retry was not reported");
    }
    {
        Capture capture;reset();enable();
        for(unsigned i=0;i<1100;++i){tick=1+uint64_t(i)*hz/20;SwitchCpuSampler::poll(true);}
        require(pauses==1024 && contexts==1024 && resumes==1024 && !paused,"fixed observation capacity exceeded or peer leaked paused");
        const auto text=capture.text.str();
        require(text.find("reason=sample_limit samples=1024")!=std::string::npos,"fixed capacity did not auto-dump");
        require(text.size()<65536,"compact dump exceeds existing telemetry buffer");
    }
    {
        reset();const pid_t child=fork();require(child>=0,"cannot fork failure-policy check");
        if(child==0){enable();resumeFailures=2;SwitchCpuSampler::poll(true);std::_Exit(94);}
        int status=0;require(waitpid(child,&status,0)==child,"cannot wait for failure-policy child");
        require(WIFEXITED(status) && WEXITSTATUS(status)==77,"two failed resumes did not terminate without cleanup/logging");
    }
    std::cout<<"Switch native CPU sampler: "<<checks<<" checks passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
