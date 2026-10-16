#pragma once
#include <cstdint>
#include <cstddef>
namespace SwitchInput {
class HoldToggle {
    uint64_t started=0;
    bool tracking=false,latched=false;
public:
    bool update(bool held,uint64_t now,bool active=true) {
        if(!active || !held){tracking=latched=false;return false;}
        if(!tracking || now<started){tracking=true;started=now;return false;}
        if(!latched && now-started>=3000000000ull){latched=true;return true;}
        return false;
    }
};
}

namespace SwitchInput {
struct ReplayButtons {
    bool l=false,r=false,up=false,down=false,left=false,right=false;
    bool confirm=false,cancel=false;
    // Optional host summary of remaining digital buttons, used only to drain
    // held input when leaving the picker or returning from background.
    bool otherHeld=false;
    bool any() const {
        return l||r||up||down||left||right||confirm||cancel||otherHeld;
    }
};
enum class ReplayAction {
    None,StartRecording,StopRecording,OpenPicker,SelectPrevious,SelectNext,
    PlayReplay,CancelPicker
};
struct ReplayUpdate {
    ReplayAction action=ReplayAction::None;
    std::size_t selectedIndex=0;
    bool pickerOpen=false;
    // consumeAll includes the closing frame and the following held-button tail.
    // Hosts must neutralize both buttons and analog axes when it is true.
    bool consumeAll=false;
    // Mask L/R and every D-pad direction, retaining ordinary gameplay controls.
    // This remains asserted until every bumper/direction in the chord is up.
    bool consumeChord=false;
    bool toggleMetrics=false;
};
class ReplayControls {
    ReplayButtons previous{};
    HoldToggle metrics;
    std::size_t selected=0,previousCount=0;
    bool picker=false,releaseBarrier=false,drainChord=false,suppressMetrics=false;
public:
    // Call on the same polling thread after refreshing the catalog on open.
    void selectNewest(std::size_t replayCount) {
        previousCount=replayCount;selected=replayCount?replayCount-1:0;
    }
    // Host-side UI/open failures must drain the same held input as an ordinary
    // cancel; closing the UI alone would leave this state machine in picker mode.
    void closePicker() {
        picker=false;releaseBarrier=true;previous={};metrics=HoldToggle{};
    }
    std::size_t selectedIndex() const {return selected;}
    bool pickerOpen() const {return picker;}
    // The catalog is ordered oldest -> newest. Count can be updated by the
    // host after OpenPicker without filesystem access in this state machine.
    ReplayUpdate update(ReplayButtons input,uint64_t now,std::size_t replayCount,
                        bool active=true) {
        ReplayUpdate out;
        const bool directions=input.up||input.down||input.left||input.right;
        const bool chordHeld=input.l&&input.r;
        if(!active) {
            if(picker)out.action=ReplayAction::CancelPicker;
            picker=false;previous={};releaseBarrier=true;
            drainChord=false;suppressMetrics=false;
            metrics.update(false,now,false);
            out.selectedIndex=selected;out.consumeAll=true;
            return out;
        }
        if(releaseBarrier) {
            // Require one neutral observation; do not synthesize a press when
            // foregrounding with a chord or confirmation button already held.
            releaseBarrier=input.any();previous=input;
            if(!releaseBarrier){drainChord=false;suppressMetrics=false;}
            metrics.update(false,now);
            out.selectedIndex=selected;out.consumeAll=true;
            return out;
        }
        if(!input.l && !input.r)suppressMetrics=false;
        if(chordHeld && directions){drainChord=true;suppressMetrics=true;}
        if(drainChord && !(input.l||input.r||directions))drainChord=false;
        out.consumeChord=drainChord;
        if(replayCount!=previousCount) {
            if(!previousCount && replayCount)selected=replayCount-1;
            else if(!replayCount)selected=0;
            else if(selected>=replayCount)selected=replayCount-1;
            previousCount=replayCount;
        }
        const bool wasPicker=picker;
        if(picker) {
            out.consumeAll=true;
            if(input.cancel) {
                if(!previous.cancel) {
                    picker=false;releaseBarrier=true;out.action=ReplayAction::CancelPicker;
                }
            } else if(input.confirm && !previous.confirm) {
                if(replayCount) {
                    picker=false;releaseBarrier=true;out.action=ReplayAction::PlayReplay;
                }
            } else if(input.left && !input.right && !previous.left && replayCount) {
                selected=selected?selected-1:replayCount-1;
                out.action=ReplayAction::SelectPrevious;
            } else if(input.right && !input.left && !previous.right && replayCount) {
                selected=selected+1==replayCount?0:selected+1;
                out.action=ReplayAction::SelectNext;
            }
        } else if(chordHeld && directions) {
            // Reject conflicting directions instead of recording/launching an
            // arbitrary action. A new clean chord edge is required afterwards.
            const unsigned count=unsigned(input.up)+unsigned(input.down)
                +unsigned(input.left)+unsigned(input.right);
            const bool previousChord=previous.l&&previous.r;
            if(count==1) {
                if(input.up && !(previousChord&&previous.up))out.action=ReplayAction::StartRecording;
                else if(input.down && !(previousChord&&previous.down))out.action=ReplayAction::StopRecording;
                else if((input.left && !(previousChord&&previous.left))
                     || (input.right && !(previousChord&&previous.right))) {
                    picker=true;selected=replayCount?replayCount-1:0;
                    out.action=ReplayAction::OpenPicker;out.consumeAll=true;
                }
            }
        }
        if(wasPicker || picker || suppressMetrics || drainChord)
            metrics.update(false,now);
        else out.toggleMetrics=metrics.update(chordHeld,now);
        previous=input;
        out.selectedIndex=selected;out.pickerOpen=picker;
        return out;
    }
};
}
