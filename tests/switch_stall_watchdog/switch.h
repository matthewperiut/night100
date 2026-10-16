#pragma once
#include <cstdint>
#include <cstddef>
using Handle=uint32_t;using Result=uint32_t;using u64=uint64_t;using s64=int64_t;
#define R_FAILED(x) ((x)!=0)
#define R_SUCCEEDED(x) ((x)==0)
constexpr size_t FS_MAX_PATH=0x301;
struct Service {Handle session=0;};struct FsFileSystem {Service s;};struct FsFile {Service s;};
struct Params {bool in_send_pid=false;unsigned out_num_objects=0;Service* out_objects=nullptr;};
Result mockDispatch(Service*,unsigned,Params);
#define serviceDispatchIn(s,c,v,...) mockDispatch(s,c,Params{__VA_ARGS__})
#define serviceDispatch(s,c,...) mockDispatch(s,c,Params{__VA_ARGS__})
inline bool serviceIsActive(Service* s){return s->session!=0;}
void serviceClose(Service*);
Result smGetService(Service*,const char*);
Result fsFsCreateFile(FsFileSystem*,const char*,s64,unsigned);
Result fsFsOpenFile(FsFileSystem*,const char*,unsigned,FsFile*);
Result fsFileGetSize(FsFile*,s64*);
Result fsFileWrite(FsFile*,s64,const void*,size_t,unsigned);
void fsFileClose(FsFile*);void fsFsClose(FsFileSystem*);
constexpr unsigned FsOpenMode_Write=1,FsOpenMode_Append=2,FsWriteOption_Flush=1;
enum ThreadActivity {ThreadActivity_Runnable=0,ThreadActivity_Paused=1};
struct CpuRegister {uint64_t x;};struct ThreadContext {uint64_t lr,sp;CpuRegister pc;};
struct Thread {Handle handle;};
Thread* threadGetSelf();bool envIsSyscallHinted(unsigned);uint64_t armGetSystemTick();
void svcSleepThread(int64_t);
Result svcSetThreadActivity(Handle,ThreadActivity);
Result svcGetThreadContext3(ThreadContext*,Handle);
[[noreturn]] void svcExitProcess();
