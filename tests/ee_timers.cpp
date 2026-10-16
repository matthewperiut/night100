#include "runtime/ps2_memory.h"
#include "runtime/state_archive.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

namespace {
constexpr uint64_t clockHz = 294912000;
constexpr std::array<uint64_t, 4> frequencies{147456000, 9216000, 576000, 15734};
constexpr uint32_t cue=0x80, zret=0x40, cmpe=0x100, ovfe=0x200, equf=0x400, ovff=0x800;
void require(bool value, const char *message) { if(!value) throw std::runtime_error(message); }

// An integer-wide rational clock oracle; unlike production this directly
// multiplies the complete cycle count without splitting into seconds or shifts.
uint64_t clockAdvance(PS2Memory::EeTimer &timer, uint64_t cycles) {
    const auto scaled=static_cast<unsigned __int128>(cycles)*frequencies[timer.mode&3]+timer.clockRemainder;
    timer.clockRemainder=static_cast<uint64_t>(scaled%clockHz);
    return static_cast<uint64_t>(scaled/clockHz);
}

// Literal tick stepping is an independent oracle for count/reset/interrupt order.
// For huge advances, step two complete 16-bit periods first; all reachable first
// events have then happened, and complete periodic cycles can be omitted.
uint32_t stepTimer(PS2Memory::EeTimer &timer, uint64_t cycles, unsigned index) {
    if(!cycles || !(timer.mode&cue)) return 0;
    uint64_t ticks=clockAdvance(timer, cycles);
    uint32_t interrupt=0;
    auto step=[&] {
        const uint32_t next=(timer.count+1)&0xffff;
        if(!next && (timer.mode&ovfe) && !(timer.mode&ovff)) {
            timer.mode|=ovff;interrupt|=1u<<index;
        }
        timer.count=next;
        if(next==(timer.compare&0xffff)) {
            if((timer.mode&cmpe) && !(timer.mode&equf)) {
                timer.mode|=equf;interrupt|=1u<<index;
            }
            if(timer.mode&zret) timer.count=0;
        }
    };
    const uint64_t prefix=std::min<uint64_t>(ticks,131072);
    for(uint64_t tick=0;tick<prefix;++tick) step();
    ticks-=prefix;
    const uint64_t period=(timer.mode&zret) && (timer.compare&0xffff) ? (timer.compare&0xffff) : 65536;
    ticks%=period;
    for(uint64_t tick=0;tick<ticks;++tick) step();
    return interrupt;
}

void equal(const PS2Memory::EeTimer &actual, const PS2Memory::EeTimer &expected) {
    require(actual.count==expected.count,"Timer count differs from tick oracle");
    require(actual.mode==expected.mode,"Timer event flags differ from tick oracle");
    require(actual.clockRemainder==expected.clockRemainder,"Timer rational phase was lost");
    require(actual.compare==expected.compare && actual.hold==expected.hold,"Timer configuration changed");
}
uint64_t cases=0;
void check(PS2Memory &memory,uint64_t cycles) {
    auto expected=memory.m_eeTimers;
    uint32_t mask=0;
    for(unsigned index=0;index<4;++index)mask|=stepTimer(expected[index],cycles,index);
    require(memory.advanceEeTimers(cycles)==mask,"Timer interrupt delivery changed at a checkpoint");
    for(unsigned index=0;index<4;++index)equal(memory.m_eeTimers[index],expected[index]);
    ++cases;
}
}

int main() { try {
    PS2Memory memory;
    std::mt19937_64 random(0x7e010d7);
    // Exercise canonical but arbitrary saved fractional phases, every config,
    // pre-existing flags, disabled clocks, zero/wrap compares and all four clocks.
    for(unsigned config=0;config<1024;++config) {
        for(unsigned variant=0;variant<4;++variant) {
            for(unsigned index=0;index<4;++index) {
                auto &timer=memory.m_eeTimers[index];
                timer.count=variant==0?65535:random()&0xffff;
                timer.mode=config|(variant<<10);
                timer.compare=variant==0?0:variant==1?timer.count:random()&0xffff;
                timer.hold=0xcafe + index;
                timer.clockRemainder=random()%clockHz;
            }
            check(memory,random()%67);
            check(memory,0);
        }
    }
    const std::array<uint64_t,10> advances{1,2,31,32,511,512,294911999,294912000,
        std::numeric_limits<uint64_t>::max()-1,std::numeric_limits<uint64_t>::max()};
    for(unsigned mode=0;mode<4;++mode)for(unsigned variant=0;variant<12;++variant) {
        for(auto cycles:advances) {
            for(auto &timer:memory.m_eeTimers) {
                timer={};timer.mode=mode|cue|cmpe|ovfe|(variant&1?zret:0);
                timer.count=variant<2?65535:random()&65535;
                timer.compare=variant<2?0:random()&65535;
                timer.clockRemainder=variant<3?variant:random()%clockHz;
            }
            check(memory,cycles);
        }
    }
    // Segments must preserve phase/count. IRQs remain per-checkpoint: accumulate
    // delivered masks without clearing sticky flags between calls.
    for(unsigned mode=0;mode<4;++mode) {
        for(auto &timer:memory.m_eeTimers) {
            timer={};timer.mode=mode|cue|cmpe|ovfe;timer.compare=123;
            timer.clockRemainder=clockHz-1;
        }
        const auto initial=memory.m_eeTimers;
        uint32_t segmentedMask=0;
        for(unsigned segment=0;segment<80;++segment)segmentedMask|=memory.advanceEeTimers(13);
        const auto segmented=memory.m_eeTimers;
        memory.m_eeTimers=initial;
        require(memory.advanceEeTimers(1040)==segmentedMask,"Segmented checkpoint IRQs changed");
        for(unsigned index=0;index<4;++index)equal(memory.m_eeTimers[index],segmented[index]);
        // Serialize and restore a fractional phase, then keep checking the oracle.
        ps2x::state::Writer saved;saved.pod(memory.m_eeTimers);
        memory.m_eeTimers={};ps2x::state::Reader reader(saved.data);reader.pod(memory.m_eeTimers);reader.finish();
        check(memory,34567);
        // Changing clock / enable clears phase; other mode writes preserve it.
        memory.m_eeTimers[0].clockRemainder=17;
        const uint32_t current=memory.m_eeTimers[0].mode;
        memory.writeIORegister(0x10000010,current^zret);
        require(memory.m_eeTimers[0].clockRemainder==17,"Non-clock mode write reset phase");
        memory.writeIORegister(0x10000010,(current^1u));
        require(memory.m_eeTimers[0].clockRemainder==0,"Clock mode change retained old phase");
        check(memory,1234);
    }
    // Noncanonical uint64 phases preserve the legacy unsigned-wrap expression.
    // This is compatibility coverage rather than the physical rational oracle.
    for(unsigned mode=0;mode<4;++mode)for(uint64_t phase:{clockHz,UINT64_MAX}) {
        memory.m_eeTimers={};auto &timer=memory.m_eeTimers[0];
        timer.mode=mode|cue;timer.clockRemainder=phase;
        const uint64_t cycles=UINT64_MAX;
        const uint64_t scaled=(cycles%clockHz)*frequencies[mode]+phase;
        const uint64_t ticks=(cycles/clockHz)*frequencies[mode]+scaled/clockHz;
        require(memory.advanceEeTimers(cycles)==0,"Disabled IRQ unexpectedly delivered");
        require(timer.count==(ticks&65535) && timer.clockRemainder==scaled%clockHz,
                "Noncanonical saved phase compatibility changed");
    }
    // Existing GIF FIFO completion boundary is intentionally unchanged.
    memory.m_ioRegisters[0x10003020]=0x1f000123;
    memory.advanceEeTimers(0);
    require(memory.m_ioRegisters[0x10003020]==0x1f000123,"Zero checkpoint cleared GIF FIFO early");
    memory.advanceEeTimers(1);
    require(memory.m_ioRegisters[0x10003020]==0x123,"GIF FIFO completion boundary changed");
    // Checkpoint FIFO drain uses canonical MMIO storage, including changes
    // after a saved machine replaces all map nodes. A cached host address must
    // never become serialized state or survive that replacement.
    {
        PS2Memory fifo;require(fifo.initialize(),"FIFO cache fixture initialization failed");
        constexpr uint32_t gif=0x10003020u,fqc=0x1f000000u,other=0xa01234u;
        ps2x::state::Writer absent;fifo.state(absent);
        fifo.advanceEeTimers(1);
        require(!fifo.m_ioRegisters.contains(gif),"Checkpoint inserted an absent GIF_STAT register");
        fifo.writeIORegister(gif,fqc|other);
        fifo.advanceEeTimers(0);
        require(fifo.m_ioRegisters.at(gif)==(fqc|other),"Zero advance drained a newly inserted FIFO");
        fifo.advanceEeTimers(1);
        require(fifo.m_ioRegisters.at(gif)==other,"Cached FIFO drain damaged unrelated status bits");
        const auto buckets=fifo.m_ioRegisters.bucket_count();
        fifo.m_ioRegisters.reserve(8192);
        require(fifo.m_ioRegisters.bucket_count()>buckets,"Fixture did not force MMIO rehash");
        fifo.writeIORegister(gif,(7u<<24)|other);fifo.advanceEeTimers(1);
        require(fifo.m_ioRegisters.at(gif)==other,"MMIO rehash invalidated the cached FIFO register");
        fifo.writeIORegister(gif,(3u<<24)|other);
        ps2x::state::Writer queued;fifo.state(queued);
        fifo.advanceEeTimers(1);
        {
            ps2x::state::Reader restore(queued.data);fifo.state(restore);restore.finish();
        }
        require(fifo.m_ioRegisters.at(gif)==((3u<<24)|other),"Queued FIFO status was not restored");
        fifo.advanceEeTimers(1);
        require(fifo.m_ioRegisters.at(gif)==other,"Restored GIF_STAT retained queued FIFO bits");
        {
            ps2x::state::Reader restore(absent.data);fifo.state(restore);restore.finish();
        }
        fifo.advanceEeTimers(1);
        require(!fifo.m_ioRegisters.contains(gif),"Loading absent GIF_STAT retained a stale cache node");
        fifo.writeIORegister(gif,(9u<<24)|other);fifo.advanceEeTimers(1);
        require(fifo.m_ioRegisters.at(gif)==other,"Restored absent register did not admit later insertion");
        require(fifo.initialize(),"FIFO cache reinitialization failed");
        fifo.advanceEeTimers(1);
        require(!fifo.m_ioRegisters.contains(gif),"Reinitialization retained old GIF_STAT");
        fifo.writeIORegister(gif,(11u<<24)|other);fifo.advanceEeTimers(1);
        require(fifo.m_ioRegisters.at(gif)==other,"Reinitialized FIFO cache points at an old map node");
        // A truncated Reader may throw after the map was cleared/replaced.
        // Cache invalidation must happen before any archive mutation.
        fifo.writeIORegister(gif,(13u<<24)|other);
        auto truncated=queued.data;truncated.resize(truncated.size()-1);
        bool rejected=false;
        try {ps2x::state::Reader restore(truncated);fifo.state(restore);}
        catch(const std::runtime_error&){rejected=true;}
        require(rejected,"Truncated memory image was accepted");
        fifo.advanceEeTimers(1);
        require(fifo.m_ioRegisters.at(gif)==other,"Partial failed load retained a freed cached map node");
    }
    // Deferred scheduler checkpoints must match eager advances at every
    // observation: interrupt bits, COUNT, phase, register reads, cycle queries,
    // GIF FIFO status and the saved archive.
    {
        std::mt19937_64 rng(0xdef3e7edull);
        auto pick=[&](uint64_t bound){return bound?rng()%bound:uint64_t{0};};
        auto sameTimers=[&](PS2Memory &actual,PS2Memory &expected,const char *message) {
            for(unsigned index=0;index<4;++index) {
                const auto &a=actual.m_eeTimers[index], &e=expected.m_eeTimers[index];
                require(a.count==e.count && a.mode==e.mode && a.compare==e.compare &&
                        a.hold==e.hold && a.clockRemainder==e.clockRemainder, message);
            }
        };
        auto writeReg=[](PS2Memory &memory,unsigned index,uint32_t offset,uint32_t value) {
            memory.writeIORegister(0x10000000u+index*0x800u+offset,value);
        };
        for(unsigned trial=0;trial<1500;++trial) {
            PS2Memory eager,deferred;
            // Archive probes need initialized memory; they are sampled sparsely.
            if(trial%100==0)require(eager.initialize()&&deferred.initialize(),"Timer archive fixture failed");
            std::array<uint32_t,4> counts{},modes{},compares{};
            for(unsigned index=0;index<4;++index) {
                counts[index]=static_cast<uint32_t>(pick(65536));
                compares[index]=pick(4)==0?0u:pick(4)==0?counts[index]:static_cast<uint32_t>(pick(65536));
                modes[index]=static_cast<uint32_t>(pick(1024))|(pick(5)?cue:0u);
            }
            for(PS2Memory *memory:{&eager,&deferred}) {
                for(unsigned index=0;index<4;++index) {
                    writeReg(*memory,index,0x00u,counts[index]);
                    writeReg(*memory,index,0x20u,compares[index]);
                    writeReg(*memory,index,0x10u,modes[index]);
                }
                memory->write32(0x10003020u,0x1f000000u|0x55u);
            }
            const unsigned segments=60+static_cast<unsigned>(pick(140));
            for(unsigned segment=0;segment<segments;++segment) {
                const uint64_t roll=pick(10);
                uint64_t cycles=roll<6?1+pick(40):roll<9?1+pick(70000):1+pick(uint64_t{1}<<31);
                // Exact interrupt horizons test both sides of the deferral bound.
                const uint64_t horizon=eager.cyclesUntilNextEeTimerInterrupt();
                if(horizon!=std::numeric_limits<uint64_t>::max() && horizon>1 && pick(3)==0)
                    cycles=horizon+pick(3)-1;
                const uint32_t expectedMask=eager.advanceEeTimers(cycles);
                require(deferred.accumulateEeTimerCycles(cycles)==expectedMask,
                        "Deferred checkpoint changed interrupt timing");
                require(deferred.m_ioRegisters.count(0x10003020u)==eager.m_ioRegisters.count(0x10003020u) &&
                        (!eager.m_ioRegisters.count(0x10003020u) ||
                         deferred.m_ioRegisters.at(0x10003020u)==eager.m_ioRegisters.at(0x10003020u)),
                        "Deferred checkpoint changed GIF FIFO status");
                const unsigned index=static_cast<unsigned>(pick(4));
                switch(pick(48)) {
                case 0: { // COUNT and MODE reads observe exact deferred state.
                    const uint32_t base=0x10000000u+index*0x800u;
                    require(deferred.readIORegister(base)==eager.readIORegister(base),"Deferred COUNT read is stale");
                    require(deferred.readIORegister(base+0x10u)==eager.readIORegister(base+0x10u),"Deferred MODE read is stale");
                    break; }
                case 1: { // Mode writes apply earlier time under the old mode.
                    const uint32_t value=static_cast<uint32_t>(pick(1024))|(pick(4)?cue:0u);
                    writeReg(eager,index,0x10u,value);
                    writeReg(deferred,index,0x10u,value);
                    break; }
                case 2: { // Count writes also reset phase after deferred cycles.
                    const uint32_t value=static_cast<uint32_t>(pick(65536));
                    writeReg(eager,index,0x00u,value);
                    writeReg(deferred,index,0x00u,value);
                    break; }
                case 3: { // Scheduler wait path asks for the exact next interrupt distance.
                    deferred.flushEeTimers();
                    require(deferred.takeEeTimerInterrupts()==0u,"Flush produced an interrupt without a checkpoint");
                    require(deferred.cyclesUntilNextEeTimerInterrupt()==eager.cyclesUntilNextEeTimerInterrupt(),
                            "Deferred next-interrupt distance changed");
                    break; }
                case 4: { // GIF FQC is still cleared at the checkpoint, not at timer flushes.
                    const uint32_t value=0x1f000000u|static_cast<uint32_t>(pick(1u<<24));
                    eager.write32(0x10003020u,value);
                    deferred.write32(0x10003020u,value);
                    break; }
                default:
                    break;
                }
            }
            deferred.flushEeTimers();
            require(deferred.takeEeTimerInterrupts()==0u,"Final flush produced an interrupt without a checkpoint");
            sameTimers(deferred,eager,"Deferred timer state differs at trial end");
            if(trial%100==0) {
                ps2x::state::Writer saved;
                deferred.state(saved);
                sameTimers(deferred,eager,"Saving a deferred timer changed its archived state");
            }
            ++cases;
        }
    }
    std::cout<<"GIF FIFO cache: absent/insertion, zero advance, rehash, archive replacement and failure, reinitialization passed\n";
    std::cout<<"EE timers: "<<cases<<" independent checkpoint cases passed\n";
    return 0;
} catch(const std::exception &error) {
    std::cerr<<error.what()<<'\n';return 1;
} }
