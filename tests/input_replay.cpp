#include "input_replay.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

using namespace InputReplay;
namespace {
unsigned checks=0;
void check(bool value,const char* description) {
    ++checks;if(!value)throw std::runtime_error(description);
}
template<class F>void rejects(F function,const char* description) {
    bool rejected=false;try{function();}catch(const std::runtime_error&){rejected=true;}
    check(rejected,description);
}
std::string read(const std::filesystem::path& path){std::ifstream file(path,std::ios::binary);return {std::istreambuf_iterator<char>(file),{}};}
void write(const std::filesystem::path& path,const std::string& value){std::ofstream file(path,std::ios::binary);file<<value;}
uint32_t hash(const std::string& value){uint32_t result=2166136261u;for(unsigned char c:value){result^=c;result*=16777619u;}return result;}
std::string line(const std::string& value){std::ostringstream text;text<<value<<" #"<<std::hex<<hash(value)<<'\n';return text.str();}
}
int main() {
    const auto directory=std::filesystem::temp_directory_path()/("night100-input-checks-"+std::to_string(getpid()));
    try {
        std::filesystem::create_directories(directory);
        const auto path=directory/"session.inputs";
        const Context boot{2,0,0,0},game{11,1,2,0x48303031};
        Pad first;first.result=1;for(unsigned i=0;i<32;++i)first.bytes[i]=uint8_t(i*7+1);
        first.bytes[2]=0xff;first.bytes[3]=0xbf;first.bytes[4]=0x00;first.bytes[5]=0xff;
        Pad second=first;second.port=1;second.slot=3;second.result=2;second.bytes[3]=0xdf;
        Pad third=first;third.bytes[3]=0xff;third.bytes[7]=0x7f;
        {
            Session record(Mode::Record,path,"skip=1 graphics=native renderer=gpu cpu=1");
            record.pad(first,boot);record.pad(second,boot);record.completed(boot);
            record.pad(third,game);record.completed(game);record.finish("window-closed");
            check(record.frame()==2 && record.ended(),"record completed frames and footer");
        }
        const auto original=read(path);
        rejects([&]{Session record(Mode::Record,path,"");},"existing recordings must never be overwritten");
        check(read(path)==original,"failed overwrite must preserve original contents");
        {
            Session replay(Mode::Replay,path,"skip=1 graphics=native renderer=gpu cpu=1");
            Pad pad;pad.port=0;replay.pad(pad,boot);check(pad==first,"buttons analog pressure status and result roundtrip");
            pad={};pad.port=1;pad.slot=3;replay.pad(pad,boot);check(pad==second,"second controller and slot roundtrip");
            replay.completed(boot);pad={};replay.pad(pad,game);check(pad==third,"same and following frame events retain ordering");
            check(!replay.ended(),"replay retains final completed frame");
            replay.completed(game);check(replay.ended() && replay.frame()==2 && !replay.interrupted(),"normal replay end");
            pad.bytes.fill(99);replay.pad(pad,game);check(pad.bytes[0]==99,"ended replay returns controller unchanged");
        }
        rejects([&]{Session replay(Mode::Replay,path,"other");},"startup option mismatch rejected");
        rejects([&]{Session replay(Mode::Replay,path,"skip=1 graphics=native renderer=gpu cpu=1","wrong-elf");},"ELF mismatch rejected");
        const std::string options="skip=1 graphics=native renderer=gpu cpu=1";
        {
            const auto history=directory/"prefix-history.inputs",prefix=directory/"prefix.inputs";
            const auto pending=directory/"prefix-pending.inputs",later=directory/"prefix-later.inputs",finished=directory/"prefix-finished.inputs";
            Session record(Mode::Record,history,options);
            rejects([&]{record.savePrefix(prefix);},"prefix without a completed frame must reject");
            check(!std::filesystem::exists(prefix),"empty prefix rejection must not leave a file");
            record.pad(first,boot);
            rejects([&]{record.savePrefix(prefix);},"poll-only first frame cannot become a completed prefix");
            check(!std::filesystem::exists(prefix),"poll-only prefix rejection must not leave a file");
            record.completed(boot);const auto liveFirst=read(history);record.savePrefix(prefix);
            const auto firstPrefix=read(prefix);
            check(read(history)==liveFirst && !record.ended() && record.frame()==1,"saving prefix must not finish or change live recorder");
            check(firstPrefix==liveFirst+line("E 1 \"snapshot\""),"prefix must preserve checksummed history and add a complete footer");
            rejects([&]{record.savePrefix(prefix);},"prefix destination must be exclusive");
            check(read(prefix)==firstPrefix && read(history)==liveFirst,"failed prefix overwrite must preserve both files");
            rejects([&]{record.savePrefix(history);},"prefix must not replace its live source");
            check(read(history)==liveFirst,"source overwrite rejection must preserve live history");
            record.pad(third,game);const auto livePending=read(history);record.savePrefix(pending);
            check(read(pending)==firstPrefix && read(history)==livePending && record.frame()==1,"prefix must exclude uncompleted frame polls without changing live cursor");
            {
                Session replay(Mode::Replay,pending,options);Pad pad;replay.pad(pad,boot);
                check(pad==first && !replay.interrupted(),"completed prefix must replay original effective input");
                replay.completed(boot);check(replay.ended() && replay.frame()==1,"prefix must end at latest completed frame");
                const auto forbidden=directory/"replay-cannot-export.inputs";
                rejects([&]{replay.savePrefix(forbidden);},"replay mode must not export recording history");
                check(!std::filesystem::exists(forbidden),"replay export rejection must not create destination");
            }
            record.completed(game);record.savePrefix(later);
            check(read(prefix)==firstPrefix && read(later)!=firstPrefix && record.frame()==2 && !record.ended(),"later recorded frames must leave old snapshot immutable");
            {
                Session replay(Mode::Replay,later,options);Pad pad;replay.pad(pad,boot);replay.completed(boot);
                replay.pad(pad,game);check(pad==third,"live recording must retain pending inputs after prefix export");
                replay.completed(game);check(replay.ended() && replay.frame()==2,"continued recording prefix must replay through next frame");
            }
            record.finish();const auto liveFinished=read(history);record.savePrefix(finished);
            check(read(finished)==read(later) && read(history)==liveFinished,"finished source footer must not leak into exported snapshot");
            auto corrupt=read(later);corrupt[corrupt.find("P 0")+2]='9';write(pending,corrupt);
            rejects([&]{Session replay(Mode::Replay,pending,options);},"prefix event checksum corruption must reject");
            auto badHistory=liveFinished;badHistory[badHistory.find("P 0")+2]='9';write(history,badHistory);
            const auto rejected=directory/"prefix-corrupt-source.inputs";
            rejects([&]{record.savePrefix(rejected);},"prefix export must validate source history checksums");
            check(!std::filesystem::exists(rejected),"corrupt source rejection must not publish a partial prefix");
            for(const auto& file:std::filesystem::directory_iterator(directory))
                check(file.path().filename().string().find(".tmp-")==std::string::npos,"prefix failure must remove temporary output");
        }
        {
            EndPolicy normal;check(!normal.begin(false,true),"normal EOF permits the next game logic to run");
            check(normal.completed(),"normal EOF permits exactly one finishing game frame");
            EndPolicy input;check(!input.begin(false,false),"EOF at pad poll lets its game logic execute");
            check(input.completed(),"final pad EOF stops only after its game frame completes");
            EndPolicy crash;check(!crash.begin(true,false),"crash EOF retains a bounded reproduction tail");
            for(unsigned i=0;i<119;++i)check(!crash.completed(),"crash tail must permit 120 real frames");
            check(crash.completed(),"crash tail has finite frame limit");
        }
        {
            Session replay(Mode::Replay,path,options);Pad pad;
            rejects([&]{replay.pad(pad,game);},"state drift fails clearly");
        }
        {
            Session replay(Mode::Replay,path,options);Pad pad;pad.port=1;
            replay.pad(pad,boot);check(pad.bytes[2]==255 && pad.bytes[3]==255,"unrecorded extra port poll stays neutral");
            pad={};replay.pad(pad,boot);check(pad==first,"extra port poll does not discard recorded input");
        }
        {
            Session replay(Mode::Replay,path,options);
            replay.completed(boot);check(replay.aligning() && replay.frame()==0,"missing meaningful poll pauses logical frame");
            Pad pad;replay.pad(pad,boot);check(pad==first,"delayed poll still receives every input change");
        }
        const auto alignment=directory/"alignment.inputs";
        Pad neutral;neutral.result=1;neutral.bytes[1]=0x73;neutral.bytes[2]=neutral.bytes[3]=255;
        for(unsigned i=4;i<8;++i)neutral.bytes[i]=128;
        Pad action=neutral;action.bytes[3]=0xbf;action.bytes[7]=0;
        {
            Session record(Mode::Record,alignment,options);
            record.pad(neutral,boot);record.completed(boot);
            record.pad(neutral,boot);record.completed(boot);
            record.pad(neutral,boot);record.completed(game);
            record.pad(action,game);record.completed(game);
            record.pad(action,game);record.completed(game);
            record.pad(neutral,game);record.completed(game);record.finish();
        }
        {
            Session replay(Mode::Replay,alignment,options);Pad pad;
            replay.pad(pad,boot);replay.completed(boot);replay.pad(pad,boot);replay.completed(boot);
            replay.pad(pad,boot);replay.completed(boot);
            check(replay.aligning() && replay.frame()==2,"late mode change pauses at expected context");
            pad.bytes[3]=0;replay.pad(pad,boot);
            check(pad.bytes[2]==255 && pad.bytes[3]==255 && pad.bytes[7]==128,"alignment wait suppresses live input and uses neutral state");
            replay.completed(boot);check(replay.frame()==2,"extra loading frames do not advance replay clock");
            replay.completed(game);check(!replay.aligning() && replay.frame()==3,"late context resumes at original logical boundary");
            replay.pad(pad,game);check(pad==action,"first action after late context is preserved");
            replay.pad(pad,game);check(pad==action,"repeated poll reuses held state without advancing event cursor");
            replay.completed(game);check(replay.frame()==4,"first action hold consumes one logical game frame");
            replay.completed(game);check(replay.frame()==5,"missing duplicate poll preserves second hold frame");
            replay.pad(pad,game);check(pad==neutral,"release occurs after the complete recorded hold");
            replay.completed(game);check(replay.ended(),"late-aligned session reaches its end");
        }
        {
            Session replay(Mode::Replay,alignment,options);Pad pad;
            replay.pad(pad,boot);replay.completed(boot);replay.pad(pad,boot);replay.completed(game);
            check(replay.frame()==3 && !replay.aligning(),"early mode change skips only neutral loading repeats");
            replay.completed(game);check(replay.frame()==3 && replay.aligning(),"missing first action poll retains its logical frame");
            replay.pad(pad,game);check(pad==action,"first action survives missing poll after early mode change");
            replay.completed(game);replay.pad(pad,game);check(pad==action,"early alignment retains second hold frame");
            replay.completed(game);replay.pad(pad,game);check(pad==neutral,"early alignment retains release after full hold");
            replay.completed(game);check(replay.ended(),"early-aligned session ends");
        }
        {
            Session replay(Mode::Replay,alignment,options);Pad pad;
            replay.pad(pad,boot);replay.completed(boot);replay.pad(pad,boot);replay.completed(boot);replay.pad(pad,boot);
            for(unsigned i=0;i<300;++i)replay.completed(boot);
            rejects([&]{replay.completed(boot);},"late-context alignment has a bounded completed-frame timeout");
        }
        {
            const Context other{11,1,2,0x49303031};
            const auto preserve=directory/"preserve.inputs";
            {Session record(Mode::Record,preserve,options);record.pad(neutral,boot);record.completed(boot);
                record.pad(action,boot);record.completed(boot);record.pad(neutral,boot);record.completed(other);record.finish();}
            Session replay(Mode::Replay,preserve,options);Pad pad;replay.pad(pad,boot);
            rejects([&]{replay.completed(other);},"early context must never skip a meaningful press or release");
        }
        const auto incomplete=directory/"interrupted.inputs";
        const auto footer=original.rfind("E ");check(footer!=std::string::npos,"normal footer present");
        write(incomplete,original.substr(0,footer));
        {
            Session replay(Mode::Replay,incomplete,options);check(replay.interrupted(),"missing footer identifies crash prefix");
            Pad pad;replay.pad(pad,boot);pad.port=1;pad.slot=3;replay.pad(pad,boot);replay.completed(boot);
            pad={};replay.pad(pad,game);replay.completed(game);check(replay.ended(),"interrupted recording replays all complete events");
        }
        write(incomplete,original.substr(0,footer)+"P 2 0 0 0 1 ffff");
        {Session replay(Mode::Replay,incomplete,options);check(replay.interrupted(),"partial last write recovers intact prefix");}
        write(incomplete,original.substr(0,footer)+line("E 2 \"crash\""));
        {Session replay(Mode::Replay,incomplete,options);check(replay.interrupted(),"explicit crash footer keeps reproduction tail");}
        auto corrupted=original;corrupted[corrupted.find("P 0")+2]='9';write(incomplete,corrupted);
        rejects([&]{Session replay(Mode::Replay,incomplete,options);},"checksum corruption rejected");
        write(incomplete,original.substr(0,original.find('\n')));
        rejects([&]{Session replay(Mode::Replay,incomplete,options);},"truncated header rejected");
        const auto header=original.substr(0,original.find('\n')+1);
        write(incomplete,header+line("F 2 2 0 0 0"));
        rejects([&]{Session replay(Mode::Replay,incomplete,options);},"frame gaps rejected even with valid checksum");
        write(incomplete,header+line("F 1 2 0 0 0")+line("E 1 \"stop\"")+line("F 2 2 0 0 0"));
        rejects([&]{Session replay(Mode::Replay,incomplete,options);},"events after footer rejected");
        write(incomplete,header+line("P 0 0 2 0 1 "+std::string(64,'0')+" 2 0 0 0"));
        rejects([&]{Session replay(Mode::Replay,incomplete,options);},"invalid controller port rejected");
        write(incomplete,header+line("P 0 0 0 0 1 "+std::string(64,'z')+" 2 0 0 0"));
        rejects([&]{Session replay(Mode::Replay,incomplete,options);},"invalid controller hex rejected");
        write(incomplete,header);
        rejects([&]{Session replay(Mode::Replay,incomplete,options);},"empty session rejected");
        std::filesystem::remove_all(directory);
        std::cout<<"Input replay checks: "<<checks<<" passed\n";
        return 0;
    } catch(const std::exception& error) {
        std::filesystem::remove_all(directory);std::cerr<<error.what()<<'\n';return 1;
    }
}
