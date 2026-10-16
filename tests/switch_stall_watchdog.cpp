#include "switch.h"
#include "platform/switch/stall_watchdog.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>
namespace {
constexpr uint64_t hz=19200000;
thread_local Thread self{4};
std::atomic<uint64_t> tick{1};
std::atomic<unsigned> paused{0},captures{0},pauses{0},resumes{0},polls{0},writes{0};
std::array<bool,8> peerPaused{};
bool hinted=true;unsigned initFailure=0,pauseFailure=0,contextFailure=0,resumeFailures=0;
std::mutex outputMutex;std::string output,path;
unsigned checks=0;
void require(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
void unpaused(){if(paused.load())std::_Exit(91);}
void reset(){self.handle=1;tick=1;paused=captures=pauses=resumes=polls=writes=0;peerPaused={};hinted=true;initFailure=pauseFailure=contextFailure=resumeFailures=0;output.clear();path.clear();}
void settle(){const auto until=polls.load()+3;for(unsigned n=0;n<2000 && polls.load()<until;++n)std::this_thread::sleep_for(std::chrono::milliseconds(1));}
void awaitCapture(){for(unsigned n=0;n<2000 && !captures.load();++n)std::this_thread::sleep_for(std::chrono::milliseconds(1));require(captures.load()==1,"watchdog did not emit one capture");}
void producer(SwitchStallWatchdog::Watchdog& w){self.handle=2;w.producerThreadReady();self.handle=1;}
}
void* operator new(size_t size){if(paused.load())std::_Exit(99);if(auto* p=std::malloc(size?size:1))return p;throw std::bad_alloc();}
void operator delete(void* p) noexcept {std::free(p);}void operator delete(void* p,size_t) noexcept {std::free(p);}
Thread* threadGetSelf(){return &self;}
bool envIsSyscallHinted(unsigned n){return hinted && (n==0x32 || n==0x33);}
uint64_t armGetSystemTick(){return tick.load();}
void svcSleepThread(int64_t){++polls;std::this_thread::sleep_for(std::chrono::milliseconds(1));}
Result svcSetThreadActivity(Handle h,ThreadActivity activity){
 if(h<1 || h>3)std::_Exit(90);
 if(activity==ThreadActivity_Paused){++pauses;if(pauseFailure)return pauseFailure;if(peerPaused[h])std::_Exit(89);peerPaused[h]=true;++paused;return 0;}
 ++resumes;if(!peerPaused[h])std::_Exit(88);if(resumeFailures){--resumeFailures;return 17;}peerPaused[h]=false;--paused;return 0;
}
Result svcGetThreadContext3(ThreadContext* c,Handle h){if(!peerPaused[h])std::_Exit(87);if(contextFailure)return contextFailure;c->pc.x=0x7100000000+h;c->lr=0x7200000000+h;c->sp=0x7300000000+h;return 0;}
[[noreturn]] void svcExitProcess(){std::_Exit(77);}
Result smGetService(Service* s,const char*){unpaused();if(initFailure)return initFailure;s->session=10;return 0;}
Result mockDispatch(Service* s,unsigned cmd,Params p){unpaused();if(s->session!=10)std::_Exit(86);if(cmd==18){if(!p.out_objects)std::_Exit(85);p.out_objects->session=11;}return 0;}
void serviceClose(Service* s){unpaused();s->session=0;}
Result fsFsCreateFile(FsFileSystem*,const char*,s64,unsigned){unpaused();return 0;}
Result fsFsOpenFile(FsFileSystem*,const char* name,unsigned,FsFile* f){unpaused();path=name;f->s.session=12;return 0;}
Result fsFileGetSize(FsFile*,s64* out){unpaused();*out=0;return 0;}
Result fsFileWrite(FsFile*,s64,const void* data,size_t bytes,unsigned){unpaused();std::lock_guard lock(outputMutex);const char* text=static_cast<const char*>(data);output.append(text,bytes);++writes;if(output.find("[stall-watchdog] captured")!=std::string::npos)captures.store(1,std::memory_order_release);return 0;}
void fsFileClose(FsFile* f){unpaused();f->s.session=0;}void fsFsClose(FsFileSystem* f){unpaused();f->s.session=0;}
int main(){try {
 reset();{SwitchStallWatchdog::Watchdog w(false,"/switch/night100","disabled");w.heartbeat();w.producerThreadReady();w.audioThreadReady(3);require(!w.ready() && writes==0 && polls==0,"disabled watchdog has side effects");}
 reset();initFailure=23;{SwitchStallWatchdog::Watchdog w(true,"/switch/night100","failed");require(!w.ready() && w.setupResult()==23,"setup service failure escaped or was lost");}
 reset();hinted=false;{SwitchStallWatchdog::Watchdog w(true,"/switch/night100","unsupported");require(!w.ready() && output.find("disabled=")!=std::string::npos && polls==0,"unsupported context calls started worker");}
 reset();{SwitchStallWatchdog::Watchdog w(true,"/switch/night100","../escape");require(!w.ready() && writes==0,"unsafe diagnostic path accepted");}
 reset();{SwitchStallWatchdog::Watchdog w(true,"/switch/night100","normal");require(w.ready(),"watchdog did not arm");producer(w);w.audioThreadReady(3);tick=4*hz;w.heartbeat();settle();tick=8*hz;settle();require(captures==0,"fresh heartbeat captured peers");w.heartbeat(false);tick=100*hz;settle();require(captures==0,"background heartbeat caused false stall");w.heartbeat(true);tick=106*hz;awaitCapture();require(paused==0 && pauses==3 && resumes==3,"main/producer/audio were not all resumed");require(path=="/switch/night100/logs/stall-normal.log","unexpected direct FS report path");require(output.find("main handle=0x1 attempted=1 available=1 pc=0x7100000001")!=std::string::npos && output.find("producer handle=0x2")!=std::string::npos && output.find("audio handle=0x3")!=std::string::npos,"capture metadata missing");require(output.find("anchor=0x")!=std::string::npos,"ASLR anchor missing");tick=120*hz;settle();require(pauses==3 && captures==1,"watchdog captured more than once");}
 reset();contextFailure=31;resumeFailures=1;{SwitchStallWatchdog::Watchdog w(true,"/switch/night100","context");producer(w);tick=6*hz;awaitCapture();require(paused==0 && pauses==2 && resumes==3,"failed context/retry did not resume all peers");require(output.find("context_result=0x1f")!=std::string::npos && output.find("resume_retries=1")!=std::string::npos,"context/retry failure not retained");}
 reset();pauseFailure=41;{SwitchStallWatchdog::Watchdog w(true,"/switch/night100","pause");producer(w);tick=6*hz;awaitCapture();require(paused==0 && resumes==0 && output.find("pause_result=0x29")!=std::string::npos,"pause failure incorrectly attempted resume");}
 reset();{const pid_t child=fork();require(child>=0,"cannot fork failure policy test");if(child==0){resumeFailures=2;SwitchStallWatchdog::Watchdog w(true,"/switch/night100","fatal");tick=6*hz;for(;;)std::this_thread::sleep_for(std::chrono::milliseconds(1));}int status=0;require(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==77,"two failed resumes did not exit directly");}
 std::cout<<"Switch stall watchdog: "<<checks<<" checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
