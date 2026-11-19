// Persistent GPU server: one detached process per compile owns the D3D12 device, pipeline and uploaded scenes, and serves every worker's requests.
// Control messages go over a named pipe; bulk data travels in anonymous shared mappings.
// @@
#pragma once

#ifdef VRAD3_STATIC_GPU
#include <atomic>
#include <fcntl.h>
#include <io.h>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "gpu_scene_identity.h"
namespace gpuServer {
using Handle=prefetch::Handle;
// Protocol id; both ends reject a message with a different value.
constexpr uint32_t protocol=0x16300005;
constexpr uint64_t maximumMapping=8ull<<30,maximumRegion=1ull<<30;
static DWORD timeoutSetting(DWORD fallback) {
    auto text=environmentValue(L"VRAD3_GPU_SERVER_TIMEOUT_S");
    if(text.empty() || text.find_first_not_of(L"0123456789")!=std::wstring::npos || text.size()>6) return fallback;
    return std::max<DWORD>(1,static_cast<DWORD>(std::stoul(text)))*1000;
}
static const DWORD ipcTimeout=timeoutSetting(120000);
static const DWORD workTimeout=timeoutSetting(600000);
// Request: worker asks for a turn; Grant: server accepts; Scene: worker sends the scene mapping name; SliceRequest/SliceReady: server asks for, worker supplies, the next slice; Done: result status; Shutdown: server exits.
enum Kind : uint32_t { Request=1, Grant=2, Scene=3, SliceRequest=4, SliceReady=5, Done=6, Shutdown=7 };
enum Status : uint32_t { Ok=0, Unavailable=1, Rejected=2, NeedScene=3 };
struct Message {
    uint32_t version=protocol,kind=0;
    uint32_t constants[24]{};
    uint64_t totalPaths=0;
    uint8_t hash[32]{},quick[32]{};
    // flags: 1 = scene not sent (resolve by quick token), 2 = quick token present.
    uint32_t flags=0;
    uint32_t adaptiveMin=0,blockWidth=0; float adaptiveTol=0;
    uint32_t clientPid=0,haveScene=0,status=0,serverPid=0,first=0,luxels=0;
    uint64_t budget=0,bytes=0,ioBytes=0;
    double queueMs=0;
    wchar_t mapping[96]{},io[96]{},slices[4][96]{};
    uint64_t sliceBytes[4]{},sliceCapacity[4]{};
    char error[512]{};
    GpuLightingTiming timing{};
    GpuLightingDiagnostics diagnostics{};
    GpuServerStats stats{};
};
static_assert(sizeof(Message)<8192,"Small control channel");
struct Region { uint64_t offset=0,bytes=0; };
// Shared-mapping layouts: the wire carries offsets and mapping names, never pointers. IoHeader holds output, CSR and extra regions; SceneHeader holds the 13 scene buffers.
struct IoHeader { uint32_t version=protocol; Region output,csr,extra[3]; uint32_t progress=0; };
struct SceneHeader { uint32_t version=protocol; Region buffers[13]; };
struct MappedSlice;
struct View {
    Handle handle; uint8_t* data=nullptr; uint64_t bytes=0;
    View()=default;
    View(const View&)=delete;
    View& operator=(const View&)=delete;
    ~View() { if(data) UnmapViewOfFile(data); }
    void open(const wchar_t* name,uint64_t size,bool create) {
        if(size<16 || size>maximumMapping) throw std::runtime_error("GPU server mapping size out of bounds");
        bytes=size;
        if(create) {
            requirePhysicalMemory(size,"GPU server shared mapping");
            handle=Handle(CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,
                static_cast<DWORD>(size>>32),static_cast<DWORD>(size),name));
            if(GetLastError()==ERROR_ALREADY_EXISTS) throw std::runtime_error("GPU server mapping name collision");
        } else handle=Handle(OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,name));
        if(!handle) throw std::runtime_error("Cannot open GPU server shared mapping (Win32 "+std::to_string(GetLastError())+")");
        data=static_cast<uint8_t*>(MapViewOfFile(handle.value,FILE_MAP_ALL_ACCESS,0,0,static_cast<SIZE_T>(size)));
        if(!data) throw std::runtime_error("Cannot map GPU server shared memory");
    }
};
static std::wstring pipeName(const std::wstring& key) {
    if(key.size()!=64 || key.find_first_not_of(L"0123456789abcdefABCDEF")!=std::wstring::npos)
        throw std::runtime_error("Invalid GPU server compile key");
    return L"\\\\.\\pipe\\vrad3-gpu-"+key;
}
static void nameMapping(wchar_t (&target)[96]) {
    uint8_t random[16];
    if(BCryptGenRandom(nullptr,random,sizeof(random),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)
        throw std::runtime_error("Cannot name GPU server mapping");
    std::wostringstream name; name<<L"Local\\vrad3-gpu-";
    for(auto byte:random) name<<std::hex<<std::setw(2)<<std::setfill(L'0')<<static_cast<unsigned>(byte);
    wcscpy_s(target,name.str().c_str());
}
// Exact-size message exchange over an overlapped byte pipe, with a deadline.
static void transfer(HANDLE pipe,Message& message,bool write,DWORD timeout) {
    Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));
    if(!event) throw std::runtime_error("Cannot create GPU server IPC event");
    auto* bytes=reinterpret_cast<uint8_t*>(&message); size_t done=0;
    const ULONGLONG deadline=GetTickCount64()+timeout;
    while(done<sizeof(message)) {
        OVERLAPPED operation{}; operation.hEvent=event.value; ResetEvent(event.value);
        DWORD count=0;
        BOOL complete=write?WriteFile(pipe,bytes+done,static_cast<DWORD>(sizeof(message)-done),&count,&operation)
                           :ReadFile(pipe,bytes+done,static_cast<DWORD>(sizeof(message)-done),&count,&operation);
        if(!complete) {
            DWORD error=GetLastError();
            if(error!=ERROR_IO_PENDING)
                throw std::runtime_error(std::string("GPU server pipe ")+(write?"write":"read")+" failed (Win32 "+std::to_string(error)+")");
            const ULONGLONG now=GetTickCount64();
            if(WaitForSingleObject(event.value,deadline>now?static_cast<DWORD>(deadline-now):0)!=WAIT_OBJECT_0) {
                CancelIoEx(pipe,&operation); GetOverlappedResult(pipe,&operation,&count,TRUE);
                throw std::runtime_error("GPU server IPC timeout");
            }
            if(!GetOverlappedResult(pipe,&operation,&count,FALSE))
                throw std::runtime_error(std::string("GPU server pipe ")+(write?"write":"read")+" failed (Win32 "+std::to_string(GetLastError())+")");
        }
        if(!count) throw std::runtime_error("GPU server pipe closed");
        done+=count;
    }
    if(message.version!=protocol) throw std::runtime_error("GPU server protocol mismatch");
}
// Regions are 16-byte aligned within a mapping.
static Region append(uint64_t& bytes,uint64_t n) {
    bytes=(bytes+15)&~15ull;
    if(n>maximumRegion || bytes>maximumMapping-n) throw std::runtime_error("GPU server buffer size out of bounds");
    Region region{bytes,n}; bytes+=n; return region;
}
static void* region(View& view,Region r) {
    if(r.offset<16 || r.offset>view.bytes || r.bytes>view.bytes-r.offset ||
       !r.bytes || r.bytes>maximumRegion || (r.offset&15))
        throw std::runtime_error("Invalid GPU server mapping region");
    return view.data+r.offset;
}
struct SceneOptions { bool hardware,lightCulling,compare; };
static SceneOptions sceneOptions(bool direct) {
    return {environmentValue(L"VRAD3_GPU_HARDWARE_TRAVERSAL")!=L"0",
            !direct,
            environmentValue(L"VRAD3_GPU_RESEARCH_COMPARE")==L"1"};
}
// compile-key.txt is replaced atomically by workers, so a read can land mid-replacement; retry briefly.
static std::wstring readCompileKey() {
    if(!fs::exists(compileTempRoot()/L"compile-key.txt")) return {};
    for(unsigned attempt=0;attempt<40;++attempt) {
        std::ifstream file(compileTempRoot()/L"compile-key.txt"); std::string key; file>>key;
        if(key.size()==64) return prefetch::wide(key);
        Sleep(25);
    }
    return {};
}
static fs::path failureFile() { return compileTempRoot()/L"gpu-server.failures"; }
static void recordFailure(const std::string& reason,unsigned weight=1) noexcept {
    try {
        std::ofstream file(failureFile(),std::ios::app);
        for(unsigned i=0;i<weight;++i) file<<launchMillis()<<'\t'<<logField(reason)<<'\n';
    } catch(...) {}
}
// Failures from the last 15 minutes are counted; two disable the server for this compile.
static unsigned recentFailures() noexcept {
    try {
        std::ifstream file(failureFile()); std::string line; unsigned count=0; const auto now=launchMillis();
        while(std::getline(file,line)) {
            uint64_t stamp=0; try { stamp=std::stoull(line); } catch(...) { continue; }
            if(now>=stamp && now-stamp<15ull*60*1000) ++count;
        }
        return count;
    } catch(...) { return 0; }
}

namespace host {
static Handle launcher;
static std::atomic<int> active{0};
static std::atomic<bool> stopping{false};
static std::atomic<ULONGLONG> activity{0};
static HANDLE logHandle=INVALID_HANDLE_VALUE; static std::mutex logLock;
static void say(const std::string& text) {
    if(logHandle==INVALID_HANDLE_VALUE) return;
    std::lock_guard<std::mutex> guard(logLock);
    const std::string line=std::to_string(GetTickCount64())+" "+text+"\n";
    DWORD written=0; WriteFile(logHandle,line.data(),static_cast<DWORD>(line.size()),&written,nullptr);
}
struct LogBuffer : std::streambuf {
    static void put(const char* text,std::streamsize count) {
        std::lock_guard<std::mutex> guard(logLock); DWORD written=0;
        WriteFile(logHandle,text,static_cast<DWORD>(count),&written,nullptr);
    }
    int_type overflow(int_type c) override { if(c!=traits_type::eof()) { char ch=static_cast<char>(c); put(&ch,1); } return traits_type::not_eof(c); }
    std::streamsize xsputn(const char* text,std::streamsize count) override { put(text,count); return count; }
};
struct WideLogBuffer : std::wstreambuf {
    static void put(const wchar_t* text,std::streamsize count) {
        std::string utf8(static_cast<size_t>(count)*3+4,' ');
        int bytes=WideCharToMultiByte(CP_UTF8,0,text,static_cast<int>(count),utf8.data(),static_cast<int>(utf8.size()),nullptr,nullptr);
        if(bytes>0) LogBuffer::put(utf8.data(),bytes);
    }
    int_type overflow(int_type c) override { if(c!=traits_type::eof()) { wchar_t ch=static_cast<wchar_t>(c); put(&ch,1); } return traits_type::not_eof(c); }
    std::streamsize xsputn(const wchar_t* text,std::streamsize count) override { put(text,count); return count; }
};
// Requests are served first come, first served; a request waits for its turn before touching the GPU.
struct Gate {
    std::mutex lock; std::condition_variable turn; uint64_t next=0,serving=0;
    uint64_t enter() {
        std::unique_lock<std::mutex> guard(lock); const uint64_t ticket=next++;
        turn.wait(guard,[&] { return serving==ticket; }); return ticket;
    }
    void leave() { std::lock_guard<std::mutex> guard(lock); ++serving; turn.notify_all(); }
};
static Gate gate;
static std::mutex quickLock;
static std::map<std::array<uint8_t,32>,std::array<uint8_t,32>> quickScenes;
struct Turn {
    bool held=false;
    void take() { gate.enter(); held=true; }
    ~Turn() { if(held) gate.leave(); }
};
struct ActiveScope {
    ActiveScope() { ++active; }
    ~ActiveScope() { activity=GetTickCount64(); --active; }
};
static void writeStatus() noexcept {
    try {
        GpuServerStats stats; GetGpuLightingStats(&stats);
        PROCESS_MEMORY_COUNTERS_EX counters{}; counters.cb=sizeof(counters);
        GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),sizeof(counters));
        std::ostringstream text;
        text<<"VRAD3_GPU_SERVER_1\n"<<GetCurrentProcessId()<<' '<<counters.PrivateUsage<<' '<<stats.vramCurrent<<' '
            <<stats.vramPeak<<' '<<stats.residentBytes<<' '<<GetTickCount64()<<'\n';
        prefetch::atomicText(compileTempRoot()/L"gpu-server.status",text.str());
    } catch(...) {}
}
struct SliceContext {
    HANDLE pipe; std::unique_ptr<View> scene,views[4]; wchar_t names[4][96]{};
    bool pending=false; uint32_t pendingFirst=0,pendingLuxels=0; uint64_t pendingBudget=0;
};
// Reads and drops the answer to an outstanding prefetch request.
static void drainPrefetch(SliceContext& context) {
    if(!context.pending) return;
    context.pending=false;
    Message ignored; transfer(context.pipe,ignored,false,workTimeout);
}
// Asks the worker for the next slice now, while the renderer traces the current one; the worker overwrites slice arrays the renderer has finished with.
static int prefetch(void* opaque,uint32_t first,uint32_t luxels,uint64_t budget) {
    try {
        auto& context=*static_cast<SliceContext*>(opaque);
        if(context.pending) return 1;
        Message message; message.kind=SliceRequest; message.first=first; message.luxels=luxels; message.budget=budget;
        transfer(context.pipe,message,true,ipcTimeout);
        context.pending=true; context.pendingFirst=first; context.pendingLuxels=luxels; context.pendingBudget=budget;
        return 0;
    } catch(...) { return 1; }
}
static int slice(void* opaque,uint32_t first,uint32_t luxels,uint64_t budget,
                 GpuLightingSlice* output,char* error,uint32_t errorBytes) {
    try {
        auto& context=*static_cast<SliceContext*>(opaque);
        context.scene.reset();
        Message message;
        if(context.pending && context.pendingFirst==first && context.pendingLuxels==luxels && context.pendingBudget==budget)
            context.pending=false;
        else {
            drainPrefetch(context);
            message.kind=SliceRequest; message.first=first; message.luxels=luxels; message.budget=budget;
            transfer(context.pipe,message,true,ipcTimeout);
        }
        message=Message{};
        transfer(context.pipe,message,false,workTimeout);
        if(message.kind!=SliceReady) throw std::runtime_error("Invalid GPU server slice reply");
        message.error[sizeof(message.error)-1]=0;
        if(message.error[0]) throw std::runtime_error(message.error);
        if(!message.luxels || message.luxels>luxels) throw std::runtime_error("Invalid GPU server slice range");
        output->luxels=message.luxels;
        for(size_t i=0;i<4;++i) {
            message.slices[i][95]=0;
            if(!message.sliceBytes[i] || message.sliceBytes[i]>message.sliceCapacity[i] || message.sliceCapacity[i]>maximumMapping)
                throw std::runtime_error("Invalid GPU server slice array");
            if(!context.views[i] || wcscmp(context.names[i],message.slices[i])) {
                context.views[i].reset(); context.views[i]=std::make_unique<View>();
                context.views[i]->open(message.slices[i],message.sliceCapacity[i],false);
                wcscpy_s(context.names[i],message.slices[i]);
            }
            output->buffers[i]={context.views[i]->data,message.sliceBytes[i]};
        }
        return 0;
    } catch(const std::exception& e) { strncpy_s(error,errorBytes,e.what(),_TRUNCATE); return 1; }
}
static void finish(HANDLE pipe,Message& done) {
    done.version=protocol; done.kind=Done; done.serverPid=GetCurrentProcessId();
    GetGpuLightingStats(&done.stats);
    transfer(pipe,done,true,ipcTimeout);
}
static void serveRequest(HANDLE pipe,Message& request) {
    const uint32_t count=request.constants[0],samples=request.constants[1];
    if(!count || count>kMaxBlockLuxels || !samples || samples>4096) throw std::runtime_error("GPU server request out of bounds");
    request.io[95]=0;
    struct IoView { std::wstring name; DWORD pid; std::shared_ptr<View> view; };
    static std::mutex ioLock; static std::vector<IoView> ioViews;
    auto alive=[](DWORD pid) {
        HANDLE process=OpenProcess(SYNCHRONIZE,FALSE,pid);
        if(!process) return false;
        const bool running=WaitForSingleObject(process,0)==WAIT_TIMEOUT; CloseHandle(process); return running;
    };
    std::shared_ptr<View> held;
    {
        std::lock_guard<std::mutex> guard(ioLock);
        for(auto it=ioViews.begin();it!=ioViews.end();++it)
            if(it->name==request.io) { if(it->view->bytes>=request.bytes) held=it->view; ioViews.erase(it); break; }
        ioViews.erase(std::remove_if(ioViews.begin(),ioViews.end(),[&](const IoView& v) { return v.pid==request.clientPid || !alive(v.pid); }),ioViews.end());
    }
    if(!held) { held=std::make_shared<View>(); held->open(request.io,request.bytes,false); }
    {
        std::lock_guard<std::mutex> guard(ioLock);
        ioViews.insert(ioViews.begin(),IoView{std::wstring(request.io),request.clientPid,held});
        if(ioViews.size()>2) ioViews.resize(2);
    }
    View& io=*held;
    if(request.bytes<sizeof(IoHeader)) throw std::runtime_error("Truncated GPU server request");
    IoHeader header; memcpy(&header,io.data,sizeof(header));
    const bool direct=(request.constants[3]&0x80000000u)!=0,probe=(request.constants[3]&0x40000000u)!=0,
               reject=(request.constants[3]&0x10000000u)!=0,inline_=probe || reject;
    if(header.version!=protocol || header.output.bytes!=uint64_t(count)*(probe?probeRecordBytes:reject?16u:204u) || header.csr.bytes!=(count+1ull)*4 ||
       (probe && (header.extra[0].bytes!=uint64_t(count)*12 || header.extra[1].bytes!=uint64_t(count)*12 || header.extra[2].bytes!=uint64_t(count)*36)) ||
       (reject && (header.extra[0].bytes!=std::max<uint64_t>(96,count*3ull)*4 || header.extra[1].bytes!=384 || header.extra[2].bytes!=uint64_t(count)*36)) ||
       (!inline_ && (header.extra[0].bytes || header.extra[1].bytes || header.extra[2].bytes)))
        throw std::runtime_error("Invalid GPU server request layout");
    void* output=region(io,header.output); void* csr=region(io,header.csr);
    auto quickKey=[&] { std::array<uint8_t,32> token; memcpy(token.data(),request.quick,32); return token; };
    if(request.flags&1) {
        bool known;
        { std::lock_guard<std::mutex> guard(quickLock); known=quickScenes.count(quickKey())!=0; }
        if(!known) {
            say("request scene not resident (token unknown)");
            Message done; done.status=NeedScene; finish(pipe,done);
            return;
        }
    }
    const ULONGLONG queued=GetTickCount64();
    Turn turn; turn.take();
    std::array<uint8_t,32> sceneHash; memcpy(sceneHash.data(),request.hash,32);
    if(request.flags&1) {
        bool known=false;
        { std::lock_guard<std::mutex> guard(quickLock);
          auto found=quickScenes.find(quickKey());
          if(found!=quickScenes.end() && GpuLightingHasScene(found->second.data())) { sceneHash=found->second; known=true; } }
        if(!known) {
            say("request scene not resident (evicted while queued)");
            Message done; done.status=NeedScene; finish(pipe,done);
            return;
        }
    }
    Message grant; grant.kind=Grant; grant.queueMs=static_cast<double>(GetTickCount64()-queued);
    grant.haveScene=GpuLightingHasScene(sceneHash.data())?1:0;
    if((request.flags&1) && !grant.haveScene) throw std::runtime_error("GPU server scene vanished");
    grant.serverPid=GetCurrentProcessId();
    transfer(pipe,grant,true,ipcTimeout);
    SliceContext context{pipe,{},{}};
    GpuLightingRequest gpu{}; gpu.version=6;
    memcpy(gpu.constants,request.constants,sizeof(gpu.constants));
    if(!grant.haveScene) {
        Message sceneMessage; transfer(pipe,sceneMessage,false,workTimeout);
        if(sceneMessage.kind!=Scene) throw std::runtime_error("Invalid GPU server scene message");
        sceneMessage.mapping[95]=0;
        context.scene=std::make_unique<View>(); context.scene->open(sceneMessage.mapping,sceneMessage.bytes,false);
        if(sceneMessage.bytes<sizeof(SceneHeader)) throw std::runtime_error("Truncated GPU server scene");
        SceneHeader scene; memcpy(&scene,context.scene->data,sizeof(scene));
        if(scene.version!=protocol) throw std::runtime_error("Invalid GPU server scene header");
        GpuSceneBuffer hashed[13]{};
        for(size_t i=0;i<13;++i) if(i<7 || i>10) {
            gpu.buffers[i]={region(*context.scene,scene.buffers[i]),scene.buffers[i].bytes};
            hashed[i]={gpu.buffers[i].data,gpu.buffers[i].bytes};
        }
        const auto options=sceneOptions(direct);
        auto actual=gpuSceneIdentity(hashed,request.constants,options.hardware,options.lightCulling,options.compare);
        if(memcmp(actual.data(),request.hash,32))
            throw std::runtime_error("GPU server scene identity mismatch (options or transfer); use in-worker GPU");
    }
    gpu.buffers[7]={csr,header.csr.bytes};
    if(inline_) for(size_t i=0;i<3;++i) gpu.buffers[8+i]={region(io,header.extra[i]),header.extra[i].bytes};
    gpu.output=output; gpu.outputBytes=header.output.bytes;
    gpu.shaderBytes=lightingShaderBytes(); gpu.shaderSize=lightingShaderSize();
    gpu.totalPaths=request.totalPaths;
    gpu.adaptiveMin=request.adaptiveMin; gpu.blockWidth=request.blockWidth; gpu.adaptiveTol=request.adaptiveTol;
    if(!inline_) { gpu.readSlice=slice; gpu.prefetchSlice=prefetch; gpu.sliceContext=&context; }
    gpu.persistentSceneHash=sceneHash.data();
    gpu.progress=reinterpret_cast<volatile uint32_t*>(io.data+offsetof(IoHeader,progress));
    Message done; done.kind=Done;
    int result=BakeGpuLighting(&gpu,&done.timing,done.error,sizeof(done.error));
    try { drainPrefetch(context); } catch(...) {}
    GetGpuLightingDiagnostics(&done.diagnostics);
    done.haveScene=grant.haveScene; done.queueMs=grant.queueMs;
    done.status=result==0?Ok:(GpuLightingLastDecision()?Rejected:Unavailable);
    if(result==0 && (request.flags&2)) {
        std::array<uint8_t,32> token; memcpy(token.data(),request.quick,32);
        std::lock_guard<std::mutex> guard(quickLock); quickScenes[token]=sceneHash;
    }
    say("request "+std::string(probe?"probes":reject?"sample-reject":direct?"directlight":"pathtrace")+" luxels="+std::to_string(count)+" spp="+std::to_string(samples)+
        " scene="+(grant.haveScene?"reused":"uploaded")+" queue_ms="+std::to_string(grant.queueMs)+
        " status="+std::to_string(done.status)+(result?(" error="+logField(done.error)):std::string()));
    context.scene.reset();
    writeStatus();
    finish(pipe,done);
}
static void connection(HANDLE raw) {
    Handle pipe(raw); ActiveScope scope;
    try {
        Message message; transfer(pipe.value,message,false,30000);
        if(message.kind==Shutdown) { say("shutdown requested"); stopping=true; }
        else if(message.kind==Request) {
            try { serveRequest(pipe.value,message); }
            catch(const std::exception& e) {
                say(std::string("request failed: ")+e.what());
                Message done; done.status=Unavailable; strncpy_s(done.error,e.what(),_TRUNCATE);
                try { finish(pipe.value,done); } catch(...) {}
            }
        }
    } catch(const std::exception& e) { say(std::string("connection dropped: ")+e.what()); }
    FlushFileBuffers(pipe.value); DisconnectNamedPipe(pipe.value);
}
static HANDLE createInstance(const std::wstring& name,bool first) {
    return CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|(first?FILE_FLAG_FIRST_PIPE_INSTANCE:0),
        PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,PIPE_UNLIMITED_INSTANCES,65536,65536,0,nullptr);
}
}

static int serve(const std::wstring& key,DWORD launcherPid) {
    try {
        SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
        auto name=pipeName(key);
        host::launcher=Handle(OpenProcess(SYNCHRONIZE,FALSE,launcherPid));
        if(!host::launcher) throw std::runtime_error("Cannot watch GPU server launcher");
        if(readCompileKey()!=key) return 0;
        if(wrapperLogging==2) {
            const auto path=(compileTempRoot()/L"gpu-server.log").wstring();
            HANDLE log=CreateFileW(path.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(log!=INVALID_HANDLE_VALUE) {
                host::logHandle=log;
                static auto* narrow=new host::LogBuffer; static auto* wide=new host::WideLogBuffer;
                std::cout.rdbuf(narrow); std::cerr.rdbuf(narrow); std::wcout.rdbuf(wide); std::wcerr.rdbuf(wide);
            }
        }
        Handle instance(host::createInstance(name,true));
        if(!instance) return 0;
        std::thread([handle=host::launcher.value] {
            WaitForSingleObject(handle,INFINITE); TerminateProcess(GetCurrentProcess(),0);
        }).detach();
        host::activity=GetTickCount64();
        host::say("start pid="+std::to_string(GetCurrentProcessId())+" launcher="+std::to_string(launcherPid));
        std::thread([] {
            host::ActiveScope scope; host::Turn turn; turn.take();
            char error[512]{};
            if(PrepareGpuSession(lightingShaderBytes(),lightingShaderSize(),error,sizeof(error))) {
                host::say(std::string("warm-up failed: ")+error);
                recordFailure(std::string("GPU server warm-up: ")+error,2);
                host::stopping=true;
            } else host::say("warm-up complete");
            host::writeStatus();
        }).detach();
        ULONGLONG idleSeconds=120;
        auto idleSetting=environmentValue(L"VRAD3_GPU_SERVER_IDLE_S");
        if(!idleSetting.empty() && idleSetting.find_first_not_of(L"0123456789")==std::wstring::npos) idleSeconds=std::max<ULONGLONG>(1,std::stoull(idleSetting));
        Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));
        if(!event) throw std::runtime_error("Cannot create GPU server connect event");
        ULONGLONG lastStatus=0;
        for(;;) {
            OVERLAPPED connect{}; connect.hEvent=event.value; ResetEvent(event.value);
            BOOL connected=ConnectNamedPipe(instance.value,&connect); DWORD error=connected?0:GetLastError();
            bool reason=false;
            if(!connected && error==ERROR_IO_PENDING) {
                while(WaitForSingleObject(event.value,250)!=WAIT_OBJECT_0) {
                    const ULONGLONG now=GetTickCount64();
                    if(now-lastStatus>=2000) { host::writeStatus(); lastStatus=now; }
                    if(host::stopping || (host::active==0 && now-host::activity>idleSeconds*1000)) {
                        host::say(host::stopping?"stop requested":"stop idle");
                        CancelIoEx(instance.value,&connect); reason=true; break;
                    }
                }
                DWORD ignored=0;
                if(reason) { GetOverlappedResult(instance.value,&connect,&ignored,TRUE); break; }
                if(!GetOverlappedResult(instance.value,&connect,&ignored,FALSE)) {
                    host::say("connect failed (Win32 "+std::to_string(GetLastError())+"); new pipe instance");
                    instance=Handle(host::createInstance(name,false)); if(!instance) break; continue;
                }
            } else if(!connected && error!=ERROR_PIPE_CONNECTED) break;
            host::activity=GetTickCount64();
            HANDLE accepted=instance.value; instance.value=INVALID_HANDLE_VALUE;
            std::thread([accepted] { host::connection(accepted); }).detach();
            instance=Handle(host::createInstance(name,false));
            if(!instance) break;
        }
        for(unsigned wait=0;host::active>0 && wait<200;++wait) Sleep(50);
        if(host::active>0) TerminateProcess(GetCurrentProcess(),0);
        ShutdownGpuLighting();
        host::say("exit");
        if(host::logHandle!=INVALID_HANDLE_VALUE) { HANDLE log=host::logHandle; host::logHandle=INVALID_HANDLE_VALUE; CloseHandle(log); }
        DeleteFileW((compileTempRoot()/L"gpu-server.status").c_str());
        return 0;
    } catch(const std::exception& e) {
        host::say(std::string("fatal: ")+e.what());
        recordFailure(std::string("GPU server fatal: ")+e.what(),2);
        ShutdownGpuLighting(); return 1;
    }
}

enum class Outcome { Served, Unavailable, Rejected, NeedScene };
// Slice arrays live in mappings reused from slice to slice: a fresh mapping costs a page fault per 4 KiB in each process, more than the transfer. An array that outgrows its mapping gets a larger one under a new name.
struct MappedSlice : SliceStorage {
    std::unique_ptr<View> views[4]; wchar_t names[4][96]{}; uint64_t sizes[4]{},capacity[4]{};
    uint8_t* allocate(size_t index,size_t bytes) override {
        if(!views[index] || capacity[index]<bytes) {
            views[index].reset();
            const uint64_t grown=std::max<uint64_t>(16,bytes+bytes/4+4096);
            nameMapping(names[index]); views[index]=std::make_unique<View>();
            views[index]->open(names[index],grown,true); capacity[index]=grown;
        }
        sizes[index]=bytes; return views[index]->data;
    }
};
static bool enabled=false,canLaunch=false;
static bool active() { return enabled; }
static std::wstring compileKey;
static DWORD launcherPid=0;
static std::string lastError,lastNote;
static Handle serverProcess;
static double lastQueueMs=0,lastServerMs=0;
static bool lastReused=false;
static DWORD serverPid=0;
static GpuServerStats serverStats;
static bool launchServer() {
    if(!canLaunch) return false;
    auto module=prefetch::modulePath();
    auto command=quoteArgument(module.wstring())+L" --gpu-server "+compileKey+L" "+std::to_wstring(launcherPid);
    DWORD flags=0; if(!prefetch::mayDetach(flags)) return false;
    const auto userLimit=environmentValue(L"VRAD3_GPU_USER_VRAM_MB"),workerLimit=environmentValue(L"VRAD3_GPU_VRAM_MB");
    if(!userLimit.empty()) SetEnvironmentVariableW(L"VRAD3_GPU_VRAM_MB",userLimit==L"none"?nullptr:userLimit.c_str());
    STARTUPINFOW startup{}; startup.cb=sizeof(startup); PROCESS_INFORMATION child{};
    BOOL created=CreateProcessW(module.c_str(),command.data(),nullptr,nullptr,FALSE,flags,nullptr,nullptr,&startup,&child);
    if(!userLimit.empty()) SetEnvironmentVariableW(L"VRAD3_GPU_VRAM_MB",workerLimit.empty()?nullptr:workerLimit.c_str());
    if(!created) { lastNote="launch error "+std::to_string(GetLastError()); return false; }
    CloseHandle(child.hThread); serverProcess=Handle(child.hProcess);
    return true;
}
static bool pipeExists() {
    auto name=pipeName(compileKey);
    if(WaitNamedPipeW(name.c_str(),1)) return true;
    return GetLastError()==ERROR_SEM_TIMEOUT;
}
static void configure() {
    enabled=false; lastNote="off:no compile key";
    compileKey=readCompileKey(); pipeName(compileKey);
    lastNote="off:earlier failures";
    if(recentFailures()>=2) return;
    auto launcher=environmentValue(L"VRAD3_GPU_LAUNCHER_PID");
    launcherPid=launcher.empty()?prefetch::parentPid():prefetch::number(launcher);
    DWORD flags=0; canLaunch=launcherPid && prefetch::mayDetach(flags);
    lastNote="on";
    if(!pipeExists()) {
        if(!canLaunch) { lastNote="off:no breakaway (job forbids a detached GPU server)"; return; }
        if(!launchServer()) { lastNote="off:cannot start: "+lastNote; return; }
    }
    enabled=true;
}
static Handle connectToServer() {
    auto name=pipeName(compileKey);
    const ULONGLONG deadline=GetTickCount64()+30000;
    unsigned launches=0;
    for(;;) {
        HANDLE pipe=CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
        if(pipe!=INVALID_HANDLE_VALUE) {
            DWORD mode=PIPE_READMODE_BYTE; SetNamedPipeHandleState(pipe,&mode,nullptr,nullptr);
            return Handle(pipe);
        }
        DWORD error=GetLastError();
        if(GetTickCount64()>deadline) throw std::runtime_error("GPU server missing or busy (Win32 "+std::to_string(error)+")");
        if(error==ERROR_PIPE_BUSY) { WaitNamedPipeW(name.c_str(),500); continue; }
        if(error!=ERROR_FILE_NOT_FOUND) throw std::runtime_error("Cannot open GPU server pipe (Win32 "+std::to_string(error)+")");
        const bool exited=serverProcess && WaitForSingleObject(serverProcess.value,0)==WAIT_OBJECT_0;
        if((!serverProcess || exited) && launches<2) {
            if(launchServer()) ++launches;
            else if(!prefetch::serverStatus().valid) throw std::runtime_error("GPU server unavailable and cannot be started: "+lastNote);
        }
        Sleep(20);
    }
}
static Outcome bake(const GpuLightingRequest& request,GpuLightingTiming& timing) {
    lastError.clear();
    if(!enabled) { lastError="GPU server off"; return Outcome::Unavailable; }
    try {
        const bool direct=(request.constants[3]&0x80000000u)!=0,probe=(request.constants[3]&0x40000000u)!=0,
                   inline_=probe || (request.constants[3]&0x10000000u)!=0;
        GpuSceneBuffer scene[13]{};
        for(size_t i=0;i<13;++i) scene[i]={request.buffers[i].data,request.buffers[i].bytes};
        const auto options=sceneOptions(direct);
        std::array<uint8_t,32> hash{};
        if(!request.sceneSkipped) hash=gpuSceneIdentity(scene,request.constants,options.hardware,options.lightCulling,options.compare);
        else if(!request.quickToken) throw std::runtime_error("GPU server request without scene needs a token");
        const ULONGLONG started=GetTickCount64();
        IoHeader header; uint64_t ioBytes=sizeof(IoHeader);
        header.output=append(ioBytes,request.outputBytes); header.csr=append(ioBytes,request.buffers[7].bytes);
        if(inline_) for(size_t i=0;i<3;++i) header.extra[i]=append(ioBytes,request.buffers[8+i].bytes);
        static std::unique_ptr<View> persistentIo; static wchar_t persistentName[96]{}; static uint64_t persistentBytes=0;
        Message message; message.kind=Request; message.bytes=ioBytes;
        if(!persistentIo || persistentBytes<ioBytes) {
            persistentIo.reset(); persistentBytes=0;
            const uint64_t capacity=std::min<uint64_t>(maximumMapping,std::max<uint64_t>(ioBytes,ioBytes/4*5));
            nameMapping(persistentName); persistentIo=std::make_unique<View>(); persistentIo->open(persistentName,capacity,true);
            persistentBytes=capacity;
        }
        wcscpy_s(message.io,persistentName);
        View& io=*persistentIo;
        struct DropUnlessServed { std::unique_ptr<View>& view; uint64_t& bytes; bool served=false;
            ~DropUnlessServed() { if(!served) { view.reset(); bytes=0; } } } guard{persistentIo,persistentBytes};
        memcpy(io.data,&header,sizeof(header));
        struct Watch { bool on; ~Watch() { if(on) progressWindow.watch(nullptr); } } watch{request.progress!=nullptr};
        if(watch.on) progressWindow.watch(reinterpret_cast<volatile uint32_t*>(io.data+offsetof(IoHeader,progress)));
        memcpy(region(io,header.csr),request.buffers[7].data,static_cast<size_t>(request.buffers[7].bytes));
        if(inline_) for(size_t i=0;i<3;++i) memcpy(region(io,header.extra[i]),request.buffers[8+i].data,static_cast<size_t>(request.buffers[8+i].bytes));
        memcpy(message.constants,request.constants,sizeof(message.constants));
        message.totalPaths=request.totalPaths; memcpy(message.hash,hash.data(),32); message.clientPid=GetCurrentProcessId();
        message.adaptiveMin=request.adaptiveMin; message.blockWidth=request.blockWidth; message.adaptiveTol=request.adaptiveTol;
        if(request.quickToken) { memcpy(message.quick,request.quickToken,32); message.flags|=2; }
        if(request.sceneSkipped) message.flags|=1;
        Handle pipe; Message reply;
        for(unsigned attempt=0;;++attempt) {
            try {
                pipe=connectToServer();
                transfer(pipe.value,message,true,ipcTimeout);
                transfer(pipe.value,reply,false,workTimeout);
                break;
            } catch(const std::exception& e) {
                if(attempt>=2 || std::string(e.what()).rfind("GPU server pipe ",0)!=0) throw;
                std::cout<<"GPU server connection lost before its grant, retrying: "<<e.what()<<std::endl;
                pipe.reset(); reply=Message{}; Sleep(50);
            }
        }
        std::unique_ptr<View> sceneView;
        static MappedSlice sliceArrays;
        for(bool first=true;;first=false) {
            if(!first) { reply=Message{}; transfer(pipe.value,reply,false,workTimeout); }
            if(reply.kind==Grant) {
                lastQueueMs=reply.queueMs; serverPid=reply.serverPid;
                if(reply.haveScene) { lastReused=true; continue; }
                if(request.sceneSkipped) throw std::runtime_error("GPU server asked for a scene the worker did not read");
                lastReused=false;
                SceneHeader sceneHeader; uint64_t bytes=sizeof(sceneHeader);
                for(size_t i=0;i<13;++i) if(i<7 || i>10) sceneHeader.buffers[i]=append(bytes,request.buffers[i].bytes);
                Message sceneMessage; sceneMessage.kind=Scene; nameMapping(sceneMessage.mapping); sceneMessage.bytes=bytes;
                sceneView=std::make_unique<View>(); sceneView->open(sceneMessage.mapping,bytes,true);
                memcpy(sceneView->data,&sceneHeader,sizeof(sceneHeader));
                for(size_t i=0;i<13;++i) if(i<7 || i>10)
                    memcpy(region(*sceneView,sceneHeader.buffers[i]),request.buffers[i].data,static_cast<size_t>(request.buffers[i].bytes));
                transfer(pipe.value,sceneMessage,true,ipcTimeout);
                continue;
            }
            sceneView.reset();
            if(reply.kind==SliceRequest) {
                GpuLightingSlice input{}; char error[512]{};
                auto* native=static_cast<NativeLighting*>(request.sliceContext);
                if(!native || !request.readSlice) throw std::runtime_error("GPU server needs a slice reader");
                native->storage=&sliceArrays;
                int result=request.readSlice(request.sliceContext,reply.first,reply.luxels,reply.budget,&input,error,sizeof(error));
                native->storage=nullptr;
                Message answer; answer.kind=SliceReady;
                if(result) strncpy_s(answer.error,error[0]?error:"GPU server slice unavailable",_TRUNCATE);
                else {
                    answer.luxels=input.luxels;
                    for(size_t i=0;i<4;++i) {
                        if(input.buffers[i].data!=sliceArrays.views[i]->data || input.buffers[i].bytes!=sliceArrays.sizes[i])
                            throw std::runtime_error("GPU server slice storage mismatch");
                        wcscpy_s(answer.slices[i],sliceArrays.names[i]); answer.sliceBytes[i]=sliceArrays.sizes[i];
                        answer.sliceCapacity[i]=sliceArrays.capacity[i];
                    }
                }
                transfer(pipe.value,answer,true,ipcTimeout);
                continue;
            }
            if(reply.kind==Done) break;
            throw std::runtime_error("Invalid GPU server message");
        }
        reply.error[sizeof(reply.error)-1]=0;
        if(reply.status==NeedScene) { lastError="scene not resident on the GPU server"; return Outcome::NeedScene; }
        if(reply.status==Rejected) { lastError=reply.error; return Outcome::Rejected; }
        if(reply.status!=Ok) {
            lastError=reply.error[0]?reply.error:"GPU server failed";
            recordFailure(lastError);
            return Outcome::Unavailable;
        }
        memcpy(request.output,region(io,header.output),static_cast<size_t>(request.outputBytes));
        timing=reply.timing;
        timing.peakResourceBytes=0; timing.peakLocalUsageBytes=0;
        hammerHardware=reply.diagnostics; serverPid=reply.serverPid; serverStats=reply.stats;
        lastServerMs=static_cast<double>(GetTickCount64()-started);
        guard.served=true;
        return Outcome::Served;
    } catch(const std::exception& e) {
        lastError=e.what(); recordFailure(lastError);
        return Outcome::Unavailable;
    }
}
static DWORD shutdownPid=0;
static void requestShutdown() noexcept {
    try {
        shutdownPid=0;
        compileKey=readCompileKey(); pipeName(compileKey);
        if(!pipeExists()) return;
        const auto status=prefetch::serverStatus();
        Handle pipe(CreateFileW(pipeName(compileKey).c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));
        if(!pipe) return;
        Message message; message.kind=Shutdown; transfer(pipe.value,message,true,10000);
        pipe.reset();
        if(status.valid) shutdownPid=status.pid;
    } catch(...) {}
}
static void awaitShutdown() noexcept {
    if(!shutdownPid) return;
    Handle process(OpenProcess(SYNCHRONIZE,FALSE,shutdownPid));
    const ULONGLONG deadline=GetTickCount64()+15000;
    while(GetTickCount64()<deadline) {
        if(!process || WaitForSingleObject(process.value,0)==WAIT_OBJECT_0) break;
        if(!fs::exists(compileTempRoot()/L"gpu-server.status")) break;
        Sleep(10);
    }
    shutdownPid=0;
}
static void shutdownAndWait() noexcept { requestShutdown(); awaitShutdown(); }
static unsigned servedRequests=0,fallbackRequests=0,rejectedRequests=0;
static std::string lastFailure;
static std::string summary() {
    std::string text=lastNote;
    if(enabled) text+=" served="+std::to_string(servedRequests)+" in_worker="+std::to_string(fallbackRequests)+
                      " cpu_decisions="+std::to_string(rejectedRequests);
    if(!lastFailure.empty()) text+=" last_failure="+lastFailure;
    return text;
}
static int bakeForWorker(const GpuLightingRequest& request,GpuLightingTiming& timing,std::string& reason) {
    reason.clear();
    if(!enabled) return 0;
    switch(bake(request,timing)) {
    case Outcome::Served:
        ++servedRequests;
        std::cout<<"GPU server served: pid="<<serverPid<<" scene="<<(lastReused?"reused":"uploaded")<<" queue_ms="<<lastQueueMs
                 <<" request_ms="<<lastServerMs<<" server_vram_bytes="<<serverStats.vramCurrent<<std::endl;
        return 1;
    case Outcome::Rejected: ++rejectedRequests; reason=lastError; return 2;
    case Outcome::NeedScene: reason=lastError; return 3;
    default: ++fallbackRequests; reason=lastFailure=lastError; return 0;
    }
}
static std::array<uint8_t,32> lightmapToken(const NativeLighting& inputs,uint32_t lights) {
    return sceneToken(inputs,compileKey,lights?"direct-v1":"path-v1");
}
static int bakeResident(uint32_t spp,uint32_t lights,NativeLighting& inputs,std::vector<uint8_t>& records,
                        GpuLightingTiming& timing,std::string& reason) {
    reason.clear();
    if(!enabled || compileKey.empty()) return 0;
    { BlockProfileScope profile(lights?"prepare_direct_small":"prepare_native_small");
      inputs=prepareNative(spp,lights,0,true); }
    const auto token=lightmapToken(inputs,lights);
    requirePhysicalMemory(static_cast<uint64_t>(inputs.constants[0])*204,"working records");
    records.resize(static_cast<size_t>(inputs.constants[0])*204);
    GpuLightingRequest request{}; request.version=5;
    memcpy(request.constants,inputs.constants,sizeof(inputs.constants));
    for(size_t i=0;i<inputs.buffers.size();++i) request.buffers[i]={inputs.buffers[i].data(),inputs.buffers[i].size()};
    request.output=records.data(); request.outputBytes=records.size();
    request.readSlice=readNativeSlice; request.sliceContext=&inputs; request.totalPaths=inputs.totalPaths;
    if(!lights) { request.adaptiveMin=inputs.adaptiveMin; request.blockWidth=inputs.width; request.adaptiveTol=inputs.adaptiveTol; }
    request.shaderBytes=lightingShaderBytes(); request.shaderSize=lightingShaderSize();
    request.quickToken=token.data(); request.sceneSkipped=true; request.progress=progressWindow.progress();
    const int outcome=bakeForWorker(request,timing,reason);
    if(outcome!=1) { records.clear(); inputs=NativeLighting{}; }
    return outcome;
}
}
#endif
