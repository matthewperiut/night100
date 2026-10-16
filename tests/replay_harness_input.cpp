#include "replay_harness.h"
#include "frame_telemetry.h"
#include "replay_catalog.h"
#include "replay_clock.h"
#include <iostream>
#include <stdexcept>
#include <unistd.h>

// This test never constructs a renderer or calls completed(). Fail if its
// input-only execution unexpectedly starts querying GPU statistics.
GL33Backend::Statistics GL33Backend::statistics() const {
    throw std::runtime_error("Input-only replay test queried the GPU");
}

int main() {
    const auto root=std::filesystem::temp_directory_path()/("night100-harness-input-"+std::to_string(getpid()));
    try {
        const auto entry=ReplayCatalog::reserve(root);
        ReplayCatalog::Metadata metadata;metadata.id=entry.id;metadata.complete=true;metadata.endFrame=1;metadata.options="fixture";
        ReplayCatalog::save(entry,metadata);
        InputReplay::Context context{11,1,2,0x48303031};
        InputReplay::Pad recorded;recorded.result=1;
        for(unsigned i=0;i<recorded.bytes.size();++i)recorded.bytes[i]=uint8_t(10+i*3);
        {
            InputReplay::Session journal(InputReplay::Mode::Record,entry.directory/"input.inputs",metadata.options);
            auto copy=recorded;journal.pad(copy,context);journal.pad(copy,context);journal.completed(context);journal.finish();
            ReplayClock clock(InputReplay::Mode::Record,entry.directory/"clock.bin");clock.finish(1);
        }
        ReplayHarness::Settings settings;settings.enabled=true;settings.directory=root;settings.options=metadata.options;settings.replay=entry.directory;
        ReplayHarness::configure(settings,nullptr);ReplayHarness::install();
        auto actions=ReplayHarness::actions();actions.setPickerOpen(true);
        InputReplay::Pad actual;ReplayHarness::pad(actual,context);
        if(actual!=recorded)throw std::runtime_error("Opening the picker replaced immutable recorded input");
        actions.setPickerOpen(false);actual={};ReplayHarness::pad(actual,context);
        if(actual!=recorded)throw std::runtime_error("Picker closing neutralization replaced immutable recorded input");
        settings.replay.clear();ReplayHarness::configure(settings,nullptr);
        actions=ReplayHarness::actions();actions.setPickerOpen(true);actual=recorded;ReplayHarness::pad(actual,context);
        if(actual.bytes[2]!=255 || actual.bytes[3]!=255 || actual.bytes[4]!=128 || actual.bytes[8]!=0)
            throw std::runtime_error("Physical picker controls leaked into live gameplay");
        actions.setPickerOpen(false);actions.startRecording();
        bool rejected=false;try{actions.recordings();}catch(const std::runtime_error&){rejected=true;}
        if(!rejected)throw std::runtime_error("Pending snapshot allowed the picker to interrupt recording startup");
        rejected=false;try{actions.replay(entry.directory);}catch(const std::runtime_error&){rejected=true;}
        if(!rejected)throw std::runtime_error("Pending snapshot allowed a replay restart during recording startup");
        ReplayHarness::configure({},nullptr);std::filesystem::remove_all(root);
        std::cout<<"Replay harness input checks: 5 passed\n";return 0;
    }catch(const std::exception& error){std::filesystem::remove_all(root);std::cerr<<error.what()<<'\n';return 1;}
}
