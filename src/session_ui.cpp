#include "session_ui.h"
#include <raylib.h>
#include <algorithm>
#include <cmath>
#include <charconv>
#include <cstdio>
#include <exception>
#include <utility>

namespace {
constexpr Color panel{12,16,22,232},ink{232,238,244,255},accent{125,222,208,255},muted{175,187,200,255};
std::string metric(const std::optional<double>& value,const char* format,const char* pending="pending") {
    if(!value || !std::isfinite(*value))return pending;
    char result[64];std::snprintf(result,sizeof(result),format,*value);return result;
}
uint64_t selectedId(const std::string& value) {
    uint64_t result=0;const auto parsed=std::from_chars(value.data(),value.data()+value.size(),result);
    return parsed.ec==std::errc{} && parsed.ptr==value.data()+value.size()?result:0;
}
std::string fit(std::string text,int width,int font=18) {
    if(MeasureText(text.c_str(),font)<=width)return text;
    while(!text.empty() && MeasureText((text+"...").c_str(),font)>width)text.pop_back();
    return text+"...";
}
}
SessionUi::SessionUi(Actions value):actions(std::move(value)){}
void SessionUi::notify(std::string value){message=std::move(value);messageUntil=GetTime()+4;}
void SessionUi::closePicker() {
    pickerVisible=false;selection.clear();
    if(actions.setPickerOpen)actions.setPickerOpen(false);
}
void SessionUi::poll() {
    try {
        if(IsKeyPressed(KEY_F3)) {
            setVisible(!overlayVisible);
        }
        if(IsKeyPressed(KEY_F11)) {
            if(actions.startRecording)actions.startRecording();
            else notify("Input recording is unavailable.");
        }
        if(IsKeyPressed(KEY_F12)) {
            if(actions.stopRecording)actions.stopRecording();
        }
        if(IsKeyPressed(KEY_F10)) {
            if(pickerVisible)closePicker();
            else {
                recordingList=actions.recordings?actions.recordings():std::vector<Recording>{};
                std::erase_if(recordingList,[](const auto& item){return item.id==0;});
                std::sort(recordingList.begin(),recordingList.end(),[](const auto& a,const auto& b){return a.id<b.id;});
                largestId=recordingList.empty()?0:recordingList.back().id;
                selection.clear();page=0;pickerVisible=true;
                if(actions.setPickerOpen)actions.setPickerOpen(true);
            }
        }
        if(!pickerVisible)return;
        if(IsKeyPressed(KEY_ESCAPE)){closePicker();return;}
        for(int character=GetCharPressed();character;character=GetCharPressed())
            if(character>='0' && character<='9' && selection.size()<20)selection+=char(character);
        if(IsKeyPressed(KEY_BACKSPACE) && !selection.empty())selection.pop_back();
        if(IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
            const auto id=selectedId(selection);
            const auto item=std::find_if(recordingList.begin(),recordingList.end(),[&](const auto& entry){return entry.id==id;});
            if(!id || item==recordingList.end()){notify("That replay ID is unavailable. Choose an ID shown in the list.");return;}
            const auto path=item->path;
            if(!actions.replay){notify("Input replay is unavailable.");return;}
            actions.replay(path);closePicker();
        }
    }catch(const std::exception& error){notify(error.what());}
}
void SessionUi::drawOverlay() const {
    const auto data=actions.metrics?actions.metrics():Metrics{};
    const std::string fps="Game FPS  "+metric(data.gameFps,"%.1f");
    const std::string cpu="CPU time  "+metric(data.cpuMilliseconds,"%.2f ms");
    const std::string gpu="GPU time  "+(data.gpuState==GpuState::Unsupported?std::string("N/A"):
        data.gpuState==GpuState::Pending?std::string("pending"):metric(data.gpuMilliseconds,"%.2f ms"));
    constexpr int font=20;
    const int width=std::max({MeasureText(fps.c_str(),font),MeasureText(cpu.c_str(),font),MeasureText(gpu.c_str(),font)})+28;
    DrawRectangle(12,12,width,110,panel);
    DrawText(fps.c_str(),26,24,font,accent);DrawText(cpu.c_str(),26,55,font,ink);DrawText(gpu.c_str(),26,86,font,ink);
}
void SessionUi::drawPicker() {
    const int width=std::max(240,std::min(780,GetScreenWidth()-40));
    const int height=std::max(180,std::min(560,GetScreenHeight()-110));
    const int x=(GetScreenWidth()-width)/2,y=(GetScreenHeight()-height)/2-16;
    const size_t rows=size_t(std::max(1,(height-132)/26)),pages=std::max(size_t(1),(recordingList.size()+rows-1)/rows);
    const auto wheel=GetMouseWheelMove();
    if(IsKeyPressed(KEY_PAGE_DOWN) || wheel<0)page=std::min(page+1,pages-1);
    if(IsKeyPressed(KEY_PAGE_UP) || wheel>0){if(page)--page;}
    const uint64_t chosen=selectedId(selection);
    const auto chosenItem=std::find_if(recordingList.begin(),recordingList.end(),[&](const auto& entry){return entry.id==chosen;});
    if(chosenItem!=recordingList.end())page=size_t(chosenItem-recordingList.begin())/rows;
    page=std::min(page,pages-1);
    DrawRectangle(0,0,GetScreenWidth(),GetScreenHeight(),Color{0,0,0,140});DrawRectangle(x,y,width,height,panel);
    DrawText("Replay recorded inputs",x+20,y+18,24,accent);
    if(recordingList.empty())DrawText("No input recordings found.",x+20,y+64,18,muted);
    for(size_t row=0,index=page*rows;row<rows && index<recordingList.size();++row,++index) {
        const auto& item=recordingList[index];
        const std::string text="Replay "+std::to_string(item.id)+": "+(item.label.empty()?item.path.filename().string():item.label);
        DrawText(fit(text,width-40).c_str(),x+20,y+62+int(row)*26,18,chosen==item.id?accent:ink);
    }
    const std::string prompt="Replay ID"+(largestId?" (1.."+std::to_string(largestId)+")":"")+": "+selection+"_";
    DrawText(prompt.c_str(),x+20,y+height-64,20,ink);
    const std::string help="Enter: replay   F10 / Esc: close"+(pages>1?"   PgUp/PgDn: page "+std::to_string(page+1)+"/"+std::to_string(pages):"");
    DrawText(fit(help,width-40,16).c_str(),x+20,y+height-30,16,muted);
}
void SessionUi::draw() {
    poll();
    if(overlayVisible)drawOverlay();
    if(pickerVisible)drawPicker();
    const std::string status=actions.status?actions.status():"";
    const double now=GetTime();
    if(status!=lastStatus){lastStatus=status;statusUntil=now+4;}
    const std::string notice=now<messageUntil?message:now<statusUntil?status:"";
    if(!notice.empty()) {
        const int width=std::max(0,GetScreenWidth()-48),y=std::max(0,GetScreenHeight()-46);
        const auto text=fit(notice,width);
        DrawRectangle(12,y,std::min(GetScreenWidth()-24,MeasureText(text.c_str(),18)+24),34,panel);
        DrawText(text.c_str(),24,y+8,18,ink);
    }
}
