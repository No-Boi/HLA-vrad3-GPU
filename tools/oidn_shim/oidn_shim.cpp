// OpenImageDenoise.dll shim: lets Valve's OIDN 1.x calls run on OIDN 2 GPU devices by staging images in device buffers.
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

typedef void* Device; typedef void* Filter; typedef void* Buffer;
typedef bool (*Progress)(void*,double);
// OIDN image format values (same in OIDN 1 and 2). OIDN is Intel's, Apache-2.0, and is not shipped here.
enum { FORMAT_FLOAT=1, FORMAT_FLOAT2, FORMAT_FLOAT3, FORMAT_FLOAT4, FORMAT_HALF=257, FORMAT_HALF2, FORMAT_HALF3, FORMAT_HALF4 };

namespace real {
    // Entry points of the real OIDN 2 DLL (OpenImageDenoise_real.dll), forwarding targets for the 1.x calls Valve's vrad3 makes.
    HMODULE module=nullptr;
    Device (*newDevice)(int); void (*commitDevice)(Device); void (*releaseDevice)(Device);
    int (*getDeviceError)(Device,const char**); int (*getDeviceInt)(Device,const char*);
    Filter (*newFilter)(Device,const char*); void (*commitFilter)(Filter); void (*executeFilter)(Filter); void (*releaseFilter)(Filter);
    void (*setSharedFilterImage)(Filter,const char*,void*,int,size_t,size_t,size_t,size_t,size_t);
    void (*setFilterImage)(Filter,const char*,Buffer,int,size_t,size_t,size_t,size_t,size_t);
    void (*setProgress)(Filter,Progress,void*);
    Buffer (*newBuffer)(Device,size_t); void (*releaseBuffer)(Buffer);
    void (*writeBuffer)(Buffer,size_t,size_t,const void*); void (*readBuffer)(Buffer,size_t,size_t,void*);
    template<class T> bool get(T& f,const char* name) { f=reinterpret_cast<T>(GetProcAddress(module,name)); return f!=nullptr; }
    bool load() {
        static std::once_flag once; static bool ok=false;
        std::call_once(once,[] {
            HMODULE self=nullptr; wchar_t path[32768]{};
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&load),&self);
            DWORD n=GetModuleFileNameW(self,path,32768); std::wstring file(path,n);
            file=file.substr(0,file.find_last_of(L"\\/")+1)+L"OpenImageDenoise_real.dll";
            module=LoadLibraryExW(file.c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
            ok=module && get(newDevice,"oidnNewDevice") && get(commitDevice,"oidnCommitDevice") && get(releaseDevice,"oidnReleaseDevice")
               && get(getDeviceError,"oidnGetDeviceError") && get(getDeviceInt,"oidnGetDeviceInt") && get(newFilter,"oidnNewFilter")
               && get(commitFilter,"oidnCommitFilter") && get(executeFilter,"oidnExecuteFilter") && get(releaseFilter,"oidnReleaseFilter")
               && get(setSharedFilterImage,"oidnSetSharedFilterImage") && get(setFilterImage,"oidnSetFilterImage")
               && get(setProgress,"oidnSetFilterProgressMonitorFunction") && get(newBuffer,"oidnNewBuffer") && get(releaseBuffer,"oidnReleaseBuffer")
               && get(writeBuffer,"oidnWriteBuffer") && get(readBuffer,"oidnReadBuffer");
        });
        return ok;
    }
}

struct Image { void* host=nullptr; int format=0; size_t width=0,height=0,offset=0,pixel=0,row=0,bytes=0; Buffer buffer=nullptr; };
struct FilterState { Device device=nullptr; std::string type; bool hostMemory=true; std::map<std::string,Image> images; Progress progress=nullptr; void* user=nullptr; };
static std::mutex lock;
static std::map<Filter,FilterState> filters;
static size_t formatBytes(int format) {
    switch(format) { case FORMAT_FLOAT: return 4; case FORMAT_FLOAT2: return 8; case FORMAT_FLOAT3: return 12; case FORMAT_FLOAT4: return 16;
                     case FORMAT_HALF: return 2; case FORMAT_HALF2: return 4; case FORMAT_HALF3: return 6; case FORMAT_HALF4: return 8; }
    return 0;
}
static void releaseBuffers(FilterState& state) {
    for(auto& item:state.images) if(item.second.buffer) { real::releaseBuffer(item.second.buffer); item.second.buffer=nullptr; }
}
// Device type 1 = OIDN 2's CPU device (oidnNewDevice).
static bool cpuFallback(const FilterState& state) {
    Device cpu=real::newDevice(1); if(!cpu) return false;
    real::commitDevice(cpu);
    Filter filter=real::newFilter(cpu,state.type.c_str());
    for(const auto& item:state.images) { const Image& image=item.second;
        real::setSharedFilterImage(filter,item.first.c_str(),image.host,image.format,image.width,image.height,image.offset,image.pixel,image.row); }
    if(state.progress) real::setProgress(filter,state.progress,state.user);
    real::commitFilter(filter); real::executeFilter(filter);
    const char* message=nullptr; const bool ok=real::getDeviceError(cpu,&message)==0;
    real::releaseFilter(filter); real::releaseDevice(cpu);
    return ok;
}

extern "C" {
__declspec(dllexport) Device oidnNewDevice(int type) { return real::load()?real::newDevice(type):nullptr; }
__declspec(dllexport) void oidnCommitDevice(Device device) { if(device) real::commitDevice(device); }
__declspec(dllexport) void oidnReleaseDevice(Device device) { if(device) real::releaseDevice(device); }
__declspec(dllexport) int oidnGetDeviceError(Device device,const char** message) {
    if(!real::load()) { if(message) *message="OpenImageDenoise_real.dll (OIDN 2) not found beside the shim"; return 1; }
    return real::getDeviceError(device,message);
}
__declspec(dllexport) Filter oidnNewFilter(Device device,const char* type) {
    Filter filter=real::newFilter(device,type);
    if(filter) { std::lock_guard<std::mutex> guard(lock); FilterState& state=filters[filter]; state.device=device; state.type=type?type:"";
                 // OIDN 2 device property: 0 on GPU devices that cannot read host memory, so shared images need device buffers.
                 state.hostMemory=real::getDeviceInt(device,"systemMemorySupported")!=0; }
    return filter;
}
__declspec(dllexport) void oidnSetSharedFilterImage(Filter filter,const char* name,void* host,int format,size_t width,size_t height,
                                                    size_t offset,size_t pixel,size_t row) {
    std::lock_guard<std::mutex> guard(lock);
    auto found=filters.find(filter);
    if(found==filters.end() || found->second.hostMemory) { real::setSharedFilterImage(filter,name,host,format,width,height,offset,pixel,row); }
    if(found==filters.end()) return;
    Image& image=found->second.images[name];
    if(image.buffer) { real::releaseBuffer(image.buffer); image.buffer=nullptr; }
    image.host=host; image.format=format; image.width=width; image.height=height; image.offset=offset;
    image.pixel=pixel?pixel:formatBytes(format); image.row=row?row:image.pixel*width;
    image.bytes=height && width ? image.row*(height-1)+image.pixel*width : 0;
}
__declspec(dllexport) void oidnSetFilterProgressMonitorFunction(Filter filter,Progress progress,void* user) {
    real::setProgress(filter,progress,user);
    std::lock_guard<std::mutex> guard(lock);
    auto found=filters.find(filter); if(found!=filters.end()) { found->second.progress=progress; found->second.user=user; }
}
__declspec(dllexport) void oidnCommitFilter(Filter filter) {
    {
        std::lock_guard<std::mutex> guard(lock);
        auto found=filters.find(filter);
        if(found!=filters.end() && !found->second.hostMemory) {
            FilterState& state=found->second;
            for(auto& item:state.images) {
                Image& image=item.second;
                // OIDN 2 oidnSetFilterImage binds a device buffer; OIDN 1's oidnSetSharedFilterImage wrapped host memory directly.
                if(!image.host) { real::setFilterImage(filter,item.first.c_str(),nullptr,image.format,0,0,0,0,0); continue; }
                if(!image.buffer) image.buffer=real::newBuffer(state.device,image.bytes);
                real::setFilterImage(filter,item.first.c_str(),image.buffer,image.format,image.width,image.height,0,image.pixel,image.row);
            }
        }
    }
    real::commitFilter(filter);
}
__declspec(dllexport) void oidnExecuteFilter(Filter filter) {
    FilterState state;
    { std::lock_guard<std::mutex> guard(lock); auto found=filters.find(filter); if(found!=filters.end()) state=found->second; }
    if(state.hostMemory) { real::executeFilter(filter); return; }
    const char* message=nullptr;
    real::getDeviceError(state.device,&message);
    for(const auto& item:state.images)
        if(item.first!="output" && item.second.buffer && item.second.host)
            real::writeBuffer(item.second.buffer,0,item.second.bytes,static_cast<const uint8_t*>(item.second.host)+item.second.offset);
    real::executeFilter(filter);
    auto output=state.images.find("output");
    if(output!=state.images.end() && output->second.buffer && output->second.host)
        real::readBuffer(output->second.buffer,0,output->second.bytes,static_cast<uint8_t*>(output->second.host)+output->second.offset);
    if(real::getDeviceError(state.device,&message)!=0) {
        if(cpuFallback(state)) real::getDeviceError(state.device,&message);
    }
}
__declspec(dllexport) void oidnReleaseFilter(Filter filter) {
    {
        std::lock_guard<std::mutex> guard(lock);
        auto found=filters.find(filter);
        if(found!=filters.end()) { releaseBuffers(found->second); filters.erase(found); }
    }
    real::releaseFilter(filter);
}
}
