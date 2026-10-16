#include "deko3d_backend.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/cpu_work_profile.h"
#include "platform/switch/video.h"
#include <switch.h>
#include <deko3d.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <set>

namespace {
constexpr uint32_t framesInFlight=3,commandBytes=4*1024*1024,streamBytes=4*1024*1024;
// Widescreen: main 640-wide targets are 854 wide. GS coordinates keep their
// 640-pixel area at widePad; world projections scale x by 640/854 so the same
// guest view fills the full target.
constexpr uint32_t gsWidth=640,wideWidth=854,widePad=(wideWidth-gsWidth)/2;
constexpr float wideProjectionScale=float(gsWidth)/float(wideWidth);
// Display-resolution rendering: main (fbw 10) targets are rendered at the 1280x720
// output size, 960 columns for the 4:3 area (pad 160 when wide) and 720 rows for the
// 448 GS rows. GS coordinates scale by hdScaleX/hdScaleY; 3D scales through the viewport.
constexpr uint32_t hdWidth=960,hdWideWidth=1280,hdPad=(hdWideWidth-hdWidth)/2,hdRows=720,gsRows=448;
constexpr float hdScaleX=float(hdWidth)/float(gsWidth),hdScaleY=float(hdRows)/float(gsRows);
constexpr uint32_t descriptorCount=4096;
constexpr uint64_t renderTexturePoolBytesPerSlot=16*1024*1024;
constexpr uint32_t renderTexturePoolImagesPerSlot=64;
uint32_t alignUp(uint32_t value,uint32_t alignment) {return (value+alignment-1)&~(alignment-1);}
[[noreturn]] void fail(const std::string& message) {throw std::runtime_error("deko3d: "+message);}
DkPrimitive primitive(uint32_t type) {
    constexpr DkPrimitive values[]={DkPrimitive_Points,DkPrimitive_Lines,DkPrimitive_LineStrip,
        DkPrimitive_Triangles,DkPrimitive_TriangleStrip,DkPrimitive_TriangleFan};
    if(type>=std::size(values))fail("unsupported primitive type");return values[type];
}
DkBlendFactor blend(uint32_t value) {
    if(value<1 || value>11)fail("unsupported RenderWare blend factor");
    return static_cast<DkBlendFactor>(value);
}
DkWrapMode wrap(uint32_t value) {
    return value==1?DkWrapMode_Repeat:value==2?DkWrapMode_MirroredRepeat:DkWrapMode_ClampToEdge;
}
std::optional<dk::BlendState> gsBlendState(unsigned a,unsigned b,unsigned c,unsigned d,unsigned fixed,std::string& reason) {
    // (A-B)*C+D = Src*(sourceBase+sourceSlope*C)
    //                + Dst*(destinationBase+destinationSlope*C).
    // A/B/D selectors 2 and 3 both select zero in the GS reference.
    const int sourceSlope=int(a==0)-int(b==0),destinationSlope=int(a==1)-int(b==1);
    if(c>=2 && fixed>128 && (sourceSlope || destinationSlope)) {
        reason="FIX above 128 requires an unclamped blend factor";return std::nullopt;
    }
    const DkBlendFactor factor=c==0?DkBlendFactor_SrcAlpha:c==1?DkBlendFactor_DstAlpha:DkBlendFactor_ConstAlpha;
    const DkBlendFactor inverse=c==0?DkBlendFactor_InvSrcAlpha:c==1?DkBlendFactor_InvDstAlpha:DkBlendFactor_InvConstAlpha;
    struct Coefficient {DkBlendFactor factor;bool negative=false;};
    auto coefficient=[&](bool base,int slope)->std::optional<Coefficient> {
        if(c>=2 && fixed==0)slope=0;
        if(!slope)return Coefficient{base?DkBlendFactor_One:DkBlendFactor_Zero};
        if(slope<0)return Coefficient{base?inverse:factor,!base};
        if(!base)return Coefficient{factor};
        reason="equation requires a 1+C blend factor";return std::nullopt;
    };
    const auto source=coefficient(d==0,sourceSlope),destination=coefficient(d==1,destinationSlope);
    if(!source || !destination)return std::nullopt;
    dk::BlendState result;
    result.setFactors(source->factor,destination->factor,DkBlendFactor_One,DkBlendFactor_Zero);
    result.setColorBlendOp(source->negative?DkBlendOp_RevSub:destination->negative?DkBlendOp_Sub:DkBlendOp_Add);
    return result;
}
struct alignas(16) Transform {
    NativeMatrix world,camera;
    std::array<float,4> depthSurface;
    std::array<uint32_t,4> config;
    std::array<float,4> material,ambient;
    std::array<std::array<float,4>,8> lightColor,lightDirection;
    std::array<float,4> fog;
};
static_assert(sizeof(Transform)==464);
struct alignas(16) Fragment {
    std::array<float,4> controls{},fog{},gsAlphaTest{};
};
static_assert(sizeof(Fragment)==48);

// Opt-in render thread. Threaded mode runs
// every Impl operation on one worker in issue order. The producer never touches
// Impl there: it posts closures that own copies of their inputs, or calls one
// and waits. When not running, operations execute on the caller as before.
class RenderQueue {
public:
    using Operation=std::function<void()>;
    static constexpr size_t maxPendingBatches=framesInFlight;
    static constexpr size_t maxOperationsPerBatch=256;

    void start(bool enable) {
        if(!enable)return;
        std::lock_guard lock(mutex);
        running=true;
        worker=std::thread([this]{run();});workerId=worker.get_id();
    }
    // Seals the open batch, lets the worker finish queued work, then joins it.
    void stop() {
        {
            std::lock_guard lock(mutex);
            if(!running)return;
            if(!open.empty()){ready.push_back(std::move(open));open.clear();}
            running=false;stopping=true;
        }
        work.notify_all();
        worker.join();
    }
    ~RenderQueue() {stop();}

    // Fire-and-forget. The closure must own its inputs by value.
    // While a DirectScope is alive, every operation runs inline on the caller and
    // the worker is drained, so synchronous exceptions and reads keep their order.
    class DirectScope {
    public:
        explicit DirectScope(RenderQueue& queue):queue(queue) {
            queue.drain();
            std::lock_guard lock(queue.mutex);queue.direct=true;
        }
        ~DirectScope() {std::lock_guard lock(queue.mutex);queue.direct=false;}
        DirectScope(const DirectScope&)=delete;
        DirectScope& operator=(const DirectScope&)=delete;
    private:
        RenderQueue& queue;
    };

    template<class F> void post(F&& operation,bool boundary=false) {
        std::unique_lock lock(mutex);
        if(!running || direct){lock.unlock();operation();return;}
        rethrowFailure();
        open.emplace_back(std::forward<F>(operation));
        if(boundary || open.size()>=maxOperationsPerBatch)sealLocked(lock);
    }
    void seal() {
        std::unique_lock lock(mutex);
        if(running)sealLocked(lock);
    }
    // Synchronous: runs on the worker and returns its result. Exceptions thrown
    // by the operation propagate to the caller, exactly as the inline path did.
    template<class F> auto call(F&& operation) -> std::invoke_result_t<F&> {
        using R=std::invoke_result_t<F&>;
        bool inlineNow;
        { std::lock_guard lock(mutex); inlineNow=!running || direct; }
        if(inlineNow || std::this_thread::get_id()==workerId)return operation();
        auto promise=std::make_shared<std::promise<R>>();
        auto result=promise->get_future();
        post([promise,&operation]{
            try {
                if constexpr(std::is_void_v<R>) {operation();promise->set_value();}
                else promise->set_value(operation());
            } catch(...) {promise->set_exception(std::current_exception());}
        },true);
        // Only the queued closure holds the promise, so a discarded closure
        // breaks it and wakes this waiter.
        promise.reset();
        try {
            if constexpr(std::is_void_v<R>) {result.get();}
            else {return result.get();}
        } catch(const std::future_error&) {
            std::lock_guard lock(mutex);
            rethrowFailure();
            throw std::runtime_error("deko3d: render operation was discarded without a result");
        }
    }
    void drain() {call([]{});}

private:
    void run() {
        for(;;) {
            std::vector<Operation> batch;
            {
                std::unique_lock lock(mutex);
                work.wait(lock,[this]{return !ready.empty() || stopping;});
                if(ready.empty())return;
                batch=std::move(ready.front());ready.pop_front();
            }
            space.notify_all();
            for(auto& operation:batch) {
                if(failed.load(std::memory_order_acquire))break;
                try {operation();}
                catch(...) {recordFailure(std::current_exception());break;}
            }
            // Discarded closures break their call() promises, waking waiters.
            batch.clear();
        }
    }
    void recordFailure(std::exception_ptr error) {
        std::string message;
        try {std::rethrow_exception(error);}
        catch(const std::exception& e) {message=e.what();}
        catch(...) {message="non-standard exception";}
        std::cerr<<"[deko3d-render] fatal: "<<message<<'\n';
        std::lock_guard lock(mutex);
        if(!failure)failure=error;
        failed.store(true,std::memory_order_release);
        space.notify_all();
    }
    // Caller holds mutex.
    void rethrowFailure() const {if(failure)std::rethrow_exception(failure);}
    void sealLocked(std::unique_lock<std::mutex>& lock) {
        if(open.empty())return;
        space.wait(lock,[this]{return ready.size()<maxPendingBatches || failed.load(std::memory_order_acquire);});
        if(failed.load(std::memory_order_acquire)){open.clear();rethrowFailure();}
        ready.push_back(std::move(open));open.clear();
        work.notify_one();
    }

    std::mutex mutex;
    std::condition_variable work,space;
    std::deque<std::vector<Operation>> ready;
    std::vector<Operation> open;
    std::exception_ptr failure;
    std::atomic<bool> failed{false};
    bool running=false,stopping=false,direct=false;
    std::thread::id workerId;
    std::thread worker;
};

// Inline is the default. NIGHT100_INLINE_RENDER=1 always forces inline.
bool renderThreadEnabled() {
    if(const char* forced=std::getenv("NIGHT100_INLINE_RENDER"); forced && *forced=='1')return false;
    const char* requested=std::getenv("NIGHT100_RENDER_THREAD");
    return !requested || *requested!='0';
}
std::array<uint8_t,7> metricGlyph(char character) {
    // Five-bit rows, authored here so the diagnostic HUD needs no game font.
    switch(character) {
        case '0':return {14,17,19,21,25,17,14};
        case '1':return {4,12,4,4,4,4,14};
        case '2':return {14,17,1,2,4,8,31};
        case '3':return {30,1,1,14,1,1,30};
        case '4':return {2,6,10,18,31,2,2};
        case '5':return {31,16,16,30,1,1,30};
        case '6':return {14,16,16,30,17,17,14};
        case '7':return {31,1,2,4,8,8,8};
        case '8':return {14,17,17,14,17,17,14};
        case '9':return {14,17,17,15,1,1,14};
        case 'A':return {14,17,17,31,17,17,17};
        case 'B':return {30,17,17,30,17,17,30};
        case 'C':return {14,17,16,16,16,17,14};
        case 'D':return {30,17,17,17,17,17,30};
        case 'E':return {31,16,16,30,16,16,31};
        case 'F':return {31,16,16,30,16,16,16};
        case 'G':return {14,17,16,23,17,17,15};
        case 'H':return {17,17,17,31,17,17,17};
        case 'I':return {14,4,4,4,4,4,14};
        case 'J':return {7,2,2,2,18,18,12};
        case 'K':return {17,18,20,24,20,18,17};
        case 'L':return {16,16,16,16,16,16,31};
        case 'M':return {17,27,21,21,17,17,17};
        case 'N':return {17,25,25,21,19,19,17};
        case 'O':return {14,17,17,17,17,17,14};
        case 'P':return {30,17,17,30,16,16,16};
        case 'Q':return {14,17,17,17,21,18,13};
        case 'R':return {30,17,17,30,20,18,17};
        case 'S':return {15,16,16,14,1,1,30};
        case 'T':return {31,4,4,4,4,4,4};
        case 'U':return {17,17,17,17,17,17,14};
        case 'V':return {17,17,17,17,17,10,4};
        case 'W':return {17,17,17,21,21,27,17};
        case 'X':return {17,17,10,4,10,17,17};
        case 'Y':return {17,17,10,4,4,4,4};
        case 'Z':return {31,1,2,4,8,16,31};
        case '/':return {1,2,2,4,8,8,16};
        case '.':return {0,0,0,0,0,12,12};
        case ':':return {0,12,12,0,12,12,0};
        case '-':return {0,0,0,31,0,0,0};
        case '<':return {2,4,8,16,8,4,2};
        case '>':return {8,4,2,1,2,4,8};
        case '(':return {2,4,8,8,8,4,2};
        case ')':return {8,4,2,2,2,4,8};
        case '%':return {17,2,4,4,8,16,17};
        default:return {};
    }
}
}

struct Deko3DBackend::Impl {
    GSCpuBackend memory; // Address/transfer/CLUT codec only; never rasterizes.
    mutable std::recursive_mutex mutex;
    RenderQueue render;
    std::string shaderDirectory;
    Statistics stats{};
    static_assert(std::atomic<Stage>::is_always_lock_free && std::atomic<uint32_t>::is_always_lock_free && std::atomic<uint64_t>::is_always_lock_free);
    std::atomic<Stage> diagnosticStage{Stage::Idle};
    std::atomic<uint32_t> diagnosticSlot{0};
    std::atomic<uint64_t> diagnosticSerial{0},diagnosticSubmissions{0},diagnosticGameFrames{0},diagnosticReadbacks{0};
    std::atomic<uint32_t> diagnosticTextures{0},diagnosticLegacyTextures{0},diagnosticDescriptorHighWater{3},diagnosticRetiredDescriptors{0};
    std::atomic<uint64_t> diagnosticRttAllocations{0},diagnosticRttReuses{0},diagnosticRttPoolBytes{0};
    std::atomic<uint32_t> diagnosticRttPoolImages{0};
    std::atomic<uint64_t> diagnosticDynamicAllocations{0},diagnosticDynamicUploads{0},diagnosticDynamicUploadBytes{0},diagnosticDynamicBufferBytes{0};
    std::atomic<uint32_t> diagnosticDynamicBlocks{0};
    std::atomic<bool(*)()> presentationActivePredicate{nullptr};
    std::atomic<bool> wideConfig{false},worldGate{false},guestWide{false};
    std::atomic<int> resolutionMode{1}; // 0 half, 1 native, 2 double (720p)
    std::atomic<uint64_t> probeWideTargets{0},probeGateTokens{0},probeFullWidth{0};
    std::atomic<uint64_t> probeTimingReads{0},probeTimingRejected{0},probeTimingRenderOnly{0};
    std::atomic<uint64_t> probeLastStart{0},probeLastComposition{0},probeLastEnd{0};
    std::set<uint64_t> wideGateTokens;
    std::atomic<bool> metricsEnabled{false};
    std::atomic<double> metricsGameFps{0},metricsCpuMs{0},metricsGpuMs{-1};
    std::vector<NativeScreenVertex> metricsVertices;
    std::mutex replayOverlayMutex;
    std::string replayOverlay;
    std::atomic<bool> replayOverlayEnabled{false};
    struct DiagnosticScope {
        Impl& owner;
        Stage previous;
        DiagnosticScope(Impl& owner,Stage stage):owner(owner),previous(owner.diagnosticStage.load(std::memory_order_relaxed)) {
            owner.diagnosticSlot.store(owner.slot,std::memory_order_relaxed);
            owner.diagnosticSerial.fetch_add(1,std::memory_order_relaxed);
            owner.diagnosticStage.store(stage,std::memory_order_relaxed);
        }
        ~DiagnosticScope(){owner.diagnosticStage.store(previous,std::memory_order_relaxed);}
    };
    dk::UniqueDevice device;
    dk::UniqueQueue queue;
    dk::UniqueCmdBuf commands;
    dk::UniqueCmdBuf stateEncoder;
    std::map<uint32_t,std::array<uint32_t,7>> blendCommands;
    struct Image {
        dk::UniqueMemBlock memory;
        dk::Image image;
        uint32_t width=0,height=0;
    };
    struct Slot {
        struct DynamicBuffer {dk::UniqueMemBlock memory;uint32_t cursor=0,capacity=0;};
        dk::UniqueMemBlock command,stream;
        dk::Fence fence;
        bool submitted=false;
        uint32_t cursor=0;
        // Staging and retired resources stay alive through this slot's GPU fence.
        std::vector<dk::UniqueMemBlock> transient;
        std::vector<std::shared_ptr<const void>> lifetimes;
        std::vector<uint32_t> retiredDescriptors;
        // These images remain allocated between batches. Only begin(), after
        // waiting this slot's fence, resets the immutable snapshot cursor.
        std::vector<Image> renderTextureImages;
        uint32_t renderTextureUsed=0;
        uint64_t renderTextureBytes=0;
        std::vector<DynamicBuffer> dynamicBuffers;
        uint32_t dynamicBuffer=0;
        // Timestamps at 16-byte strides: [0] batch start, [16] composition, [32] batch end.
        // Read only after this slot's fence has completed.
        dk::UniqueMemBlock timing;
        bool timingPending=false,timingComposition=false;
    };
    struct GpuFrame {bool open=false;uint64_t start=0,end=0;} gpuFrame;
    std::atomic<uint64_t> diagnosticGpuNs{0},diagnosticGpuSamples{0},diagnosticCompositionNs{0},diagnosticCompositionSamples{0};
    std::array<Slot,framesInFlight> slots;
    uint32_t slot=0;
    bool recording=false;
    std::vector<dk::UniqueMemBlock> spillCommands;
    dk::UniqueMemBlock imageDescriptors,samplerDescriptors;
    dk::UniqueMemBlock zeroMatrices;
    std::vector<uint32_t> freeDescriptors;
    uint32_t nextDescriptor=3;
    struct Shader {dk::UniqueMemBlock code;dk::Shader shader;};
    Shader modelShader,screenShader,fragmentShader;
    Shader videoVertex,videoFragment;
    Shader renderTextureFragment;
    struct Target {
        GSContext context{};
        Image color,depth;
        bool colorDirty=false,depthDirty=false;
        bool wide=false;
        // Host layout: pad columns before the 640-column GS area, GS rows covered,
        // and the host-per-GS pixel scale (1 for native, hdScale* for 720p).
        uint32_t pad=0,rows=0;float sx=1.f,sy=1.f;
        // Set when a CPU write changed canonical memory under this target.
        bool reseed=false;
    };
    struct Layout{uint32_t width,pad,height;float sx,sy;};
    Layout layoutFor(const GSContext& c,bool wide,uint32_t rows) const {
        const uint32_t gsw=c.frame.fbw*64u;
        Layout l{};
        const int mode=c.frame.fbw==10?resolutionMode.load(std::memory_order_relaxed):1;
        if(mode==2) {l.sx=hdScaleX;l.sy=hdScaleY;l.pad=wide?hdPad:0u;l.width=wide?hdWideWidth:hdWidth;}
        else if(mode==0) {l.sx=0.5f;l.sy=0.5f;l.pad=wide?widePad/2:0u;l.width=wide?wideWidth/2:gsw/2;}
        else {l.sx=1.f;l.sy=1.f;l.pad=wide?widePad:0u;l.width=wide?wideWidth:gsw;}
        l.height=std::max(1u,uint32_t(std::lround(double(rows)*l.sy)));
        return l;
    }
    static uint32_t hostRowsOf(const Target& t,uint32_t rows){return std::max(1u,uint32_t(std::lround(double(rows)*t.sy)));}
    Image white;
    bool whiteReady=false;
    std::map<std::array<uint32_t,4>,Target> targets;
    Target* active=nullptr;
    Target* lastMain=nullptr;
    using TargetKey=std::array<uint32_t,4>;
    std::optional<TargetKey> completedTarget;
    uint32_t completedWidth=0,completedHeight=0;
    struct SavedTarget {
        GSContext context{};uint32_t width=0,height=0;
        std::vector<uint8_t> color,depth;
    };
    struct SavedTexture {
        std::array<uint64_t,5> key{};
        uint32_t width=0,height=0,wrapU=0,wrapV=0;bool linear=false;
        std::vector<uint8_t> rgba;
    };
    std::vector<SavedTarget> savedTargets;
    std::vector<SavedTexture> savedTextures;
    uint64_t stateRevision=0,preparedRevision=~0ull;
    std::array<Image,framesInFlight> displays;
    dk::UniqueSwapchain swapchain;
    struct Geometry {
        dk::UniqueMemBlock vertices,indices,inverse;
        std::weak_ptr<const NativeAssetLifetime> lifetime;
        uint64_t revision=0;
    };
    struct GeometryBinding {DkGpuAddr vertices=0,indices=0,inverse=0;};
    struct Texture {
        Image image;
        uint32_t descriptor=0;
        uint64_t legacyEpoch=0;
        std::weak_ptr<const NativeAssetLifetime> lifetime;
        bool linear=false;
        uint32_t wrapU=0,wrapV=0;
    };
    std::unordered_map<uint64_t,Geometry> geometries;
    std::unordered_map<uint64_t,Texture> textures;
    struct RenderSource {GSTex0Reg gs;GSTexaReg alpha;std::weak_ptr<const NativeAssetLifetime> lifetime;};
    std::unordered_map<uint64_t,RenderSource> renderSources;
    std::map<uint64_t,std::variant<NativeModelDraw,NativeScreenDraw>> pending;
    uint64_t nextToken=0,nextRenderSource=0;
    GSTransferCommand transfer{};
    uint64_t textureEpoch=0;
    std::map<std::array<uint64_t,5>,uint64_t> legacyTextures;
    uint64_t nextLegacyTexture=1ull<<62;
    unsigned targetReports=0;
    unsigned reportedAlphaFailModes=0;

    explicit Impl(std::string path):shaderDirectory(std::move(path)) {}
    ~Impl() {
        if(queue && !queue.isInErrorState()){DiagnosticScope diagnostic(*this,Stage::QueueIdle);queue.waitIdle();}
        // Slot members otherwise destruct in reverse declaration order. Keep
        // FFmpeg's external storage alive until deko's imported maps are gone,
        // just as normal, fence-completed slot retirement does.
        for(auto& s:slots){s.transient.clear();s.lifetimes.clear();}
    }
    bool presentationActive() const noexcept {
        const auto predicate=presentationActivePredicate.load(std::memory_order_relaxed);
        return !predicate || predicate();
    }
    void checkedFenceWait(dk::Fence& fence,const char* operation) {
        if(queue.isInErrorState())fail(std::string(operation)+": queue is in error state before fence wait");
        const auto result=fence.wait();
        if(result!=DkResult_Success)fail(std::string(operation)+": GPU fence wait failed result="+std::to_string(int(result))+
            " command_slot="+std::to_string(slot));
        if(queue.isInErrorState())fail(std::string(operation)+": queue entered error state while waiting on fence");
    }
    dk::UniqueMemBlock allocate(uint32_t size,uint32_t flags) {
        auto block=dk::MemBlockMaker{device,alignUp(std::max(1u,size),DK_MEMBLOCK_ALIGNMENT)}.setFlags(flags).create();
        if(!block)fail("GPU memory allocation failed: "+std::to_string(size));return dk::UniqueMemBlock{std::move(block)};
    }
    void loadShader(Shader& output,const char* filename) {
        std::ifstream file(shaderDirectory+"/"+filename,std::ios::binary|std::ios::ate);
        if(!file)fail("cannot open shader "+shaderDirectory+"/"+filename);
        const auto length=file.tellg();if(length<24 || length>4*1024*1024)fail("invalid DKSH length");
        std::vector<uint8_t> bytes(static_cast<size_t>(length));file.seekg(0);file.read(reinterpret_cast<char*>(bytes.data()),bytes.size());
        uint32_t header[6];std::memcpy(header,bytes.data(),sizeof(header));
        if(header[0]!=0x48534b44 || header[2]>bytes.size() || header[3]>bytes.size()-header[2])fail("invalid DKSH header");
        output.code=allocate(header[3]+DK_SHADER_CODE_UNUSABLE_SIZE,DkMemBlockFlags_CpuUncached|DkMemBlockFlags_GpuCached|DkMemBlockFlags_Code);
        std::memcpy(output.code.getCpuAddr(),bytes.data()+header[2],header[3]);
        dk::ShaderMaker{output.code,0}.setControl(bytes.data()).initialize(output.shader);
        if(!output.shader.isValid())fail("invalid compiled shader "+std::string(filename));
    }
    void initializeGpu() {
        if(device)return;
        DiagnosticScope diagnostic(*this,Stage::Initializing);
        device=dk::DeviceMaker{}.setCbDebug([](void*,const char* context,DkResult result,const char* message) {
            std::cerr<<"[deko3d-driver] "<<(context?context:"")<<" result="<<int(result)<<" "<<(message?message:"")<<'\n';
            if(result!=DkResult_Success) {
                // Validation errors can terminate inside the driver before the
                // host telemetry interval flushes. Preserve that exact error.
                std::ofstream file("sdmc:/switch/night100/logs/deko-driver-errors.log",std::ios::app);
                file<<(context?context:"")<<" result="<<int(result)<<" "<<(message?message:"")<<'\n';
            }
        }).create();if(!device)fail("device creation failed");
        queue=dk::QueueMaker{device}.setFlags(DkQueueFlags_Graphics).create();if(!queue)fail("queue creation failed");
        commands=dk::CmdBufMaker{device}.setUserData(this).setCbAddMem([](void* p,DkCmdBuf cmd,size_t required) {
            auto& self=*static_cast<Impl*>(p);
            auto block=self.allocate(std::max(commandBytes,uint32_t(required)),DkMemBlockFlags_CpuUncached|DkMemBlockFlags_GpuCached);
            dkCmdBufAddMemory(cmd,block,0,block.getSize());self.slots[self.slot].transient.push_back(std::move(block));
        }).create();
        stateEncoder=dk::CmdBufMaker{device}.create();
        for(auto& s:slots) {
            s.command=allocate(commandBytes,DkMemBlockFlags_CpuUncached|DkMemBlockFlags_GpuCached);
            s.stream=allocate(streamBytes,DkMemBlockFlags_CpuUncached|DkMemBlockFlags_GpuCached);
        }
        imageDescriptors=allocate(descriptorCount*sizeof(DkImageDescriptor),DkMemBlockFlags_GpuCached);
        samplerDescriptors=allocate(descriptorCount*sizeof(DkSamplerDescriptor),DkMemBlockFlags_GpuCached);
        loadShader(modelShader,"native.vert.dksh");loadShader(screenShader,"screen.vert.dksh");loadShader(fragmentShader,"native.frag.dksh");
        loadShader(videoVertex,"video.vert.dksh");loadShader(videoFragment,"video.frag.dksh");
        loadShader(renderTextureFragment,"render_texture.frag.dksh");
        white=makeImage(1,1,DkImageFormat_RGBA8_Unorm,DkImageFlags_Usage2DEngine);
        for(auto& display:displays)display=makeImage(1280,720,DkImageFormat_RGBA8_Unorm,DkImageFlags_UsageRender|DkImageFlags_UsagePresent|DkImageFlags_Usage2DEngine);
        std::array<DkImage const*,framesInFlight> images{};for(size_t i=0;i<images.size();++i)images[i]=&displays[i].image;
        swapchain=dk::SwapchainMaker{device,nwindowGetDefault(),images}.create();if(!swapchain)fail("swapchain creation failed");
        std::cout<<"[deko3d] device, graphics queue, six Maxwell shaders and 1280x720 swapchain initialized\n";
    }
    Image makeImage(uint32_t w,uint32_t h,DkImageFormat format,uint32_t flags) {
        Image result;result.width=w;result.height=h;
        dk::ImageLayout layout;dk::ImageLayoutMaker{device}.setFormat(format).setDimensions(w,h).setFlags(flags).initialize(layout);
        if(layout.getSize()>UINT32_MAX)fail("oversized image");
        result.memory=allocate(layout.getSize(),DkMemBlockFlags_GpuCached|DkMemBlockFlags_Image);
        result.image.initialize(layout,result.memory,0);return result;
    }
    void publishRenderTextureDiagnostics() {
        uint64_t bytes=0;uint32_t images=0;
        for(const auto& s:slots){bytes+=s.renderTextureBytes;images+=s.renderTextureImages.size();}
        diagnosticRttAllocations.store(stats.renderTextureImageAllocations,std::memory_order_relaxed);
        diagnosticRttReuses.store(stats.renderTextureImageReuses,std::memory_order_relaxed);
        diagnosticRttPoolBytes.store(bytes,std::memory_order_relaxed);
        diagnosticRttPoolImages.store(images,std::memory_order_relaxed);
    }
    Image& renderTextureImage(uint32_t width,uint32_t height,Image& transient) {
        auto& s=slots[slot];
        for(size_t i=s.renderTextureUsed;i<s.renderTextureImages.size();++i) {
            const auto& candidate=s.renderTextureImages[i];
            if(candidate.width==width && candidate.height==height) {
                std::swap(s.renderTextureImages[i],s.renderTextureImages[s.renderTextureUsed]);
                ++stats.renderTextureImageReuses;publishRenderTextureDiagnostics();
                return s.renderTextureImages[s.renderTextureUsed++];
            }
        }
        auto image=makeImage(width,height,DkImageFormat_RGBA8_Unorm,DkImageFlags_UsageRender|DkImageFlags_Usage2DEngine);
        ++stats.renderTextureImageAllocations;const uint64_t bytes=image.memory.getSize();
        if(bytes<=renderTexturePoolBytesPerSlot) {
            // Only the unused tail belongs solely to a completed earlier
            // batch. Eviction on a dimension change never touches this batch's
            // earlier snapshots, which may still be sampled by the GPU.
            while(s.renderTextureImages.size()>s.renderTextureUsed &&
                  (s.renderTextureImages.size()>=renderTexturePoolImagesPerSlot || s.renderTextureBytes+bytes>renderTexturePoolBytesPerSlot)) {
                s.renderTextureBytes-=s.renderTextureImages.back().memory.getSize();s.renderTextureImages.pop_back();
            }
            if(s.renderTextureImages.size()<renderTexturePoolImagesPerSlot && s.renderTextureBytes+bytes<=renderTexturePoolBytesPerSlot) {
                s.renderTextureBytes+=bytes;s.renderTextureImages.push_back(std::move(image));
                std::swap(s.renderTextureImages.back(),s.renderTextureImages[s.renderTextureUsed]);
                publishRenderTextureDiagnostics();return s.renderTextureImages[s.renderTextureUsed++];
            }
        }
        // Unusual oversized/high-count batches retain the prior fenced
        // transient behavior instead of imposing a new rendering limit.
        transient=std::move(image);publishRenderTextureDiagnostics();return transient;
    }
    // Converts one completed batch's stamps into the per-game-frame sums. Frames
    // close at their composition batch; a frame without one remains open.
    void readGpuTiming(Slot& s) {
        if(!s.timingPending)return;
        s.timingPending=false;
        // Each report needs a 16-byte aligned destination (deko3d misaligned-data error).
        const auto* base=static_cast<const uint8_t*>(s.timing.getCpuAddr());
        uint64_t start=0,composition=0,end=0;
        // Each report is a 16-byte record {value, timestamp}. The console shows the
        // first word stays zero, so the timestamp is the second word of each record.
        std::memcpy(&start,base+8,8);std::memcpy(&composition,base+24,8);std::memcpy(&end,base+40,8);
        const uint64_t readIndex=probeTimingReads.fetch_add(1,std::memory_order_relaxed);
        if(readIndex<3) {
            uint64_t raw[8]{};std::memcpy(raw,base,64);
            std::cout<<"[gpu-timing] read="<<readIndex<<" slot="<<slot<<" addr="<<std::hex<<s.timing.getGpuAddr()<<std::dec
                <<" words="<<std::hex<<raw[0]<<","<<raw[1]<<","<<raw[2]<<","<<raw[3]<<","<<raw[4]<<","<<raw[5]<<std::dec<<" composition_flag="<<s.timingComposition<<'\n';
        }
        probeLastStart.store(start,std::memory_order_relaxed);probeLastComposition.store(composition,std::memory_order_relaxed);
        probeLastEnd.store(end,std::memory_order_relaxed);
        if(!s.timingComposition)probeTimingRenderOnly.fetch_add(1,std::memory_order_relaxed);
        if(!start || end<start) {probeTimingRejected.fetch_add(1,std::memory_order_relaxed);return;}
        if(!gpuFrame.open) {gpuFrame.open=true;gpuFrame.start=start;}
        const bool hasComposition=s.timingComposition && composition>=start && end>=composition;
        gpuFrame.end=hasComposition?composition:end;
        if(!hasComposition)return;
        stats.gpuNanoseconds+=dkTimestampToNs(gpuFrame.end-gpuFrame.start);++stats.gpuSamples;
        stats.compositionNanoseconds+=dkTimestampToNs(end-composition);++stats.compositionSamples;
        gpuFrame.open=false;
        diagnosticGpuNs.store(stats.gpuNanoseconds,std::memory_order_relaxed);
        diagnosticGpuSamples.store(stats.gpuSamples,std::memory_order_relaxed);
        diagnosticCompositionNs.store(stats.compositionNanoseconds,std::memory_order_relaxed);
        diagnosticCompositionSamples.store(stats.compositionSamples,std::memory_order_relaxed);
    }
    void begin() {
        ++stateRevision;
        initializeGpu();if(recording)return;
        auto& s=slots[slot];if(s.submitted){DiagnosticScope diagnostic(*this,Stage::FenceWait);checkedFenceWait(s.fence,"reuse command slot");s.submitted=false;readGpuTiming(s);}
        s.renderTextureUsed=0;
        s.dynamicBuffer=0;for(auto& buffer:s.dynamicBuffers)buffer.cursor=0;
        // Descriptor slots, like their images, cannot be reused until every
        // earlier consumer in this command slot has completed on the GPU.
        freeDescriptors.insert(freeDescriptors.end(),s.retiredDescriptors.begin(),s.retiredDescriptors.end());
        s.retiredDescriptors.clear();
        {DiagnosticScope diagnostic(*this,Stage::ResourceRetirement);s.transient.clear();s.lifetimes.clear();}
        s.cursor=0;commands.clear();commands.addMemory(s.command,0,s.command.getSize());
        if(!s.timing)s.timing=allocate(64,DkMemBlockFlags_CpuUncached|DkMemBlockFlags_GpuCached);
        // Sentinel: a counter the GPU never wrote still reads 0xA5 after its fence.
        std::memset(s.timing.getCpuAddr(),0xA5,64);
        s.timingComposition=false;commands.reportCounter(DkCounter_TimestampPipelineTop,s.timing.getGpuAddr());
        commands.barrier(DkBarrier_None,DkInvalidateFlags_Shader|DkInvalidateFlags_L2Cache);
        commands.bindImageDescriptorSet(imageDescriptors.getGpuAddr(),descriptorCount);
        commands.bindSamplerDescriptorSet(samplerDescriptors.getGpuAddr(),descriptorCount);
        recording=true;active=nullptr;retire();
        if(!whiteReady) {
            const uint32_t rgba=0xffffffffu;uploadPixels(white,&rgba);descriptors(2,white.image,false,3,3);whiteReady=true;
        }
        if(!zeroMatrices) {
            const std::array<uint8_t,sizeof(NativeMatrix)*64> zero{};zeroMatrices=upload(zero.data(),zero.size());
        }
    }
    void submit() {
        if(!recording)return;
        DiagnosticScope diagnostic(*this,Stage::QueueSubmit);
        commands.reportCounter(DkCounter_Timestamp,slots[slot].timing.getGpuAddr()+32);slots[slot].timingPending=true;
        commands.signalFence(slots[slot].fence,true);queue.submitCommands(commands.finishList());queue.flush();
        slots[slot].submitted=true;++stats.gpuSubmissions;recording=false;slot=(slot+1)%framesInFlight;active=nullptr;
        diagnosticSubmissions.store(stats.gpuSubmissions,std::memory_order_relaxed);
        if(queue.isInErrorState())fail("GPU queue entered error state");
    }
    void idle() {
        submit();if(!queue)return;
        DiagnosticScope diagnostic(*this,Stage::QueueIdle);
        if(queue.isInErrorState())fail("quiesce: queue is in error state before GPU drain");
        dk::Fence fence;queue.signalFence(fence,true);queue.flush();checkedFenceWait(fence,"quiesce GPU queue");
    }
    DkGpuAddr stream(const void* data,uint32_t bytes,uint32_t alignment=16) {
        begin();auto& s=slots[slot];const uint32_t offset=alignUp(s.cursor,alignment);
        if(bytes>streamBytes || offset>streamBytes-bytes) {
            // Overflow allocations are fenced with the current batch; no wrap over live data.
            auto block=allocate(bytes,DkMemBlockFlags_CpuUncached|DkMemBlockFlags_GpuCached);
            std::memcpy(block.getCpuAddr(),data,bytes);auto address=block.getGpuAddr();s.transient.push_back(std::move(block));return address;
        }
        std::memcpy(static_cast<uint8_t*>(s.stream.getCpuAddr())+offset,data,bytes);s.cursor=offset+bytes;
        return s.stream.getGpuAddr()+offset;
    }
    dk::UniqueMemBlock upload(const void* data,uint32_t bytes) {
        begin();auto gpu=allocate(bytes,DkMemBlockFlags_GpuCached);
        const auto source=stream(data,bytes);commands.copyBuffer(source,gpu.getGpuAddr(),bytes);
        commands.barrier(DkBarrier_Full,DkInvalidateFlags_L2Cache);return std::move(gpu);
    }
    void publishDynamicDiagnostics() {
        uint64_t bytes=0;uint32_t blocks=0;
        for(const auto& s:slots)for(const auto& buffer:s.dynamicBuffers){bytes+=buffer.capacity;++blocks;}
        diagnosticDynamicAllocations.store(stats.dynamicBufferAllocations,std::memory_order_relaxed);
        diagnosticDynamicUploads.store(stats.dynamicGeometryUploads,std::memory_order_relaxed);
        diagnosticDynamicUploadBytes.store(stats.dynamicUploadBytes,std::memory_order_relaxed);
        diagnosticDynamicBufferBytes.store(bytes,std::memory_order_relaxed);
        diagnosticDynamicBlocks.store(blocks,std::memory_order_relaxed);
    }
    DkGpuAddr dynamicUpload(const void* data,uint32_t bytes,uint32_t alignment=16) {
        begin();auto& s=slots[slot];
        for(;;++s.dynamicBuffer) {
            if(s.dynamicBuffer==s.dynamicBuffers.size()) {
                Slot::DynamicBuffer buffer;
                buffer.memory=allocate(std::max(streamBytes,alignUp(bytes,DK_MEMBLOCK_ALIGNMENT)),DkMemBlockFlags_GpuCached);
                buffer.capacity=buffer.memory.getSize();
                s.dynamicBuffers.push_back(std::move(buffer));++stats.dynamicBufferAllocations;
            }
            auto& buffer=s.dynamicBuffers[s.dynamicBuffer];const uint32_t offset=alignUp(buffer.cursor,alignment);
            if(bytes>buffer.capacity || offset>buffer.capacity-bytes)continue;
            const auto destination=buffer.memory.getGpuAddr()+offset;
            const auto staging=stream(data,bytes,alignment);commands.copyBuffer(staging,destination,bytes);
            buffer.cursor=offset+bytes;stats.dynamicUploadBytes+=bytes;return destination;
        }
    }
    void uploadPixels(const Image& destination,const void* pixels) {
        const uint32_t stride=alignUp(destination.width*4,DK_IMAGE_LINEAR_STRIDE_ALIGNMENT);
        std::vector<uint8_t> padded(size_t(stride)*destination.height);
        for(uint32_t y=0;y<destination.height;++y)
            std::memcpy(padded.data()+size_t(y)*stride,static_cast<const uint8_t*>(pixels)+size_t(y)*destination.width*4,destination.width*4);
        auto address=stream(padded.data(),padded.size(),DK_IMAGE_LINEAR_STRIDE_ALIGNMENT);
        commands.copyBufferToImage({address,stride,stride*destination.height},dk::ImageView{destination.image},
            {0,0,0,destination.width,destination.height,1});
        commands.barrier(DkBarrier_Full,DkInvalidateFlags_Image);
    }
    void retireBlock(dk::UniqueMemBlock& block) {if(block)slots[slot].transient.push_back(std::move(block));}
    void publishCacheDiagnostics() {
        diagnosticTextures.store(textures.size(),std::memory_order_relaxed);
        diagnosticLegacyTextures.store(legacyTextures.size(),std::memory_order_relaxed);
        uint32_t pending=0;for(const auto& s:slots)pending+=s.retiredDescriptors.size();
        diagnosticRetiredDescriptors.store(pending,std::memory_order_relaxed);
    }
    void retire() {
        for(auto it=geometries.begin();it!=geometries.end();) {
            if(it->second.lifetime.expired()) {
                retireBlock(it->second.vertices);retireBlock(it->second.indices);retireBlock(it->second.inverse);it=geometries.erase(it);
            }else ++it;
        }
        for(auto it=legacyTextures.begin();it!=legacyTextures.end();) {
            if(it->first[2]!=textureEpoch)it=legacyTextures.erase(it);
            else ++it;
        }
        for(auto it=textures.begin();it!=textures.end();) {
            const bool legacy=it->first>=(1ull<<62);
            if(legacy?it->second.legacyEpoch!=textureEpoch:it->second.lifetime.expired()) {
                retireBlock(it->second.image.memory);slots[slot].retiredDescriptors.push_back(it->second.descriptor);it=textures.erase(it);
            }else ++it;
        }
        publishCacheDiagnostics();
    }
    // Seeds an existing target's images from canonical GS memory. Wide targets place
    // the 640 columns at widePad; the pad columns stay transparent.
    // Seeds a target's images from canonical GS memory for its first `rows` GS rows.
    // Host pixels sample their GS pixel (nearest); pad columns stay transparent.
    void seedFromMemory(Target& t,const GSContext& context,uint32_t rows) {
        const uint32_t gsw=context.frame.fbw*64u,w=t.color.width,h=std::min(t.color.height,hostRowsOf(t,rows));
        std::vector<uint32_t> raw;memory.ReadSurface(context,gsw,rows,raw);
        std::vector<uint8_t> rgba(size_t(w)*t.color.height*4,0);std::vector<float> depth(size_t(w)*t.color.height,0.f);
        for(uint32_t y=0;y<h;++y) {
            const uint32_t gy=std::min(rows-1,uint32_t(y/t.sy));
            for(uint32_t x=0;x<w;++x) {
                if(x<t.pad)continue;
                const uint32_t gx=uint32_t(float(x-t.pad)/t.sx);if(gx>=gsw)continue;
                const size_t g=size_t(gy)*gsw+gx,d=size_t(y)*w+x;
                uint32_t color=raw[g*2];
                if(context.frame.psm==GS_PSM_CT16 || context.frame.psm==GS_PSM_CT16S)
                    color=((color&31)*255/31)|(((color>>5)&31)*255/31<<8)|(((color>>10)&31)*255/31<<16)|((color&0x8000)?0x80000000u:0);
                rgba[d*4]=color;rgba[d*4+1]=color>>8;rgba[d*4+2]=color>>16;rgba[d*4+3]=std::min(255u,(color>>24)*2);
                depth[d]=float(raw[g*2+1]/65536.0);
            }
        }
        uploadPixels(t.color,rgba.data());uploadPixels(t.depth,depth.data());
    }
    bool wideFor(const GSContext& c) const {
        return wideConfig.load(std::memory_order_relaxed) && c.frame.fbw==10;
    }
    // GS scissors name the 640-pixel area. Wide targets shift them into the
    // centered 4:3 region; fullWidth covers the whole target for 3D and quads.
    void setGsScissor(const GSContext& context,const Target& t,bool fullWidth=false) {
        if(!t.wide && t.sx==1.f && t.sy==1.f && !fullWidth) {
            commands.setScissors(0,{{context.scissor.x0,context.scissor.y0,
                uint32_t(context.scissor.x1-context.scissor.x0+1),uint32_t(context.scissor.y1-context.scissor.y0+1)}});
            return;
        }
        const uint32_t width=t.color.width,height=t.color.height;
        uint32_t x0=t.pad+uint32_t(std::floor(context.scissor.x0*t.sx)),x1=t.pad+uint32_t(std::ceil((context.scissor.x1+1)*t.sx))-1;
        uint32_t y0=uint32_t(std::floor(context.scissor.y0*t.sy)),y1=uint32_t(std::ceil((context.scissor.y1+1)*t.sy))-1;
        if(fullWidth) {x0=0;x1=width-1;}
        x0=std::min(x0,width-1);x1=std::min(x1,width-1);y0=std::min(y0,height-1);y1=std::min(y1,height-1);
        commands.setScissors(0,{{x0,y0,x1-x0+1,y1-y0+1}});
    }
    // Places a completed game image. Wide frames keep the centered 4:3 region
    // unless the world gate is on, in which case the whole display is used.
    void blitGameImage(const dk::ImageView& source,uint32_t sourceWidth,uint32_t sourceHeight,const dk::ImageView& output,bool worldWide) {
        const uint32_t pad=sourceWidth==wideWidth?widePad:sourceWidth==hdWideWidth?hdPad:sourceWidth==wideWidth/2?widePad/2:0u;
        if(pad && worldWide) {
            commands.blitImage(source,{0,0,0,sourceWidth,sourceHeight,1},output,{0,0,0,1280,720,1},DkBlitFlag_FilterLinear);
            return;
        }
        commands.blitImage(source,{pad,0,0,sourceWidth-2*pad,sourceHeight,1},output,{160,0,0,960,720,1},DkBlitFlag_FilterLinear);
    }
    Target& target(const GSContext& context,uint32_t requiredHeight=448) {
        begin();const auto key=std::array<uint32_t,4>{context.frame.fbp,context.frame.fbw,context.frame.psm,context.zbuf.zbp};
        const uint32_t scissorHeight=uint32_t(context.scissor.y1)+1;
        // SDK clear packets can leave SCISSOR at 2047 despite touching only
        // 448 rows. Match GL33Backend::ensureSurface: bounded scissors reserve
        // their full extent; larger scissors use the actual draw extent.
        // Native projection is normalized to that target, so allocating 2048
        // here also incorrectly moves most of the world below the display.
        const uint32_t requiredExtent=std::max(1u,scissorHeight<=448?scissorHeight:std::min(requiredHeight,scissorHeight));
        auto it=targets.find(key);
        if(it==targets.end()) {
            if(!context.frame.fbw || context.frame.fbw>32 || context.scissor.y1>2047)fail("invalid render target extent");
            const bool wide=wideFor(context);
            const auto lay=layoutFor(context,wide,requiredExtent);
            if(wide)probeWideTargets.fetch_add(1,std::memory_order_relaxed);
            Target t;t.context=context;t.wide=wide;t.pad=lay.pad;t.rows=requiredExtent;t.sx=lay.sx;t.sy=lay.sy;
            t.color=makeImage(lay.width,lay.height,DkImageFormat_RGBA8_Unorm,DkImageFlags_UsageRender|DkImageFlags_Usage2DEngine);
            t.depth=makeImage(lay.width,lay.height,DkImageFormat_ZF32,DkImageFlags_UsageRender|DkImageFlags_Usage2DEngine);
            it=targets.emplace(key,std::move(t)).first;
            if(targetReports++<64)std::cout<<"[deko3d] target fbp="<<context.frame.fbp<<" extent="<<lay.width<<'x'<<lay.height
                <<" scissor_height="<<scissorHeight<<" draw_height="<<requiredHeight<<" psm="<<unsigned(context.frame.psm)<<" fbw="<<context.frame.fbw<<" wide="<<wide<<" scale="<<lay.sx<<'\n';
            // A target can be entered after a guest upload or partial clear.
            // Seed once from canonical GS memory; regular frames remain on GPU.
            seedFromMemory(it->second,context,requiredExtent);
        }
        auto& t=it->second;
        if(hostRowsOf(t,requiredExtent)>t.color.height) {
            const auto height=alignUp(hostRowsOf(t,requiredExtent),32);
            if(height>2048)fail("render target exceeds supported GPU extent");
            auto color=makeImage(t.color.width,height,DkImageFormat_RGBA8_Unorm,DkImageFlags_UsageRender|DkImageFlags_Usage2DEngine);
            auto depth=makeImage(t.depth.width,height,DkImageFormat_ZF32,DkImageFlags_UsageRender|DkImageFlags_Usage2DEngine);
            dk::ImageView cv{color.image},dv{depth.image};commands.bindRenderTargets(&cv,&dv);
            commands.setScissors(0,{{0,0,color.width,color.height}});
            commands.clearColor(0,DkColorMask_RGBA,0.f,0.f,0.f,0.f);commands.clearDepthStencil(true,0.f,0,0);
            commands.barrier(DkBarrier_Fragments,0);
            const DkImageRect bounds{0,0,0,t.color.width,t.color.height,1};
            commands.copyImage(dk::ImageView{t.color.image},bounds,dk::ImageView{color.image},bounds);
            commands.copyImage(dk::ImageView{t.depth.image},bounds,dk::ImageView{depth.image},bounds);
            retireBlock(t.color.memory);retireBlock(t.depth.memory);
            t.color=std::move(color);t.depth=std::move(depth);active=nullptr;t.rows=std::max(t.rows,requiredExtent);
        }
        if(active!=&t) {
            commands.barrier(DkBarrier_Fragments,DkInvalidateFlags_Image);
            dk::ImageView cv{t.color.image},dv{t.depth.image};commands.bindRenderTargets(&cv,&dv);active=&t;
        }
        if(t.reseed) {seedFromMemory(t,context,std::max(1u,std::min<uint32_t>(2048u,uint32_t(std::lround(t.color.height/t.sy)))));t.reseed=false;}
        t.context=context;
        commands.setViewports(0,{{0.f,0.f,float(t.color.width),float(t.color.height),0.f,1.f}});
        setGsScissor(context,t);
        if(t.color.width>=640 || t.context.frame.fbw==10)lastMain=&t;
        return t;
    }
    Geometry& geometry(const NativeGeometry& source) {
        auto& result=geometries[source.id];if(result.revision==source.revision)return result;
        retireBlock(result.vertices);retireBlock(result.indices);retireBlock(result.inverse);
        if(source.vertices.empty())fail("empty native geometry");
        result.vertices=upload(source.vertices.data(),source.vertices.size()*sizeof(NativeVertex));
        if(!source.indices.empty())result.indices=upload(source.indices.data(),source.indices.size()*2);
        if(source.boneCount)result.inverse=upload(source.inverseBind.data(),sizeof(source.inverseBind));
        result.revision=source.revision;result.lifetime=source.lifetime;++stats.geometryUploads;return result;
    }
    GeometryBinding geometryBinding(const NativeGeometry& source) {
        if(!source.dynamic) {
            auto& cached=geometry(source);
            return {cached.vertices.getGpuAddr(),cached.indices?cached.indices.getGpuAddr():0,
                source.boneCount?cached.inverse.getGpuAddr():zeroMatrices.getGpuAddr()};
        }
        if(source.vertices.empty())fail("empty dynamic native geometry");
        GeometryBinding binding;binding.vertices=dynamicUpload(source.vertices.data(),source.vertices.size()*sizeof(NativeVertex));
        if(!source.indices.empty())binding.indices=dynamicUpload(source.indices.data(),source.indices.size()*sizeof(uint16_t));
        binding.inverse=source.boneCount?dynamicUpload(source.inverseBind.data(),sizeof(source.inverseBind),DK_UNIFORM_BUF_ALIGNMENT):zeroMatrices.getGpuAddr();
        commands.barrier(DkBarrier_Full,DkInvalidateFlags_L2Cache);
        ++stats.geometryUploads;++stats.dynamicGeometryUploads;publishDynamicDiagnostics();return binding;
    }
    uint32_t descriptor() {
        if(!freeDescriptors.empty()){auto id=freeDescriptors.back();freeDescriptors.pop_back();return id;}
        if(nextDescriptor==descriptorCount)fail("texture descriptor limit exceeded: live="+std::to_string(textures.size())+
            " legacy="+std::to_string(legacyTextures.size())+" pending="+std::to_string(diagnosticRetiredDescriptors.load(std::memory_order_relaxed)));
        const uint32_t id=nextDescriptor++;
        if(nextDescriptor>diagnosticDescriptorHighWater.load(std::memory_order_relaxed))
            diagnosticDescriptorHighWater.store(nextDescriptor,std::memory_order_relaxed);
        return id;
    }
    void descriptors(uint32_t id,const dk::Image& image,bool linear,uint32_t u,uint32_t v) {
        dk::ImageDescriptor im;im.initialize(image);
        dk::Sampler sampler;sampler.setFilter(linear?DkFilter_Linear:DkFilter_Nearest,linear?DkFilter_Linear:DkFilter_Nearest);
        sampler.setWrapMode(wrap(u),wrap(v),DkWrapMode_ClampToEdge);
        dk::SamplerDescriptor sm;sm.initialize(sampler);
        commands.pushData(imageDescriptors.getGpuAddr()+id*sizeof(DkImageDescriptor),&im,sizeof(im));
        commands.pushData(samplerDescriptors.getGpuAddr()+id*sizeof(DkSamplerDescriptor),&sm,sizeof(sm));
        commands.barrier(DkBarrier_Full,DkInvalidateFlags_Descriptors|DkInvalidateFlags_Image);
        commands.bindTextures(DkStage_Fragment,0,dkMakeTextureHandle(id,id));
    }
    bool bindTexture(const std::shared_ptr<const NativeTexture>& source,Target& destination) {
        if(!source){commands.bindTextures(DkStage_Fragment,0,dkMakeTextureHandle(2,2));return false;}
        if(source->renderTarget) {
            const auto entry=renderSources.find(source->renderTarget->sourceId);
            if(entry==renderSources.end())fail("expired native render texture source");
            const auto& gs=entry->second.gs;Target* found=nullptr;
            for(auto& [key,t]:targets)if(t.context.frame.fbp*32==gs.tbp0 && t.context.frame.fbw==gs.tbw && t.context.frame.psm==gs.psm){found=&t;break;}
            if(!found)fail("render texture source was never rendered by GPU");
            if(source->width!=1u<<gs.tw || source->height!=1u<<gs.th ||
               source->width>found->color.width || source->height>found->color.height)
                fail("native render texture extent differs from registered source");
            // Match GL's normalized-camera -> GS storage -> TEXA conversion.
            // A raw RGBA copy leaks fractional CT16 alpha into shadow receivers
            // where the GS alpha bit and TA0 require complete transparency.
            Image transientImage;auto& image=renderTextureImage(source->width,source->height,transientImage);
            commands.barrier(DkBarrier_Fragments,0);
            // Immutable descriptors also prevent a later snapshot from changing
            // the texture seen by an earlier in-flight receiver draw.
            const auto inputDescriptor=descriptor(),outputDescriptor=descriptor();
            descriptors(inputDescriptor,found->color.image,false,3,3);
            dk::ImageView output{image.image};commands.bindRenderTargets(&output);active=nullptr;
            commands.setViewports(0,{{0.f,0.f,float(source->width),float(source->height),0.f,1.f}});
            commands.setScissors(0,{{0,0,source->width,source->height}});
            commands.bindShaders(DkStageFlag_GraphicsMask,{&videoVertex.shader,&renderTextureFragment.shader});
            state(false,false,false,2,1);
            const std::array<uint32_t,4> conversion{gs.psm,entry->second.alpha.ta0,entry->second.alpha.ta1,entry->second.alpha.aem?1u:0u};
            const auto uniform=stream(conversion.data(),sizeof(conversion),DK_UNIFORM_BUF_ALIGNMENT);
            commands.bindUniformBuffer(DkStage_Fragment,0,uniform,sizeof(conversion));
            commands.draw(DkPrimitive_Triangles,3,1,0,0);
            commands.barrier(DkBarrier_Fragments,DkInvalidateFlags_Image);
            descriptors(outputDescriptor,image.image,source->linear,source->wrapU,source->wrapV);
            if(transientImage.memory)slots[slot].transient.push_back(std::move(transientImage.memory));
            ++stats.renderTextureCopies;
            slots[slot].retiredDescriptors.push_back(inputDescriptor);
            slots[slot].retiredDescriptors.push_back(outputDescriptor);publishCacheDiagnostics();
            return true;
        }
        auto it=textures.find(source->id);
        if(it==textures.end()) {
            if(!source->width || !source->height || source->rgba.size()!=size_t(source->width)*source->height*4)fail("invalid native texture pixels");
            Texture tex;tex.image=makeImage(source->width,source->height,DkImageFormat_RGBA8_Unorm,DkImageFlags_Usage2DEngine);
            tex.descriptor=descriptor();tex.lifetime=source->lifetime;tex.linear=source->linear;tex.wrapU=source->wrapU;tex.wrapV=source->wrapV;
            if(source->id>=(1ull<<62))tex.legacyEpoch=textureEpoch;
            uploadPixels(tex.image,source->rgba.data());
            it=textures.emplace(source->id,std::move(tex)).first;++stats.textureUploads;
            publishCacheDiagnostics();
            descriptors(it->second.descriptor,it->second.image.image,source->linear,source->wrapU,source->wrapV);
        } else commands.bindTextures(DkStage_Fragment,0,dkMakeTextureHandle(it->second.descriptor,it->second.descriptor));
        (void)destination;return true;
    }
    void bindBlend(const dk::BlendState& state) {
        // deko3d 0.5.0's encoder uses dstColorBlendFactor for FuncAlphaDst,
        // ignoring dstAlphaBlendFactor. This changes fractional font alpha to
        // opaque when RGB uses InvSrcAlpha and alpha must use Zero.
        // https://github.com/devkitPro/deko3d/blob/v0.5.0/source/maxwell/gpu_3d_state.cpp#L130
        // Public capture/replay lets us correct that one field without private
        // headers or replacing the installed driver. Use a separate encoder:
        // beginCaptureCmds clears its command buffer before capturing.
        static_assert(sizeof(DkBlendState)==4);
        uint32_t key=0;std::memcpy(&key,&state,sizeof(key));
        auto found=blendCommands.find(key);
        if(found==blendCommands.end()) {
            // The driver reserves with an end-exclusive safety word, so give
            // the capture extra capacity while still checking its exact length.
            std::array<uint32_t,16> captured{};
            stateEncoder.beginCaptureCmds(captured.data(),captured.size());
            stateEncoder.bindBlendStates(0,{state});
            const uint32_t count=stateEncoder.endCaptureCmds();
            std::array<uint32_t,7> encoded{};
            std::copy_n(captured.begin(),encoded.size(),encoded.begin());
            auto factor=[](uint32_t value){return (value&31u)|((value&32u)?0xc000u:0u);};
            // Validate the complete six-field payload before correcting it.
            // A new command layout fails explicitly rather than patching an
            // unknown register. A future fixed encoder is accepted as well.
            if(count!=7 || encoded[1]!=uint32_t(state.colorBlendOp) ||
               encoded[2]!=factor(state.srcColorBlendFactor) || encoded[3]!=factor(state.dstColorBlendFactor) ||
               encoded[4]!=uint32_t(state.alphaBlendOp) || encoded[5]!=factor(state.srcAlphaBlendFactor) ||
               (encoded[6]!=factor(state.dstColorBlendFactor) && encoded[6]!=factor(state.dstAlphaBlendFactor)))
                fail("incompatible deko3d blend command encoding; separate-alpha workaround cannot be applied");
            encoded[6]=factor(state.dstAlphaBlendFactor);
            found=blendCommands.emplace(key,encoded).first;
        }
        commands.replayCmds(found->second);
    }
    void state(bool depthTest,bool depthWrite,bool alpha,uint32_t src,uint32_t dst,uint32_t cull=1) {
        dk::RasterizerState raster;raster.setCullMode(cull==2?DkFace_Back:cull==3?DkFace_Front:DkFace_None).setFrontFace(DkFrontFace_CW);
        commands.bindRasterizerState(raster);
        dk::DepthStencilState depth;depth.setDepthTestEnable(depthTest||depthWrite).setDepthWriteEnable(depthWrite).setDepthCompareOp(depthTest?DkCompareOp_Gequal:DkCompareOp_Always);
        commands.bindDepthStencilState(depth);
        dk::ColorState color;color.setBlendEnable(0,alpha);commands.bindColorState(color);
        dk::ColorWriteState write;commands.bindColorWriteState(write);
        dk::BlendState blending;blending.setFactors(blend(src),blend(dst),DkBlendFactor_One,DkBlendFactor_Zero);bindBlend(blending);
    }
    void fragment(bool textured,float cutoff,bool fog,const std::array<float,4>& color,float decal=0,
        const GSDrawState* gs=nullptr,bool alphaFailure=false) {
        Fragment f{{textured?1.f:0.f,cutoff,fog?1.f:0.f,decal},color,
            {gs?1.f:0.f,gs && (gs->context.test&1)?float((gs->context.test>>1)&7):1.f,
                gs?float((gs->context.test>>4)&255):0.f,alphaFailure?1.f:0.f}};
        auto address=stream(&f,sizeof(f),DK_UNIFORM_BUF_ALIGNMENT);commands.bindUniformBuffer(DkStage_Fragment,0,address,sizeof(f));
    }
    void model(const GSContext& context,const NativeModelDraw& draw,bool worldWide=false) {
        if(!draw.geometry)fail("native model has no geometry");
        auto& t=target(context);const auto geo=geometryBinding(*draw.geometry);
        // Only world-camera draws widen, and only while the gameplay gate is on.
        // HUD/menu models (xModelRender2D) and every gate-off frame keep the 4:3
        // placement: a 640-column viewport at the pad, no projection change, so
        // the cropped presentation is pixel-identical to the 4:3 build.
        const bool fullWidth=worldWide && t.wide && !draw.hud;
        const auto applyViewport=[&] {
            if(!t.wide && t.sx==1.f)return;
            if(fullWidth){commands.setViewports(0,{{0.f,0.f,float(t.color.width),float(t.color.height),0.f,1.f}});setGsScissor(context,t,true);}
            else {commands.setViewports(0,{{float(t.pad),0.f,float(t.color.width-2*t.pad),float(t.color.height),0.f,1.f}});setGsScissor(context,t);}
        };
        applyViewport();
        if(fullWidth)probeFullWidth.fetch_add(1,std::memory_order_relaxed);
        // Wide world draws scale clip x by 640/854 about the optical center
        // (x' = s*x + (1-s)/2*w, w = row 2), which widens the view to the full
        // target without changing object sizes or moving the center.
        NativeMatrix camera=draw.camera;
        // Scale clip x about the optical center so the projection ends up wide for
        // world draws while the gate is on and 4:3 for everything else, whatever state
        // the camera's clip transform is actually in (draw.cameraWidened).
        const float current=draw.cameraWidened?wideProjectionScale:1.f,wanted=fullWidth?wideProjectionScale:1.f;
        const float s=t.wide?wanted/current:1.f;
        if(s!=1.f)for(size_t c=0;c<16;c+=4)camera[c]=s*camera[c]+(1.f-s)*0.5f*camera[c+2];
        auto bindModelState=[&] {
        commands.bindShaders(DkStageFlag_GraphicsMask,{&modelShader.shader,&fragmentShader.shader});
        state(draw.depthTest,draw.depthWrite,draw.vertexAlpha,draw.sourceBlend,draw.destinationBlend,draw.cullMode);
        const std::array<DkVtxAttribState,6> attributes={
            DkVtxAttribState{0,0,offsetof(NativeVertex,position),DkVtxAttribSize_3x32,DkVtxAttribType_Float,0},
            {0,0,offsetof(NativeVertex,normal),DkVtxAttribSize_3x32,DkVtxAttribType_Float,0},
            {0,0,offsetof(NativeVertex,uv),DkVtxAttribSize_2x32,DkVtxAttribType_Float,0},
            {0,0,offsetof(NativeVertex,color),DkVtxAttribSize_4x8,DkVtxAttribType_Unorm,0},
            {0,0,offsetof(NativeVertex,weights),DkVtxAttribSize_4x32,DkVtxAttribType_Float,0},
            {0,0,offsetof(NativeVertex,bones),DkVtxAttribSize_4x8,DkVtxAttribType_Uint,0}};
        commands.bindVtxAttribState(attributes);commands.bindVtxBufferState({{sizeof(NativeVertex),0}});
        commands.bindVtxBuffer(0,geo.vertices,draw.geometry->vertices.size()*sizeof(NativeVertex));
        if(draw.indexed)commands.bindIdxBuffer(DkIdxFormat_Uint16,geo.indices);
        // native.vert clamps every bone index to boneCount-1, so only the first boneCount matrices are read.
        const uint32_t poseBytes=draw.geometry->boneCount?std::min<uint32_t>(draw.geometry->boneCount,64u)*sizeof(NativeMatrix):sizeof(draw.pose);
        auto pose=draw.geometry->boneCount?stream(draw.pose.data(),poseBytes,DK_UNIFORM_BUF_ALIGNMENT):geo.inverse;
        commands.bindUniformBuffer(DkStage_Vertex,1,pose,poseBytes);
        commands.bindUniformBuffer(DkStage_Vertex,2,geo.inverse,sizeof(draw.geometry->inverseBind));
        };
        bindModelState();
        for(size_t meshIndex=0;meshIndex<draw.geometry->meshes.size();++meshIndex) {
            const auto& mesh=draw.geometry->meshes[meshIndex];
            if(meshIndex>=draw.materials.size())fail("native mesh material outside snapshot");
            const auto& m=draw.materials[meshIndex];
            const bool textured=bindTexture(m.texture,t);
            if(m.texture && m.texture->renderTarget){target(context);bindModelState();applyViewport();}
            Transform u{draw.world,camera,{draw.zScale,draw.zShift,m.ambient,m.diffuse},
                {draw.geometry->boneCount,draw.geometry->flags,std::min(draw.lights.count,8u),draw.projectionType},
                m.color,draw.lights.ambient,draw.lights.color,draw.lights.direction,{draw.fogStart,draw.fogEnd,draw.fogEnabled?1.f:0.f,0.f}};
            auto address=stream(&u,sizeof(u),DK_UNIFORM_BUF_ALIGNMENT);commands.bindUniformBuffer(DkStage_Vertex,0,address,sizeof(u));
            fragment(textured,draw.alphaReference,draw.fogEnabled,draw.fogColor);
            if(draw.indexed)commands.drawIndexed((draw.geometry->flags&1)?DkPrimitive_TriangleStrip:DkPrimitive_Triangles,mesh.indexCount,1,mesh.firstIndex,0,0);
            else commands.draw((draw.geometry->flags&1)?DkPrimitive_TriangleStrip:DkPrimitive_Triangles,mesh.indexCount,1,mesh.firstIndex,0);
            ++stats.modelDraws;
        }
        t.colorDirty=true;t.depthDirty|=draw.depthWrite;
        // Later 2D draws expect the full-target viewport the target bind set.
        if((t.wide || t.sx!=1.f) && !fullWidth)commands.setViewports(0,{{0.f,0.f,float(t.color.width),float(t.color.height),0.f,1.f}});
    }
    void screen(const GSContext& context,const NativeScreenDraw& draw,float alphaReference=0,bool fog=false,const std::array<float,4>& fogColor={},float decal=0,const GSDrawState* gs=nullptr,bool alphaFailure=false) {
        if(draw.vertices.empty())return;
        uint32_t requiredHeight=448;
        if(gs) {
            float maxY=0.f;for(const auto& vertex:draw.vertices)maxY=std::max(maxY,vertex.position[1]);
            requiredHeight=uint32_t(std::max(1,int(std::ceil(maxY))+(gs->prim.type==GS_PRIM_SPRITE?0:1)));
        }
        auto& t=target(context,requiredHeight);
        const bool textured=bindTexture(draw.texture,t);
        if(draw.texture && draw.texture->renderTarget)target(context,requiredHeight);
        // Wide targets: GS 2D moves into the centered 4:3 area. Quads spanning the
        // whole 640-pixel width (fades, backdrops, letterboxes) stretch to the target.
        std::vector<NativeScreenVertex> widened;
        const auto* vertexData=&draw.vertices;
        if(t.wide || t.sx!=1.f || t.sy!=1.f) {
            float minX=std::numeric_limits<float>::max(),maxX=std::numeric_limits<float>::lowest();
            for(const auto& vertex:draw.vertices) {minX=std::min(minX,vertex.position[0]);maxX=std::max(maxX,vertex.position[0]);}
            const bool fullWidth=t.wide && minX<=0.5f && maxX>=float(gsWidth)-0.5f;
            widened=draw.vertices;
            for(auto& vertex:widened) {
                if(fullWidth && vertex.position[0]<=minX+0.5f)vertex.position[0]=0.f;
                else if(fullWidth && vertex.position[0]>=maxX-0.5f)vertex.position[0]=float(t.color.width);
                else vertex.position[0]=vertex.position[0]*t.sx+float(t.pad);
                vertex.position[1]*=t.sy;
            }
            vertexData=&widened;
            if(fullWidth)setGsScissor(context,t,true);
        }
        commands.bindShaders(DkStageFlag_GraphicsMask,{&screenShader.shader,&fragmentShader.shader});
        state(draw.depthTest,draw.depthWrite,draw.vertexAlpha,draw.sourceBlend,draw.destinationBlend);
        bool writesDepth=draw.depthWrite;
        uint32_t colorMask=DkColorMask_RGBA;
        if(gs) {
            dk::ColorState color;color.setBlendEnable(0,draw.vertexAlpha);
            // The fragment shader compares the final GS alpha byte. Testing
            // expanded normalized alpha against AREF/128 loses byte equality
            // and alpha values above 128, especially after texture modulation.
            color.setAlphaCompareOp(DkCompareOp_Always);commands.bindColorState(color);
            dk::ColorWriteState writes;uint32_t mask=0;
            for(unsigned channel=0;channel<4;++channel) {
                const auto byte=(context.frame.fbmsk>>(channel*8))&255;
                if(byte!=0 && byte!=255)fail("partial GS channel-bit mask requires an integer fragment path");
                if(byte==0)mask|=1u<<channel;
            }
            if(alphaFailure) {
                // Each Submit represents one GS primitive. The disjoint alpha
                // predicates preserve its stream position while selecting the
                // failed fragment's planes independently, without readback.
                const unsigned action=(context.test>>12)&3;
                if(action==2)mask=0; // ZB_ONLY
                else writesDepth=false; // FB_ONLY / RGB_ONLY
                if(action==3 && context.frame.psm==GS_PSM_CT32)mask&=DkColorMask_RGB;
            }
            colorMask=mask;
            writes.setMask(0,mask);commands.bindColorWriteState(writes);
            dk::DepthStencilState depth;depth.setDepthTestEnable(((context.test>>16)&1)||writesDepth)
                .setDepthWriteEnable(writesDepth).setDepthCompareOp(((context.test>>17)&3)==3?DkCompareOp_Greater:draw.depthTest?DkCompareOp_Gequal:DkCompareOp_Always);
            commands.bindDepthStencilState(depth);
            if(draw.vertexAlpha) {
                const unsigned a=context.alpha&3,b=(context.alpha>>2)&3,c=(context.alpha>>4)&3,d=(context.alpha>>6)&3;
                const unsigned fixed=(context.alpha>>32)&255;
                std::string reason;const auto blending=gsBlendState(a,b,c,d,fixed,reason);
                if(!blending)fail("unsupported GS blend equation A="+std::to_string(a)+" B="+std::to_string(b)+
                    " C="+std::to_string(c)+" D="+std::to_string(d)+" FIX="+std::to_string(fixed)+
                    " ABE="+std::to_string(gs->prim.abe)+" primitive="+std::to_string(gs->prim.type)+": "+reason);
                commands.setBlendConst(1,1,1,float(fixed)/128.f);
                bindBlend(*blending);
            }
        }
        const std::array<DkVtxAttribState,3> attributes={
            DkVtxAttribState{0,0,offsetof(NativeScreenVertex,position),DkVtxAttribSize_4x32,DkVtxAttribType_Float,0},
            {0,0,offsetof(NativeScreenVertex,uv),DkVtxAttribSize_4x32,DkVtxAttribType_Float,0},
            {0,0,offsetof(NativeScreenVertex,color),DkVtxAttribSize_4x32,DkVtxAttribType_Float,0}};
        commands.bindVtxAttribState(attributes);commands.bindVtxBufferState({{sizeof(NativeScreenVertex),0}});
        const auto address=stream(vertexData->data(),vertexData->size()*sizeof(NativeScreenVertex));
        commands.bindVtxBuffer(0,address,vertexData->size()*sizeof(NativeScreenVertex));
        std::array<float,4> viewport{float(t.color.width),float(t.color.height),0,0};
        auto vp=stream(&viewport,sizeof(viewport),DK_UNIFORM_BUF_ALIGNMENT);commands.bindUniformBuffer(DkStage_Vertex,0,vp,sizeof(viewport));
        fragment(textured,alphaReference,fog,fogColor,decal,gs,alphaFailure);
        if(draw.indices.empty())commands.draw(primitive(draw.primitive),vertexData->size(),1,0,0);
        else {auto index=stream(draw.indices.data(),draw.indices.size()*2);commands.bindIdxBuffer(DkIdxFormat_Uint16,index);commands.drawIndexed(primitive(draw.primitive),draw.indices.size(),1,0,0,0);}
        t.colorDirty|=colorMask!=0;t.depthDirty|=writesDepth;++stats.screenDraws;
    }

    std::vector<uint8_t> download(const Image& image) {
        CpuWorkProfile::Scope cpuWork(CpuWorkProfile::Phase::Graphics);
        DiagnosticScope diagnostic(*this,Stage::Readback);
        begin();const uint32_t pitch=alignUp(image.width*4,DK_IMAGE_LINEAR_STRIDE_ALIGNMENT);
        auto staging=allocate(pitch*image.height,DkMemBlockFlags_CpuUncached|DkMemBlockFlags_GpuCached);
        commands.barrier(DkBarrier_Fragments,0);
        commands.copyImageToBuffer(dk::ImageView{image.image},{0,0,0,image.width,image.height,1},
            {staging.getGpuAddr(),pitch,pitch*image.height});
        idle();++stats.readbacks;
        diagnosticReadbacks.store(stats.readbacks,std::memory_order_relaxed);
        std::vector<uint8_t> pixels(size_t(image.width)*image.height*4);
        for(uint32_t y=0;y<image.height;++y)
            std::memcpy(pixels.data()+size_t(y)*image.width*4,
                static_cast<const uint8_t*>(staging.getCpuAddr())+size_t(y)*pitch,image.width*4);
        return pixels;
    }
    void writeback(Target& t,const std::vector<uint8_t>* savedColor=nullptr,const std::vector<uint8_t>* savedDepth=nullptr) {
        if(!t.colorDirty && !t.depthDirty)return;
        // Wide targets map their centered 640 columns back to canonical GS memory.
        const uint32_t gsw=t.context.frame.fbw*64u,stride=t.color.width,hostRows=t.color.height;
        const uint32_t rows=std::max(1u,std::min<uint32_t>(2048u,uint32_t(std::lround(hostRows/t.sy))));
        // Host pixel for a GS pixel: the center sample of its scaled cell (nearest).
        const auto hostIndex=[&](uint32_t x,uint32_t y){
            const uint32_t px=std::min(stride-1,t.pad+uint32_t((x+0.5f)*t.sx)),py=std::min(hostRows-1,uint32_t((y+0.5f)*t.sy));
            return size_t(py)*stride+px;};
        std::vector<uint32_t> before;memory.ReadSurface(t.context,gsw,rows,before);
        auto after=before;
        uint32_t planes=0;
        if(t.colorDirty) {
            const auto readback=savedColor?std::vector<uint8_t>{}:download(t.color);
            const auto& rgba=savedColor?*savedColor:readback;planes|=1;
            for(uint32_t y=0;y<rows;++y)for(uint32_t x=0;x<gsw;++x) {
                const size_t i=size_t(y)*gsw+x,p=hostIndex(x,y);
                uint32_t color=uint32_t(rgba[p*4])|(uint32_t(rgba[p*4+1])<<8)|(uint32_t(rgba[p*4+2])<<16)|((uint32_t(rgba[p*4+3])+1)/2<<24);
                if(t.context.frame.psm==GS_PSM_CT16 || t.context.frame.psm==GS_PSM_CT16S)
                    color=((color>>3)&31)|(((color>>11)&31)<<5)|(((color>>19)&31)<<10)|(rgba[p*4+3]>=128?0x8000:0);
                after[i*2]=color;
            }
        }
        if(t.depthDirty) {
            const auto readback=savedDepth?std::vector<uint8_t>{}:download(t.depth);
            const auto& bytes=savedDepth?*savedDepth:readback;planes|=2;
            for(uint32_t y=0;y<rows;++y)for(uint32_t x=0;x<gsw;++x) {
                const size_t i=size_t(y)*gsw+x,p=hostIndex(x,y);
                float depth;std::memcpy(&depth,bytes.data()+p*4,4);
                after[i*2+1]=uint32_t(std::clamp(double(depth)*65536.0,0.0,4294967295.0));
            }
        }
        memory.WriteSurface(t.context,gsw,rows,after,before,planes,planes);
        t.colorDirty=t.depthDirty=false;
    }
    void writebackAll() {for(auto& [key,t]:targets)writeback(t);}
    void prepareSnapshot() {
        if(!pending.empty())throw ps2x::state::NotReady("Snapshot requires consumed ordered native commands");
        if(preparedRevision==stateRevision)return;
        savedTargets.clear();savedTextures.clear();begin();idle();
        for(auto& [key,t]:targets) {
            SavedTarget saved;saved.context=t.context;saved.width=t.color.width;saved.height=t.color.height;
            saved.color=download(t.color);saved.depth=download(t.depth);
            // Canonical VRAM must be current before the outer machine archive
            // copies PS2Memory. Reuse these explicit snapshot readbacks.
            writeback(t,&saved.color,&saved.depth);savedTargets.push_back(std::move(saved));
        }
        // Whole decoded uploads can retain older texels than the CPU decoder's
        // last physical page; decoding again would change subsequent draws.
        for(const auto& [key,id]:legacyTextures) {
            const auto found=textures.find(id);if(found==textures.end())fail("snapshot GS texture has no GPU upload");
            const auto& texture=found->second;
            SavedTexture saved;saved.key=key;saved.width=texture.image.width;saved.height=texture.image.height;
            saved.wrapU=texture.wrapU;saved.wrapV=texture.wrapV;saved.linear=texture.linear;
            saved.rgba=download(texture.image);savedTextures.push_back(std::move(saved));
        }
        idle();preparedRevision=stateRevision;
    }
    Target* completedSource() {
        if(completedTarget) {
            const auto found=targets.find(*completedTarget);if(found!=targets.end())return &found->second;
        }
        return nullptr;
    }
    static bool overlap(uint32_t base,uint32_t size,uint32_t other,uint32_t count) {
        constexpr uint32_t vramBytes=4*1024*1024;
        base%=vramBytes;other%=vramBytes;
        if(base+size>vramBytes || other+count>vramBytes)return true;
        return base<other+count && other<base+size;
    }
    bool aliases(const Target& t,uint32_t address,uint32_t bytes) const {
        // Canonical GS extent: a wide host image does not occupy more GS memory.
        const uint32_t gsRowsCovered=std::max(1u,std::min<uint32_t>(2048u,uint32_t(std::lround(t.color.height/t.sy))));
        const uint32_t pixels=t.context.frame.fbw*64u*alignUp(gsRowsCovered,64);
        const uint32_t colorBytes=pixels*((t.context.frame.psm==GS_PSM_CT16 || t.context.frame.psm==GS_PSM_CT16S)?2:4);
        const uint32_t depthBytes=pixels*((t.context.zbuf.psm==GS_PSM_Z16 || t.context.zbuf.psm==GS_PSM_Z16S)?2:4);
        return overlap(t.context.frame.fbp*8192,colorBytes,address,bytes) || overlap(t.context.zbuf.zbp*8192,depthBytes,address,bytes);
    }
    void cpuMutation(uint32_t address,uint32_t bytes) {
        for(auto it=targets.begin();it!=targets.end();) {
            if(!aliases(it->second,address,bytes)){++it;continue;}
            // Keep GPU-visible content in canonical memory before the CPU write lands.
            // The target keeps its images; its pixels reseed in place on next use.
            writeback(it->second);idle();
            it->second.reseed=true;++it;
        }
    }
    void drawMetricsOverlay(const Image& display) {
        const bool metrics=metricsEnabled.load(std::memory_order_relaxed);
        const bool replayEnabled=replayOverlayEnabled.load(std::memory_order_acquire);
        if(!metrics && !replayEnabled)return;
        std::string replay;
        if(replayEnabled){std::lock_guard lock(replayOverlayMutex);replay=replayOverlay;}
        if(!metrics && replay.empty())return;
        auto number=[](double value) {
            if(!std::isfinite(value) || value<0)return std::string("N/A");
            char text[24];std::snprintf(text,sizeof(text),"%.1f",std::min(value,9999.9));return std::string(text);
        };
        auto timing=[&](double value) {return number(value)+(std::isfinite(value) && value>=0?" MS":"");};
        auto& vertices=metricsVertices;vertices.clear();vertices.reserve(4096);
        auto rectangle=[&](float x,float y,float width,float height,std::array<float,4> color) {
            for(const auto& offset:std::array<std::array<float,2>,6>{{{0,0},{1,0},{0,1},{0,1},{1,0},{1,1}}}) {
                NativeScreenVertex vertex{};vertex.position={x+offset[0]*width,y+offset[1]*height,0,1};
                vertex.uv={0,0,1,0};vertex.color=color;vertices.push_back(vertex);
            }
        };
        auto textLine=[&](std::string_view text,float top) {
            rectangle(168,top-12,float(text.size()*18+24),45,{0,0,0,192});
            for(size_t character=0;character<text.size();++character) {
                char letter=text[character];if(letter>='a' && letter<='z')letter-=32;
                const auto glyph=metricGlyph(letter);
                for(unsigned row=0;row<glyph.size();++row)for(unsigned column=0;column<5;) {
                    if(!(glyph[row]&(16u>>column))){++column;continue;}
                    const unsigned first=column;while(column<5 && (glyph[row]&(16u>>column)))++column;
                    rectangle(180.f+float(character*18+first*3),top+float(row*3),float((column-first)*3),3,{255,255,255,255});
                }
            }
        };
        if(metrics)textLine("FPS "+number(metricsGameFps.load(std::memory_order_relaxed))+"  CPU "+
            timing(metricsCpuMs.load(std::memory_order_relaxed))+"  GPU "+timing(metricsGpuMs.load(std::memory_order_relaxed)),24);
        for(size_t offset=0,line=0;offset<replay.size() && line<12;++line) {
            const size_t end=replay.find('\n',offset);
            const size_t count=std::min(size_t(50),(end==std::string::npos?replay.size():end)-offset);
            textLine(std::string_view(replay).substr(offset,count),float((metrics?78:24)+line*48));
            offset+=count;if(offset<replay.size() && replay[offset]=='\n')++offset;
        }
        commands.barrier(DkBarrier_Full,DkInvalidateFlags_L2Cache);
        const dk::ImageView output{display.image};commands.bindRenderTargets(&output);
        commands.setViewports(0,{{0,0,1280,720,0,1}});commands.setScissors(0,{{0,0,1280,720}});
        commands.bindShaders(DkStageFlag_GraphicsMask,{&screenShader.shader,&fragmentShader.shader});
        state(false,false,true,5,6);
        const std::array<DkVtxAttribState,3> attributes={
            DkVtxAttribState{0,0,offsetof(NativeScreenVertex,position),DkVtxAttribSize_4x32,DkVtxAttribType_Float,0},
            {0,0,offsetof(NativeScreenVertex,uv),DkVtxAttribSize_4x32,DkVtxAttribType_Float,0},
            {0,0,offsetof(NativeScreenVertex,color),DkVtxAttribSize_4x32,DkVtxAttribType_Float,0}};
        commands.bindVtxAttribState(attributes);commands.bindVtxBufferState({{sizeof(NativeScreenVertex),0}});
        const auto address=stream(vertices.data(),vertices.size()*sizeof(NativeScreenVertex));
        commands.bindVtxBuffer(0,address,vertices.size()*sizeof(NativeScreenVertex));
        const std::array<float,4> viewport{1280,720,0,0};
        commands.bindUniformBuffer(DkStage_Vertex,0,stream(&viewport,sizeof(viewport),DK_UNIFORM_BUF_ALIGNMENT),sizeof(viewport));
        fragment(false,0,false,{});commands.draw(DkPrimitive_Triangles,vertices.size(),1,0,0);
    }
    bool presentGameImage(const Image& image,uint32_t sourceWidth,uint32_t sourceHeight,
        std::vector<uint8_t>* diagnosticPixels=nullptr,bool worldWide=false) {
        begin();
        if(!presentationActive()){submit();return false;}
        int display;
        {DiagnosticScope diagnostic(*this,Stage::SwapchainAcquire);display=queue.acquireImage(swapchain);}
        if(display<0 || unsigned(display)>=framesInFlight)fail("failed to acquire presentation image");
        dk::ImageView output{displays[display].image};commands.bindRenderTargets(&output);
        slots[slot].timingComposition=true;commands.reportCounter(DkCounter_TimestampPipelineTop,slots[slot].timing.getGpuAddr()+16);
        commands.setScissors(0,{{0,0,1280,720}});
        commands.clearColor(0,DkColorMask_RGBA,0.f,0.f,0.f,1.f);
        commands.barrier(DkBarrier_Fragments,0);
        // The PS2's 640x448 raster uses non-square pixels. Its intended display
        // aspect is 4:3, so fit it into 960x720 with 160-pixel black pillarboxes.
        blitGameImage(dk::ImageView{image.image},sourceWidth,sourceHeight,output,worldWide);
        drawMetricsOverlay(displays[display]);
        // Self-tests alone inspect the acquired image before ownership passes
        // to the display compositor. Normal presentation never reads it back.
        if(diagnosticPixels)*diagnosticPixels=download(displays[display]);
        submit();
        {DiagnosticScope diagnostic(*this,Stage::SwapchainPresent);queue.presentImage(swapchain,display);}
        return true;
    }
};

Deko3DBackend::Deko3DBackend(std::string path):impl(std::make_unique<Impl>(std::move(path))) {impl->render.start(renderThreadEnabled());}
Deko3DBackend::~Deko3DBackend() {impl->render.stop();}
void Deko3DBackend::Initialize(uint8_t* vram,uint32_t size) {impl->render.call([impl=impl.get(),vram,size]{std::lock_guard lock(impl->mutex);impl->memory.Initialize(vram,size);});}
void Deko3DBackend::Reset() {
    CpuWorkProfile::Scope cpuWork(CpuWorkProfile::Phase::Graphics);
    impl->render.call([impl=impl.get()]{
    std::lock_guard lock(impl->mutex);Impl::DiagnosticScope diagnostic(*impl,Stage::Reset);
    impl->idle();impl->targets.clear();impl->active=impl->lastMain=nullptr;
    impl->geometries.clear();impl->textures.clear();impl->pending.clear();impl->renderSources.clear();impl->legacyTextures.clear();
    impl->nextDescriptor=3;impl->freeDescriptors.clear();impl->memory.Reset();
    impl->completedTarget.reset();impl->completedWidth=impl->completedHeight=0;
    impl->savedTargets.clear();impl->savedTextures.clear();++impl->stateRevision;
    for(auto& slot:impl->slots)slot.retiredDescriptors.clear();
    impl->publishCacheDiagnostics();
    });
}
void Deko3DBackend::prepareStateSave() {
    CpuWorkProfile::Scope cpuWork(CpuWorkProfile::Phase::Graphics);
    impl->render.call([impl=impl.get()]{std::lock_guard lock(impl->mutex);impl->prepareSnapshot();});
}
void Deko3DBackend::saveState(ps2x::state::Writer& archive) {
    CpuWorkProfile::Scope cpuWork(CpuWorkProfile::Phase::Graphics);
    impl->render.call([impl=impl.get(),&archive]{
    std::lock_guard lock(impl->mutex);impl->prepareSnapshot();
    archive(uint32_t(0x444b5353),uint32_t(1),uint32_t(sizeof(GSContext)));
    impl->memory.saveState(archive);archive(impl->textureEpoch);archive.pod(impl->transfer);
    archive(uint64_t(impl->savedTargets.size()));
    std::optional<Impl::TargetKey> lastMain;
    for(const auto& [key,target]:impl->targets)if(&target==impl->lastMain)lastMain=key;
    for(const auto& target:impl->savedTargets) {
        archive.pod(target.context);archive(target.width,target.height,target.color,target.depth);
    }
    archive(lastMain,impl->completedTarget,impl->completedWidth,impl->completedHeight);
    archive(uint64_t(impl->savedTextures.size()));
    for(const auto& texture:impl->savedTextures)
        archive(texture.key,texture.width,texture.height,texture.wrapU,texture.wrapV,texture.linear,texture.rgba);
    });
}
void Deko3DBackend::loadState(ps2x::state::Reader& archive) {
    CpuWorkProfile::Scope cpuWork(CpuWorkProfile::Phase::Graphics);
    impl->render.call([impl=impl.get(),&archive]{
    std::lock_guard lock(impl->mutex);
    uint32_t magic=0,version=0,contextSize=0;archive(magic,version,contextSize);
    if(magic!=0x444b5353 || version!=1 || contextSize!=sizeof(GSContext))
        fail("snapshot backend/schema is incompatible with deko3d");
    impl->memory.loadState(archive);
    uint64_t epoch=0;GSTransferCommand transfer{};archive(epoch);archive.pod(transfer);
    const auto targetCount=archive.count();if(targetCount>512)fail("snapshot has too many GS targets");
    std::vector<Impl::SavedTarget> targets(targetCount);
    std::set<Impl::TargetKey> keys;
    for(auto& target:targets) {
        archive.pod(target.context);archive(target.width,target.height,target.color,target.depth);
        const auto& c=target.context;
        const Impl::TargetKey key{c.frame.fbp,c.frame.fbw,c.frame.psm,c.zbuf.zbp};
        if(!c.frame.fbw || c.frame.fbw>32 || c.frame.fbp>=512 || c.zbuf.zbp>=512 ||
           (target.width!=c.frame.fbw*64 && !((target.width==wideWidth || target.width==hdWidth || target.width==hdWideWidth || target.width==gsWidth/2 || target.width==wideWidth/2) && c.frame.fbw==10)) || !target.height || target.height>2048 ||
           target.color.size()!=uint64_t(target.width)*target.height*4 || target.depth.size()!=target.color.size() ||
           !keys.insert(key).second)fail("invalid snapshot native target");
    }
    std::optional<Impl::TargetKey> lastMain,completed;
    uint32_t completedWidth=0,completedHeight=0;
    archive(lastMain,completed,completedWidth,completedHeight);
    if((lastMain && !keys.contains(*lastMain)) || (completed && !keys.contains(*completed)) ||
       (completed && (!completedWidth || completedWidth>hdWideWidth || !completedHeight || completedHeight>hdRows)) ||
       (!completed && (completedWidth || completedHeight)))fail("invalid snapshot completed target");
    for(const auto& target:targets) {
        const auto& c=target.context;const Impl::TargetKey key{c.frame.fbp,c.frame.fbw,c.frame.psm,c.zbuf.zbp};
        if(completed && key==*completed && (completedWidth>target.width || completedHeight>target.height))
            fail("snapshot completed extent exceeds its source image");
    }
    const auto textureCount=archive.count();if(textureCount>descriptorCount-3)fail("snapshot has too many decoded textures");
    std::vector<Impl::SavedTexture> textures(textureCount);std::set<std::array<uint64_t,5>> textureKeys;
    for(auto& texture:textures) {
        archive(texture.key,texture.width,texture.height,texture.wrapU,texture.wrapV,texture.linear,texture.rgba);
        if(!texture.width || !texture.height || texture.width>1024 || texture.height>1024 || texture.key[2]!=epoch ||
           texture.rgba.size()!=uint64_t(texture.width)*texture.height*4 || texture.wrapU<1 || texture.wrapU>3 ||
           texture.wrapV<1 || texture.wrapV>3 || !textureKeys.insert(texture.key).second)
            fail("invalid snapshot decoded texture");
    }
    if(transfer.direction>3)fail("invalid snapshot GPU transfer");
    // The outer loader has already replaced canonical VRAM. Submit/drain the
    // abandoned GPU timeline, but never write its dirty targets back over it.
    impl->idle();impl->targets.clear();impl->active=impl->lastMain=nullptr;
    impl->geometries.clear();impl->textures.clear();impl->pending.clear();impl->renderSources.clear();impl->legacyTextures.clear();
    impl->completedTarget.reset();impl->savedTargets.clear();impl->savedTextures.clear();
    for(auto& slot:impl->slots){slot.transient.clear();slot.lifetimes.clear();slot.retiredDescriptors.clear();}
    impl->freeDescriptors.clear();impl->nextDescriptor=3;impl->textureEpoch=epoch;impl->transfer=transfer;
    impl->begin();
    for(auto& saved:targets) {
        // Snapshots store host images. A saved main frame whose layout differs from
        // the current one (aspect or internal resolution) is recreated at the current
        // layout and reseeded from the restored canonical GS memory instead.
        const bool wide=impl->wideFor(saved.context);
        const float savedScaleY=(saved.width==hdWidth || saved.width==hdWideWidth)?hdScaleY:(saved.width==gsWidth/2 || saved.width==wideWidth/2)?0.5f:1.f;
        const uint32_t savedRows=std::max(1u,uint32_t(std::lround(saved.height/savedScaleY)));
        const auto lay=impl->layoutFor(saved.context,wide,savedRows);
        Impl::Target target;target.context=saved.context;target.wide=wide;target.pad=lay.pad;target.rows=savedRows;target.sx=lay.sx;target.sy=lay.sy;
        const bool sameLayout=lay.width==saved.width && lay.height==saved.height;
        target.color=impl->makeImage(lay.width,lay.height,DkImageFormat_RGBA8_Unorm,DkImageFlags_UsageRender|DkImageFlags_Usage2DEngine);
        target.depth=impl->makeImage(lay.width,lay.height,DkImageFormat_ZF32,DkImageFlags_UsageRender|DkImageFlags_Usage2DEngine);
        if(sameLayout){impl->uploadPixels(target.color,saved.color.data());impl->uploadPixels(target.depth,saved.depth.data());}
        else target.reseed=true;
        const auto& c=target.context;const Impl::TargetKey key{c.frame.fbp,c.frame.fbw,c.frame.psm,c.zbuf.zbp};
        auto inserted=impl->targets.emplace(key,std::move(target)).first;
        if(lastMain && *lastMain==key)impl->lastMain=&inserted->second;
    }
    for(auto& saved:textures) {
        Impl::Texture texture;texture.image=impl->makeImage(saved.width,saved.height,DkImageFormat_RGBA8_Unorm,DkImageFlags_Usage2DEngine);
        texture.descriptor=impl->descriptor();texture.legacyEpoch=epoch;
        texture.linear=saved.linear;texture.wrapU=saved.wrapU;texture.wrapV=saved.wrapV;
        impl->uploadPixels(texture.image,saved.rgba.data());
        impl->descriptors(texture.descriptor,texture.image.image,texture.linear,texture.wrapU,texture.wrapV);
        const auto id=++impl->nextLegacyTexture;impl->textures.emplace(id,std::move(texture));impl->legacyTextures.emplace(saved.key,id);
    }
    impl->completedTarget=completed;impl->completedWidth=completedWidth;impl->completedHeight=completedHeight;
    impl->preparedRevision=~0ull;impl->publishCacheDiagnostics();impl->submit();
    });
}
uint64_t Deko3DBackend::QueueNativeModel(NativeModelDraw draw) {
    const auto token=0x4e31303000000000ull|++impl->nextToken;
    const bool worldWide=impl->worldGate.load(std::memory_order_relaxed);
    impl->render.post([impl=impl.get(),token,worldWide,draw=std::move(draw)]() mutable {
        std::lock_guard lock(impl->mutex);impl->pending.emplace(token,std::move(draw));
        if(worldWide)impl->wideGateTokens.insert(token);});
    if(worldWide)impl->probeGateTokens.fetch_add(1,std::memory_order_relaxed);
    return token;
}
uint64_t Deko3DBackend::QueueNativeScreen(NativeScreenDraw draw) {
    const auto token=0x4e31303000000000ull|++impl->nextToken;
    impl->render.post([impl=impl.get(),token,draw=std::move(draw)]() mutable {
        std::lock_guard lock(impl->mutex);impl->pending.emplace(token,std::move(draw));});
    return token;
}
uint64_t Deko3DBackend::RegisterNativeRenderTexture(const GSTex0Reg& gs,const GSTexaReg& alpha,const std::shared_ptr<const NativeAssetLifetime>& lifetime) {
    if(!gs.tbw || gs.tw>11 || gs.th>11 || gs.tbp0%32)return 0;
    const auto id=++impl->nextRenderSource;
    impl->render.post([impl=impl.get(),id,gs,alpha,lifetime]{
        std::lock_guard lock(impl->mutex);impl->renderSources.emplace(id,Impl::RenderSource{gs,alpha,lifetime});});
    return id;
}
void Deko3DBackend::NativeCommand(uint64_t token,const GSContext& context) {
    CpuWorkProfile::Scope cpuWork(CpuWorkProfile::Phase::Graphics);
    impl->render.post([impl=impl.get(),token,context]{
    std::lock_guard lock(impl->mutex);auto it=impl->pending.find(token);
    if(it==impl->pending.end())fail("missing ordered native command token");
    auto command=std::move(it->second);impl->pending.erase(it);
    const bool worldWide=impl->wideGateTokens.erase(token)!=0;
    std::visit([&](const auto& draw){if constexpr(std::is_same_v<std::decay_t<decltype(draw)>,NativeModelDraw>)impl->model(context,draw,worldWide);else impl->screen(context,draw);},command);
    });
}
void Deko3DBackend::finishTimingInterval() {impl->render.post([impl=impl.get()]{std::lock_guard lock(impl->mutex);impl->submit();},true);}
void Deko3DBackend::Flush() {impl->render.post([impl=impl.get()]{std::lock_guard lock(impl->mutex);impl->submit();},true);}
void Deko3DBackend::TextureFlush() {impl->render.post([impl=impl.get()]{std::lock_guard lock(impl->mutex);impl->memory.TextureFlush();++impl->textureEpoch;++impl->stateRevision;if(impl->recording)impl->commands.barrier(DkBarrier_Fragments,DkInvalidateFlags_Image);});}
void Deko3DBackend::Sync(GSSyncReason) {impl->render.post([impl=impl.get()]{std::lock_guard lock(impl->mutex);impl->submit();},true);}
void Deko3DBackend::LoadClut(const GSTex0Reg& texture,const GSTexClutReg& clut) {impl->render.post([impl=impl.get(),texture,clut]{std::lock_guard lock(impl->mutex);impl->memory.LoadClut(texture,clut);});}
bool Deko3DBackend::ClearFramebuffer(const GSContext& context,uint32_t rgba) {
    CpuWorkProfile::Scope cpuWork(CpuWorkProfile::Phase::Graphics);
    if(!context.frame.fbw)return false;
    impl->render.post([impl=impl.get(),context,rgba]{
    std::lock_guard lock(impl->mutex);
    auto& t=impl->target(context,uint32_t(context.scissor.y1)+1);
    if(t.wide)impl->setGsScissor(context,t,true);
    if(context.frame.fbmsk!=0)fail("masked HLE framebuffer clear is unsupported");
    impl->commands.clearColor(0,DkColorMask_RGBA,float(rgba&255)/255.f,float((rgba>>8)&255)/255.f,
        float((rgba>>16)&255)/255.f,std::min(1.f,float(rgba>>24)/128.f));
    t.colorDirty=true;
    });
    return true;
}
void Deko3DBackend::Submit(const GSPrimitiveBatch& batch) {
    CpuWorkProfile::Scope cpuWork(CpuWorkProfile::Phase::Graphics);
    impl->render.post([impl=impl.get(),batch]{
    std::lock_guard lock(impl->mutex);const auto& state=batch.state;const auto& c=state.context;
    if(!batch.vertexCount)return;
    ++impl->stats.gsPrimitives;
    // The normal native engine still emits SDK clear sprites. Clear-only depth
    // replaces the aliased plane directly on GPU, preserving the color surface.
    const bool depthWrite=((c.test>>16)&1) && !c.zbuf.zmask;
    if(((c.test>>16)&1) && ((c.test>>17)&3)==0)return;
    const bool colorWrite=c.frame.fbmsk!=0xffffffffu;
    if(state.prim.type==GS_PRIM_SPRITE && !state.prim.tme && !state.prim.abe && !colorWrite && depthWrite &&
       (!(c.test&1) || ((c.test>>1)&7)==1)) {
        const auto& a=batch.vertices[0];const auto& b=batch.vertices[1];
        const int x=std::max(0,int(std::min(a.x,b.x)-c.xyoffset.ofx/16.f));
        const int y=std::max(0,int(std::min(a.y,b.y)-c.xyoffset.ofy/16.f));
        const int w=int(std::abs(b.x-a.x)),h=int(std::abs(b.y-a.y));
        auto& t=impl->target(c,std::max(1,y+h));
        uint32_t sx=uint32_t(x),sw=uint32_t(w),sy=uint32_t(y),sh=uint32_t(h);
        if(t.wide && w>=int(gsWidth)-64){sx=0;sw=t.color.width;}
        else {sx=t.pad+uint32_t(x*t.sx);sw=std::max(1u,uint32_t(std::ceil(w*t.sx)));}
        sy=uint32_t(y*t.sy);sh=std::max(1u,uint32_t(std::ceil(h*t.sy)));
        sx=std::min(sx,t.color.width-1);sw=std::min(sw,t.color.width-sx);sy=std::min(sy,t.color.height-1);sh=std::min(sh,t.color.height-sy);
        impl->commands.setScissors(0,{{sx,sy,sw,sh}});
        impl->commands.clearDepthStencil(true,float(b.z/65536.0),0,0);t.depthDirty=true;return;
    }
    if(state.pabe || (c.test&(1u<<14)))fail("GS destination-alpha/PABE requires an integer fragment path");
    NativeScreenDraw draw;draw.primitive=std::min(uint32_t(state.prim.type),5u);
    draw.depthTest=((c.test>>16)&1) && ((c.test>>17)&3)>=2;draw.depthWrite=depthWrite;
    draw.vertexAlpha=state.prim.abe;draw.sourceBlend=5;draw.destinationBlend=6;
    auto convert=[&](const GSVertex& vertex) {
        NativeScreenVertex result;
        result.position={vertex.x-c.xyoffset.ofx/16.f,vertex.y-c.xyoffset.ofy/16.f,float(vertex.z),vertex.fog/255.f};
        const float colorScale=state.prim.tme && c.tex0.tfx==0?2.f:1.f;
        result.color={vertex.r*colorScale,vertex.g*colorScale,vertex.b*colorScale,std::min(255.f,float(vertex.a)*2.f)};
        if(state.prim.tme)result.uv=state.prim.fst?std::array<float,4>{vertex.u/(16.f*state.textureWidth),vertex.v/(16.f*state.textureHeight),1,0}:
            std::array<float,4>{vertex.s/std::max(vertex.q,0.000001f),vertex.t/std::max(vertex.q,0.000001f),vertex.q,0};
        else result.uv={0,0,1,0};
        result.uv[3]=vertex.a; // Unexpanded byte for the GS-only alpha predicate.
        return result;
    };
    if(state.prim.type==GS_PRIM_SPRITE) {
        draw.primitive=4;auto a=convert(batch.vertices[0]),d=convert(batch.vertices[1]);auto b=a,cc=d;
        b.position[0]=d.position[0];b.uv[0]=d.uv[0];cc.position[0]=a.position[0];cc.uv[0]=a.uv[0];
        a.color=b.color=cc.color=d.color;draw.vertices={a,b,cc,d};
    }else for(unsigned i=0;i<batch.vertexCount;++i)draw.vertices.push_back(convert(batch.vertices[i]));
    if(!state.prim.iip)for(auto& v:draw.vertices)v.color=draw.vertices.back().color;
    if(state.prim.tme) {
        uint64_t tex=0;static_assert(sizeof(c.tex0)<=64);
        const auto* p=reinterpret_cast<const uint8_t*>(&c.tex0);
        // Hash scalar fields explicitly, avoiding struct padding as identity.
        tex=uint64_t(c.tex0.tbp0)|(uint64_t(c.tex0.tbw)<<14)|(uint64_t(c.tex0.psm)<<20)|(uint64_t(c.tex0.cbp)<<32)|(uint64_t(c.tex0.csa)<<56);
        (void)p;
        const uint64_t alphaSampling=uint64_t(state.texa.ta0)|(uint64_t(state.texa.ta1)<<8)|
            (uint64_t(state.texa.aem)<<16)|(uint64_t(state.linearFilter)<<17);
        const std::array<uint64_t,5> key{tex,uint64_t(state.textureWidth)|(uint64_t(state.textureHeight)<<32),impl->textureEpoch,c.clamp,alphaSampling};
        auto found=impl->legacyTextures.find(key);
        uint64_t id=0;
        if(found!=impl->legacyTextures.end())id=found->second;
        else {
            std::vector<uint32_t> pixels;
            {Impl::DiagnosticScope diagnostic(*impl,Stage::CanonicalDecode);impl->memory.DecodeTexture(state,state.textureWidth,state.textureHeight,pixels);}
            auto image=std::make_shared<NativeTexture>();image->id=++impl->nextLegacyTexture;id=image->id;
            image->width=state.textureWidth;image->height=state.textureHeight;image->linear=state.linearFilter;
            image->wrapU=(c.clamp&3)==0?1:3;image->wrapV=((c.clamp>>2)&3)==0?1:3;
            image->rgba.resize(pixels.size()*4);
            // Legacy GS textures retain their raw alpha byte for integer TFX
            // combination and testing. Native textures keep expanded alpha.
            for(size_t i=0;i<pixels.size();++i){auto color=pixels[i];image->rgba[i*4]=color;image->rgba[i*4+1]=color>>8;image->rgba[i*4+2]=color>>16;image->rgba[i*4+3]=color>>24;}
            draw.texture=image;
            impl->legacyTextures.emplace(key,id);
        }
        if(!draw.texture) {
            // The GPU cache owns the immutable image; only its host ID is needed.
            auto image=std::make_shared<NativeTexture>();image->id=id;draw.texture=image;
        }
    }
    if(state.prim.tme && c.tex0.tfx>1)fail("GS highlight texture functions require an integer fragment path");
    impl->screen(c,draw,0,state.prim.fge,{state.fogR/255.f,state.fogG/255.f,state.fogB/255.f,1.f},
        float(c.tex0.tfx)+(c.tex0.tcc?0.f:2.f),&state);
    if((c.test&1) && ((c.test>>12)&3)!=0 && ((c.test>>1)&7)!=1) {
        const unsigned action=(c.test>>12)&3;
        if(!(impl->reportedAlphaFailModes&(1u<<action))) {
            impl->reportedAlphaFailModes|=1u<<action;
            std::cout<<"[deko3d] GPU alpha-fail mode="<<action<<" comparison="<<((c.test>>1)&7)
                <<" reference="<<((c.test>>4)&255)<<" fbp="<<c.frame.fbp<<" psm="<<unsigned(c.frame.psm)
                <<" depth_write="<<draw.depthWrite<<" textured="<<state.prim.tme<<'\n';
        }
        impl->screen(c,draw,0,state.prim.fge,{state.fogR/255.f,state.fogG/255.f,state.fogB/255.f,1.f},
            float(c.tex0.tfx)+(c.tex0.tcc?0.f:2.f),&state,true);
    }
    });
}

PresentationFrame Deko3DBackend::PresentCompleted(const GSPresentationRequest& request,bool) {
    CpuWorkProfile::Scope cpuWork(CpuWorkProfile::Phase::Graphics);
    const bool worldWide=impl->worldGate.load(std::memory_order_relaxed);
    return impl->render.call([impl=impl.get(),&request,worldWide]() -> PresentationFrame {
    std::lock_guard lock(impl->mutex);impl->begin();
    Impl::Target* source=nullptr;
    const uint32_t fbp=request.hasPreferredSource?request.preferredSource.fbp:uint32_t(request.dispfb1&0x1ffu);
    for(auto& [key,t]:impl->targets)if(t.context.frame.fbp==fbp && (t.color.width>=640 || t.context.frame.fbw==10)){source=&t;break;}
    if(!source)source=impl->lastMain;
    if(!source){impl->submit();return {};}
    const uint32_t presentRows=std::min(Impl::hostRowsOf(*source,gsRows),source->color.height);
    if(impl->presentGameImage(source->color,source->color.width,presentRows,nullptr,worldWide)) {
        ++impl->stats.gameFrames;
        impl->completedTarget=Impl::TargetKey{source->context.frame.fbp,source->context.frame.fbw,source->context.frame.psm,source->context.zbuf.zbp};
        impl->completedWidth=source->color.width;impl->completedHeight=presentRows;
    }
    impl->diagnosticGameFrames.store(impl->stats.gameFrames,std::memory_order_relaxed);
    PresentationFrame frame;frame.width=640;frame.height=448;frame.sourceFbp=source->context.frame.fbp;frame.displayFbp=fbp;frame.keepPrevious=true;
    return frame;
    });
}
PresentationFrame Deko3DBackend::Present(const GSPresentationRequest& request) {return PresentCompleted(request);}
void Deko3DBackend::BeginTransfer(const GSTransferCommand& command) {
    CpuWorkProfile::Scope cpuWork(CpuWorkProfile::Phase::Graphics);
    impl->render.post([impl=impl.get(),command]{
    std::lock_guard lock(impl->mutex);++impl->stateRevision;impl->transfer=command;
    if(command.direction==1 || command.direction==2) {
        const auto source=command.bitbltbuf.sbp*256;
        const auto size=std::max(1u,uint32_t(command.bitbltbuf.sbw))*64*alignUp(uint32_t(command.trxpos.ssay)+command.trxreg.rrh,64)*4;
        for(auto& [key,t]:impl->targets)if(impl->aliases(t,source,size))impl->writeback(t);
    }
    if(command.direction==0 || command.direction==2) {
        const auto size=std::max(1u,uint32_t(command.bitbltbuf.dbw))*64*alignUp(uint32_t(command.trxpos.dsay)+command.trxreg.rrh,64)*4;
        impl->cpuMutation(command.bitbltbuf.dbp*256,size);
    }
    impl->memory.BeginTransfer(command);++impl->textureEpoch;
    });
}
void Deko3DBackend::UploadImage(const uint8_t* data,uint32_t size) {
    CpuWorkProfile::Scope cpuWork(CpuWorkProfile::Phase::Graphics);
    // The producer's buffer is not retained; the worker uploads its own copy.
    std::vector<uint8_t> copy;if(data && size)copy.assign(data,data+size);
    impl->render.post([impl=impl.get(),copy=std::move(copy)]{
        std::lock_guard lock(impl->mutex);impl->memory.UploadImage(copy.data(),uint32_t(copy.size()));++impl->textureEpoch;++impl->stateRevision;});
}
uint32_t Deko3DBackend::ConsumeLocalToHostBytes(uint8_t* data,uint32_t size) {CpuWorkProfile::Scope cpuWork(CpuWorkProfile::Phase::Graphics);return impl->render.call([impl=impl.get(),data,size]() -> uint32_t {std::lock_guard lock(impl->mutex);impl->idle();return impl->memory.ConsumeLocalToHostBytes(data,size);});}
uint32_t Deko3DBackend::ReadVram(uint32_t p,uint32_t b,uint32_t w,uint32_t x,uint32_t y) const {CpuWorkProfile::Scope cpuWork(CpuWorkProfile::Phase::Graphics);return impl->render.call([impl=impl.get(),p,b,w,x,y]() -> uint32_t {std::lock_guard lock(impl->mutex);impl->writebackAll();return impl->memory.ReadVram(p,b,w,x,y);});}
void Deko3DBackend::WriteVram(uint32_t p,uint32_t b,uint32_t w,uint32_t x,uint32_t y,uint32_t value) {CpuWorkProfile::Scope cpuWork(CpuWorkProfile::Phase::Graphics);impl->render.post([impl=impl.get(),p,b,w,x,y,value]{std::lock_guard lock(impl->mutex);impl->cpuMutation(b*256,std::max(1u,w)*64*alignUp(y+1,64)*4);impl->memory.WriteVram(p,b,w,x,y,value);++impl->textureEpoch;++impl->stateRevision;});}
void Deko3DBackend::SnapshotVram(std::vector<uint8_t>& output) const {CpuWorkProfile::Scope cpuWork(CpuWorkProfile::Phase::Graphics);impl->render.call([impl=impl.get(),&output]{std::lock_guard lock(impl->mutex);impl->writebackAll();impl->idle();impl->memory.SnapshotVram(output);});}
GSTransferSnapshot Deko3DBackend::GetTransferSnapshot() const {return impl->render.call([impl=impl.get()]() -> GSTransferSnapshot {std::lock_guard lock(impl->mutex);return impl->memory.GetTransferSnapshot();});}
void Deko3DBackend::setWideScreen(bool enabled) noexcept {impl->wideConfig.store(enabled,std::memory_order_relaxed);}
void Deko3DBackend::setInternalResolution(InternalResolution mode) noexcept {
    const int value=mode==InternalResolution::Half?0:mode==InternalResolution::Double?2:1;
    if(impl->resolutionMode.exchange(value,std::memory_order_relaxed)==value)return;
    // Main targets are rebuilt at the new layout on their next use: write the
    // current GPU content back to canonical GS memory first, then drop them.
    impl->render.post([impl=impl.get()]{
        std::lock_guard lock(impl->mutex);
        for(auto it=impl->targets.begin();it!=impl->targets.end();) {
            if(it->second.context.frame.fbw!=10){++it;continue;}
            impl->writeback(it->second);impl->idle();
            if(impl->active==&it->second)impl->active=nullptr;
            if(impl->lastMain==&it->second)impl->lastMain=nullptr;
            impl->retireBlock(it->second.color.memory);impl->retireBlock(it->second.depth.memory);
            it=impl->targets.erase(it);
        }
        impl->completedTarget.reset();impl->completedWidth=impl->completedHeight=0;
    });
}
void Deko3DBackend::setWorldFrameGate(bool enabled) noexcept {impl->worldGate.store(enabled,std::memory_order_relaxed);}
void Deko3DBackend::setGuestWidensProjection(bool enabled) noexcept {impl->guestWide.store(enabled,std::memory_order_relaxed);}
Deko3DBackend::Probes Deko3DBackend::probes() const noexcept {
    const auto load=[](const std::atomic<uint64_t>& value){return value.load(std::memory_order_relaxed);};
    return {load(impl->probeWideTargets),load(impl->probeGateTokens),load(impl->probeFullWidth),
        load(impl->probeTimingReads),load(impl->probeTimingRejected),load(impl->probeTimingRenderOnly),
        load(impl->probeLastStart),load(impl->probeLastComposition),load(impl->probeLastEnd),
        impl->wideConfig.load(std::memory_order_relaxed),impl->worldGate.load(std::memory_order_relaxed)};
}
Deko3DBackend::GpuTiming Deko3DBackend::gpuTiming() const noexcept {
    return {impl->diagnosticGpuNs.load(std::memory_order_relaxed),impl->diagnosticGpuSamples.load(std::memory_order_relaxed),
        impl->diagnosticCompositionNs.load(std::memory_order_relaxed),impl->diagnosticCompositionSamples.load(std::memory_order_relaxed)};
}
Deko3DBackend::Statistics Deko3DBackend::statistics() const {return impl->render.call([impl=impl.get()]() -> Statistics {std::lock_guard lock(impl->mutex);return impl->stats;});}
void Deko3DBackend::setPresentationActivePredicate(bool(*predicate)()) noexcept {
    impl->presentationActivePredicate.store(predicate,std::memory_order_relaxed);
}
void Deko3DBackend::quiesce() {
    CpuWorkProfile::Scope cpuWork(CpuWorkProfile::Phase::Graphics);
    impl->render.call([impl=impl.get()]{std::lock_guard lock(impl->mutex);impl->idle();});
}
void Deko3DBackend::setMetricsOverlay(const MetricsOverlay& metrics) noexcept {
    impl->metricsGameFps.store(metrics.gameFps,std::memory_order_relaxed);
    impl->metricsCpuMs.store(metrics.producerCpuMs,std::memory_order_relaxed);
    impl->metricsGpuMs.store(metrics.gpuMs,std::memory_order_relaxed);
    impl->metricsEnabled.store(metrics.enabled,std::memory_order_relaxed);
}
void Deko3DBackend::setReplayOverlay(std::string text) {
    if(text.size()>4096)text.resize(4096);
    std::lock_guard lock(impl->replayOverlayMutex);impl->replayOverlay=std::move(text);
    impl->replayOverlayEnabled.store(!impl->replayOverlay.empty(),std::memory_order_release);
}
bool Deko3DBackend::presentOverlay() {
    CpuWorkProfile::Scope cpuWork(CpuWorkProfile::Phase::Graphics);
    return impl->render.call([impl=impl.get()]() -> bool {
        std::lock_guard lock(impl->mutex);auto* source=impl->completedSource();
        return source && impl->presentGameImage(source->color,impl->completedWidth,impl->completedHeight,nullptr,
            impl->worldGate.load(std::memory_order_relaxed));
    });
}
bool Deko3DBackend::copyCompletedFrameRgba(std::vector<uint8_t>& pixels,uint32_t& width,uint32_t& height) {
    CpuWorkProfile::Scope cpuWork(CpuWorkProfile::Phase::Graphics);
    return impl->render.call([impl=impl.get(),&pixels,&width,&height]() -> bool {
    std::lock_guard lock(impl->mutex);auto* source=impl->completedSource();
    pixels.clear();width=height=0;if(!source)return false;
    impl->begin();auto image=impl->makeImage(1280,720,DkImageFormat_RGBA8_Unorm,DkImageFlags_UsageRender|DkImageFlags_Usage2DEngine);
    dk::ImageView output{image.image};impl->commands.bindRenderTargets(&output);impl->active=nullptr;
    impl->commands.setScissors(0,{{0,0,1280,720}});impl->commands.clearColor(0,DkColorMask_RGBA,0,0,0,1);
    impl->commands.barrier(DkBarrier_Fragments,0);
    impl->blitGameImage(dk::ImageView{source->color.image},impl->completedWidth,impl->completedHeight,output,
        impl->worldGate.load(std::memory_order_relaxed));
    pixels=impl->download(image);width=1280;height=720;return true;
    });
}
Deko3DBackend::Diagnostics Deko3DBackend::diagnostics() const noexcept {
    return {impl->diagnosticStage.load(std::memory_order_relaxed),impl->diagnosticSlot.load(std::memory_order_relaxed),
        impl->diagnosticSerial.load(std::memory_order_relaxed),impl->diagnosticSubmissions.load(std::memory_order_relaxed),
        impl->diagnosticGameFrames.load(std::memory_order_relaxed),impl->diagnosticReadbacks.load(std::memory_order_relaxed),
        impl->diagnosticTextures.load(std::memory_order_relaxed),impl->diagnosticLegacyTextures.load(std::memory_order_relaxed),
        impl->diagnosticDescriptorHighWater.load(std::memory_order_relaxed),impl->diagnosticRetiredDescriptors.load(std::memory_order_relaxed),
        impl->diagnosticRttAllocations.load(std::memory_order_relaxed),impl->diagnosticRttReuses.load(std::memory_order_relaxed),
        impl->diagnosticRttPoolBytes.load(std::memory_order_relaxed),impl->diagnosticRttPoolImages.load(std::memory_order_relaxed),
        impl->diagnosticDynamicAllocations.load(std::memory_order_relaxed),impl->diagnosticDynamicUploads.load(std::memory_order_relaxed),
        impl->diagnosticDynamicUploadBytes.load(std::memory_order_relaxed),impl->diagnosticDynamicBufferBytes.load(std::memory_order_relaxed),
        impl->diagnosticDynamicBlocks.load(std::memory_order_relaxed)};
}
const char* Deko3DBackend::diagnosticStageName(Stage stage) noexcept {
    switch(stage) {
        case Stage::Idle:return "idle";
        case Stage::Initializing:return "initializing";
        case Stage::FenceWait:return "slot_fence_wait";
        case Stage::QueueSubmit:return "queue_submit";
        case Stage::SwapchainAcquire:return "swapchain_acquire";
        case Stage::SwapchainPresent:return "swapchain_present";
        case Stage::QueueIdle:return "queue_idle_wait";
        case Stage::CanonicalDecode:return "canonical_texture_decode";
        case Stage::Readback:return "readback";
        case Stage::Reset:return "reset";
        case Stage::VideoImport:return "video_import";
        case Stage::ResourceRetirement:return "resource_retirement";
    }
    return "unknown";
}
std::string Deko3DBackend::device() const {return "NVIDIA Tegra X1 / deko3d native GPU";}
void* Deko3DBackend::deviceHandle() {return impl->render.call([impl=impl.get()]() -> void* {std::lock_guard lock(impl->mutex);impl->initializeGpu();return static_cast<DkDevice>(impl->device);});}
void* Deko3DBackend::queueHandle() {return impl->render.call([impl=impl.get()]() -> void* {std::lock_guard lock(impl->mutex);impl->initializeGpu();return static_cast<DkQueue>(impl->queue);});}

bool Deko3DBackend::presentVideo(const SwitchVideo::Frame& frame) {
    return impl->render.call([impl=impl.get(),&frame]() -> bool {
    std::lock_guard lock(impl->mutex);
    if(!impl->presentationActive()){impl->submit();return true;}
    if(!frame.storage || !frame.storageBytes || !frame.width || !frame.height || !frame.lifetime)
        fail("invalid NVDEC frame allocation");
    impl->begin();
    auto memory=[&] {
        Impl::DiagnosticScope diagnostic(*impl,Stage::VideoImport);
        auto imported=dk::MemBlockMaker{impl->device,frame.storageBytes}
            .setFlags(DkMemBlockFlags_CpuUncached|DkMemBlockFlags_GpuCached|DkMemBlockFlags_Image)
            .setStorage(frame.storage).create();
        if(!imported)fail("cannot import NVDEC GPU storage");
        return dk::UniqueMemBlock{std::move(imported)};
    }();
    std::array<dk::Image,2> planes;
    std::array<uint32_t,2> widths{},heights{};
    // Layout follows nvtegra's NV12 surfaces (the same mapping contract used
    // by mpv's deko3d hwdec adapter). CPU addresses identify storage only.
    for(unsigned plane=0;plane<2;++plane) {
        const uint32_t texelBytes=plane?2:1;
        widths[plane]=plane?(frame.width+1)/2:frame.width;
        heights[plane]=plane?(frame.height+1)/2:frame.height;
        const uint32_t texelAlignment=(frame.pitchLinear?256:64)/texelBytes;
        if(alignUp(widths[plane],texelAlignment)!=frame.planePitch[plane]/texelBytes)
            widths[plane]=frame.planePitch[plane]/texelBytes;
        if(!widths[plane] || frame.planeOffset[plane]>=frame.storageBytes)fail("invalid NVDEC plane bounds");
        uint32_t flags=DkImageFlags_UsageLoadStore|DkImageFlags_Usage2DEngine;
        flags|=frame.pitchLinear?DkImageFlags_PitchLinear:DkImageFlags_UsageVideo;
        dk::ImageLayout layout;
        dk::ImageLayoutMaker{impl->device}.setFormat(plane?DkImageFormat_RG8_Unorm:DkImageFormat_R8_Unorm)
            .setDimensions(widths[plane],heights[plane]).setFlags(flags).setPitchStride(frame.pitchLinear?frame.planePitch[plane]:0).initialize(layout);
        if(layout.getSize()>frame.storageBytes-frame.planeOffset[plane])fail("NVDEC plane extends outside imported storage");
        planes[plane].initialize(layout,memory,frame.planeOffset[plane]);
        // Reserved descriptor IDs are safe because updates travel on the queue
        // after preceding draws; the CPU never rewrites an in-flight table.
        impl->descriptors(plane,planes[plane],true,3,3);
    }
    impl->commands.bindTextures(DkStage_Fragment,0,{dkMakeTextureHandle(0,0),dkMakeTextureHandle(1,1)});
    // ID 1 is reserved for video, alongside render-texture scratch ID 0.
    impl->nextDescriptor=std::max(impl->nextDescriptor,2u);
    impl->commands.barrier(DkBarrier_None,DkInvalidateFlags_Image);
    if(!impl->presentationActive()) {
        impl->slots[impl->slot].transient.push_back(std::move(memory));
        impl->slots[impl->slot].lifetimes.push_back(frame.lifetime);
        impl->submit();return true;
    }
    int slot;
    {Impl::DiagnosticScope diagnostic(*impl,Stage::SwapchainAcquire);slot=impl->queue.acquireImage(impl->swapchain);}
    if(slot<0 || unsigned(slot)>=framesInFlight)fail("cannot acquire video presentation image");
    dk::ImageView color{impl->displays[slot].image};impl->commands.bindRenderTargets(&color);
    impl->commands.setScissors(0,{{0,0,1280,720}});
    impl->commands.clearColor(0,DkColorMask_RGBA,0.f,0.f,0.f,1.f);
    const double aspect=std::isfinite(frame.aspect) && frame.aspect>0?frame.aspect:double(frame.width)/frame.height;
    const float width=std::min(1280.0,720.0*aspect),height=std::min(720.0,1280.0/aspect);
    impl->commands.setViewports(0,{{(1280.f-width)/2,(720.f-height)/2,width,height,0.f,1.f}});
    impl->commands.bindShaders(DkStageFlag_GraphicsMask,{&impl->videoVertex.shader,&impl->videoFragment.shader});
    impl->state(false,false,false,2,1);
    float kr=.299f,kb=.114f;
    if(frame.matrix==SwitchVideo::Matrix::Bt709){kr=.2126f;kb=.0722f;}
    else if(frame.matrix==SwitchVideo::Matrix::Bt2020){kr=.2627f;kb=.0593f;}
    struct alignas(16) Conversion {std::array<float,4> crop,range,coefficients;};
    Conversion conversion{{float(frame.width)/widths[0],float(frame.height)/heights[0],float((frame.width+1)/2)/widths[1],float((frame.height+1)/2)/heights[1]},
        {frame.fullRange?1.f:255.f/219.f,frame.fullRange?0.f:-16.f/219.f,frame.fullRange?1.f:255.f/224.f,0},
        {2*(1-kr),-2*kb*(1-kb)/(1-kr-kb),-2*kr*(1-kr)/(1-kr-kb),2*(1-kb)}};
    auto address=impl->stream(&conversion,sizeof(conversion),DK_UNIFORM_BUF_ALIGNMENT);
    impl->commands.bindUniformBuffer(DkStage_Fragment,0,address,sizeof(conversion));
    impl->commands.draw(DkPrimitive_Triangles,3,1,0,0);
    impl->drawMetricsOverlay(impl->displays[slot]);
    impl->slots[impl->slot].transient.push_back(std::move(memory));
    impl->slots[impl->slot].lifetimes.push_back(frame.lifetime);
    impl->submit();
    {Impl::DiagnosticScope diagnostic(*impl,Stage::SwapchainPresent);impl->queue.presentImage(impl->swapchain,slot);}
    return true;
    });
}

bool Deko3DBackend::runSelfTest() {
    // Self-tests rely on synchronous exceptions and direct Impl reads, so they
    // run inline with the worker drained.
    RenderQueue::DirectScope direct(impl->render);
    std::lock_guard lock(impl->mutex);
    // These independent pixel expectations deliberately exercise non-clear
    // output, fractional alpha, weighted noncommuting matrices, and GPU RTT.
    std::vector<uint8_t> vram(4*1024*1024);
    impl->memory.Initialize(vram.data(),vram.size());
    auto lifetime=std::make_shared<NativeAssetLifetime>();
    GSContext c{};c.frame={400,1,GS_PSM_CT32,0};c.zbuf={440,GS_PSM_Z32,false};c.scissor={0,63,0,63};
    const std::string capturePath="sdmc:/switch/night100/logs/gpu-self-test-"+std::to_string(armGetSystemTick())+".ppm";
    auto inspect=[&](const char* fixture,const std::array<int,4>& expected,int tolerance=1) {
        auto& target=impl->target(c);const auto rgba=impl->download(target.color);
        // Keep actual GPU output even if a fixture fails. PPM needs no image
        // codec or original game resources and is readily converted on host.
        std::ofstream capture(capturePath,std::ios::binary|std::ios::trunc);
        if(capture) {
            capture<<"P6\n64 64\n255\n";
            for(size_t pixel=0;pixel<rgba.size()/4;++pixel)
                capture.write(reinterpret_cast<const char*>(rgba.data()+pixel*4),3);
        }
        const size_t offset=(32*64+32)*4;
        for(unsigned channel=0;channel<4;++channel)if(std::abs(int(rgba[offset+channel])-expected[channel])>tolerance)
            fail(std::string("fixture ")+fixture+" channel "+std::to_string(channel)+" expected "+std::to_string(expected[channel])+" got "+std::to_string(rgba[offset+channel]));
        std::cout<<"[deko3d-self-test] "<<fixture<<" PASS rgba="<<unsigned(rgba[offset])<<','<<unsigned(rgba[offset+1])<<','<<unsigned(rgba[offset+2])<<','<<unsigned(rgba[offset+3])<<'\n';
    };
    auto identity=[] {return NativeMatrix{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};};
    auto quad=[&](std::array<float,4> color,float depth) {
        NativeScreenDraw draw;draw.primitive=3;draw.vertexAlpha=false;
        for(const auto& xy:std::array<std::array<float,2>,4>{{{16,16},{48,16},{16,48},{48,48}}}) {
            NativeScreenVertex vertex;vertex.position={xy[0],xy[1],depth,1};vertex.uv={(xy[0]-16)/32,(xy[1]-16)/32,1,0};vertex.color=color;draw.vertices.push_back(vertex);
        }
        draw.indices={0,1,2,2,1,3};return draw;
    };
    ClearFramebuffer(c,0x800000ffu);inspect("ordered color clear",{255,0,0,255});
    auto screen=quad({0,255,0,255},50000);screen.depthWrite=true;
    NativeCommand(QueueNativeScreen(std::move(screen)),c);
    auto behind=quad({255,0,0,255},10000);behind.depthTest=true;
    NativeCommand(QueueNativeScreen(std::move(behind)),c);
    inspect("indexed UI and reversed depth",{0,255,0,255});
    ClearFramebuffer(c,0x80ff0000u);
    auto fractional=quad({255,0,0,255},0);fractional.vertexAlpha=true;
    auto tex=std::make_shared<NativeTexture>();tex->id=0x70000001;tex->width=2;tex->height=2;tex->lifetime=lifetime;
    for(unsigned i=0;i<4;++i)tex->rgba.insert(tex->rgba.end(),{255,255,255,64});
    fractional.texture=tex;
    NativeCommand(QueueNativeScreen(std::move(fractional)),c);
    inspect("explicit texture upload and fractional UI alpha",{64,0,191,64},2);
    auto geometry=std::make_shared<NativeGeometry>();geometry->id=0x70000001;geometry->revision=1;geometry->lifetime=lifetime;geometry->flags=8|64;
    for(auto& matrix:geometry->inverseBind)matrix=identity();
    for(const auto& p:std::array<std::array<float,3>,4>{{{.1f,.1f,1},{.9f,.1f,1},{.1f,.9f,1},{.9f,.9f,1}}}) {
        NativeVertex vertex;vertex.position=p;vertex.normal={0,0,1};vertex.color=0xffff00ff;geometry->vertices.push_back(vertex);
    }
    geometry->indices={0,1,2,2,1,3};geometry->meshes.push_back({0,6,0});
    NativeModelDraw model;model.geometry=geometry;model.world=identity();model.camera=identity();model.zScale=32768;
    model.materials.push_back({});model.cullMode=2;model.depthTest=false;model.depthWrite=false;model.vertexAlpha=false;
    ClearFramebuffer(c,0x80000000u);NativeCommand(QueueNativeModel(model),c);
    inspect("native world projection and indexed geometry",{255,0,255,255});
    geometry=std::make_shared<NativeGeometry>(*geometry);geometry->id=0x70000002;geometry->boneCount=2;
    for(auto& vertex:geometry->vertices) {
        vertex.position[0]=vertex.position[0]<.5f?.1f:.2f;vertex.position[1]=vertex.position[1]<.5f?.1f:.2f;
        vertex.weights={.5f,.5f,0,0};vertex.bones={0,1,0,0};
    }
    geometry->inverseBind[0][12]=.25f;geometry->inverseBind[1][13]=-.25f;
    model.geometry=geometry;
    model.pose[0]={0,1,0,0,-1,0,0,0,0,0,1,0,.55f,.1f,0,1};
    model.pose[1]={0,1,0,0,-1,0,0,0,0,0,1,0,.5f,.35f,0,1};
    ClearFramebuffer(c,0x80000000u);NativeCommand(QueueNativeModel(model),c);
    inspect("two-bone weighted noncommuting GPU skinning",{255,0,255,255});
    geometry=std::make_shared<NativeGeometry>(*geometry);geometry->id=0x70000003;geometry->flags=32|64;geometry->boneCount=0;
    for(auto& vertex:geometry->vertices){vertex.position[0]=vertex.position[0]<.15f?.1f:.9f;vertex.position[1]=vertex.position[1]<.15f?.1f:.9f;}
    model.geometry=geometry;model.materials[0].color={1,.5f,1,1};model.lights.ambient={.25f,.5f,.75f,0};
    model.fogEnabled=true;model.fogStart=0;model.fogEnd=2;model.fogColor={1,0,0,1};
    ClearFramebuffer(c,0x80000000u);NativeCommand(QueueNativeModel(model),c);
    inspect("GPU lighting material and linear fog",{159,32,96,255},2);
    GSContext shadow=c;shadow.frame.fbp=420;shadow.zbuf.zbp=460;
    ClearFramebuffer(shadow,0x8000ff00u);
    GSTex0Reg gs{};gs.tbp0=shadow.frame.fbp*32;gs.tbw=1;gs.psm=GS_PSM_CT32;gs.tw=gs.th=6;
    auto imported=std::make_shared<NativeTexture>();imported->id=0x70000004;imported->width=imported->height=64;imported->lifetime=lifetime;
    imported->renderTarget=NativeRenderTexture{RegisterNativeRenderTexture(gs,{},lifetime)};
    auto receiver=quad({255,255,255,255},0);receiver.texture=imported;
    ClearFramebuffer(c,0x80000000u);NativeCommand(QueueNativeScreen(receiver),c);
    inspect("GPU render-to-texture shadow import",{0,255,0,255});
    auto& target=impl->target(c);const auto rgba=impl->download(target.color);
    const size_t outside=(4*64+4)*4;
    if(rgba[outside] || rgba[outside+1] || rgba[outside+2])fail("fixture render-to-texture changed pixels outside quad");
    auto cameraTexture=[&](const GSContext& camera,GSTexaReg alpha) {
        GSTex0Reg source{};source.tbp0=camera.frame.fbp*32;source.tbw=1;source.psm=camera.frame.psm;source.tw=source.th=6;
        auto texture=std::make_shared<NativeTexture>();texture->width=texture->height=64;texture->lifetime=lifetime;
        texture->renderTarget=NativeRenderTexture{RegisterNativeRenderTexture(source,alpha,lifetime)};return texture;
    };
    GSContext camera=shadow;camera.frame.fbp=430;camera.zbuf.zbp=470;
    for(unsigned format:std::array<unsigned,2>{GS_PSM_CT16,GS_PSM_CT16S}) {
        camera.frame.psm=format;auto texture=cameraTexture(camera,{7,false,91});
        for(unsigned alpha:std::array<unsigned,6>{0,64,128,200,254,255}) {
            ClearFramebuffer(camera,0);NativeCommand(QueueNativeScreen(quad({23,71,139,float(alpha)},0)),camera);
            auto received=quad({255,255,255,255},0);received.texture=texture;
            NativeCommand(QueueNativeScreen(received),c);
            inspect(("CT16 RTT format="+std::to_string(format)+" alpha="+std::to_string(alpha)).c_str(),{16,64,136,alpha==255?182:14});
        }
    }
    camera.frame.psm=GS_PSM_CT16S;
    auto shadowTexture=cameraTexture(camera,{0,false,128});
    for(unsigned alpha:std::array<unsigned,2>{128,255}) {
        ClearFramebuffer(camera,0);NativeCommand(QueueNativeScreen(quad({0,0,0,float(alpha)},0)),camera);
        auto shadowReceiver=quad({255,255,255,255},0);shadowReceiver.texture=shadowTexture;shadowReceiver.vertexAlpha=true;
        ClearFramebuffer(c,0x80ff0000u);NativeCommand(QueueNativeScreen(shadowReceiver),c);
        inspect(("CT16 shadow receiver fractional alpha="+std::to_string(alpha)).c_str(),alpha==128?std::array<int,4>{0,0,255,0}:std::array<int,4>{0,0,0,255});
    }
    auto aemTexture=cameraTexture(camera,{64,true,91});
    for(unsigned alpha:std::array<unsigned,2>{128,255}) {
        ClearFramebuffer(camera,0);NativeCommand(QueueNativeScreen(quad({3,3,3,float(alpha)},0)),camera);
        auto aemReceiver=quad({255,255,255,255},0);aemReceiver.texture=aemTexture;
        NativeCommand(QueueNativeScreen(aemReceiver),c);
        inspect(("CT16 RTT quantized-black AEM alpha="+std::to_string(alpha)).c_str(),{0,0,0,alpha==255?182:0});
    }
    auto rectangle=[&](std::array<float,4> color,float left,float top,float right,float bottom) {
        auto result=quad(color,0);
        for(auto& vertex:result.vertices){vertex.position[0]=vertex.position[0]==16?left:right;vertex.position[1]=vertex.position[1]==16?top:bottom;}
        return result;
    };
    camera.frame.psm=GS_PSM_CT32;auto orderedTexture=cameraTexture(camera,{});
    ClearFramebuffer(c,0x80ff0000u);ClearFramebuffer(camera,0x8000ff00u);
    auto leftReceiver=rectangle({255,255,255,255},0,0,32,64);leftReceiver.texture=orderedTexture;
    NativeCommand(QueueNativeScreen(leftReceiver),c);
    ClearFramebuffer(camera,0x800000ffu);
    auto rightReceiver=rectangle({255,255,255,255},32,0,64,64);rightReceiver.texture=orderedTexture;
    NativeCommand(QueueNativeScreen(rightReceiver),c);
    auto checkRttPixel=[&](const char* fixture,const std::vector<uint8_t>& pixels,unsigned x,unsigned y,std::array<int,4> expected) {
        for(unsigned channel=0;channel<4;++channel)if(std::abs(int(pixels[(y*64+x)*4+channel])-expected[channel])>1)
            fail(std::string("fixture ")+fixture+" channel="+std::to_string(channel));
    };
    const auto orderedPixels=impl->download(impl->target(c).color);
    checkRttPixel("ordered RTT left snapshot",orderedPixels,16,32,{0,255,0,255});
    checkRttPixel("ordered RTT right snapshot",orderedPixels,48,32,{255,0,0,255});
    // Asymmetric rows also verify that the conversion doesn't vertically flip.
    ClearFramebuffer(camera,0x800000ffu);
    NativeCommand(QueueNativeScreen(rectangle({0,255,0,255},0,0,64,32)),camera);
    auto fullReceiver=rectangle({255,255,255,255},0,0,64,64);fullReceiver.texture=orderedTexture;
    NativeCommand(QueueNativeScreen(fullReceiver),c);
    const auto orientedPixels=impl->download(impl->target(c).color);
    checkRttPixel("RTT upper row orientation",orientedPixels,32,16,{0,255,0,255});
    checkRttPixel("RTT lower row orientation",orientedPixels,32,48,{255,0,0,255});
    std::cout<<"[deko3d-self-test] immutable ordered RTT snapshots and asymmetric orientation PASS\n";
    ClearFramebuffer(camera,0x8000ff00u);ClearFramebuffer(c,0x80ff0000u);
    auto receivingGeometry=std::make_shared<NativeGeometry>();receivingGeometry->id=0x70000006;receivingGeometry->revision=1;
    receivingGeometry->lifetime=lifetime;receivingGeometry->flags=32|64;receivingGeometry->boneCount=1;receivingGeometry->inverseBind[0]=identity();
    for(const auto& position:std::array<std::array<float,3>,4>{{{.1f,.1f,1},{.9f,.1f,1},{.1f,.9f,1},{.9f,.9f,1}}}) {
        NativeVertex vertex;vertex.position=position;vertex.normal={0,0,1};vertex.uv={.5f,.5f};vertex.color=0xffffffff;
        vertex.weights={1,0,0,0};receivingGeometry->vertices.push_back(vertex);
    }
    receivingGeometry->indices={0,1,2,2,1,3};receivingGeometry->meshes.push_back({0,6,0});
    NativeModelDraw receivingModel;receivingModel.geometry=receivingGeometry;receivingModel.world=identity();receivingModel.world[12]=.1f;receivingModel.world[13]=-.1f;
    receivingModel.camera=identity();receivingModel.pose[0]={0,1,0,0,-1,0,0,0,0,0,1,0,.8f,.1f,0,1};receivingModel.zScale=32768;
    receivingModel.materials.push_back({});receivingModel.materials[0].texture=orderedTexture;receivingModel.lights.ambient={.25f,.5f,.75f,0};
    receivingModel.depthTest=false;receivingModel.depthWrite=false;receivingModel.vertexAlpha=false;
    NativeCommand(QueueNativeModel(receivingModel),c);
    NativeCommand(QueueNativeScreen(rectangle({255,0,0,255},0,0,8,8)),c);
    const auto restoredPixels=impl->download(impl->target(c).color);
    checkRttPixel("RTT receiver skin transform light material",restoredPixels,32,32,{0,128,0,255});
    checkRttPixel("ordinary UI after RTT receiving model",restoredPixels,4,4,{255,0,0,255});
    std::cout<<"[deko3d-self-test] RTT receiver restores skinning lighting transform state and subsequent UI PASS\n";
    uint64_t warmedAllocations=0,warmedReuses=0;
    for(unsigned frame=0;frame<12;++frame) {
        const bool even=(frame&1)==0;
        ClearFramebuffer(c,0x80000000u);ClearFramebuffer(camera,even?0x8000ff00u:0x800000ffu);
        NativeCommand(QueueNativeScreen(leftReceiver),c);
        ClearFramebuffer(camera,even?0x800000ffu:0x8000ff00u);
        NativeCommand(QueueNativeScreen(rightReceiver),c);
        const auto pixels=impl->download(impl->target(c).color);
        checkRttPixel("pooled immutable left snapshot",pixels,16,32,even?std::array<int,4>{0,255,0,255}:std::array<int,4>{255,0,0,255});
        checkRttPixel("pooled immutable right snapshot",pixels,48,32,even?std::array<int,4>{255,0,0,255}:std::array<int,4>{0,255,0,255});
        const auto pool=diagnostics();
        if(frame==2){warmedAllocations=pool.renderTextureImageAllocations;warmedReuses=pool.renderTextureImageReuses;}
        if(frame>2 && pool.renderTextureImageAllocations!=warmedAllocations)
            fail("fixture stable RTT dimensions allocated after all command slots warmed");
    }
    const auto pool=diagnostics();
    if(pool.renderTextureImageReuses-warmedReuses<18 || pool.renderTexturePoolImages>framesInFlight*renderTexturePoolImagesPerSlot ||
       pool.renderTexturePoolBytes>framesInFlight*renderTexturePoolBytesPerSlot)
        fail("fixture RTT pool did not reuse bounded fence-owned images");
    std::cout<<"[deko3d-self-test] fence-owned RTT pool PASS warmed_allocations="<<warmedAllocations
        <<" allocations="<<pool.renderTextureImageAllocations<<" reuses="<<pool.renderTextureImageReuses
        <<" pooled_images="<<pool.renderTexturePoolImages<<" pooled_bytes="<<pool.renderTexturePoolBytes<<'\n';
    // Reserved particle/shadow IDs change in place. Both VB and IB slices must
    // remain immutable until their slot fence, even with another revision of
    // the same ID queued immediately afterward.
    const auto dynamicBefore=diagnostics();const auto staticCacheBefore=impl->geometries.size();
    uint64_t warmedDynamicAllocations=0;
    for(unsigned frame=0;frame<12;++frame) {
        ClearFramebuffer(c,0x80000000u);
        for(unsigned side=0;side<2;++side) {
            auto animated=std::make_shared<NativeGeometry>();animated->id=0x70000007;animated->revision=frame*2+side+1;
            animated->lifetime=lifetime;animated->dynamic=true;animated->flags=8|64;
            const bool green=((frame+side)&1)==0;
            for(unsigned group=0;group<2;++group) {
                const float shift=(frame&1)?.02f:-.02f;
                const float left=group?.55f:.1f,right=group?.9f:.45f;
                for(const auto& xy:std::array<std::array<float,2>,4>{{{left,.2f},{right,.2f},{left,.8f},{right,.8f}}}) {
                    NativeVertex vertex;vertex.position={xy[0]+shift,xy[1]-shift,1};vertex.color=green?0xff00ff00u:0xff0000ffu;
                    animated->vertices.push_back(vertex);
                }
            }
            for(unsigned index:std::array<unsigned,6>{0,1,2,2,1,3})animated->indices.push_back(index+side*4);
            animated->meshes.push_back({0,6,0});
            NativeModelDraw animatedDraw;animatedDraw.geometry=animated;animatedDraw.world=identity();animatedDraw.camera=identity();
            animatedDraw.zScale=32768;animatedDraw.materials.push_back({});animatedDraw.depthTest=false;animatedDraw.depthWrite=false;animatedDraw.vertexAlpha=false;
            NativeCommand(QueueNativeModel(animatedDraw),c);
        }
        const auto pixels=impl->download(impl->target(c).color);const bool even=(frame&1)==0;
        checkRttPixel("dynamic immutable vertex/index left revision",pixels,16,32,even?std::array<int,4>{0,255,0,255}:std::array<int,4>{255,0,0,255});
        checkRttPixel("dynamic immutable vertex/index right revision",pixels,48,32,even?std::array<int,4>{255,0,0,255}:std::array<int,4>{0,255,0,255});
        const auto dynamic=diagnostics();
        if(frame==2)warmedDynamicAllocations=dynamic.dynamicBufferAllocations;
        if(frame>2 && dynamic.dynamicBufferAllocations!=warmedDynamicAllocations)
            fail("fixture stable dynamic geometry allocated after three-slot warmup");
    }
    auto vertexOnly=std::make_shared<NativeGeometry>();vertexOnly->id=0x70000007;vertexOnly->revision=100;
    vertexOnly->lifetime=lifetime;vertexOnly->dynamic=true;vertexOnly->flags=8|64;
    for(const auto& xy:std::array<std::array<float,2>,3>{{{.1f,0},{.4f,0},{.25f,.2f}}}) {
        NativeVertex vertex;vertex.position={xy[0],xy[1],1};vertex.color=0xff00ffffu;vertexOnly->vertices.push_back(vertex);
    }
    vertexOnly->meshes.push_back({0,3,0});
    NativeModelDraw vertexOnlyDraw;vertexOnlyDraw.geometry=vertexOnly;vertexOnlyDraw.world=identity();vertexOnlyDraw.camera=identity();
    vertexOnlyDraw.zScale=32768;vertexOnlyDraw.materials.push_back({});vertexOnlyDraw.indexed=false;
    vertexOnlyDraw.depthTest=false;vertexOnlyDraw.depthWrite=false;vertexOnlyDraw.vertexAlpha=false;
    NativeCommand(QueueNativeModel(vertexOnlyDraw),c);
    const auto vertexOnlyPixels=impl->download(impl->target(c).color);
    checkRttPixel("dynamic unindexed shadow geometry",vertexOnlyPixels,16,4,{255,255,0,255});
    ClearFramebuffer(camera,0x8000ff00u);
    auto dynamicSkinGeometry=std::make_shared<NativeGeometry>(*receivingGeometry);
    dynamicSkinGeometry->id=0x70000007;dynamicSkinGeometry->revision=101;dynamicSkinGeometry->dynamic=true;
    auto dynamicSkinDraw=receivingModel;dynamicSkinDraw.geometry=dynamicSkinGeometry;
    NativeCommand(QueueNativeModel(dynamicSkinDraw),c);
    inspect("dynamic GPU skin inverse stream and RTT state",{0,128,0,255});
    const auto dynamicAfter=diagnostics();
    constexpr uint64_t expectedDynamicBytes=24*(8*sizeof(NativeVertex)+6*sizeof(uint16_t))+3*sizeof(NativeVertex)+
        4*sizeof(NativeVertex)+6*sizeof(uint16_t)+sizeof(NativeMatrix)*64;
    if(dynamicAfter.dynamicBufferAllocations!=warmedDynamicAllocations ||
       dynamicAfter.dynamicGeometryUploads-dynamicBefore.dynamicGeometryUploads!=26 ||
       dynamicAfter.dynamicUploadBytes-dynamicBefore.dynamicUploadBytes!=expectedDynamicBytes ||
       impl->geometries.size()!=staticCacheBefore)
        fail("fixture dynamic geometry did not reuse streaming storage or uploaded unused inverse matrices");
    std::cout<<"[deko3d-self-test] animated dynamic geometry stream PASS allocations="<<dynamicAfter.dynamicBufferAllocations
        <<" uploads="<<dynamicAfter.dynamicGeometryUploads<<" uploaded_bytes="<<dynamicAfter.dynamicUploadBytes
        <<" persistent_bytes="<<dynamicAfter.dynamicBufferBytes<<'\n';
    // Alpha failure selects framebuffer, depth, or RGB writes independently.
    // A later green primitive probes actual depth, rather than inferring it
    // from whether the failed fragment changed the color attachment.
    constexpr std::array<const char*,4> alphaFailNames{"KEEP","FB_ONLY","ZB_ONLY","RGB_ONLY"};
    auto prepareAlphaFixture=[&] {
        ClearFramebuffer(c,0x80ff0000u);
        GSPrimitiveBatch depthClear{};depthClear.state.context=c;depthClear.state.context.frame.fbmsk=0xffffffffu;
        depthClear.state.context.test=3u<<16;depthClear.state.prim.type=GS_PRIM_SPRITE;depthClear.vertexCount=2;
        depthClear.vertices[1].x=depthClear.vertices[1].y=64;depthClear.vertices[1].z=10000;Submit(depthClear);
    };
    for(unsigned action=0;action<4;++action) {
        GSPrimitiveBatch alphaSprite{};alphaSprite.state.context=c;alphaSprite.state.prim.type=GS_PRIM_SPRITE;alphaSprite.vertexCount=2;
        alphaSprite.state.context.test=1u|(6u<<1)|(64u<<4)|(action<<12)|(1u<<16)|(2u<<17);
        alphaSprite.vertices[0].x=alphaSprite.vertices[0].y=16;alphaSprite.vertices[1].x=alphaSprite.vertices[1].y=48;
        for(auto& vertex:alphaSprite.vertices){vertex.z=50000;vertex.r=255;vertex.a=32;}
        prepareAlphaFixture();Submit(alphaSprite);
        const std::array<int,4> failedColor=action==1?std::array<int,4>{255,0,0,64}:
            action==3?std::array<int,4>{255,0,0,255}:std::array<int,4>{0,0,255,255};
        const auto label=std::string("GS alpha-fail ")+alphaFailNames[action];
        inspect((label+" failed color and alpha").c_str(),failedColor);
        auto probeDepth=quad({0,255,0,255},30000);probeDepth.depthTest=true;
        NativeCommand(QueueNativeScreen(probeDepth),c);
        inspect((label+" failed depth mask").c_str(),action==2?std::array<int,4>{0,0,255,255}:std::array<int,4>{0,255,0,255});
        prepareAlphaFixture();for(auto& vertex:alphaSprite.vertices)vertex.a=96;Submit(alphaSprite);
        inspect((label+" passing color and alpha").c_str(),{255,0,0,192});
        NativeCommand(QueueNativeScreen(probeDepth),c);
        inspect((label+" passing depth write").c_str(),{255,0,0,192});
    }
    GSPrimitiveBatch byteAlpha{};byteAlpha.state.context=c;byteAlpha.state.prim.type=GS_PRIM_SPRITE;byteAlpha.vertexCount=2;
    byteAlpha.vertices[0].x=byteAlpha.vertices[0].y=16;byteAlpha.vertices[1].x=byteAlpha.vertices[1].y=48;
    for(auto& vertex:byteAlpha.vertices){vertex.z=50000;vertex.r=255;}
    // Literal truth masks for alpha={63,64,65}, AREF=64 cover all eight
    // comparisons and catch the former a*2/255 versus AREF/128 mismatch.
    constexpr std::array<uint8_t,8> expectedAlphaPass{0,7,1,3,2,6,4,5};
    for(unsigned op=0;op<8;++op)for(unsigned sample=0;sample<3;++sample) {
        const unsigned alpha=63+sample;
        byteAlpha.state.context.test=1u|(op<<1)|(64u<<4)|(2u<<12)|(1u<<16)|(2u<<17);
        for(auto& vertex:byteAlpha.vertices)vertex.a=alpha;
        prepareAlphaFixture();Submit(byteAlpha);
        const bool pass=expectedAlphaPass[op]&(1u<<sample);
        inspect(("GS alpha-byte boundary op="+std::to_string(op)+" a="+std::to_string(alpha)).c_str(),
            pass?std::array<int,4>{255,0,0,int(alpha*2)}:std::array<int,4>{0,0,255,255});
    }
    for(unsigned reference:std::array<unsigned,2>{0,255})for(bool equal:std::array<bool,2>{true,false}) {
        const unsigned alpha=equal?reference:reference==0?1:254;
        byteAlpha.state.context.test=1u|(4u<<1)|(reference<<4)|(2u<<12)|(1u<<16)|(2u<<17);
        for(auto& vertex:byteAlpha.vertices)vertex.a=alpha;
        prepareAlphaFixture();Submit(byteAlpha);
        inspect(("GS alpha-byte endpoint ref="+std::to_string(reference)+" a="+std::to_string(alpha)).c_str(),
            equal?std::array<int,4>{255,0,0,int(std::min(alpha*2,255u))}:std::array<int,4>{0,0,255,255});
    }
    // These cases require the final texture-combined byte, including raw
    // values above 128 and integer MODULATE truncation before comparison.
    struct TextureAlphaCase {unsigned tfx,tcc,vertex,texture,reference;bool pass;};
    constexpr std::array<TextureAlphaCase,6> textureAlphaCases{{
        {0,1,200,64,100,true},{0,1,64,129,64,true},{1,1,32,200,200,true},
        {0,0,64,200,64,true},{1,0,64,200,64,true},{0,1,200,64,99,false}}};
    byteAlpha.state.prim.tme=true;byteAlpha.state.prim.fst=true;byteAlpha.state.textureWidth=byteAlpha.state.textureHeight=1;
    byteAlpha.state.context.tex0.tbp0=256;byteAlpha.state.context.tex0.tbw=1;byteAlpha.state.context.tex0.psm=GS_PSM_CT32;
    for(const auto& fixture:textureAlphaCases) {
        WriteVram(GS_PSM_CT32,256,1,0,0,0x000000ffu|(fixture.texture<<24));
        // CPU writes deliberately leave the GS texture page cache stale.
        // Model the guest's TEXFLUSH before expecting the replacement texel.
        TextureFlush();
        byteAlpha.state.context.tex0.tfx=fixture.tfx;byteAlpha.state.context.tex0.tcc=fixture.tcc;
        byteAlpha.state.context.test=1u|(4u<<1)|(fixture.reference<<4)|(2u<<12)|(1u<<16)|(2u<<17);
        for(auto& vertex:byteAlpha.vertices){vertex.r=128;vertex.a=fixture.vertex;}
        std::vector<uint32_t> decoded;impl->memory.DecodeTexture(byteAlpha.state,1,1,decoded);
        if(decoded.size()!=1 || (decoded[0]>>24)!=fixture.texture)
            fail("fixture GS texture-alpha source was not replaced after TEXFLUSH");
        prepareAlphaFixture();Submit(byteAlpha);
        inspect(("GS texture alpha tfx="+std::to_string(fixture.tfx)+" tcc="+std::to_string(fixture.tcc)+
            " va="+std::to_string(fixture.vertex)+" ta="+std::to_string(fixture.texture)).c_str(),
            fixture.pass?std::array<int,4>{255,0,0,int(std::min(fixture.reference*2,255u))}:std::array<int,4>{0,0,255,255});
    }
    WriteVram(GS_PSM_CT24,256,1,0,0,0xffu);
    TextureFlush();
    byteAlpha.state.context.tex0.psm=GS_PSM_CT24;byteAlpha.state.context.tex0.tfx=1;byteAlpha.state.context.tex0.tcc=1;
    for(unsigned textureAlpha:std::array<unsigned,2>{64,200}) {
        byteAlpha.state.texa.ta0=textureAlpha;
        byteAlpha.state.context.test=1u|(4u<<1)|(textureAlpha<<4)|(2u<<12)|(1u<<16)|(2u<<17);
        std::vector<uint32_t> decoded;impl->memory.DecodeTexture(byteAlpha.state,1,1,decoded);
        if(decoded.size()!=1 || (decoded[0]>>24)!=textureAlpha)
            fail("fixture GS TEXA source alpha did not change with TA0");
        prepareAlphaFixture();Submit(byteAlpha);
        inspect(("GS TEXA alpha cache ta0="+std::to_string(textureAlpha)).c_str(),{255,0,0,int(std::min(textureAlpha*2,255u))});
    }
    // All 23 representable A/B/D combinations for each of source alpha,
    // destination alpha and FIX. The four 1+C equations are intentionally
    // excluded; their explicit rejection is checked below.
    constexpr std::array<std::array<int,3>,3> blendRgb{{{160,64,32},{48,96,128},{0,0,0}}};
    GSPrimitiveBatch blendSprite{};blendSprite.state.context=c;blendSprite.state.prim.type=GS_PRIM_SPRITE;
    blendSprite.state.prim.abe=true;blendSprite.vertexCount=2;blendSprite.state.context.test=(1u<<16)|(2u<<17);
    blendSprite.vertices[0].x=blendSprite.vertices[0].y=16;blendSprite.vertices[1].x=blendSprite.vertices[1].y=48;
    for(auto& vertex:blendSprite.vertices){vertex.r=160;vertex.g=64;vertex.b=32;vertex.a=64;vertex.z=50000;}
    for(unsigned coefficient=0;coefficient<3;++coefficient)for(unsigned a=0;a<3;++a)for(unsigned b=0;b<3;++b)for(unsigned d=0;d<3;++d) {
        if((a==0 && d==0 && b!=0) || (a==1 && d==1 && b!=1))continue;
        ClearFramebuffer(c,0x80000000u);
        auto destination=quad({48,96,128,192},10000);destination.depthWrite=true;
        NativeCommand(QueueNativeScreen(destination),c);
        blendSprite.state.context.alpha=a|(b<<2)|(coefficient<<4)|(d<<6)|(uint64_t(32)<<32);
        Submit(blendSprite);
        const int factor=coefficient==0?64:coefficient==1?96:32;
        std::array<int,4> expected{0,0,0,128};
        for(unsigned channel=0;channel<3;++channel)
            expected[channel]=std::clamp(((blendRgb[a][channel]-blendRgb[b][channel])*factor>>7)+blendRgb[d][channel],0,255);
        const auto label="GS blend A="+std::to_string(a)+" B="+std::to_string(b)+" C="+std::to_string(coefficient)+" D="+std::to_string(d);
        inspect(label.c_str(),expected,2);
        auto behindBlend=quad({0,255,0,255},30000);behindBlend.depthTest=true;
        NativeCommand(QueueNativeScreen(behindBlend),c);
        inspect((label+" depth and separate alpha").c_str(),expected,2);
    }
    blendSprite.state.context.alpha=(1u<<2)|(2u<<4)|(uint64_t(32)<<32); // (Src-Dst)*FIX+Src requires 1+FIX.
    bool rejectedBlend=false;
    try {Submit(blendSprite);}catch(const std::runtime_error& error) {
        const std::string message=error.what();
        rejectedBlend=message.find("A=0 B=1 C=2 D=0 FIX=32 ABE=1 primitive=6")!=std::string::npos &&
            message.find("1+C")!=std::string::npos;
        if(!rejectedBlend)throw;
    }
    if(!rejectedBlend)fail("fixture unrepresentable GS blend did not report its exact selectors");
    // Exercise explicit GS cache coherence, then replace more textures than
    // the descriptor table can hold without fence-safe epoch retirement.
    auto cachedSprite=byteAlpha;cachedSprite.state.context.tex0.psm=GS_PSM_CT32;
    cachedSprite.state.context.tex0.tfx=1;cachedSprite.state.context.tex0.tcc=1;
    cachedSprite.state.context.test=3u<<16;
    WriteVram(GS_PSM_CT32,256,1,0,0,0x400000ffu);TextureFlush();Submit(cachedSprite);
    inspect("GS cached texture initial alpha",{255,0,0,128});
    WriteVram(GS_PSM_CT32,256,1,0,0,0x600000ffu);Submit(cachedSprite);
    inspect("GS texture retains stale texel before TEXFLUSH",{255,0,0,128});
    TextureFlush();Submit(cachedSprite);
    inspect("GS TEXFLUSH refreshes decoded GPU texture without another write",{255,0,0,192});
    const auto uploadsBeforeStress=impl->stats.textureUploads;
    for(unsigned replacement=0;replacement<descriptorCount+64;++replacement) {
        WriteVram(GS_PSM_CT32,256,1,0,0,0xffu|((64u+(replacement&1u)*32u)<<24));
        TextureFlush();Submit(cachedSprite);
        if((replacement&7)==7)Flush();
    }
    inspect("GS texture cache recycles beyond descriptor capacity",{255,0,0,192});
    const auto cacheDiagnostics=diagnostics();
    if(impl->stats.textureUploads-uploadsBeforeStress!=descriptorCount+64 ||
       cacheDiagnostics.descriptorHighWater>128 || cacheDiagnostics.legacyCacheEntries>8)
        fail("fixture GS cache retirement exceeded bounded descriptor/image storage");
    std::cout<<"[deko3d-self-test] 4160 texture replacements PASS descriptor_high_water="<<cacheDiagnostics.descriptorHighWater
        <<" legacy_cache="<<cacheDiagnostics.legacyCacheEntries<<" pending_descriptors="<<cacheDiagnostics.descriptorPendingRetirement<<'\n';
    // The title's SDK clears touch 640x448 while SCISSOR remains 2047. The
    // native camera that follows still projects into 448 rows, not 2048.
    GSContext mainContext{};mainContext.frame={0,10,GS_PSM_CT32,0};
    mainContext.zbuf={280,GS_PSM_Z32,false};mainContext.scissor={0,2047,0,2047};mainContext.test=3u<<16;
    GSPrimitiveBatch sdkClear{};sdkClear.state.context=mainContext;sdkClear.state.prim.type=GS_PRIM_SPRITE;sdkClear.vertexCount=2;
    sdkClear.vertices[1].x=640;sdkClear.vertices[1].y=448;
    for(auto& vertex:sdkClear.vertices){vertex.b=255;vertex.a=128;}
    Submit(sdkClear);
    sdkClear.state.context.frame.fbmsk=0xffffffffu;Submit(sdkClear);
    auto viewportGeometry=std::make_shared<NativeGeometry>();
    viewportGeometry->id=0x70000005;viewportGeometry->revision=1;viewportGeometry->lifetime=lifetime;viewportGeometry->flags=8|64;
    for(const auto& position:std::array<std::array<float,3>,4>{{{.25f,.15f,1},{.75f,.15f,1},{.25f,.4f,1},{.75f,.4f,1}}}) {
        NativeVertex vertex;vertex.position=position;vertex.color=0xffff00ff;viewportGeometry->vertices.push_back(vertex);
    }
    viewportGeometry->indices={0,1,2,2,1,3};viewportGeometry->meshes.push_back({0,6,0});
    NativeModelDraw viewportModel;viewportModel.geometry=viewportGeometry;viewportModel.world=identity();viewportModel.camera=identity();
    viewportModel.zScale=32768;viewportModel.materials.push_back({});viewportModel.depthTest=true;viewportModel.depthWrite=false;viewportModel.vertexAlpha=false;
    NativeCommand(QueueNativeModel(viewportModel),mainContext);
    auto& mainTarget=impl->target(mainContext);
    if(mainTarget.color.width!=640 || mainTarget.color.height!=448)
        fail("fixture oversized GS scissor changed native viewport to "+std::to_string(mainTarget.color.width)+"x"+std::to_string(mainTarget.color.height));
    const auto mainPixels=impl->download(mainTarget.color);
    auto viewportPixel=[&](uint32_t x,uint32_t y,std::array<uint8_t,3> expected) {
        for(unsigned channel=0;channel<3;++channel)
            if(mainPixels[(size_t(y)*640+x)*4+channel]!=expected[channel])
                fail("fixture oversized GS scissor misplaced native projection at "+std::to_string(x)+","+std::to_string(y));
    };
    viewportPixel(320,112,{255,0,255});viewportPixel(320,336,{0,0,255});viewportPixel(16,32,{0,0,255});
    std::cout<<"[deko3d-self-test] oversized GS scissor and SDK clears retain 640x448 native projection PASS\n";
    // Applet activity is published without calling the driver on the applet
    // thread. Suppressed presentation must still submit the producer's work;
    // its subsequent drain must preserve cached geometry for the next frame.
    static std::atomic<bool> selfTestPresentationActive{false};
    struct RestorePresentationPredicate {
        Deko3DBackend& backend;bool(*previous)();
        ~RestorePresentationPredicate(){backend.setPresentationActivePredicate(previous);}
    } restorePredicate{*this,impl->presentationActivePredicate.load(std::memory_order_relaxed)};
    setPresentationActivePredicate(+[] {return selfTestPresentationActive.load(std::memory_order_relaxed);});
    selfTestPresentationActive.store(false,std::memory_order_relaxed);
    auto boundedMain=mainContext;boundedMain.scissor={0,639,0,447};
    ClearFramebuffer(boundedMain,0x80ff0000u);NativeCommand(QueueNativeModel(viewportModel),boundedMain);
    GSPresentationRequest resumeRequest{};resumeRequest.hasPreferredSource=true;resumeRequest.preferredSource=boundedMain.frame;
    const auto beforeSuppression=impl->stats;
    PresentCompleted(resumeRequest);
    if(impl->recording || impl->stats.gpuSubmissions<=beforeSuppression.gpuSubmissions ||
       impl->stats.gameFrames!=beforeSuppression.gameFrames)
        fail("fixture inactive presentation did not submit work without displaying a frame");
    quiesce();
    const auto inactivePixels=impl->download(impl->target(boundedMain).color);
    for(unsigned channel=0;channel<3;++channel) {
        if(inactivePixels[(size_t(112)*640+320)*4+channel]!=std::array<uint8_t,3>{255,0,255}[channel] ||
           inactivePixels[(size_t(336)*640+320)*4+channel]!=std::array<uint8_t,3>{0,0,255}[channel])
            fail("fixture quiesce lost queued drawing while presentation was inactive");
    }
    selfTestPresentationActive.store(true,std::memory_order_relaxed);
    ClearFramebuffer(boundedMain,0x800000ffu);NativeCommand(QueueNativeModel(viewportModel),boundedMain);
    if(impl->stats.geometryUploads!=beforeSuppression.geometryUploads)
        fail("fixture quiesce discarded persistent GPU geometry");
    PresentCompleted(resumeRequest);
    if(impl->stats.gameFrames!=beforeSuppression.gameFrames+1)
        fail("fixture reactivated presentation did not display its next completed frame");
    std::vector<uint8_t> resumedPixels;
    if(!impl->presentGameImage(impl->target(boundedMain).color,640,448,&resumedPixels))
        fail("fixture reactivated presentation remained disabled");
    for(const auto& sample:std::array<std::array<uint32_t,5>,4>{{
        {0,180,0,0,0},{1279,180,0,0,0},{640,180,255,0,255},{640,540,255,0,0}}})
        for(unsigned channel=0;channel<3;++channel)
            if(resumedPixels[(size_t(sample[1])*1280+sample[0])*4+channel]!=sample[2+channel])
                fail("fixture reactivated presentation has stale pixels or misplaced geometry");
    setPresentationActivePredicate(restorePredicate.previous);
    std::cout<<"[deko3d-self-test] inactive submission, checked quiesce and cached next-frame presentation PASS\n";
    // Exercise the production swapchain composition, including exact content
    // boundaries. A blue field makes a misplaced/oversized black bar visible;
    // the green center also rejects a presentation that merely clears black.
    ClearFramebuffer(c,0x80ff0000u);NativeCommand(QueueNativeScreen(receiver),c);
    std::vector<uint8_t> presented;
    impl->presentGameImage(impl->target(c).color,64,64,&presented);
    auto presentationPixel=[&](uint32_t x,uint32_t y,std::array<uint8_t,3> expected) {
        const auto offset=(size_t(y)*1280+x)*4;
        for(unsigned channel=0;channel<3;++channel)
            if(presented[offset+channel]!=expected[channel])
                fail("fixture 4:3 presentation at "+std::to_string(x)+","+std::to_string(y)+" channel "+std::to_string(channel)+
                    " expected "+std::to_string(expected[channel])+" got "+std::to_string(presented[offset+channel]));
    };
    for(uint32_t y=0;y<720;++y) {
        for(uint32_t x=0;x<160;++x)presentationPixel(x,y,{0,0,0});
        for(uint32_t x=1120;x<1280;++x)presentationPixel(x,y,{0,0,0});
    }
    presentationPixel(160,360,{0,0,255});presentationPixel(1119,360,{0,0,255});
    presentationPixel(640,0,{0,0,255});presentationPixel(640,719,{0,0,255});
    presentationPixel(640,360,{0,255,0});
    const auto presentationCapture="sdmc:/switch/night100/logs/gpu-presentation-"+std::to_string(armGetSystemTick())+".ppm";
    {
        std::ofstream capture(presentationCapture,std::ios::binary);
        if(capture) {
            capture<<"P6\n1280 720\n255\n";
            for(size_t pixel=0;pixel<presented.size()/4;++pixel)
                capture.write(reinterpret_cast<const char*>(presented.data()+pixel*4),3);
        }
    }
    std::cout<<"[deko3d-self-test] centered 960x720 game with black pillarboxes PASS capture="<<presentationCapture<<'\n';
    // The optional HUD must be absent by default, use actual glyph pixels,
    // preserve the pillarboxes, and disappear on the next disabled frame.
    presentationPixel(180,24,{0,0,255});
    const MetricsOverlay previousMetrics{impl->metricsEnabled.load(std::memory_order_relaxed),
        impl->metricsGameFps.load(std::memory_order_relaxed),impl->metricsCpuMs.load(std::memory_order_relaxed),
        impl->metricsGpuMs.load(std::memory_order_relaxed)};
    setMetricsOverlay({true,59.5,12.3,-1});
    impl->presentGameImage(impl->target(c).color,64,64,&presented);
    presentationPixel(180,24,{255,255,255}); // F's top horizontal stroke.
    presentationPixel(193,28,{0,0,63}); // F's open area, dark panel over blue.
    // Fixed fixture text ends with GPU N/A, rather than a fabricated zero.
    presentationPixel(666,24,{255,255,255}); // N, first column.
    presentationPixel(696,24,{255,255,255}); // Slash, final column.
    presentationPixel(705,24,{255,255,255}); // A, second column.
    presentationPixel(684,24,{0,0,63}); // Slash, empty first column.
    presentationPixel(100,24,{0,0,0});presentationPixel(1200,24,{0,0,0});
    presentationPixel(640,360,{0,255,0});
    setMetricsOverlay({});
    impl->presentGameImage(impl->target(c).color,64,64,&presented);
    presentationPixel(180,24,{0,0,255});presentationPixel(640,360,{0,255,0});
    setMetricsOverlay(previousMetrics);
    std::cout<<"[deko3d-self-test] optional metrics HUD glyph, panel, pillarboxes and disable PASS\n";
    // Explicit checkpoints preserve normalized GPU channels and depth exactly,
    // while publishing canonical VRAM before the outer archive copies it.
    Reset();std::fill(vram.begin(),vram.end(),0);setMetricsOverlay({});
    auto snapshotDraw=quad({23,71,139,63},50000);snapshotDraw.depthWrite=true;
    ClearFramebuffer(c,0x80ff0000u);NativeCommand(QueueNativeScreen(snapshotDraw),c);
    prepareStateSave();const auto preparedReads=impl->stats.readbacks;
    ps2x::state::Writer targetState;saveState(targetState);const auto targetVram=vram;
    if(impl->stats.readbacks!=preparedReads)fail("fixture snapshot repeated its prepared target readback");
    ClearFramebuffer(c,0x800000ffu);NativeCommand(QueueNativeScreen(quad({0,255,0,255},60000)),c);
    std::copy(targetVram.begin(),targetVram.end(),vram.begin());
    const auto readsBeforeLoad=impl->stats.readbacks;
    {ps2x::state::Reader reader(targetState.data);loadState(reader);reader.finish();}
    if(vram!=targetVram || impl->stats.readbacks!=readsBeforeLoad)
        fail("fixture snapshot restore wrote back the abandoned GPU timeline");
    inspect("snapshot exact native target RGBA including odd alpha",{23,71,139,63});
    auto behindSnapshot=quad({255,0,0,255},40000);behindSnapshot.depthTest=true;
    NativeCommand(QueueNativeScreen(behindSnapshot),c);
    inspect("snapshot native depth blocks the next lower-depth draw",{23,71,139,63});
    behindSnapshot.vertices[0].position[2]=behindSnapshot.vertices[1].position[2]=
        behindSnapshot.vertices[2].position[2]=behindSnapshot.vertices[3].position[2]=60000;
    NativeCommand(QueueNativeScreen(behindSnapshot),c);
    inspect("snapshot native depth permits the next higher-depth draw",{255,0,0,255});
    const auto deferredToken=QueueNativeScreen(quad({0,255,0,255},0));
    unsigned rejectedPending=0;
    try{prepareStateSave();}catch(const ps2x::state::NotReady&){++rejectedPending;}
    try{ps2x::state::Writer writer;saveState(writer);}catch(const ps2x::state::NotReady&){++rejectedPending;}
    if(rejectedPending!=2)fail("fixture snapshot accepted an unconsumed native command");
    NativeCommand(deferredToken,c);
    auto incompatibleTargetState=targetState.data;incompatibleTargetState[0]^=1;bool rejectedBackend=false;
    try{ps2x::state::Reader reader(incompatibleTargetState);loadState(reader);}catch(const std::runtime_error&){rejectedBackend=true;}
    if(!rejectedBackend)fail("fixture snapshot accepted an incompatible backend tag");
    inspect("snapshot preflight rejection preserves subsequent rendering",{0,255,0,255});
    auto snapshotModel=viewportModel;snapshotModel.depthTest=snapshotModel.depthWrite=false;
    const auto uploadsBeforeRebuild=impl->stats.geometryUploads;
    NativeCommand(QueueNativeModel(snapshotModel),c);
    const auto rebuiltModelPixels=impl->download(impl->target(c).color);
    if(impl->stats.geometryUploads!=uploadsBeforeRebuild+1 ||
       rebuiltModelPixels[(16*64+32)*4]!=255 || rebuiltModelPixels[(16*64+32)*4+1]!=0 ||
       rebuiltModelPixels[(16*64+32)*4+2]!=255)
        fail("fixture snapshot did not rebuild and render native geometry");
    std::cout<<"[deko3d-self-test] snapshot exact target, depth, dirty timeline and pending-command rejection PASS\n";

    // This whole GPU upload has an intentionally stale prefix, whereas the
    // CPU decoder ends on a different physical page. Re-decoding after load
    // would expose green VRAM instead of preserving the red GPU prefix.
    Reset();auto staleUpload=cachedSprite;staleUpload.state.context=c;
    staleUpload.state.context.tex0.tbp0=0x2000;staleUpload.state.context.tex0.tbw=2;
    staleUpload.state.context.tex0.tfx=1;staleUpload.state.context.tex0.tcc=1;
    staleUpload.state.textureWidth=staleUpload.state.textureHeight=1;
    staleUpload.vertices[0].x=staleUpload.vertices[0].y=100;
    staleUpload.vertices[1].x=staleUpload.vertices[1].y=116;
    WriteVram(GS_PSM_CT32,0x2000,2,0,0,0x400000ff);TextureFlush();Submit(staleUpload);
    WriteVram(GS_PSM_CT32,0x2000,2,0,0,0x4000ff00);
    staleUpload.state.textureWidth=128;staleUpload.state.textureHeight=64;Submit(staleUpload);
    auto visibleStaleUpload=staleUpload;
    visibleStaleUpload.vertices[0].x=visibleStaleUpload.vertices[0].y=16;
    visibleStaleUpload.vertices[1].x=visibleStaleUpload.vertices[1].y=48;
    ClearFramebuffer(c,0x80ff0000u);Submit(visibleStaleUpload);
    inspect("snapshot stale prefix oracle before save: DECAL RGBA byte64",{255,0,0,128});
    prepareStateSave();ps2x::state::Writer textureState;saveState(textureState);const auto textureVram=vram;
    GSCpuBackend decoderReference;auto decoderVram=textureVram;decoderReference.Initialize(decoderVram.data(),decoderVram.size());
    {ps2x::state::Reader reader(textureState.data);uint32_t a,b,d;reader(a,b,d);decoderReference.loadState(reader);}
    std::vector<uint32_t> decodedAgain;decoderReference.DecodeTexture(staleUpload.state,128,64,decodedAgain);
    if(decodedAgain.empty() || decodedAgain[0]!=0x4000ff00)fail("fixture stale GPU upload did not differ from a new decode");
    TextureFlush();Submit(staleUpload);std::copy(textureVram.begin(),textureVram.end(),vram.begin());
    {ps2x::state::Reader reader(textureState.data);loadState(reader);reader.finish();}
    staleUpload.vertices[0].x=staleUpload.vertices[0].y=16;
    staleUpload.vertices[1].x=staleUpload.vertices[1].y=48;
    ClearFramebuffer(c,0x80ff0000u);Submit(staleUpload);
    inspect("snapshot preserves whole decoded GPU texture's stale prefix",{255,0,0,128});
    TextureFlush();Submit(staleUpload);inspect("snapshot TEXFLUSH exposes restored newer VRAM",{0,255,0,128});

    Reset();for(unsigned x=0;x<4;++x)WriteVram(GS_PSM_CT32,0x1000,1,x,0,0xaa000000);
    GSTransferCommand partial{};partial.direction=0;partial.bitbltbuf={0,1,GS_PSM_CT32,0x1000,1,GS_PSM_CT24};partial.trxreg={4,1};
    const uint8_t transferBytes[]={1,2,3,4,5,6,7,8,9,10,11,12};
    BeginTransfer(partial);UploadImage(transferBytes,6);prepareStateSave();
    ps2x::state::Writer partialState;saveState(partialState);const auto partialVram=vram;
    UploadImage(transferBytes+6,6);const auto finishedVram=vram;
    std::copy(partialVram.begin(),partialVram.end(),vram.begin());
    {ps2x::state::Reader reader(partialState.data);loadState(reader);reader.finish();}
    UploadImage(transferBytes+6,6);
    if(vram!=finishedVram || ReadVram(GS_PSM_CT32,0x1000,1,3,0)!=0xaa0c0b0a)
        fail("fixture snapshot partial CT24 upload did not preserve its cursor and alpha lane");
    partial.direction=1;partial.bitbltbuf={0x1000,1,GS_PSM_CT32,0,1,GS_PSM_CT32};BeginTransfer(partial);
    uint8_t prefix[3],expectedTail[16]{},actualTail[16]{};
    if(ConsumeLocalToHostBytes(prefix,3)!=3)fail("fixture snapshot local transfer prefix failed");
    ps2x::state::Writer localState;saveState(localState);const auto expectedCount=ConsumeLocalToHostBytes(expectedTail,16);
    {ps2x::state::Reader reader(localState.data);loadState(reader);reader.finish();}
    const auto actualCount=ConsumeLocalToHostBytes(actualTail,16);
    if(expectedCount!=13 || actualCount!=expectedCount || std::memcmp(expectedTail,actualTail,expectedCount))
        fail("fixture snapshot local readback cursor changed");
    std::cout<<"[deko3d-self-test] snapshot partial upload and consumed local transfer PASS\n";

    Reset();auto replayContext=boundedMain;ClearFramebuffer(replayContext,0x80ff0000u);
    auto replayQuad=quad({0,255,0,255},0);
    for(auto& vertex:replayQuad.vertices){vertex.position[0]*=10;vertex.position[1]*=7;}
    NativeCommand(QueueNativeScreen(replayQuad),replayContext);
    setReplayOverlay("REPLAY 3\nB PLAY / A CANCEL");PresentCompleted(resumeRequest);
    const auto overlayFrames=impl->stats.gameFrames,overlayReads=impl->stats.readbacks;
    if(!presentOverlay() || impl->stats.gameFrames!=overlayFrames || impl->stats.readbacks!=overlayReads)
        fail("fixture paused replay redraw counted game frames or read pixels");
    impl->presentGameImage(impl->completedSource()->color,640,448,&presented);
    presentationPixel(180,24,{255,255,255});presentationPixel(180,72,{255,255,255});
    presentationPixel(100,24,{0,0,0});presentationPixel(640,360,{0,255,0});
    uint32_t checkpointWidth=0,checkpointHeight=0;
    if(!copyCompletedFrameRgba(presented,checkpointWidth,checkpointHeight) || checkpointWidth!=1280 || checkpointHeight!=720)
        fail("fixture completed checkpoint capture is unavailable");
    presentationPixel(180,24,{0,0,255});presentationPixel(640,360,{0,255,0});
    const auto checkpointPixels=presented;
    prepareStateSave();ps2x::state::Writer completedState;saveState(completedState);const auto completedVram=vram;
    Reset();std::copy(completedVram.begin(),completedVram.end(),vram.begin());
    {ps2x::state::Reader reader(completedState.data);loadState(reader);reader.finish();}
    if(!presentOverlay() || !copyCompletedFrameRgba(presented,checkpointWidth,checkpointHeight) || presented!=checkpointPixels)
        fail("fixture restored completed target/picker composition changed");
    setReplayOverlay("");
    std::cout<<"[deko3d-self-test] paused replay picker redraw and overlay-free restored checkpoint PASS\n";
    impl->idle();Reset();
    // No guest memory is attached during standalone fixtures. The real runtime
    // calls Initialize with its own canonical VRAM before executing the ELF.
    impl->memory.Initialize(nullptr,0);
    std::cout<<"[deko3d-self-test] 30 GPU fixture groups PASS capture="<<capturePath<<'\n';
    return true;
}
