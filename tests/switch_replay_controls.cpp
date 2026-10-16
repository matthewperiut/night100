#include "input_chords.h"
#include <iostream>
#include <stdexcept>

namespace {
using namespace SwitchInput;
unsigned checks=0;
constexpr uint64_t second=1000000000;
void require(bool value,const char* message) {
    ++checks;if(!value)throw std::runtime_error(message);
}
ReplayButtons bumpers(){ReplayButtons b;b.l=b.r=true;return b;}
void recordingEdges() {
    ReplayControls controls;auto b=bumpers();
    controls.update({},0,4);
    controls.update(b,second,4);b.up=true;
    auto out=controls.update(b,2*second,4);
    require(out.action==ReplayAction::StartRecording && out.consumeChord,"Up chord did not record and consume");
    out=controls.update(b,6*second,4);
    require(out.action==ReplayAction::None && !out.toggleMetrics,"Held record retriggered or toggled metrics");
    b.up=false;controls.update(b,7*second,4);
    out=controls.update(b,20*second,4);
    require(!out.toggleMetrics && out.consumeChord,"Metrics fired after directional chord while bumpers stayed held");
    b.down=true;out=controls.update(b,21*second,4);
    require(out.action==ReplayAction::StopRecording,"Down chord did not stop");
    require(controls.update(b,22*second,4).action==ReplayAction::None,"Held stop repeated");
    b.l=false;out=controls.update(b,23*second,4);
    require(out.consumeChord,"Partially released chord leaked a held direction");
    b.r=false;out=controls.update(b,24*second,4);
    require(out.consumeChord,"Direction tail leaked after bumper release");
    b.down=false;out=controls.update(b,25*second,4);
    require(!out.consumeChord,"Neutral chord never drained");
    b=bumpers();controls.update(b,26*second,4);
    out=controls.update(b,29*second,4);
    require(out.toggleMetrics,"Fresh plain bumper hold did not rearm metrics");
    require(!controls.update(b,35*second,4).toggleMetrics,"Held metrics chord toggled twice");
}
void pickerNavigation() {
    ReplayControls controls;auto b=bumpers();b.left=true;
    auto out=controls.update(b,0,4);
    require(out.action==ReplayAction::OpenPicker && out.pickerOpen && out.selectedIndex==3,"Picker did not open at newest");
    require(out.consumeAll && out.consumeChord,"Opening picker leaked game input");
    out=controls.update(b,second,4);
    require(out.action==ReplayAction::None && out.selectedIndex==3,"Opening direction also moved selection");
    controls.update({},2*second,4);
    b={};b.right=true;out=controls.update(b,3*second,4);
    require(out.action==ReplayAction::SelectNext && out.selectedIndex==0,"Picker next did not wrap");
    require(out.consumeAll,"Picker navigation leaked controls");
    require(controls.update(b,4*second,4).action==ReplayAction::None,"Held next autorepeated");
    controls.update({},5*second,4);b={};b.left=true;
    out=controls.update(b,6*second,4);
    require(out.action==ReplayAction::SelectPrevious && out.selectedIndex==3,"Picker previous did not wrap");
    controls.update({},7*second,4);b={};b.confirm=true;
    out=controls.update(b,8*second,4);
    require(out.action==ReplayAction::PlayReplay && out.selectedIndex==3 && !out.pickerOpen,"Physical bottom confirmation did not launch chosen replay");
    require(out.consumeAll,"Confirm closing frame leaked to game");
    out=controls.update(b,9*second,4);
    require(out.action==ReplayAction::None && out.consumeAll,"Held confirmation leaked after picker closed");
    out=controls.update({},10*second,4);
    require(out.consumeAll,"Neutral release boundary was not consumed");
    require(!controls.update({},11*second,4).consumeAll,"Picker input drain never cleared");
}
void emptyAndChangingCatalog() {
    ReplayControls controls;auto b=bumpers();b.right=true;
    auto out=controls.update(b,0,0);
    require(out.action==ReplayAction::OpenPicker && out.pickerOpen && out.consumeAll,"Empty catalog leaked opening chord");
    controls.update({},second,0);b={};b.confirm=true;
    out=controls.update(b,2*second,0);
    require(out.action==ReplayAction::None && out.pickerOpen && out.consumeAll,"Empty picker launched or closed");
    controls.selectNewest(7);
    require(controls.selectedIndex()==6 && controls.pickerOpen(),"Fresh catalog did not select newest in open picker");
    out=controls.update(b,3*second,7);
    require(out.action==ReplayAction::None && out.selectedIndex==6,"Catalog refresh synthesized a held confirm");
    out=controls.update({},4*second,2);
    require(out.selectedIndex==1 && out.consumeAll,"Shrinking catalog left out-of-range selection");
    out=controls.update({},5*second,0);
    require(out.selectedIndex==0 && out.pickerOpen,"Empty refresh destroyed picker state");
    out=controls.update({},6*second,5);
    require(out.selectedIndex==4,"Initially empty catalog did not select fresh newest");
    b={};b.cancel=true;b.confirm=true;
    out=controls.update(b,7*second,5);
    require(out.action==ReplayAction::CancelPicker && !out.pickerOpen && out.consumeAll,"Cancel did not take precedence over confirm");
    require(controls.update(b,8*second,5).consumeAll,"Held cancel leaked after picker closed");
}
void lifecycleAndConflicts() {
    ReplayControls controls;auto b=bumpers();b.up=b.down=true;
    auto out=controls.update(b,0,3);
    require(out.action==ReplayAction::None && out.consumeChord,"Conflicting record/stop picked arbitrary action");
    require(!controls.update(b,9*second,3).toggleMetrics,"Conflicting directions toggled metrics");
    controls.update({},10*second,3);b=bumpers();b.right=true;
    controls.update(b,11*second,3);
    out=controls.update(b,12*second,3,false);
    require(out.action==ReplayAction::CancelPicker && !out.pickerOpen && out.consumeAll,"Background did not cancel picker");
    out=controls.update(b,20*second,3);
    require(out.action==ReplayAction::None && !out.pickerOpen && out.consumeAll,"Resume replayed held opening chord");
    require(!controls.update(b,30*second,3).toggleMetrics,"Resume held chord toggled metrics");
    controls.update({},31*second,3);controls.update({},32*second,3);
    b=bumpers();b.up=true;
    require(controls.update(b,33*second,3).action==ReplayAction::StartRecording,"Neutral resume did not rearm record");
    controls.update({},34*second,3);b=bumpers();controls.update(b,35*second,3);
    require(!controls.update(b,second,3).toggleMetrics,"Regressed timestamp triggered metrics");
    require(controls.update(b,4*second,3).toggleMetrics,"Metrics clock regression did not restart hold");
}
void hostOpenFailure() {
    ReplayControls controls;auto b=bumpers();b.left=true;
    require(controls.update(b,0,3).action==ReplayAction::OpenPicker,"Open failure fixture did not open");
    controls.closePicker();
    require(!controls.pickerOpen(),"Host open failure did not close state machine");
    auto out=controls.update(b,second,3);
    require(!out.pickerOpen && out.action==ReplayAction::None && out.consumeAll,"Failed picker leaked held opening chord");
    out=controls.update(b,9*second,3);
    require(!out.toggleMetrics && out.consumeAll,"Failed picker turned into HUD chord");
    controls.update({},10*second,3);controls.update({},11*second,3);
    require(controls.update(b,12*second,3).action==ReplayAction::OpenPicker,"Failed picker could not reopen after neutral");
    controls.closePicker();controls.closePicker();
    require(controls.update(b,13*second,3).consumeAll,"Repeated host close broke held-button draining");
}

void gameplayPassthroughAndCatalogSetter() {
    ReplayControls controls;ReplayButtons b;b.up=true;b.confirm=true;
    auto out=controls.update(b,0,2);
    require(out.action==ReplayAction::None && !out.consumeAll && !out.consumeChord,"Ordinary game input was captured");
    b=bumpers();b.up=true;
    require(controls.update(b,second,2).action==ReplayAction::StartRecording,"Completing modifiers around held direction did not count chord edge");
    controls.update({},2*second,2);b=bumpers();b.left=true;controls.update(b,3*second,2);
    controls.selectNewest(10);
    require(controls.selectedIndex()==9,"Explicit catalog refresh did not select latest replay");
    controls.update({},4*second,10);b={};b.cancel=true;controls.update(b,5*second,10);
    b={};b.otherHeld=true;
    require(controls.update(b,6*second,10).consumeAll,"Other held digital buttons escaped close drain");
    controls.update({},7*second,10);
    require(!controls.update({},8*second,10).consumeAll,"Other-button drain did not recover");
}
}
int main() {
    try {
        recordingEdges();pickerNavigation();emptyAndChangingCatalog();
        lifecycleAndConflicts();hostOpenFailure();gameplayPassthroughAndCatalogSetter();
        std::cout<<"PASS: "<<checks<<" Switch replay chord, picker, input-drain and lifecycle checks\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;
    }
}
