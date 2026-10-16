#include "clock_boost.h"
#include "night100_switch_host.h"
#include <switch.h>
#include <iostream>

namespace SwitchClockBoost {
namespace {
constexpr uint32_t cpuFastHz=1785000000u;
constexpr uint32_t gpuFastHz=921600000u;
constexpr uint32_t gpuFallbackHz=768000000u;
constexpr uint32_t emcFastHz=1600000000u;

struct Domain {
    const char* name;
    PcvModuleId module;
    ClkrstSession session{};
    bool open=false;
    uint32_t current=0;     // last successful read-back
    uint32_t saved=0;       // rate observed before boosting; 0 when unknown
    uint32_t boosted=0;     // rate this process last requested successfully
};

void log(const std::string& text) {std::cout<<"[clock] "<<text<<'\n';}
}

bool parseMode(const std::string& text,Mode& mode) {
    if(text=="fast"){mode=Mode::Fast;return true;}
    if(text=="default"){mode=Mode::Default;return true;}
    return false;
}
const char* modeName(Mode mode) {return mode==Mode::Fast?"fast":"default";}

struct Controller::Impl {
    Mode mode;
    bool serviceReady=false;
    bool boosted=false;
    Domain cpu{"cpu",PcvModuleId_CpuBus},gpu{"gpu",PcvModuleId_GPU},emc{"emc",PcvModuleId_EMC};
    Domain* domains[3]={&cpu,&gpu,&emc};
    std::string lastSummary="clock_state=unavailable";

    explicit Impl(Mode selected):mode(selected) {
        const Result init=clkrstInitialize();
        if(R_FAILED(init)) {
            log("clkrst_initialize_failed result="+std::to_string(init)+" mode="+modeName(mode));
            return;
        }
        serviceReady=true;
        for(auto* domain:domains) {
            const Result opened=clkrstOpenSession(&domain->session,domain->module,3);
            if(R_FAILED(opened)) {
                log(std::string("open_session_failed domain=")+domain->name+" result="+std::to_string(opened));
                continue;
            }
            domain->open=true;
        }
    }
    ~Impl() {
        // Restore before closing the sessions; the destructor runs on exit.
        restore();
        for(auto* domain:domains)if(domain->open)clkrstCloseSession(&domain->session);
        if(serviceReady)clkrstExit();
    }
    bool readRate(Domain& domain) {
        if(!domain.open)return false;
        uint32_t hz=0;
        const Result rc=clkrstGetClockRate(&domain.session,&hz);
        if(R_FAILED(rc)) {
            log(std::string("get_rate_failed domain=")+domain.name+" result="+std::to_string(rc));
            return false;
        }
        domain.current=hz;
        return true;
    }
    bool request(Domain& domain,uint32_t hz,uint32_t& applied) {
        if(!domain.open)return false;
        const Result rc=clkrstSetClockRate(&domain.session,hz);
        if(R_FAILED(rc)) {
            log(std::string("set_rate_failed domain=")+domain.name+" requested_hz="+std::to_string(hz)+" result="+std::to_string(rc));
            return false;
        }
        applied=hz;
        return true;
    }
    void boost() {
        // Capture the rate in force now, so a system change between sessions is
        // restored rather than a stale value from an earlier boost.
        for(auto* domain:domains)if(readRate(*domain))domain->saved=domain->current;
        uint32_t applied=0;
        if(cpu.open && request(cpu,cpuFastHz,applied))cpu.boosted=applied;
        if(gpu.open) {
            if(request(gpu,gpuFastHz,applied))gpu.boosted=applied;
            else if(request(gpu,gpuFallbackHz,applied))gpu.boosted=applied;
        }
        if(emc.open && request(emc,emcFastHz,applied))emc.boosted=applied;
        boosted=cpu.boosted||gpu.boosted||emc.boosted;
    }
    void restore() {
        if(!boosted)return;
        for(auto* domain:domains) {
            if(!domain->open || !domain->boosted || !domain->saved)continue;
            uint32_t applied=0;
            if(request(*domain,domain->saved,applied))domain->boosted=0;
        }
        boosted=false;
    }
    void readAll() {
        for(auto* domain:domains)readRate(*domain);
        std::string text=std::string("clock_mode=")+modeName(mode)+" clock_state="+(boosted?"boosted":"system")+
            " cpu_hz="+std::to_string(cpu.current)+" gpu_hz="+std::to_string(gpu.current)+" emc_hz="+std::to_string(emc.current);
        lastSummary=text;
    }
};

Controller::Controller(Mode mode):impl(std::make_unique<Impl>(mode)) {
    night100SwitchHostSetFocusCallback([](bool active,void* data) {
        static_cast<Controller*>(data)->setActive(active);
    },this);
    refresh();
    log(impl->lastSummary);
}

Controller::~Controller() {
    night100SwitchHostSetFocusCallback(nullptr,nullptr);
    std::lock_guard lock(mutex);
    impl->restore();
}

void Controller::setActive(bool active) {
    std::lock_guard lock(mutex);
    if(!impl->serviceReady)return;
    if(active) {
        if(impl->mode==Mode::Fast)impl->boost();
    } else {
        impl->restore();
    }
    impl->readAll();
    log(std::string(active?"focus_applied ":"focus_suspended ")+impl->lastSummary);
}

void Controller::refresh() {
    std::lock_guard lock(mutex);
    if(!impl->serviceReady)return;
    impl->readAll();
}

std::string Controller::summary() const {
    std::lock_guard lock(mutex);
    return impl->lastSummary;
}

bool Controller::available() const noexcept {
    std::lock_guard lock(mutex);
    return impl->serviceReady;
}
}
