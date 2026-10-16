#include "platform/switch/video_policy.h"
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
unsigned checks=0;
void require(bool result,const char* why) {
    ++checks;
    if(!result)throw std::runtime_error(why);
}
}
int main() {
    try {
        using namespace SwitchVideo;
        for(auto movie:{"THQLOGO.PSS","HILOGO.PSS","WBLOGO.PSS","RW4.PSS","HWTRAILR.PSS","SDTRAILR.PSS"}) {
            require(optionalMovie("FMV",movie),"Catalog logo/trailer is not optional");
            require(!optionalMovie("OTHER",movie),"Noncatalog movie was admitted");
        }
        for(unsigned demo=1;demo<=15;++demo) {
            const auto name="DEMO"+std::to_string(demo)+".PSS";
            require(optionalMovie("FMV/DEMO",name),"Catalog attract demo is not optional");
            require(!optionalMovie("DEMO",name),"Noncatalog directory was admitted");
        }
        require(optionalMovie("fmv/demo","demo12.pss"),"Case folding lost original alias compatibility");
        for(auto movie:{"CIN1.PSS","INTRO.PSS","CREDITS1.PSS","UNKNOWN.PSS"})
            require(!optionalMovie("FMV",movie),"Story/unknown movie was silently optional");
        for(auto movie:{"DEMO0.PSS","DEMO16.PSS","DEMO01.PSS","DEMOA.PSS","DEMO1.PSS;1","DEMO1.PSS/.."})
            require(!optionalMovie("FMV/DEMO",movie),"Invalid demo name was admitted");
        for(bool optional:{false,true}) {
            require(movieDecision(VideoSupport::HardwareMpeg2,optional)==VideoDecision::Play,
                    "Supported hardware movie was skipped");
            require(movieDecision(VideoSupport::Unidentified,optional)==VideoDecision::Fail,
                    "Probe/configuration failure was concealed as an optional movie skip");
        }
        require(movieDecision(VideoSupport::Unsupported,true)==VideoDecision::SkipOptional,
                "Known unsupported optional movie stops game progress");
        require(movieDecision(VideoSupport::Unsupported,false)==VideoDecision::Fail,
                "Unsupported story movie was concealed");
        constexpr uint64_t second=1000000000,baseline=1234;
        require(activeMovieNanoseconds(second,baseline,baseline)==second,"Normal playback clock changed");
        require(activeMovieNanoseconds(61*second,baseline,baseline+60*second)==second,"Background minute advanced movie clock");
        require(activeMovieNanoseconds(62*second,baseline,baseline+60*second)==2*second,"Foreground playback did not resume");
        require(activeMovieNanoseconds(61*second,baseline,baseline+61*second)==0,"Current suspend interval was not excluded");
        require(activeMovieNanoseconds(second,baseline,baseline-1)==second,"Counter regression underflowed");
        std::cout<<"PASS: "<<checks<<" Switch movie catalog, hardware-only policy and lifecycle clock checks\n";
        return 0;
    } catch(const std::exception& e) {
        std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;
    }
}
