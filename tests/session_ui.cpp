#include "session_ui.h"
#include <raylib.h>
#include <algorithm>
#include <cstring>
#include <deque>
#include <iostream>
#include <set>
#include <stdexcept>

// Exercise real UI polling without a window, GPU or synthesized OS events.
namespace {
std::set<int> pressed;
std::deque<int> characters;
std::vector<std::string> drawn;
unsigned cases=0;
double now=1;
void check(bool value,const char* label){++cases;if(!value)throw std::runtime_error(label);}
void frame(SessionUi& ui,std::initializer_list<int> keys={},const std::string& text="") {
    pressed={keys};characters={text.begin(),text.end()};drawn.clear();ui.draw();pressed.clear();
}
bool shows(const std::string& value){return std::any_of(drawn.begin(),drawn.end(),[&](const auto& line){return line.find(value)!=std::string::npos;});}
}
extern "C" {
bool IsKeyPressed(int key){return pressed.contains(key);}
int GetCharPressed(){if(characters.empty())return 0;int result=characters.front();characters.pop_front();return result;}
double GetTime(){return now;}
int GetScreenWidth(){return 960;}
int GetScreenHeight(){return 720;}
float GetMouseWheelMove(){return 0;}
int MeasureText(const char* text,int size){return int(std::strlen(text))*size/2;}
void DrawRectangle(int,int,int,int,Color){}
void DrawText(const char* text,int,int,int,Color){drawn.emplace_back(text);}
}
int main() try {
    unsigned metricsRead=0,starts=0,stops=0;
    std::vector<bool> overlays,pickers;
    std::vector<std::filesystem::path> replayed;
    SessionUi::Metrics metrics;metrics.gameFps=59.8;metrics.cpuMilliseconds=4.25;
    SessionUi::Actions actions;
    actions.setOverlayVisible=[&](bool value){overlays.push_back(value);};
    actions.setPickerOpen=[&](bool value){pickers.push_back(value);};
    actions.metrics=[&]{++metricsRead;return metrics;};
    actions.startRecording=[&]{++starts;};actions.stopRecording=[&]{++stops;};
    actions.recordings=[] {return std::vector<SessionUi::Recording>{{"third.inputs","Third",3},{"last.inputs","Last",42},{"seventh.inputs","Seventh",7}};};
    actions.replay=[&](const auto& path){replayed.push_back(path);};
    actions.status=[] {return "Recording replay 43";};
    SessionUi ui(std::move(actions));
    frame(ui);check(!ui.visible() && metricsRead==0,"hidden overlay must not request metrics");
    check(shows("Recording replay 43"),"recording status must remain visible with overlay hidden");
    frame(ui,{KEY_F3});check(ui.visible() && overlays==std::vector<bool>{true},"F3 must enable telemetry once");
    check(shows("59.8") && shows("4.25 ms") && shows("pending"),"overlay must display game metrics and pending GPU timer");
    metrics.gpuState=SessionUi::GpuState::Unsupported;frame(ui);check(shows("N/A"),"unsupported GPU timing must display N/A");
    metrics.gpuState=SessionUi::GpuState::Available;metrics.gpuMilliseconds=0.62;frame(ui);check(shows("0.62 ms"),"available GPU timing must display genuine sample");
    frame(ui,{KEY_F3});check(!ui.visible() && overlays==std::vector<bool>({true,false}),"F3 must disable telemetry");
    frame(ui,{KEY_F11});frame(ui,{KEY_F12});check(starts==1 && stops==1,"F11/F12 must request recording start/stop");
    frame(ui,{KEY_F10});check(ui.pickerOpen() && pickers==std::vector<bool>{true},"F10 must open picker and capture input");
    check(shows("Replay 3") && shows("Replay 7") && shows("Replay 42"),"picker must retain stable IDs across holes");
    frame(ui,{KEY_ENTER},"4");check(ui.pickerOpen() && replayed.empty() && shows("unavailable"),"missing replay ID must not select row four");
    frame(ui,{KEY_BACKSPACE});frame(ui,{KEY_ENTER},"42");
    check(!ui.pickerOpen() && replayed==std::vector<std::filesystem::path>{"last.inputs"},"typed stable ID must select matching recording");
    check(pickers==std::vector<bool>({true,false}),"successful replay must release picker capture");
    frame(ui,{KEY_F10});frame(ui,{KEY_ENTER},"18446744073709551616");
    check(ui.pickerOpen() && replayed.size()==1,"overflowing ID must not select a recording");
    frame(ui,{KEY_ESCAPE});check(!ui.pickerOpen(),"Escape must close picker");
    frame(ui,{KEY_F10});frame(ui,{KEY_F10});check(!ui.pickerOpen(),"F10 must also close picker");
    now=10;frame(ui);check(!shows("Recording replay 43"),"replay notices must expire after four seconds");
    frame(ui,{KEY_F10});now=20;frame(ui);
    check(ui.pickerOpen() && shows("Replay 42"),"notice expiry must not close the replay picker");
    std::cout<<"PASS "<<cases<<" session UI keyboard/telemetry checks\n";return 0;
} catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
