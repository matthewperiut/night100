#include "replay_ui.h"
#include "switch_pgo.h"
#include "replay_harness.h"
#include "graphics/deko3d_backend.h"
#include "night100_switch_host.h"
#include <algorithm>
#include <cctype>

SwitchReplayUi::SwitchReplayUi(Deko3DBackend& backend):gpu(backend),actions(ReplayHarness::actions()) {
    refresh();
}
void SwitchReplayUi::refresh() {
    entries=actions.recordings();
    selected=entries.empty()?0:entries.size()-1;
    night100SwitchHostSetReplayCatalogCount(entries.size(),true);
}
void SwitchReplayUi::close() {
    if(picker && actions.setPickerOpen)actions.setPickerOpen(false);
    picker=false;
    night100SwitchHostCloseReplayPicker();
}
void SwitchReplayUi::notify(std::string text) {
    message=std::move(text);noticeUntil=std::chrono::steady_clock::now()+std::chrono::seconds(3);
}
void SwitchReplayUi::poll() {
    const auto now=std::chrono::steady_clock::now();
    const auto state=ReplayHarness::uiState();
    if(state.statusRevision!=statusRevision) {
        statusRevision=state.statusRevision;message.clear();noticeUntil=now+std::chrono::seconds(4);
    }
    const auto input=night100SwitchHostReplayInput();
    using Action=SwitchInput::ReplayAction;
    try {
        switch(input.action) {
        case Action::StartRecording:actions.startRecording();message.clear();break;
        case Action::StopRecording:
            // L+R+Down finishes the recording; PGO builds also flush their counters here.
            SwitchPgo::dumpCounters("stop_recording_chord");
            actions.stopRecording();message.clear();break;
        case Action::OpenPicker:
            refresh();picker=true;message.clear();actions.setPickerOpen(true);break;
        case Action::SelectPrevious:case Action::SelectNext:
            if(picker && !entries.empty())selected=std::min(input.selectedIndex,entries.size()-1);
            break;
        case Action::PlayReplay:
            if(picker && !entries.empty())actions.replay(entries[selected].path);
            close();break;
        case Action::CancelPicker:close();break;
        default:break;
        }
    }catch(const std::exception& error){message=error.what();noticeUntil=now+std::chrono::seconds(4);close();}
    // Steady gameplay has no picker or notice: avoid building text each frame.
    if(!picker && now>=noticeUntil && lastText.empty())return;
    std::string text;
    if(picker) {
        text="REPLAYS\n";
        if(entries.empty())text+="NO RECORDINGS\nA BACK";
        else text+="< "+std::to_string(entries[selected].id)+" >  "+std::to_string(selected+1)+"/"+
            std::to_string(entries.size())+"\nLEFT RIGHT SELECT\nB PLAY   A BACK";
    }else if(now<noticeUntil)text=message.empty()?actions.status():message;
    // The small GPU font is deliberately uppercase. Keep long filesystem paths
    // out of the on-screen status; the complete message remains in the log.
    if(!picker) {
        const auto detail=text.find(" — ");if(detail!=std::string::npos)text.resize(detail);
        if(text.size()>80)text.resize(80);
    }
    for(char& c:text)if(static_cast<unsigned char>(c)<128)c=char(std::toupper(static_cast<unsigned char>(c)));
    if(text!=lastText){gpu.setReplayOverlay(text);lastText=std::move(text);}
}
