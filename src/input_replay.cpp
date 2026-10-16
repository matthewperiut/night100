#include "input_replay.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <atomic>
#include <chrono>
#include <iostream>
#include <algorithm>
#include <unistd.h>

namespace InputReplay {
namespace {
constexpr uint64_t maxFrames=60ull*60*60*24*31;
struct Event {
    char kind=0;
    uint64_t frame=0,poll=0;
    Context context;
    Pad pad;
};
uint32_t checksum(const std::string& value) {
    uint32_t hash=2166136261u;
    for(unsigned char c:value){hash^=c;hash*=16777619u;}
    return hash;
}
[[noreturn]] void invalid(const std::string& message) {
    throw std::runtime_error("Input recording: "+message);
}
void endOfLine(std::istringstream& input) {
    input>>std::ws;if(!input.eof())invalid("unexpected trailing fields");
}
std::string checkedLine(std::istream& input,bool* trailingPartial=nullptr) {
    std::string line;
    if(!std::getline(input,line))invalid("missing record");
    if(input.eof()) {
        if(trailingPartial){*trailingPartial=true;return {};}
        invalid("truncated header");
    }
    if(line.size()>4096)invalid("record too long");
    const auto separator=line.rfind(" #");
    if(separator==std::string::npos)invalid("missing record checksum");
    const std::string payload=line.substr(0,separator);
    std::istringstream digest(line.substr(separator+2));uint32_t expected=0;
    if(!(digest>>std::hex>>expected))invalid("invalid record checksum");
    endOfLine(digest);
    if(checksum(payload)!=expected)invalid("record checksum mismatch");
    return payload;
}
}
struct Session::Impl {
    Mode mode;
    FILE* output=nullptr;
    std::filesystem::path sourcePath;
    uint64_t writtenBytes=0,completedBytes=0;
    uint64_t frame=0,poll=0;
    size_t next=0;
    bool finished=false,interrupted=false;
    std::vector<Event> events;
    std::array<Pad,8> held;
    std::array<bool,8> haveHeld{};
    Context lastContext;
    bool haveContext=false,waiting=false;
    unsigned waitFrames=0;
    std::chrono::steady_clock::time_point waitStart;
    explicit Impl(Mode value):mode(value){}
    ~Impl(){if(output)std::fclose(output);}
    void write(const std::string& payload) {
        std::ostringstream line;line<<payload<<" #"<<std::hex<<checksum(payload)<<'\n';
        const auto value=line.str();
        if(std::fwrite(value.data(),1,value.size(),output)!=value.size()
#if !defined(__SWITCH__)
            || std::fflush(output)
#endif
        )
            invalid("cannot write recording ("+std::string(std::strerror(errno))+")");
        writtenBytes+=value.size();
    }
    static bool controlsEqual(const Pad& a,const Pad& b) {
        return std::equal(a.bytes.begin()+2,a.bytes.end(),b.bytes.begin()+2);
    }
    static bool neutral(const Pad& pad) {
        if(pad.bytes[2]!=255 || pad.bytes[3]!=255)return false;
        for(unsigned i=4;i<8;++i)if(pad.bytes[i]!=127 && pad.bytes[i]!=128)return false;
        for(unsigned i=8;i<32;++i)if(pad.bytes[i])return false;
        return true;
    }
    void remember(const Event& event) {
        if(event.kind=='P'){const auto key=event.pad.port*4+event.pad.slot;held[key]=event.pad;haveHeld[key]=true;}
        lastContext=event.context;haveContext=true;
    }
    void consume() {
        const auto& event=events[next];remember(event);
        if(event.kind=='F'){frame=event.frame;poll=0;}else poll=event.poll+1;
        ++next;if(next==events.size())finished=true;
    }
    void resume() {
        if(waiting)std::cout<<"[input] Context aligned at recorded game frame "<<frame<<" after "<<waitFrames<<" extra completed frames"<<std::endl;
        waiting=false;waitFrames=0;
    }
    void wait(const Context& context,bool completedFrame) {
        if(context!=events[next].context && (!haveContext || context!=lastContext)) {
            std::ostringstream message;message<<"unexpected replay state at recorded frame "<<frame
                <<": mode/state/ostrich/scene "<<context.mode<<'/'<<context.state<<'/'<<context.ostrich<<"/0x"<<std::hex<<context.scene;
            invalid(message.str());
        }
        if(!waiting) {
            waiting=true;waitStart=std::chrono::steady_clock::now();waitFrames=0;
            std::cout<<"[input] Waiting for recorded context/input poll at game frame "<<frame<<std::endl;
        }
        if(completedFrame)++waitFrames;
        if(waitFrames>300 || std::chrono::steady_clock::now()-waitStart>std::chrono::seconds(5))
            invalid("context/input alignment exceeded 300 frames or 5 seconds at recorded frame "+std::to_string(frame));
    }
    // Seek only across neutral repeats: every button/stick/pressure change is
    // mandatory, including a release after a held button. Gameplay holds never
    // shrink to compensate for a different loading duration.
    bool seek(char kind,const Context& context,uint32_t port=0,uint32_t slot=0) {
        auto simulated=held;auto have=haveHeld;
        for(size_t candidate=next;candidate<events.size();++candidate) {
            const auto& event=events[candidate];
            if(event.frame>frame+300)break;
            if(event.kind==kind && event.context==context &&
               (kind!='P' || (event.pad.port==port && event.pad.slot==slot))) {
                const auto old=frame;
                while(next<candidate)consume();
                if(frame!=old)std::cout<<"[input] Aligned early context across neutral recorded frames "<<old<<" -> "<<frame<<std::endl;
                resume();return true;
            }
            if(event.kind=='P') {
                const auto key=event.pad.port*4+event.pad.slot;
                if(!neutral(event.pad) || (have[key] && !controlsEqual(simulated[key],event.pad)))return false;
                simulated[key]=event.pad;have[key]=true;
            }
        }
        return false;
    }
    void supplyHeld(Pad& pad,bool makeNeutral=false) const {
        const auto key=pad.port*4+pad.slot;
        if(haveHeld[key])pad=held[key];
        if(makeNeutral || !haveHeld[key]) {
            pad.bytes[2]=pad.bytes[3]=255;
            for(unsigned i=4;i<8;++i)pad.bytes[i]=128;
            for(unsigned i=8;i<32;++i)pad.bytes[i]=0;
        }
    }
};
Session::Session(Mode mode,const std::filesystem::path& path,std::string options,std::string elf):impl(std::make_unique<Impl>(mode)) {
    if(path.empty())invalid("empty path");
    if(options.find_first_of("\r\n")!=std::string::npos)invalid("startup options contain newlines");
    if(mode==Mode::Record) {
        impl->sourcePath=std::filesystem::absolute(path);
        const int descriptor=::open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);
        if(descriptor<0)invalid("cannot create "+path.string()+" ("+std::strerror(errno)+")");
        impl->output=::fdopen(descriptor,"wb");
        if(!impl->output){::close(descriptor);invalid("cannot open recording stream");}
#if defined(__SWITCH__)
        if(std::setvbuf(impl->output,nullptr,_IOFBF,65536))invalid("cannot buffer input recording");
#endif
        std::ostringstream header;header<<"NIGHT100_INPUT 1 "<<elf<<' '<<std::quoted(options);
        impl->write(header.str());
#if defined(__SWITCH__)
        if(std::fflush(impl->output))invalid("cannot publish input recording header");
#endif
        return;
    }
    std::error_code error;
    const auto size=std::filesystem::file_size(path,error);
    if(error || size>256ull*1024*1024)invalid("unreadable or oversized replay file");
    std::ifstream input(path,std::ios::binary);
    std::istringstream header(checkedLine(input));
    std::string magic,recordedElf,recordedOptions;unsigned version=0;
    if(!(header>>magic>>version>>recordedElf>>std::quoted(recordedOptions)) || magic!="NIGHT100_INPUT" || version!=1)
        invalid("unsupported recording header");
    endOfLine(header);
    if(recordedElf!=elf)invalid("ELF identity mismatch");
    if(recordedOptions!=options)invalid("startup options mismatch: recorded "+recordedOptions+", current "+options);
    uint64_t frame=0,poll=0;bool footer=false;
    while(input.peek()!=std::char_traits<char>::eof()) {
        bool trailingPartial=false;
        const auto payload=checkedLine(input,&trailingPartial);
        if(trailingPartial)break; // A killed recorder may leave only its final write incomplete.
        std::istringstream row(payload);Event event;
        if(!(row>>event.kind>>event.frame) || event.frame>maxFrames)invalid("invalid event frame");
        if(footer)invalid("event after session footer");
        if(event.kind=='E') {
            std::string reason;if(!(row>>std::quoted(reason)) || event.frame!=frame)invalid("invalid session footer");
            endOfLine(row);footer=true;impl->interrupted=reason=="crash";continue;
        }
        if(event.kind=='P') {
            std::string hex;
            if(!(row>>event.poll>>event.pad.port>>event.pad.slot>>event.pad.result>>hex) ||
               event.frame!=frame || event.poll!=poll++ || event.pad.port>1 || event.pad.slot>3 || hex.size()!=64)
                invalid("invalid or unordered pad event");
            for(size_t i=0;i<32;++i) {
                const auto digit=[](char c)->int {if(c>='0' && c<='9')return c-'0';if(c>='a' && c<='f')return c-'a'+10;return -1;};
                const int a=digit(hex[i*2]),b=digit(hex[i*2+1]);
                if(a<0 || b<0)invalid("invalid pad bytes");event.pad.bytes[i]=uint8_t(a*16+b);
            }
        } else if(event.kind=='F') {
            if(event.frame!=frame+1)invalid("unordered completed frame");
            frame=event.frame;poll=0;
        } else invalid("unknown event kind");
        if(!(row>>event.context.mode>>event.context.state>>event.context.ostrich>>event.context.scene))invalid("missing state checkpoint");
        endOfLine(row);impl->events.push_back(event);
    }
    if(impl->events.empty())invalid("recording has no game events");
    impl->interrupted=impl->interrupted || !footer;
}
Session::~Session()=default;
void Session::pad(Pad& pad,const Context& context) {
    if(impl->finished)return;
    if(pad.port>1 || pad.slot>3)invalid("invalid controller port or slot");
    if(impl->mode==Mode::Replay) {
        const auto& expected=impl->events[impl->next];
        if(expected.kind=='P' && expected.context==context && expected.pad.port==pad.port && expected.pad.slot==pad.slot) {
            pad=expected.pad;impl->consume();impl->resume();return;
        }
        if(expected.context!=context) {
            if(impl->seek('P',context,pad.port,pad.slot)) {
                pad=impl->events[impl->next].pad;impl->consume();return;
            }
            impl->wait(context,false);impl->supplyHeld(pad,true);return;
        }
        // The game may poll a port more often in another host run. Reusing its
        // current state does not advance the recorded frame or lose an action.
        impl->supplyHeld(pad,impl->waiting);return;
    }
    std::ostringstream row;row<<"P "<<impl->frame<<' '<<impl->poll++<<' '<<pad.port<<' '<<pad.slot<<' '<<pad.result<<' ';
    for(uint8_t byte:pad.bytes)row<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(byte);
    row<<std::dec<<' '<<context.mode<<' '<<context.state<<' '<<context.ostrich<<' '<<context.scene;impl->write(row.str());
}
void Session::completed(const Context& context) {
    if(impl->finished)return;
    if(impl->mode==Mode::Replay) {
        while(impl->next<impl->events.size() && impl->events[impl->next].kind=='P') {
            const auto& event=impl->events[impl->next];const auto key=event.pad.port*4+event.pad.slot;
            if((impl->haveHeld[key] && Impl::controlsEqual(impl->held[key],event.pad)) ||
               (!impl->haveHeld[key] && Impl::neutral(event.pad)))impl->consume();
            else {impl->wait(context,true);return;}
        }
        if(impl->finished)return;
        if(impl->events[impl->next].context==context) {
            impl->consume();impl->resume();return;
        }
        if(impl->seek('F',context)){impl->consume();return;}
        impl->wait(context,true);return;
    } else {
        ++impl->frame;
        std::ostringstream row;row<<"F "<<impl->frame<<' '<<context.mode<<' '<<context.state<<' '<<context.ostrich<<' '<<context.scene;
        impl->write(row.str());
        impl->completedBytes=impl->writtenBytes;
#if defined(__SWITCH__)
        if(impl->frame%60==0 && std::fflush(impl->output))invalid("cannot flush input recording batch");
#endif
    }
    impl->poll=0;
}
void Session::finish(const std::string& reason) {
    if(impl->finished)return;
    if(reason.find_first_of("\r\n")!=std::string::npos)invalid("invalid finish reason");
    if(impl->mode==Mode::Record){std::ostringstream row;row<<"E "<<impl->frame<<' '<<std::quoted(reason);impl->write(row.str());}
    if(impl->output && std::fflush(impl->output))invalid("cannot flush completed input recording");
    impl->finished=true;
}
void Session::savePrefix(const std::filesystem::path& path) const {
    if(impl->mode!=Mode::Record)invalid("only a recorder can save its history");
    if(path.empty() || path.filename().empty())invalid("empty prefix path");
    if(!impl->completedBytes)invalid("cannot save history before its first completed frame");
    if(impl->output && std::fflush(impl->output))invalid("cannot flush recording history before copying");
    // A same-directory temporary file allows exclusive atomic publication:
    // readers see a complete footer, and an existing destination is never
    // replaced, including one created while the snapshot was being written.
    const auto destination=std::filesystem::absolute(path);
    std::string temporary=(destination.parent_path()/("."+destination.filename().string()+".tmp-XXXXXX")).string();
    std::vector<char> name(temporary.begin(),temporary.end());name.push_back('\0');
    const int descriptor=::mkstemp(name.data());
    if(descriptor<0)invalid("cannot create prefix temporary file ("+std::string(std::strerror(errno))+")");
    temporary=name.data();FILE* output=::fdopen(descriptor,"wb");
    if(!output){const auto error=errno;::close(descriptor);::unlink(temporary.c_str());invalid("cannot open prefix stream ("+std::string(std::strerror(error))+")");}
    try {
        std::ifstream source(impl->sourcePath,std::ios::binary);
        if(!source)invalid("cannot read recording history");
        const auto append=[&](const std::string& payload) {
            std::ostringstream row;row<<payload<<" #"<<std::hex<<checksum(payload)<<'\n';
            const auto bytes=row.str();
            if(std::fwrite(bytes.data(),1,bytes.size(),output)!=bytes.size())invalid("cannot write prefix");
        };
        uint64_t copied=0;std::string last;
        while(copied<impl->completedBytes) {
            last=checkedLine(source);
            const auto position=source.tellg();
            if(position<0 || uint64_t(position)>impl->completedBytes)invalid("recording history frame boundary changed");
            copied=uint64_t(position);append(last);
        }
        std::istringstream frame(last);char kind=0;uint64_t number=0;
        if(!(frame>>kind>>number) || kind!='F' || number!=impl->frame)invalid("recording history completed frame changed");
        std::ostringstream footer;footer<<"E "<<impl->frame<<" \"snapshot\"";append(footer.str());
        if(std::fflush(output) || ::fsync(::fileno(output)))invalid("cannot flush prefix");
        const int closed=std::fclose(output);output=nullptr;
        if(closed)invalid("cannot close prefix");
#if defined(__SWITCH__)
        // libnx RenameFile rejects an existing destination; FAT has no links.
        if(::rename(temporary.c_str(),destination.c_str()))invalid("cannot publish "+destination.string()+" ("+std::strerror(errno)+")");
#else
        if(::link(temporary.c_str(),destination.c_str()))invalid("cannot publish "+destination.string()+" ("+std::strerror(errno)+")");
        ::unlink(temporary.c_str());
#endif
    }catch(...) {
        if(output)std::fclose(output);
        ::unlink(temporary.c_str());throw;
    }
}
uint64_t Session::frame() const{return impl->frame;}
bool Session::ended() const{return impl->finished;}
bool Session::interrupted() const{return impl->interrupted;}
bool Session::aligning() const{return impl->waiting;}
Mode Session::mode() const{return impl->mode;}
}

#ifndef NIGHT100_INPUT_REPLAY_CORE_ONLY
#include "replay_harness.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include <cstdlib>
#include <iostream>
namespace {
std::unique_ptr<InputReplay::Session> inputSession;
PS2Runtime::RecompiledFunction inputPadOriginal;
bool inputReturnControl=false,inputEndReported=false;
InputReplay::EndPolicy inputEndPolicy;
std::array<InputReplay::Pad,8> inputLastPads;
std::array<bool,8> inputHavePad{};
std::atomic<int64_t> inputEndDeadline{0};
std::atomic<int64_t> inputAlignmentDeadline{0};
std::atomic<bool> inputAlignmentTimedOut{false};
int64_t inputNow() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
uint32_t inputWord(const uint8_t* ram,uint32_t address){uint32_t value;std::memcpy(&value,ram+address,4);return value;}
InputReplay::Context inputContext(const uint8_t* ram) {
    const auto scene=inputWord(ram,0x363b64);
    return {inputWord(ram,0x357e20),inputWord(ram,0x3574f4),inputWord(ram,0x357e24),
        scene>=0x1000 && scene<=PS2_RAM_SIZE-0x70?inputWord(ram,scene+0x6c):0};
}
void inputCheckEnd(PS2Runtime* runtime,bool atCompletedFrame) {
    if(inputSession->mode()!=InputReplay::Mode::Replay || !inputSession->ended() || inputEndReported)return;
    inputEndReported=true;
    std::cout<<"[input] Replay inputs ended at completed game frame "<<inputSession->frame();
    if(inputReturnControl){std::cout<<"; returning controller to player"<<std::endl;return;}
    const bool stop=inputEndPolicy.begin(inputSession->interrupted(),atCompletedFrame);
    if(stop){std::cout<<"; stopping runtime"<<std::endl;runtime->requestStop();}
    else {
        std::cout<<(inputSession->interrupted()?"; running up to 120 more game frames to reach the recorded fault":"; finishing the final input's game frame")
            <<" (5 second maximum)"<<std::endl;
        inputEndDeadline.store(inputNow()+5000000000ll,std::memory_order_release);
    }
}
void inputUpdateAlignmentDeadline() {
    if(!inputSession->aligning())inputAlignmentDeadline.store(0,std::memory_order_release);
    else {
        int64_t empty=0;
        inputAlignmentDeadline.compare_exchange_strong(empty,inputNow()+5000000000ll,std::memory_order_acq_rel);
    }
}
void inputPad(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    const uint32_t port=GPR_U32(cpu,4),slot=GPR_U32(cpu,5),buffer=GPR_U32(cpu,6);
    inputPadOriginal(ram,cpu,runtime);
    if(port>1 || slot>3 || buffer>PS2_RAM_SIZE-32)return;
    // Never fabricate success while the original controller service is still
    // initializing. Failed reads leave the guest buffer and replay cursor alone.
    if(!GPR_U32(cpu,2))return;
    if(!inputSession) {
        InputReplay::Pad pad;pad.port=port;pad.slot=slot;pad.result=GPR_U32(cpu,2);
        std::memcpy(pad.bytes.data(),ram+buffer,32);ReplayHarness::pad(pad,inputContext(ram));
        std::memcpy(ram+buffer,pad.bytes.data(),32);SET_GPR_U32(cpu,2,pad.result);return;
    }
    if(inputSession->ended()) {
        if(!inputReturnControl && inputHavePad[port*4+slot]) {
            const auto& last=inputLastPads[port*4+slot];
            std::memcpy(ram+buffer,last.bytes.data(),32);SET_GPR_U32(cpu,2,last.result);
        }
        return;
    }
    InputReplay::Pad pad;pad.port=port;pad.slot=slot;pad.result=GPR_U32(cpu,2);
    std::memcpy(pad.bytes.data(),ram+buffer,32);
    inputSession->pad(pad,inputContext(ram));
    inputUpdateAlignmentDeadline();
    if(inputSession->mode()==InputReplay::Mode::Replay) {
        std::memcpy(ram+buffer,pad.bytes.data(),32);SET_GPR_U32(cpu,2,pad.result);
        inputLastPads[port*4+slot]=pad;inputHavePad[port*4+slot]=true;
    }
    inputCheckEnd(runtime,false);
}
}
void installInputReplay(PS2Runtime& runtime) {
    const char* record=std::getenv("NIGHT100_INPUT_RECORD"),*replay=std::getenv("NIGHT100_INPUT_REPLAY");
    ReplayHarness::install();
    if(!record && !replay && !ReplayHarness::enabled())return;
    if(record && replay)throw std::runtime_error("Choose input recording or input replay, not both");
    const char* options=std::getenv("NIGHT100_INPUT_OPTIONS");
    if(record || replay)inputSession=std::make_unique<InputReplay::Session>(record?InputReplay::Mode::Record:InputReplay::Mode::Replay,
        record?record:replay,options?options:"");
    inputReturnControl=std::getenv("NIGHT100_INPUT_RETURN_CONTROL")!=nullptr;inputEndReported=false;
    inputHavePad.fill(false);inputEndDeadline.store(0,std::memory_order_release);
    inputAlignmentDeadline.store(0,std::memory_order_release);inputAlignmentTimedOut.store(false,std::memory_order_release);
    inputPadOriginal=runtime.lookupFunction(0x10e218);
    if(!inputPadOriginal)throw std::runtime_error("Missing original pad service for input recording");
    runtime.replaceFunction(0x10e218,inputPad);
    if(inputSession)std::cout<<"[input] "<<(record?"Recording ":"Replaying ")<<(record?record:replay)
        <<" using completed game frames"<<(inputSession->interrupted()?" (interrupted recording)":"")<<std::endl;
}
void inputReplayFrameCompleted(uint8_t* ram,PS2Runtime* runtime) {
    ReplayHarness::completed(ram,runtime);
    if(!inputSession)return;
    if(inputSession->ended()) {
        if(inputEndReported && !inputReturnControl && inputEndPolicy.completed()) {
            inputEndDeadline.store(0,std::memory_order_release);runtime->requestStop();
        }
        return;
    }
    inputSession->completed(inputContext(ram));inputUpdateAlignmentDeadline();inputCheckEnd(runtime,true);
}
void inputReplayWatchdog(PS2Runtime* runtime) {
    ReplayHarness::watchdog(runtime);
    int64_t alignment=inputAlignmentDeadline.load(std::memory_order_acquire);
    if(alignment && inputNow()>=alignment && inputAlignmentDeadline.compare_exchange_strong(alignment,0,std::memory_order_acq_rel)) {
        inputAlignmentTimedOut.store(true,std::memory_order_release);
        std::cout<<"[input] Replay context alignment reached its 5 second limit; stopping runtime"<<std::endl;
        runtime->requestStop();
    }
    int64_t deadline=inputEndDeadline.load(std::memory_order_acquire);
    if(deadline && inputNow()>=deadline && inputEndDeadline.compare_exchange_strong(deadline,0,std::memory_order_acq_rel)) {
        std::cout<<"[input] Replay tail reached its 5 second wall limit; stopping runtime"<<std::endl;
        runtime->requestStop();
    }
}
void finishInputReplay(const std::string& reason) {
    ReplayHarness::finish(reason);
    inputEndDeadline.store(0,std::memory_order_release);
    inputAlignmentDeadline.store(0,std::memory_order_release);
    if(inputSession)inputSession->finish(reason);
    if(inputAlignmentTimedOut.load(std::memory_order_acquire))
        throw std::runtime_error("Input replay failed: context alignment exceeded 5 seconds");
}
#endif
