#include "input_replay.h"
#include "replay_clock.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

#if !defined(__SWITCH__)
#error This test must exercise the Switch buffering/publication branches.
#endif
namespace {
unsigned checks=0;
void check(bool value,const char* reason){++checks;if(!value)throw std::runtime_error(reason);}
InputReplay::Pad controls(unsigned frame) {
    InputReplay::Pad pad;pad.result=1;pad.bytes[2]=255;pad.bytes[3]=uint8_t(frame);
    for(unsigned i=4;i<8;++i)pad.bytes[i]=uint8_t(100+frame+i);
    return pad;
}
}
int main() {
    const auto root=std::filesystem::temp_directory_path()/("night100-switch-storage-"+std::to_string(getpid()));
    try {
        check(std::filesystem::create_directory(root),"isolated directory required");
        const auto inputs=root/"input.inputs",ticks=root/"clock.bin";
        const InputReplay::Context context{11,1,2,0x48303031};
        InputReplay::Session journal(InputReplay::Mode::Record,inputs,"fixture");
        ReplayClock clock(InputReplay::Mode::Record,ticks);
        for(unsigned frame=0;frame<60;++frame) {
            auto pad=controls(frame);journal.pad(pad,context);clock.sample(frame,1000+frame*13);
            journal.completed(context);clock.completed(frame+1);
        }
        // A reader in another process must see the completed batch even while
        // both original streams remain open. No destructor can flush for us.
        {
            InputReplay::Session durable(InputReplay::Mode::Replay,inputs,"fixture");
            check(durable.interrupted(),"live journal must retain its incomplete status");
            for(unsigned frame=0;frame<60;++frame) {
                InputReplay::Pad pad;durable.pad(pad,context);
                check(pad==controls(frame),"batched live journal lost ordered input");
                durable.completed(context);
            }
            check(durable.ended() && durable.frame()==60,"sixtieth completed frame was not durable");
        }
        check(std::filesystem::file_size(ticks)==20+60*21,"sixtieth completion did not flush its clock samples");
        auto last=controls(60);journal.pad(last,context);clock.sample(60,1780);
        journal.completed(context);clock.completed(61);
        journal.finish();clock.finish(61);
        // Finishing one frame after the batch must publish the tail and both
        // complete footers immediately, rather than waiting for frame 120.
        InputReplay::Session replay(InputReplay::Mode::Replay,inputs,"fixture");
        ReplayClock replayClock(InputReplay::Mode::Replay,ticks);
        check(!replay.interrupted(),"explicit finish did not publish the complete journal footer");
        for(unsigned frame=0;frame<61;++frame) {
            InputReplay::Pad pad;replay.pad(pad,context);
            check(pad==controls(frame),"finished journal changed a recorded control");
            check(replayClock.sample(frame,0)==1000+frame*13,"finished clock lost exact recorded ticks");
            replay.completed(context);replayClock.completed(frame+1);
        }
        replayClock.finish(61);check(replay.ended(),"finished session has extra or missing frames");
        {
            InputReplay::Session shortJournal(InputReplay::Mode::Record,root/"short.inputs","");
            auto pad=controls(7);shortJournal.pad(pad,context);shortJournal.completed(context);
            shortJournal.savePrefix(root/"prefix.inputs");
            InputReplay::Session prefix(InputReplay::Mode::Replay,root/"prefix.inputs","");
            InputReplay::Pad actual;prefix.pad(actual,context);prefix.completed(context);
            check(actual==pad && prefix.ended() && !prefix.interrupted(),"prefix did not flush and capture a sub-batch journal");
            check(!shortJournal.ended(),"prefix finished the live recorder");
            shortJournal.finish();
        }
        std::filesystem::remove_all(root);std::cout<<"Switch replay storage checks: "<<checks<<" passed\n";return 0;
    }catch(const std::exception& error) {
        std::filesystem::remove_all(root);std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;
    }
}
