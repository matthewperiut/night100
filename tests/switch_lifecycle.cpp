#include "switch_lifecycle.h"
#include "input_chords.h"
#include <cassert>
#include <thread>

int main() {
    SwitchLifecycle::State state;
    state.reset(100,true);state.prime(100);
    assert(state.active() && !state.needsService());
    assert(state.consume()==0);
    state.setFocus(1000,false);
    assert(!state.active() && state.needsService());
    assert(state.inactiveNanoseconds(1500)==500);
    assert(state.consume()==0); // Never consume an incomplete interval.
    state.setFocus(2000,true);
    assert(state.active() && state.needsService());
    assert(state.consume()==1000);
    assert(state.consume()==0 && !state.needsService());
    // A complete sleep can occur before the producer next executes.
    state.setFocus(3000,false);state.setFocus(9000000000000ull,true);
    assert(state.consume()==9000000000000ull-3000);
    // Duplicate focus/resume notifications neither reopen nor duplicate gaps.
    state.setFocus(9000000000001ull,true);
    assert(!state.needsService() && state.consume()==0);
    const auto before=state.inactiveNanoseconds(9000000000002ull);
    state.setFocus(9000000000010ull,false);state.setFocus(9000000000020ull,false);
    state.setFocus(9000000000030ull,true);
    assert(state.consume()==20);
    assert(state.inactiveNanoseconds(9000000000040ull)==before+20);
    // Producer/main interleavings must not lose pending transitions or count
    // gaps twice. No timing assumptions: consume only complete publications.
    state.reset(0,true);state.prime(0);
    std::atomic<bool> done{false};
    std::thread publisher([&] {
        for(uint64_t i=0;i<10000;++i) {
            state.setFocus(i*10,false);state.setFocus(i*10+7,true);
        }
        done.store(true,std::memory_order_release);
    });
    uint64_t total=0;
    while(!done.load(std::memory_order_acquire))total+=state.consume();
    publisher.join();total+=state.consume();
    assert(total==70000 && !state.needsService());
    // Bootstrap establishes its own epoch, without counting pre-game history.
    state.reset(0,true);state.setFocus(1,false);state.setFocus(100,true);
    state.prime(100);assert(state.consume()==0);
    // Rapid HOME/return/HOME must require fresh worker acknowledgments: a
    // parked worker from the first background interval cannot authorize the next.
    state.setFocus(200,false);const auto oldAck=state.inactiveGeneration();
    assert(oldAck!=0);
    state.setFocus(201,true);assert(state.inactiveGeneration()==0);
    state.setFocus(202,false);const auto nextAck=state.inactiveGeneration();
    assert(nextAck!=0 && oldAck!=nextAck);
    state.setFocus(203,true);assert(state.consume()==2);
    state.reset(100,false);state.prime(1000);
    state.setFocus(2000,true);assert(state.consume()==1000);
    SwitchInput::HoldToggle chord;
    assert(!chord.update(true,0));
    assert(!chord.update(true,2999999999ull));
    assert(chord.update(true,3000000000ull));
    assert(!chord.update(true,9000000000ull));
    assert(!chord.update(false,9000000001ull));
    assert(!chord.update(true,10000000000ull));
    assert(chord.update(true,13000000000ull));
    assert(!chord.update(true,14000000000ull,false));
    assert(!chord.update(true,15000000000ull));
    assert(!chord.update(true,17999999999ull));
    assert(chord.update(true,18000000000ull));
}
