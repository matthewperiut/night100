#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "runtime/state_archive.h"
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
template<class F>void notReady(F function) {
    bool rejected=false;try{function();}catch(const ps2x::state::NotReady&){rejected=true;}
    require(rejected,"Live host continuation was silently dropped from state");
}
void initialize(PS2Runtime& runtime) {
    require(runtime.memory().initialize() && runtime.syncCoreSubsystems(),"Runtime initialization failed");
    runtime.configureGuestHeap(0x400000,0x1e00000);
    auto& cpu=runtime.cpu();cpu=R5900Context{};cpu.pc=0x1000;cpu.f[17]=-123.25f;cpu.hi=0xabcdef9876543210ull;
    SET_GPR_U32(&cpu,28,0x350000);SET_GPR_U32(&cpu,29,0x1ff8000);SET_GPR_U32(&cpu,31,0x2000);
    runtime.eeScheduler().reset(runtime.memory().getRDRAM(),cpu);
}
std::vector<uint8_t> phaseImage;
unsigned phaseOrder=0;
void priorityEntry(uint8_t*,R5900Context* cpu,PS2Runtime* runtime) {
    auto& scheduler=runtime->eeScheduler();
    EeThreadCreateParams parameters;parameters.entry=0x3000;parameters.stack=0x180000;parameters.stackSize=0x2000;parameters.priority=1;
    const int thread=scheduler.createThread(parameters);
    require(thread>0 && scheduler.startThread(thread,0,*cpu,false)==0,"Phase fixture higher-priority thread did not start");
    cpu->pc=0x1010;
    scheduler.atSafePoint([runtime] {
        ps2x::state::Writer image;runtime->saveCoreState(image);phaseImage=std::move(image.data);runtime->requestStop();
    });
}
void wrongResume(uint8_t*,R5900Context*,PS2Runtime* runtime){phaseOrder=1;runtime->requestStop();}
void priorityResume(uint8_t*,R5900Context*,PS2Runtime* runtime){phaseOrder=2;runtime->requestStop();}
}
int main(){try {
    PS2Runtime source;initialize(source);auto& mem=source.memory();auto& ee=source.eeScheduler();
    const auto allocation=source.guestMalloc(48,32);require(allocation==0x400000,"Unexpected allocator fixture base");
    const auto hole=source.guestMalloc(64,16);source.guestFree(hole);
    for(uint32_t address:{0x11000u,0x100000u,0x1ff0000u})mem.write32(address,address^0x5a1937c2u);
    mem.write32(PS2_SCRATCHPAD_BASE+0x124,0x73a29b4c);
    mem.getVU0Code()[17]=0x53;mem.getVU0Data()[99]=0x18;mem.getVU1Data()[77]=0xa1;
    mem.getGSVRAM()[12345]=0x78;mem.iop_ram[345]=0x17;
    mem.vif0_regs.row[2]=0x1234;mem.vif1_regs.mask=0xf013a244;mem.dma_regs[7].asr1=0x166617;
    mem.m_ioRegisters[0x1000f520]=0x55aacc77;mem.gs_regs.bgcolor=0xa13129;mem.gs_regs.csr.store(0x137);
    mem.m_path3Masked=true;mem.m_path3MaskedFifo={{1,2,3,4},{5,6,7,8}};
    mem.m_vif1PendingPath2ImageQwc=13;mem.m_vif1PendingPath2DirectHl=true;
    PS2Memory::PendingTransfer transfer;transfer.qwc=2;transfer.srcAddr=0x17400;transfer.chainData={0x12,0x41,0x99};
    mem.m_pendingVif0Transfers.push_back(transfer);mem.m_pendingVif1Transfers.push_back(transfer);
    mem.m_completedDmacCauses={1,5};
    // Initialization supplies the R5900's 48 slots; populate an existing slot.
    constexpr size_t tlbSlot=17;
    require(mem.m_tlbEntries.size()==48,"Unexpected initialized TLB size");
    mem.m_tlbEntries[tlbSlot]={0x1100,0x2200,0x3000,true};
    // Leave a real FMAC result pending at its four-cycle writeback boundary.
    const uint32_t program[]={0,(0xau<<21)|(2u<<16)|(1u<<11)|(3u<<6)|0x28u,0,0x2ff,0,0x2ff,0,0x2ff};
    std::memcpy(mem.getVU1Code(),program,sizeof(program));
    auto& vu=source.vu1();vu.state().vf[1][0]=1;vu.state().vf[1][2]=3;vu.state().vf[2][0]=10;vu.state().vf[2][2]=30;
    vu.state().vf[3][0]=-1;vu.state().vf[3][2]=-3;
    vu.execute(mem.getVU1Code(),PS2_VU1_CODE_SIZE,mem.getVU1Data(),PS2_VU1_DATA_SIZE,source.gs(),&mem,0,0,0,1);
    require(vu.state().vf[3][0]==-1,"VU fixture did not retain a pending FMAC write");
    EeThreadCreateParams parameters;parameters.entry=0x3000;parameters.stack=0x150000;parameters.stackSize=0x2000;parameters.priority=3;
    const int tid=ee.createThread(parameters);require(tid>0,"Thread fixture creation failed");
    auto* waiting=ee.thread(tid);waiting->context.f[7]=67.5f;waiting->context.hi1=0x81527;waiting->context.vu0_q=0.125f;
    const int sema=ee.createSemaphore(0,2,0x77,0x14);waiting->status=EeThreadStatus::Waiting;
    waiting->wait={EeWaitReason::Semaphore,EeSemaphoreWait{sema},{}};ee.semaphore(sema)->waiters.push_back(tid);
    const int flag=ee.createEventFlag(0xa7,0x42,0x99);
    source.addEeExitHandler(tid,0x127890,0x5577);source.setEeSyscallOverride(mem.getRDRAM(),0x71,0x145670);
    ee.accountCycles(64);
    const R5900Context savedMain=ee.thread(1)->context,savedWaiting=waiting->context;
    // Strict preflight must cover blocked, resumed, active and queued closures.
    waiting->wait.completion=[](R5900Context&){};notReady([&]{ee.validateStateSerializable();});waiting->wait.completion={};
    waiting->resumeCompletion=[](R5900Context&){};notReady([&]{ee.validateStateSerializable();});waiting->resumeCompletion={};
    GuestInvocation call;call.context.pc=0x160000;call.onComplete=[](const R5900Context&,R5900Context&){};
    waiting->invocations.push_back(call);notReady([&]{ee.validateStateSerializable();});waiting->invocations.clear();
    source.pauseStateClocks();ps2x::state::Writer saved;source.saveCoreState(saved);source.resumeStateClocks();
    PS2Runtime restored;initialize(restored);restored.pauseStateClocks();
    ps2x::state::Reader input(saved.data);restored.loadCoreState(input);input.finish();restored.resumeStateClocks();
    auto& actual=restored.memory();auto& kernel=restored.eeScheduler();
    require(!std::memcmp(&kernel.thread(1)->context,&savedMain,sizeof(savedMain)) &&
        !std::memcmp(&kernel.thread(tid)->context,&savedWaiting,sizeof(savedWaiting)),"EE per-thread CPU/FPU/COP0/VU0 state did not survive a new runtime");
    for(uint32_t address:{0x11000u,0x100000u,0x1ff0000u})require(actual.read32(address)==(address^0x5a1937c2u),"EE RAM did not survive a new runtime");
    require(actual.read32(PS2_SCRATCHPAD_BASE+0x124)==0x73a29b4c && actual.getVU0Code()[17]==0x53 && actual.getVU0Data()[99]==0x18 && actual.getVU1Data()[77]==0xa1,"Scratchpad or VU memories were omitted");
    require(actual.getGSVRAM()[12345]==0x78 && actual.iop_ram[345]==0x17 && actual.gs_regs.bgcolor==0xa13129 && actual.gs_regs.csr.load()==0x137,"VRAM or hardware registers were omitted");
    require(actual.vif0_regs.row[2]==0x1234 && actual.vif1_regs.mask==0xf013a244 && actual.dma_regs[7].asr1==0x166617 && actual.m_ioRegisters[0x1000f520]==0x55aacc77,"VIF/DMA/MMIO state was omitted");
    require(actual.m_path3Masked && actual.m_path3MaskedFifo==mem.m_path3MaskedFifo && actual.m_vif1PendingPath2ImageQwc==13 && actual.m_vif1PendingPath2DirectHl,"GIF/VIF continuation state was omitted");
    for(const auto* queue:{&actual.m_pendingVif0Transfers,&actual.m_pendingVif1Transfers}) {
        require(queue->size()==1,"Pending DMA queue length changed");
        const auto& pending=queue->front();
        require(pending.fromScratchpad==transfer.fromScratchpad && pending.srcAddr==transfer.srcAddr &&
            pending.qwc==transfer.qwc && pending.chainData==transfer.chainData,"Pending DMA work changed");
    }
    require(actual.m_completedDmacCauses==mem.m_completedDmacCauses,"Pending DMAC interrupt causes changed");
    require(actual.m_tlbEntries.size()==mem.m_tlbEntries.size(),"Restored TLB slot count changed");
    for(size_t index=0;index<mem.m_tlbEntries.size();++index) {
        const auto& expected=mem.m_tlbEntries[index];const auto& entry=actual.m_tlbEntries[index];
        require(entry.vpn==expected.vpn && entry.pfn==expected.pfn && entry.mask==expected.mask &&
            entry.valid==expected.valid,"Restored TLB entry changed");
    }
    require(actual.m_tlbEntries[tlbSlot].valid && actual.m_tlbEntries[tlbSlot].pfn==0x2200,"Nonzero TLB fixture was lost");
    {
        auto abi=PS2Runtime::coreStateAbi();require(abi.size()>100,"Core ABI fingerprint omits POD layouts");++abi[0];
        ps2x::state::Writer incompatible;incompatible(uint32_t(2),abi);
        ps2x::state::Reader bad(incompatible.data);bool rejected=false;
        try{restored.loadCoreState(bad);}catch(const std::runtime_error&){rejected=true;}
        require(rejected && actual.read32(0x11000)==(0x11000u^0x5a1937c2u),"ABI mismatch mutated guest state before rejection");
    }
    require(restored.guestMalloc(64,16)==source.guestMalloc(64,16),"Restored allocator chose a different free block");
    uint32_t handler=0;require(restored.findEeSyscallOverride(0x71,handler) && handler==0x145670,"EE syscall override state was omitted");
    const auto exits=restored.takeEeExitHandlers(tid);require(exits.size()==1 && exits[0].function==0x127890 && exits[0].argument==0x5577,"EE exit handler state was omitted");
    require(kernel.eventFlag(flag)->bits==0xa7 && kernel.eventFlag(flag)->attr==0x42,"EE event flags were omitted");
    require(kernel.semaphore(sema)->waiters.size()==1 && kernel.signalSemaphore(sema,false)==sema &&
        kernel.semaphore(sema)->waiters.empty() && kernel.thread(tid)->status==EeThreadStatus::Ready,
        "Restored EE waiter could not resume through its real semaphore");
    vu.resume(mem.getVU1Code(),PS2_VU1_CODE_SIZE,mem.getVU1Data(),PS2_VU1_DATA_SIZE,source.gs(),&mem,0,0,3);
    restored.vu1().resume(actual.getVU1Code(),PS2_VU1_CODE_SIZE,actual.getVU1Data(),PS2_VU1_DATA_SIZE,restored.gs(),&actual,0,0,3);
    require(vu.state().vf[3][0]==11 && restored.vu1().state().vf[3][0]==11 && restored.vu1().state().vf[3][2]==33,"VU pipeline state lost an in-flight write during restore");
    ps2x::state::Writer expectedVu,actualVu;vu.state(expectedVu);restored.vu1().state(actualVu);
    require(expectedVu.data==actualVu.data,"Resumed VU canonical pipelines differ from uninterrupted execution");
    // A saved machine's logical age can exceed the new host's entire uptime.
    // Rebasing must preserve fractional credit without unsigned epoch wrap.
    {
        constexpr uint64_t logicalAge=1000000000000000000ull;
        const uint64_t deadline=(logicalAge/1000000000)*ps2x::iop::IopHostClock::EeClockHz;
        ps2x::state::Writer clockImage;clockImage(true,logicalAge,deadline);
        // The host runtime selects the mode before loading; the image's
        // realtime flag must not override it, so configure realtime first.
        ps2x::iop::IopHostClock clock;clock.setRealtime(true);clock.pause();ps2x::state::Reader reader(clockImage.data);clock.state(reader);reader.finish();
        ps2x::state::Writer beforePause;clock.state(beforePause);std::this_thread::sleep_for(std::chrono::milliseconds(2));
        ps2x::state::Writer afterPause;clock.state(afterPause);
        require(beforePause.data==clockImage.data && afterPause.data==clockImage.data,"Snapshot wall time advanced the frozen IOP clock");
        require(clock.advance(900000,ps2x::iop::IopHostClock::monotonicNanoseconds())==0,"Paused IOP clock delivered device credit");
        clock.resume();require(clock.advance(900000,ps2x::iop::IopHostClock::monotonicNanoseconds())==0,"Restored IOP clock charged snapshot/host uptime as elapsed game time");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const auto credit=clock.advance(0,ps2x::iop::IopHostClock::monotonicNanoseconds());
        require(credit>=ps2x::iop::IopHostClock::MinimumAdvance && credit<=ps2x::iop::IopHostClock::MaximumAdvance,"Restored IOP clock did not resume bounded real-time service");
        // A deterministic host keeps its EE-cycle budget when a realtime image is loaded.
        ps2x::iop::IopHostClock deterministic;ps2x::state::Reader modeReader(clockImage.data);deterministic.state(modeReader);modeReader.finish();
        require(!deterministic.realtime() && deterministic.advance(900,0)==900,"Snapshot clock mode overrode the configured deterministic budget");
    }
    {
        PS2Runtime clocks;initialize(clocks);
        unsigned pauses=0,resumes=0,exclusions=0;uint64_t excluded=0;
        clocks.setStateClockCallbacks([&]{++pauses;},[&]{++resumes;},
            [&](uint64_t ns){++exclusions;excluded+=ns;});
        struct Deadline {uint64_t cycle=0;int64_t relative=0;EeEvent event{};uint64_t sequence=0;};
        auto deadline=[&] {
            ps2x::state::Writer image;clocks.eeScheduler().saveState(image);
            // A fresh scheduler has one VBlank event, the final schema record.
            constexpr size_t bytes=8+8+8+1+4+8+8;
            require(image.data.size()>=bytes,"EE deadline fixture archive is truncated");
            ps2x::state::Reader ar(std::span<const uint8_t>(image.data).last(bytes));
            uint64_t count=0;Deadline d;ar(count,d.cycle,d.relative,d.event,d.sequence);ar.finish();
            require(count==1 && d.event.type==EeEventType::VBlankStart,"EE deadline fixture is not the first VBlank");
            return d;
        };
        const auto before=deadline();
        constexpr uint64_t gap=3600ull*1000000000;
        clocks.excludeHostTime(gap);
        const auto after=deadline();
        require(after.cycle==before.cycle && after.sequence==before.sequence,
                "host exclusion changed deterministic EE deadline ordering/cycles");
        require(after.relative-before.relative>int64_t(gap)-100000000 &&
                after.relative-before.relative<=int64_t(gap),"EE host deadline did not shift by inactive time");
        require(exclusions==1 && excluded==gap,"native clock did not receive exactly one inactive interval");
        clocks.pauseStateClocks();clocks.pauseStateClocks();
        const auto paused=deadline();
        clocks.excludeHostTime(gap);
        clocks.eeScheduler().excludeHostTime(gap);
        const auto stillPaused=deadline();
        require(stillPaused.relative==paused.relative && exclusions==1,
                "movie/snapshot pause double-excluded background time");
        clocks.resumeStateClocks();clocks.excludeHostTime(gap);
        require(resumes==0 && exclusions==1,"nested resume exposed a still-paused clock");
        clocks.resumeStateClocks();
        require(pauses==1 && resumes==1,"host exclusion changed state-clock pause depth");
        clocks.excludeHostTime(0);require(exclusions==1,"zero interval changed native clock epoch");
        clocks.excludeHostTime(1000);require(exclusions==2 && excluded==gap+1000,
                "clock did not accept the next distinct completed inactive interval");
        bool rejected=false;
        try {clocks.excludeHostTime(UINT64_MAX);}catch(const std::runtime_error&){rejected=true;}
        require(rejected && exclusions==2,"unrepresentable interval changed native clock before rejection");
    }
    {
        PS2Runtime phase;initialize(phase);
        require(phase.registerFunction(0x1000,priorityEntry) && phase.registerFunction(0x1010,wrongResume) && phase.registerFunction(0x3000,priorityResume),"Dispatcher phase fixture registration failed");
        int oldPriority=0;require(phase.eeScheduler().changePriority(1,10,false,oldPriority)==0,"Phase fixture main priority change failed");
        phase.eeScheduler().run();require(!phaseImage.empty(),"Dispatcher did not execute the requested state operation");
        PS2Runtime continuation;initialize(continuation);continuation.pauseStateClocks();
        ps2x::state::Reader image(phaseImage);continuation.loadCoreState(image);image.finish();continuation.resumeStateClocks();
        continuation.eeScheduler().run();
        require(phaseOrder==2,"Cold restore executed the old guest continuation before pending preemption");
    }
    std::cout<<"PASS: fresh-runtime core restore, full EE contexts, RAM/scratch/VU/VRAM/MMIO, DMA/VIF queues, allocator/kernel objects, strict closure preflight and in-flight VU pipeline parity\n";
    return 0;
}catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}}
