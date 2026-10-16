#include "collision_debug.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace CollisionDebug {
namespace {
constexpr uint32_t ramSize=32u*1024u*1024u,player=0x3631f8;
// Offsets below come from retail SLUS-20349 zEntPlayer.cpp DWARF classes:
// _xEnt, _xEntFrame, _xEntCollis, _xCollis, zPlayerGlobals and _xEntDrive.
struct Guest {
    const uint8_t* ram;
    bool valid(uint32_t address,uint32_t bytes=4) const {
        return address>=0x1000 && bytes<=ramSize && address<=ramSize-bytes;
    }
    template<class T>T read(uint32_t address) const {
        T value{};if(valid(address,sizeof(T)))std::memcpy(&value,ram+address,sizeof(T));return value;
    }
    uint32_t pointer(uint32_t base,uint32_t offset) const {
        return valid(base,offset+4)?read<uint32_t>(base+offset):0;
    }
    void vector(std::ostream& out,uint32_t address,unsigned count=3) const {
        for(unsigned i=0;i<count;++i)out<<','<<read<float>(address+i*4);
    }
    void matrix(std::ostream& out,uint32_t address) const {
        out<<','<<valid(address,64);
        for(unsigned column=0;column<4;++column)vector(out,valid(address,64)?address+column*16:0);
    }
};
void vectorHeader(std::ostream& out,const char* name,unsigned count=3) {
    static constexpr const char* axis[]={"x","y","z"};
    for(unsigned i=0;i<count;++i){out<<','<<name<<'_';if(count==3)out<<axis[i];else out<<i;}
}
void matrixHeader(std::ostream& out,const char* name) {
    out<<','<<name<<"_valid";
    for(const char* axis:{"right","up","at","pos"})for(const char* component:{"x","y","z"})out<<','<<name<<'_'<<axis<<'_'<<component;
}
struct Output {
    std::ofstream frames,contacts,entities,calls,triangles,triangleContexts;
    bool enabled=false;
    uint64_t lastCompleted=0,callSequence=0,triangleSequence=0,contextRecords=0;
    Output() {
        const char* setting=std::getenv("NIGHT100_COLLISION_DEBUG");if(!setting || !*setting)return;
        try {
            const std::filesystem::path directory(setting);std::filesystem::create_directories(directory);
            const auto open=[&](std::ofstream& stream,const char* name) {
                const auto path=directory/name;
                // A diagnostic run must not silently mix its frame IDs with an old run.
                if(std::filesystem::exists(path))throw std::runtime_error("output already exists: "+path.string());
                stream.open(path,std::ios::out|std::ios::binary);if(!stream)throw std::runtime_error("cannot open "+path.string());
                stream<<std::setprecision(9);
            };
            open(frames,"frames.csv");open(contacts,"contacts.csv");open(entities,"entities.csv");open(calls,"calls.csv");
            open(triangles,"triangles.csv");open(triangleContexts,"triangle-contexts.bin");
            frames<<"frame,player,frame_ptr,frame_valid,mode,health,control,jump_state,last_jump_state,jump_timer,update_dt";
            for(const char* name:{"pos","oldpos","vel","dpos","dvel"})vectorHeader(frames,name);
            frames<<",ent_driver,drv_flags,drv_old_driver,drv_driver,drv_driven,drv_otm,drv_otmr,drv_os,drv_tm,drv_tmr,drv_s";
            for(const char* name:{"drv_op","drv_p","drv_q"})vectorHeader(frames,name);
            frames<<",drv_yaw,floor_surf";vectorHeader(frames,"floor_norm");
            frames<<",last_floor_ent";vectorHeader(frames,"last_floor_norm");
            vectorHeader(frames,"last_delta_pos");vectorHeader(frames,"floor_dist",4);vectorHeader(frames,"floor_tmr",4);
            frames<<",collis_ptr,collis_valid,chk,pen,env_s,env_e,npc_s,npc_e,dyn_s,dyn_e,stat_s,stat_e,trig_idx,idx\n";
            contacts<<"frame,slot,in_count,hit,flags,oid,optr,dist";
            for(const char* name:{"norm","tohit","depen","heading"})vectorHeader(contacts,name);
            contacts<<",ixtra\n";
            entities<<"frame,entity,entity_valid,role,id,base_type,flags,sub_type,pflags,more_flags,coll_type,coll_lev,chkby,penby,frame_ptr,model_ptr,atomic_ptr,rwframe_ptr,model_matrix_ptr,bound_type,bound_matrix_ptr";
            vectorHeader(entities,"bound_center");vectorHeader(entities,"bound_values",6);
            for(const char* name:{"physics","model","rw_ltm","bound"})matrixHeader(entities,name);
            entities<<'\n';
            calls<<"sequence,frame,stage,function,entry_pc,entry_ra,result_pc,entity,sphere_ptr,sphere_radius";
            vectorHeader(calls,"sphere_center");calls<<",physics_frame,physics_y,physics_vel_y,contact_base";
            for(unsigned slot=0;slot<6;++slot)calls<<",flags_"<<slot<<",dist_"<<slot<<",normal_y_"<<slot<<",depen_y_"<<slot;
            calls<<'\n';
            triangles<<"sequence,frame,entry_pc,entry_ra,result_pc,result,sphere_ptr,vertex0_ptr,vertex1_ptr,vertex2_ptr,normal_ptr,dist_ptr,stack_ptr,context_record,sphere_radius";
            for(const char* name:{"sphere","v0","v1","v2","normal"})vectorHeader(triangles,name);
            triangles<<",dist\n";
            // Diagnostic-only same-build CPU records. Header: magic8/contextSize8;
            // each record: sequence8/frame8/entryContext/returnContext/entryStack48.
            // CSV arguments are max_digits10 floats and preserve exact input bits.
            triangleContexts.write("COLLCPU1",8);const uint64_t contextSize=sizeof(R5900Context);
            triangleContexts.write(reinterpret_cast<const char*>(&contextSize),sizeof(contextSize));
            enabled=true;
        }catch(const std::exception& error){std::cerr<<"[collision-debug] "<<error.what()<<'\n';}
    }
    void entity(const Guest& g,uint64_t frame,uint32_t address,const char* role) {
        const bool valid=g.valid(address,0xe8);
        const uint32_t object=valid?address:0,physical=g.pointer(object,0x40),model=g.pointer(object,0x2c);
        const uint32_t atomic=g.pointer(model,0x10),rwframe=g.pointer(atomic,4),modelMatrix=g.pointer(model,0x20);
        const uint32_t bound=valid?address+0x48:0,boundMatrix=g.pointer(bound,0x28);
        entities<<frame<<','<<address<<','<<valid<<','<<role<<','<<g.read<uint32_t>(object)<<','<<unsigned(g.read<uint8_t>(object+4));
        for(uint32_t offset:{0x14,0x15,0x16,0x17,0x28,0x29,0x2a,0x2b})entities<<','<<unsigned(g.read<uint8_t>(object+offset));
        entities<<','<<physical<<','<<model<<','<<atomic<<','<<rwframe<<','<<modelMatrix<<','<<unsigned(g.read<uint8_t>(bound))<<','<<boundMatrix;
        g.vector(entities,bound?bound+4:0);g.vector(entities,bound?bound+0x10:0,6);
        g.matrix(entities,physical);g.matrix(entities,modelMatrix);g.matrix(entities,g.valid(rwframe,0x90)?rwframe+0x50:0);g.matrix(entities,boundMatrix);
        entities<<'\n';
    }
};
Output& output(){static Output instance;return instance;}
struct OriginalFunctions {PS2Runtime::RecompiledFunction sphere=nullptr,select=nullptr,triangle=nullptr;};
std::unordered_map<PS2Runtime*,OriginalFunctions> originals;
void call(const Guest& g,const char* stage,uint32_t function,uint32_t entryPc,uint32_t entryRa,
          uint32_t resultPc,uint32_t ent,uint32_t sphere,uint32_t contactBase) {
    auto& o=output();if(!o.enabled)return;
    auto& out=o.calls;
    out<<++o.callSequence<<','<<o.lastCompleted+1<<','<<stage<<','<<function<<','<<entryPc<<','<<entryRa<<','<<resultPc
        <<','<<ent<<','<<sphere<<','<<g.read<float>(g.valid(sphere,16)?sphere+12:0);
    g.vector(out,g.valid(sphere,16)?sphere:0);
    const uint32_t physical=g.pointer(ent,0x40);const bool frameValid=g.valid(physical,0xf0);
    out<<','<<physical<<','<<g.read<float>(frameValid?physical+0x34:0)<<','<<g.read<float>(frameValid?physical+0xe4:0)<<','<<contactBase;
    for(unsigned slot=0;slot<6;++slot) {
        const uint32_t contact=g.valid(contactBase,6*0x44)?contactBase+slot*0x44:0;
        out<<','<<g.read<uint32_t>(contact)<<','<<g.read<float>(contact?contact+12:0)
            <<','<<g.read<float>(contact?contact+0x14:0)<<','<<g.read<float>(contact?contact+0x2c:0);
    }
    out<<'\n';out.flush();
}
void sphereObserver(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    const auto original=originals.at(runtime).sphere;
    // A dispatcher can resume the replaced function at one of its interior
    // continuation PCs. Those entries must retain its original switch behavior;
    // a0..a3 then describe a nested call, rather than this function's arguments.
    if(cpu->pc!=0x12deb0){original(ram,cpu,runtime);return;}
    const Guest g{ram};const uint32_t pc=cpu->pc,ra=GPR_U32(cpu,31),sphere=GPR_U32(cpu,4),contacts=GPR_U32(cpu,7);
    call(g,"enter",0x12deb0,pc,ra,pc,player,sphere,contacts);
    original(ram,cpu,runtime);
    call(g,cpu->pc==ra?"return":"suspend",0x12deb0,pc,ra,cpu->pc,player,sphere,contacts);
}
void selectObserver(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    const auto original=originals.at(runtime).select;
    if(cpu->pc!=0x16fec0){original(ram,cpu,runtime);return;}
    const Guest g{ram};const uint32_t pc=cpu->pc,ra=GPR_U32(cpu,31),ent=GPR_U32(cpu,4),collis=g.pointer(ent,0x44);
    const uint32_t contacts=g.valid(collis,0x4fc)?collis+12:0,sphere=g.valid(ent,0x58)?ent+0x4c:0;
    call(g,"enter",0x16fec0,pc,ra,pc,ent,sphere,contacts);
    original(ram,cpu,runtime);
    call(g,cpu->pc==ra?"return":"suspend",0x16fec0,pc,ra,cpu->pc,ent,sphere,contacts);
}
void triangleObserver(uint8_t* ram,R5900Context* cpu,PS2Runtime* runtime) {
    const auto original=originals.at(runtime).triangle;
    if(cpu->pc!=0x276d00){original(ram,cpu,runtime);return;}
    const Guest g{ram};auto& o=output();const uint64_t sequence=++o.triangleSequence,frame=o.lastCompleted+1;
    std::array<uint32_t,7> pointers{};
    for(unsigned index=0;index<6;++index)pointers[index]=Ps2ExtractEpi32(cpu->r[4+index],0);
    pointers[6]=GPR_U32(cpu,29);
    std::array<float,13> arguments{};
    if(g.valid(pointers[0],16))std::memcpy(arguments.data(),ram+pointers[0],16);
    for(unsigned index=0;index<3;++index)if(g.valid(pointers[index+1],12))std::memcpy(arguments.data()+4+index*3,ram+pointers[index+1],12);
    const uint32_t pc=cpu->pc,ra=GPR_U32(cpu,31);
    // Bound full-context evidence; all scalar geometry/results continue in CSV.
    const bool capture=o.contextRecords<256;R5900Context entry{};std::array<uint8_t,48> stack{};
    if(capture) {
        entry=*cpu;
        if(pointers[6]>=48 && g.valid(pointers[6]-48,48))std::memcpy(stack.data(),ram+pointers[6]-48,stack.size());
    }
    original(ram,cpu,runtime);
    auto& out=o.triangles;
    out<<sequence<<','<<frame<<','<<pc<<','<<ra<<','<<cpu->pc<<','<<GPR_U32(cpu,2);
    for(uint32_t pointer:pointers)out<<','<<pointer;
    out<<','<<(capture?static_cast<int64_t>(o.contextRecords):-1)<<','<<arguments[3];
    for(unsigned index=0;index<3;++index)out<<','<<arguments[index];
    for(unsigned index=4;index<arguments.size();++index)out<<','<<arguments[index];
    g.vector(out,pointers[4]);out<<','<<g.read<float>(pointers[5])<<'\n';out.flush();
    if(capture) {
        o.triangleContexts.write(reinterpret_cast<const char*>(&sequence),sizeof(sequence));
        o.triangleContexts.write(reinterpret_cast<const char*>(&frame),sizeof(frame));
        o.triangleContexts.write(reinterpret_cast<const char*>(&entry),sizeof(entry));
        o.triangleContexts.write(reinterpret_cast<const char*>(cpu),sizeof(*cpu));
        o.triangleContexts.write(reinterpret_cast<const char*>(stack.data()),stack.size());
        o.triangleContexts.flush();++o.contextRecords;
    }
}
}
void completed(const uint8_t* ram,uint64_t frame) {
    auto& output=CollisionDebug::output();if(!output.enabled || !ram)return;
    output.lastCompleted=frame;
    const Guest g{ram};const uint32_t physical=g.pointer(player,0x40),collision=g.pointer(player,0x44),drive=player+0x3f8;
    const bool haveFrame=g.valid(physical,0xf0),haveContacts=g.valid(collision,0x4fc);
    auto& out=output.frames;
    out<<frame<<','<<player<<','<<physical<<','<<haveFrame<<','<<g.read<uint32_t>(haveFrame?physical+0xec:0)
        <<','<<g.read<uint32_t>(player+0xf0)<<','<<g.read<uint32_t>(player+0x1b8)
        <<','<<g.read<int32_t>(player+0x114)<<','<<g.read<int32_t>(player+0x118)
        <<','<<g.read<float>(player+0x11c)<<','<<g.read<float>(0x357fa0);
    for(uint32_t offset:{0x30,0x80,0xe0,0xc8,0xd4})g.vector(out,haveFrame?physical+offset:0);
    const uint32_t entDriver=g.pointer(player,0xe0),oldDriver=g.pointer(drive,0x1c),driver=g.pointer(drive,0x20);
    out<<','<<entDriver<<','<<g.read<uint32_t>(drive)<<','<<oldDriver<<','<<driver<<','<<g.pointer(drive,0x24);
    for(uint32_t offset:{4,8,12,16,20,24})out<<','<<g.read<float>(drive+offset);
    for(uint32_t offset:{0x28,0x34,0x40})g.vector(out,drive+offset);
    out<<','<<g.read<float>(drive+0x4c)<<','<<g.pointer(player,0x448);g.vector(out,player+0x44c);
    const uint32_t lastFloor=g.read<uint32_t>(0x357f88);out<<','<<lastFloor;
    g.vector(out,0x36e6f0);g.vector(out,0x36e6e0);g.vector(out,0x36e830,4);g.vector(out,0x36e840,4);
    out<<','<<collision<<','<<haveContacts;
    for(uint32_t index=0;index<12;++index)out<<','<<unsigned(g.read<uint8_t>(haveContacts?collision+index:0));
    out<<'\n';
    std::set<uint32_t> visited{player};output.entity(g,frame,player,"player");
    const auto entity=[&](uint32_t address,const char* role){if(address && visited.insert(address).second)output.entity(g,frame,address,role);};
    entity(entDriver,"ent_driver");entity(oldDriver,"old_driver");entity(driver,"driver");entity(lastFloor,"last_floor");
    if(haveContacts)for(unsigned slot=0;slot<18;++slot) {
        const uint32_t contact=collision+0xc+slot*0x44,flags=g.read<uint32_t>(contact),object=g.read<uint32_t>(contact+8);
        output.contacts<<frame<<','<<slot<<','<<(slot<g.read<uint8_t>(collision+0xb))<<','<<bool(flags&1u)
            <<','<<flags<<','<<g.read<uint32_t>(contact+4)<<','<<object<<','<<g.read<float>(contact+0xc);
        for(uint32_t offset:{0x10,0x1c,0x28,0x34})g.vector(output.contacts,contact+offset);
        output.contacts<<','<<g.read<uint32_t>(contact+0x40)<<'\n';
        if((flags&1u) && slot<g.read<uint8_t>(collision+0xb))entity(object,"contact");
    }
    output.frames.flush();output.contacts.flush();output.entities.flush();
}
void install(PS2Runtime& runtime) {
    if(!output().enabled)return;
    const OriginalFunctions functions{runtime.lookupFunction(0x12deb0),runtime.lookupFunction(0x16fec0),runtime.lookupFunction(0x276d00)};
    if(functions.sphere==sphereObserver && functions.select==selectObserver && functions.triangle==triangleObserver)return;
    if(!functions.sphere || !functions.select || !functions.triangle)throw std::runtime_error("Collision diagnostic original functions are unavailable");
    originals[&runtime]=functions;
    if(!runtime.replaceFunction(0x12deb0,sphereObserver) || !runtime.replaceFunction(0x16fec0,selectObserver) ||
        !runtime.replaceFunction(0x276d00,triangleObserver))
        throw std::runtime_error("Cannot install collision diagnostic observers");
}
}
