// Offline DX12 ray-tracing lighting stage: bakes pathtrace, directlight, light-probe and sample-reject work and returns vrad3 working records.
// Also holds the GPU session and the scenes retained for the persistent server.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cmath>
#include <stdexcept>
#include "gpu_lighting_api.h"
#include "block_profile.h"
#include "hardware_limits.h"
#include "responsiveness.h"
#pragma comment(lib,"d3d12.lib")
#pragma comment(lib,"dxgi.lib")
using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;
#ifdef VRAD3_STATIC_GPU
#define GPU_EXPORT
#else
#define GPU_EXPORT __declspec(dllexport)
#endif
static GpuLightingDiagnostics gpuDiagnostics;
extern "C" GPU_EXPORT void GetGpuLightingDiagnostics(GpuLightingDiagnostics* value) { *value=gpuDiagnostics; }
// A scene retained by a persistent session: everything the dispatch loop needs from the one-time upload and acceleration-structure build, plus its admission numbers.
struct ResidentScene {
    std::array<uint8_t,32> hash{};
    std::vector<ComPtr<ID3D12Resource>> inputs,blases,indexBuffers;
    ComPtr<ID3D12Resource> tlas,traversalBuffer;
    uint64_t sceneRequired=0,pinnedBytes=0,lastUse=0;
    uint32_t boundsOffset=0,partitions=0;
    bool hardware=false,lightCulling=false,compare=false,translucent=true;
};
struct GpuSession {
    ComPtr<ID3D12Device5> device;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    std::vector<uint8_t> shader;
    ComPtr<IDXGIAdapter3> adapter;
    std::wstring adapterName;
    bool uma=false;

    std::vector<ResidentScene> scenes;
    uint64_t useClock=0;

    ComPtr<ID3D12CommandQueue> queue;
    std::array<ComPtr<ID3D12CommandAllocator>,2> allocators;
    std::array<ComPtr<ID3D12GraphicsCommandList4>,2> lists;
    ComPtr<ID3D12Fence> fence; uint64_t fenceValue=0;
    ComPtr<ID3D12QueryHeap> timestamps; ComPtr<ID3D12Resource> timestampData;
};
// One pathtrace and one directlight scene variant.
constexpr size_t kMaxResidentScenes=2;
// Failures marked as decisions (speed guard, dispatch cap) are not retried in another GPU session, since a retry would decide the same.
struct GpuFailure : std::runtime_error {
    bool decision,keepSession;
    GpuFailure(const std::string& text,bool isDecision,bool keep):std::runtime_error(text),decision(isDecision),keepSession(keep) {}
};
static bool gpuLastDecision=false;
static GpuServerStats gpuStats;
static GpuSession& gpuSession() {
    static auto* session=new GpuSession;
    return *session;
}
static uint64_t residentBytes() {
    uint64_t total=0; for(const auto& scene:gpuSession().scenes) total+=scene.pinnedBytes; return total;
}
extern "C" GPU_EXPORT void ShutdownGpuLighting() {
    auto& session=gpuSession();
    session.scenes.clear();
    session.lists={}; session.allocators={}; session.queue.Reset(); session.fence.Reset(); session.fenceValue=0;
    session.timestamps.Reset(); session.timestampData.Reset();
    session.pipeline.Reset(); session.root.Reset(); session.device.Reset(); session.shader.clear();
    session.adapter.Reset(); session.adapterName.clear();
    gpuStats.residentBytes=0; gpuStats.scenes=0;
}
extern "C" GPU_EXPORT int GpuLightingHasScene(const uint8_t* hash) {
    for(const auto& scene:gpuSession().scenes) if(!memcmp(scene.hash.data(),hash,32)) return 1;
    return 0;
}
extern "C" GPU_EXPORT int GpuLightingLastDecision() { return gpuLastDecision?1:0; }
extern "C" GPU_EXPORT void GetGpuLightingStats(GpuServerStats* value) { *value=gpuStats; }
static std::wstring gpuOverride(const wchar_t* name) {
    wchar_t value[512]{};
    DWORD n=GetEnvironmentVariableW(name,value,512);
    if(n>=512) throw std::runtime_error("GPU override too long");
    return n?value:L"";
}
static uint64_t gpuLimit(const wchar_t* name,uint64_t defaultValue,uint64_t maximum) {
    auto value=gpuOverride(name);
    if(value.empty()) return defaultValue;
    if(value.find_first_not_of(L"0123456789")!=std::wstring::npos)
        throw std::runtime_error("GPU limit must be a positive decimal integer");
    auto limit=std::stoull(value);
    if(!limit || limit>maximum) throw std::runtime_error("GPU limit out of range");
    return limit;
}
static void check(HRESULT hr,const char* operation) {
    if(FAILED(hr)) throw std::runtime_error(std::string(operation)+" HRESULT="+std::to_string(static_cast<unsigned>(hr)));
}
static std::vector<uint8_t> load(const fs::path& path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file) throw std::runtime_error("Cannot open input file: "+path.string());
    auto length=file.tellg(); if(length<=0 || length>512ull*1024*1024) throw std::runtime_error("Input size out of bounds");
    std::vector<uint8_t> bytes(static_cast<size_t>(length)); file.seekg(0); file.read(reinterpret_cast<char*>(bytes.data()),bytes.size());
    if(!file) throw std::runtime_error("Input read failed"); return bytes;
}
struct GpuInputView {
    const uint8_t* pointer; size_t length;
    const uint8_t* data() const { return pointer; }
    size_t size() const { return length; }
};
#include "gpu_trace_scene.h"
static void ensureDevice() {
        auto& device=gpuSession().device;
        if(!device) {
        ComPtr<IDXGIFactory6> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Create factory");
        auto selected=gpuOverride(L"VRAD3_GPU_ADAPTER");
        bool byIndex=!selected.empty() && selected.find_first_not_of(L"0123456789")==std::wstring::npos;
        uint64_t selectedIndex=byIndex?std::stoull(selected):0;
        auto lower=[](std::wstring text) { std::transform(text.begin(),text.end(),text.begin(),towlower); return text; };
        bool matched=false;
        for(UINT index=0;;++index) {
            ComPtr<IDXGIAdapter1> adapter; HRESULT hr=factory->EnumAdapterByGpuPreference(index,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter));
            if(hr==DXGI_ERROR_NOT_FOUND) break; check(hr,"Enumerate adapters");
            DXGI_ADAPTER_DESC1 desc{}; adapter->GetDesc1(&desc); if(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            std::wcout << L"GPU adapter candidate: index=" << index << L" name=" << desc.Description << L"\n";
            if(!selected.empty() && !(byIndex?index==selectedIndex:lower(desc.Description).find(lower(selected))!=std::wstring::npos)) continue;
            matched=true;
            ComPtr<ID3D12Device5> candidate;
            if(FAILED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&candidate)))) {
                if(!selected.empty()) throw std::runtime_error("Selected GPU cannot create a D3D12 feature-level 12 device");
                continue;
            }
            D3D12_FEATURE_DATA_ARCHITECTURE1 architecture{};
            check(candidate->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE1,&architecture,sizeof(architecture)),"Query GPU architecture");
            std::wcout << L"GPU adapter architecture: name=" << desc.Description << L" UMA=" << architecture.UMA
                       << L" forced=" << !selected.empty() << L"\n";
            if(selected.empty() && architecture.UMA) {
                std::wcout << L"Skipping integrated/UMA adapter for automatic selection\n";
                continue;
            }
            D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
            if(SUCCEEDED(candidate->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,&options,sizeof(options))) && options.RaytracingTier>=D3D12_RAYTRACING_TIER_1_1) {
                check(adapter.As(&gpuSession().adapter),"Query adapter memory interface");
                device=candidate; gpuSession().adapterName=desc.Description; gpuSession().uma=architecture.UMA!=FALSE;
                wcscpy_s(gpuDiagnostics.adapter,desc.Description);
                gpuDiagnostics.adapterLuidLow=desc.AdapterLuid.LowPart;
                gpuDiagnostics.adapterLuidHigh=desc.AdapterLuid.HighPart;
                LARGE_INTEGER driver{};
                if(SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice),&driver)))
                    gpuDiagnostics.driver=static_cast<uint64_t>(driver.QuadPart);
                break;
            }
            if(!selected.empty()) throw std::runtime_error("Selected GPU does not support DXR Tier 1.1");
        }
        if(!device) throw std::runtime_error(!selected.empty() && !matched?"Selected GPU adapter not found":"DXR Tier 1.1 GPU required");
        std::wcout << L"GPU lighting adapter: " << gpuSession().adapterName << L"\n";
        }
}
static void ensurePipeline(const std::vector<uint8_t>& shader) {
        auto& device=gpuSession().device;
        auto& root=gpuSession().root;
        auto& pipeline=gpuSession().pipeline;
        auto& cachedShader=gpuSession().shader;
        if(pipeline && cachedShader!=shader) throw std::runtime_error("Shader changed within a GPU session");
        if(!pipeline) {
        // Root layout: 0 TLAS (t0), 1-13 scene/slice buffers (t1-t13), 14 output UAV (u0), 15 thirty 32-bit constants, 16 counter UAV (u1), 17 traversal metadata (t14).
        D3D12_ROOT_PARAMETER parameters[18]{};
        for(UINT i=0;i<14;++i) { parameters[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV; parameters[i].Descriptor.ShaderRegister=i; }
        parameters[14].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV; parameters[14].Descriptor.ShaderRegister=0;
        parameters[15].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; parameters[15].Constants.Num32BitValues=30;
        parameters[16].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV; parameters[16].Descriptor.ShaderRegister=1;
        parameters[17].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV; parameters[17].Descriptor.ShaderRegister=14;
        D3D12_ROOT_SIGNATURE_DESC rootDesc{}; rootDesc.NumParameters=18; rootDesc.pParameters=parameters;
        ComPtr<ID3DBlob> rootBlob,error; check(D3D12SerializeRootSignature(&rootDesc,D3D_ROOT_SIGNATURE_VERSION_1,&rootBlob,&error),"Serialize root signature");
        check(device->CreateRootSignature(0,rootBlob->GetBufferPointer(),rootBlob->GetBufferSize(),IID_PPV_ARGS(&root)),"Create root signature");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc{}; pipelineDesc.pRootSignature=root.Get(); pipelineDesc.CS={shader.data(),shader.size()};
        check(device->CreateComputePipelineState(&pipelineDesc,IID_PPV_ARGS(&pipeline)),"Create pipeline");
        cachedShader=shader;
        }
}
// Bounds-checks Valve's emitter tree (magic 0x45525456) before a shader walks it.
static void validateEmitterTree(const GpuInputView& bytes,uint32_t base,uint32_t gpuLights,uint64_t minimumBase) {
    auto word=[&](uint64_t at) { if(at+4>bytes.size()) throw std::runtime_error("Invalid GPU emitter tree"); uint32_t v; memcpy(&v,bytes.data()+at,4); return v; };
    if(base<minimumBase || base%16 || word(base)!=0x45525456u) throw std::runtime_error("Invalid GPU emitter tree header");
    const uint32_t nodes=word(base+4),records=word(base+8),cells=word(base+12),valveLights=word(base+16),mapped=word(base+20);
    const uint64_t nodesAt=word(base+24),recordsAt=word(base+28),cellsAt=word(base+32),mapAt=word(base+36),reverseAt=word(base+40);
    if(nodes>(1u<<20) || !records || records>(1u<<20) || valveLights>65536 || cells!=uint64_t(valveLights)*256 || mapped!=gpuLights ||
       nodesAt!=base+48 || recordsAt!=nodesAt+nodes*240ull || cellsAt!=recordsAt+records*224ull || mapAt!=cellsAt+cells*4ull ||
       reverseAt!=mapAt+valveLights*4ull || reverseAt+mapped*4ull!=bytes.size())
        throw std::runtime_error("Invalid GPU emitter tree layout");
    for(uint64_t at=nodesAt;at<cellsAt;at+=4) {
        const bool node=at<recordsAt; const uint64_t local=node?(at-nodesAt)%240:(at-recordsAt)%224;
        if(node?local<192:local>=32) { float v; memcpy(&v,bytes.data()+at,4); if(!std::isfinite(v)) throw std::runtime_error("Nonfinite GPU emitter tree"); }
    }
    for(uint32_t n=0;n<nodes;++n) for(uint32_t lane=0;lane<4;++lane) {
        const int32_t child=int32_t(word(nodesAt+n*240ull+192+lane*4)),begin=int32_t(word(nodesAt+n*240ull+208+lane*4)),end=int32_t(word(nodesAt+n*240ull+224+lane*4));
        const bool inner=child>=0 && uint32_t(child)<nodes;
        if(inner ? uint32_t(child)<=n : (begin>=0 && (end<begin || uint32_t(end)>records)))
            throw std::runtime_error("Invalid GPU emitter tree node");
    }
    for(uint32_t r=0;r<records;++r) for(uint32_t lane=0;lane<4;++lane) {
        const uint32_t light=word(recordsAt+r*224ull+lane*4),rect=word(recordsAt+r*224ull+16+lane*4),phi=word(recordsAt+r*224ull+208+lane*4);
        if(!phi) continue;
        const uint32_t u0=rect&255,v0=(rect>>8)&255,u1=(rect>>16)&255,v1=rect>>24;
        if(light>=valveLights || u0>=u1 || v0>=v1 || u1>16 || v1>16) throw std::runtime_error("Invalid GPU emitter tree packet");
    }
    for(uint32_t v=0;v<valveLights;++v) { const uint32_t g=word(mapAt+v*4ull); if(g!=0xffffffffu && g>=gpuLights) throw std::runtime_error("Invalid GPU emitter tree map"); }
    for(uint32_t g=0;g<mapped;++g) if(word(reverseAt+g*4ull)>=valveLights) throw std::runtime_error("Invalid GPU emitter tree map");
}
template<class Buffers>
static GpuLightingTiming render(const Buffers& inputData,const uint32_t* constants,
                               const std::vector<uint8_t>& shader,void* result,uint64_t resultBytes,
                               const GpuLightingRequest* request=nullptr) {
        std::vector<GpuInputView> data;
        for(const auto& bytes:inputData) data.push_back({bytes.data(),bytes.size()});
        bool sliced=request && request->readSlice;
        auto started=std::chrono::steady_clock::now();
        uint32_t count=constants[0],spp=constants[1];
        // constants[3] flags: bit 31 lightmap_directlight, bit 30 light-probe volume (256-byte records per cell), bit 28 sample reject (16-byte records per sample); otherwise 204-byte lightmap luxel records.
        const bool direct=(constants[3]&0x80000000u)!=0;
        const bool probes=(constants[3]&0x40000000u)!=0;
        const bool reject=(constants[3]&0x10000000u)!=0;
        const uint32_t outputStride=probes?256:reject?16:204;
        const bool hardware=gpuOverride(L"VRAD3_GPU_HARDWARE_TRAVERSAL")!=L"0";
        const bool lightCulling=!direct;
        const bool compareTraversal=gpuOverride(L"VRAD3_GPU_RESEARCH_COMPARE")==L"1";
        auto& session=gpuSession();
        const bool persistent=request && request->version==6 && request->persistentSceneHash;
        std::array<uint8_t,32> sceneHash{};
        ResidentScene* resident=nullptr;
        if(persistent) {
            memcpy(sceneHash.data(),request->persistentSceneHash,32);
            for(auto& scene:session.scenes) if(scene.hash==sceneHash) resident=&scene;
            if(resident) resident->lastUse=++session.useClock;
        }
        const bool reuse=resident!=nullptr;
        const bool researchRange=!gpuOverride(L"VRAD3_GPU_RESEARCH_LUXELS").empty();
        const bool researchEnabled=researchRange && gpuOverride(L"VRAD3_GPU_RESEARCH_COUNTERS")==L"1";
        const uint32_t researchFirst=researchRange?static_cast<uint32_t>(gpuLimit(L"VRAD3_GPU_RESEARCH_FIRST_PLUS_ONE",1,count))-1:0;
        const uint32_t researchLuxels=researchRange?static_cast<uint32_t>(gpuLimit(L"VRAD3_GPU_RESEARCH_LUXELS",64,4096)):0;
        if(compareTraversal && (!researchRange || researchLuxels>8 || !hardware || direct))
            throw std::runtime_error("Traversal comparison requires a hardware pathtrace research range of <=8 luxels");
        if(researchRange && (!sliced || (!compareTraversal && spp>16) || researchLuxels>count-researchFirst))
            throw std::runtime_error("Research ranges require sliced inputs, <=16 spp and an in-block range");
        if(!count || count>kMaxBlockLuxels || spp==0 || spp>4096) throw std::runtime_error("Unsupported block dimensions or sample count");
        if(data.size()!=13 || resultBytes!=static_cast<uint64_t>(count)*outputStride) throw std::runtime_error("Invalid GPU API buffers");
        if(data[7].size()!=(count+1ull)*4 || (!sliced && ((reject ? data[9].size()!=384 : data[8].size()!=data[9].size()) || data[10].size()%36)) ||
           (!reuse && (data[0].size()%48 || data[1].size()!=data[0].size()/2 || data[2].size()!=data[0].size()/6 || data[3].size()!=data[0].size()/4)))
            throw std::runtime_error("Inconsistent GPU inputs");
        if(!reuse) {
        if(data[12].size()<96) throw std::runtime_error("Invalid GPU sky header");
        uint32_t skyWidth,skyHeight,cookieCount,environmentBytes;
        memcpy(&skyWidth,data[12].data(),4); memcpy(&skyHeight,data[12].data()+4,4);
        memcpy(&cookieCount,data[12].data()+8,4); memcpy(&environmentBytes,data[12].data()+12,4);
        uint64_t skyBytes=96+static_cast<uint64_t>(skyWidth)*skyHeight*12;
        // An optional emitter tree follows the sky and cookies; its offset is header word 23 of buffer 12.
        uint32_t treeOffset; memcpy(&treeOffset,data[12].data()+92,4);
        const uint64_t expectedBytes=cookieCount?environmentBytes:skyBytes;
        const bool contentEnds=treeOffset ? treeOffset==((expectedBytes+15)&~uint64_t(15)) : data[12].size()==expectedBytes;
        if(treeOffset) validateEmitterTree(data[12],treeOffset,constants[11],(skyBytes+15)&~uint64_t(15));
        if(skyWidth>8192 || skyHeight>8192 || (!skyWidth)!=(!skyHeight) || !contentEnds ||
           (!cookieCount && environmentBytes) ||
           (cookieCount && (cookieCount>65536 || skyBytes>=environmentBytes)))
            throw std::runtime_error("Inconsistent GPU sky planes");
        std::vector<uint32_t> cookieOffsets;
        uint64_t nextCookie=skyBytes;
        for(uint32_t i=0;i<cookieCount;++i) {
            nextCookie=(nextCookie+15)&~uint64_t(15);
            if(nextCookie+16>environmentBytes) throw std::runtime_error("Invalid GPU cookie header");
            uint32_t width,height;
            memcpy(&width,data[12].data()+nextCookie,4); memcpy(&height,data[12].data()+nextCookie+4,4);
            if(!width || !height || width>8192 || height>8192 || nextCookie+16+static_cast<uint64_t>(width)*height*12>environmentBytes)
                throw std::runtime_error("Invalid GPU cookie planes");
            cookieOffsets.push_back(static_cast<uint32_t>(nextCookie));
            nextCookie+=16+static_cast<uint64_t>(width)*height*12;
        }
        if(cookieCount && nextCookie!=environmentBytes) throw std::runtime_error("Unexpected GPU cookie tail");
        if(data[11].size()<static_cast<uint64_t>(constants[11])*112) throw std::runtime_error("Invalid GPU light buffer");
        for(uint32_t i=0;i<constants[11];++i) {
            float cookie,scale;
            memcpy(&cookie,data[11].data()+i*112ull+92,4); memcpy(&scale,data[11].data()+i*112ull+108,4);
            if(cookie==0) continue;
            if(!std::isfinite(cookie) || cookie<0 || cookie>environmentBytes || cookie!=std::floor(cookie) ||
               !std::binary_search(cookieOffsets.begin(),cookieOffsets.end(),static_cast<uint32_t>(cookie)) || !std::isfinite(scale) || scale<=0)
                throw std::runtime_error("Invalid GPU cookie light reference");
        }
        }
        { BlockProfileScope profile("device_create"); ensureDevice(); }
        auto& device=gpuSession().device;
        const double targetMs=static_cast<double>(gpuLimit(L"VRAD3_GPU_BATCH_MS",defaultBatchMs()*((direct && !usagePolicy().max && gpuOverride(L"VRAD3_GPU_BATCH_MS").empty())?2:1),250));
        double checksStarted=blockProfileEnabled()?blockProfileNow():0;
        DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
        uint64_t budget=0,available=0;
        auto queryBudget=[&]() {
            check(gpuSession().adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&memory),"Query local VRAM budget");
            budget=std::min(memory.Budget,gpuLimit(L"VRAD3_GPU_VRAM_MB",UINT64_MAX/1048576,UINT64_MAX/1048576)*1048576);
            gpuDiagnostics.budget=budget;
            available=budget>memory.CurrentUsage?budget-memory.CurrentUsage:0;
            if(reuse) available+=resident->pinnedBytes;
        };
        { BlockProfileScope profile("vram_query"); queryBudget(); }
        std::vector<uint8_t> traversalMetadata;
        std::vector<TracePartition> partitions;
        uint32_t boundsOffset=0;
        auto aligned=[](uint64_t bytes) { return (std::max<uint64_t>(256,bytes)+65535)&~65535ull; };
        D3D12_RAYTRACING_GEOMETRY_DESC estimateGeometry{}; estimateGeometry.Type=D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        estimateGeometry.Triangles.VertexBuffer.StrideInBytes=16;
        estimateGeometry.Triangles.VertexCount=reuse?0:static_cast<UINT>(data[0].size()/16);
        estimateGeometry.Triangles.VertexFormat=DXGI_FORMAT_R32G32B32_FLOAT;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS estimateBottom{};
        estimateBottom.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        estimateBottom.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY; estimateBottom.NumDescs=1;
        estimateBottom.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS estimateTop{};
        estimateTop.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        estimateTop.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;
        estimateTop.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO et{};
        uint64_t bottomBytes=0,scratchBytes=0,indexBytes=0,sceneRequired=0;
        const bool hardwareScene=reuse?resident->hardware:hardware;
        const bool lightCullingScene=reuse?resident->lightCulling:lightCulling;
        const bool compareScene=reuse?resident->compare:compareTraversal;
        bool translucentScene=true;
        if(reuse) { sceneRequired=resident->sceneRequired; boundsOffset=resident->boundsOffset; translucentScene=resident->translucent; }
        else {
        { BlockProfileScope profile("traversal_partition");
          if(hardware) partitions=partitionTraceScene(data[2],data[4],constants,traversalMetadata);
          else partitions.push_back({}); }
        if(compareTraversal) { TracePartition reference; reference.mask=16; partitions.push_back(std::move(reference)); }
        translucentScene=!hardware || std::any_of(partitions.begin(),partitions.end(),[](const TracePartition& part) { return (part.mask&4)!=0; });
        if(lightCulling) boundsOffset=appendLightBounds(data[11],constants[11],traversalMetadata,gpuOverride(L"VRAD3_GPU_LIGHT_GRID")!=L"0");
        if(traversalMetadata.empty()) traversalMetadata.resize(16);
        estimateTop.NumDescs=static_cast<UINT>(partitions.size());
        { BlockProfileScope profile("as_prebuild_query");
        for(auto& part:partitions) {
            part.geometry=estimateGeometry;
            if(hardware && part.mask!=16) {
                part.geometry.Flags=part.opaque?D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE:D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
                part.geometry.Triangles.IndexFormat=DXGI_FORMAT_R32_UINT;
                part.geometry.Triangles.IndexCount=static_cast<UINT>(part.indices.size());
                indexBytes+=aligned(part.indices.size()*4ull);
            }
            estimateBottom.pGeometryDescs=&part.geometry;
            device->GetRaytracingAccelerationStructurePrebuildInfo(&estimateBottom,&part.sizes);
            bottomBytes+=aligned(part.sizes.ResultDataMaxSizeInBytes);
            scratchBytes=std::max(scratchBytes,part.sizes.ScratchDataSizeInBytes);
        }
        device->GetRaytracingAccelerationStructurePrebuildInfo(&estimateTop,&et); }
        sceneRequired=bottomBytes+aligned(et.ResultDataMaxSizeInBytes)
            +aligned(std::max(scratchBytes,et.ScratchDataSizeInBytes))+aligned(partitions.size()*64)+aligned(24)
            +indexBytes+aligned(traversalMetadata.size());
        for(size_t i=0;i<data.size();++i) if(i<7 || i>10) sceneRequired+=aligned(data[i].size());
        }
        std::cout << "GPU traversal: hardware=" << hardwareScene << " partitions=" << (reuse?resident->partitions:static_cast<uint32_t>(partitions.size()))
                  << " light_culling=" << lightCullingScene << " translucent=" << translucentScene << (reuse?" (retained scene)":"") << "\n";
        uint64_t usable=available-available/10;
        if(sceneRequired+6*65536ull>=usable && persistent && !reuse && !session.scenes.empty()) {
            const uint64_t evicted=residentBytes(),before=memory.CurrentUsage;
            session.scenes.clear();
            uint64_t previous=UINT64_MAX;
            for(unsigned attempt=0;attempt<80;++attempt) {
                queryBudget();
                const bool dropped=memory.CurrentUsage+evicted/10*9<=before;
                const bool settled=previous!=UINT64_MAX && (previous>memory.CurrentUsage?previous-memory.CurrentUsage:memory.CurrentUsage-previous)<(1u<<20);
                if(dropped && settled) break;
                previous=memory.CurrentUsage;
                Sleep(25);
            }
            usable=available-available/10;
            std::cout << "GPU server: evicted retained scenes bytes=" << evicted << " to admit a new scene\n";
        }
        if(sceneRequired+6*65536ull>=usable)
            throw std::runtime_error("Scene data alone exceeds local VRAM budget: required="+
                std::to_string(sceneRequired)+" available="+std::to_string(available));
        uint64_t sliceBudget=usable-sceneRequired-6*65536ull;
        uint64_t peakResources=sceneRequired;
        uint64_t peakLocalUsage=memory.CurrentUsage;
        // Discrete GPUs read shader buffers from VRAM (default heap) filled from upload staging copies; integrated GPUs read the upload heap, which is already their local memory.
        const bool vramInputs=!gpuSession().uma && gpuOverride(L"VRAD3_GPU_DEFAULT_HEAP")!=L"0";
        std::wcout << L"GPU block hardware: adapter=" << gpuSession().adapterName << L" budget_bytes=" << budget
                   << L" available_bytes=" << available << L" scene_required_bytes=" << sceneRequired
                   << L" slice_budget_bytes=" << sliceBudget << L" threads=" << usableCpuCount() << L" target_ms=" << targetMs << L" gpu_usage=" << usageDescriptionW().c_str()
                   << L" vram_inputs=" << vramInputs << L"\n";
        if(!reuse) requirePhysicalMemory(sceneRequired,"GPU scene resources");
        if(blockProfileEnabled()) std::cerr << "BLOCK_PROFILE hardware_checks ms=" << blockProfileNow()-checksStarted << " bytes=0\n";
        auto buffer=[&](uint64_t bytes,D3D12_HEAP_TYPE heap,D3D12_RESOURCE_STATES state,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE) {
            D3D12_HEAP_PROPERTIES properties{}; properties.Type=heap;
            D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width=std::max<uint64_t>(256,bytes); desc.Height=1; desc.DepthOrArraySize=1; desc.MipLevels=1; desc.SampleDesc.Count=1; desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR; desc.Flags=flags;
            ComPtr<ID3D12Resource> resource; check(device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&resource)),"Create buffer"); return resource;
        };
        auto upload=[&](const void* bytes,size_t length) {
            auto resource=buffer(length,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
            void* mapped=nullptr; D3D12_RANGE empty{0,0}; check(resource->Map(0,&empty,&mapped),"Map upload"); memcpy(mapped,bytes,length); resource->Unmap(0,nullptr); return resource;
        };
        struct PendingCopy { ComPtr<ID3D12Resource> staging,target; };
        std::vector<PendingCopy> pendingCopies;
        auto shaderInput=[&](const void* bytes,size_t length) {
            if(!vramInputs) return upload(bytes,length);
            ComPtr<ID3D12Resource> target;
            try { target=buffer(length,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST); }
            catch(const std::exception&) { return upload(bytes,length); }
            pendingCopies.push_back({upload(bytes,length),target});
            return target;
        };
        std::vector<ComPtr<ID3D12Resource>> inputs(13);
        { BlockProfileScope profile("scene_uploads");
          if(reuse) inputs=resident->inputs;
          else for(size_t i=0;i<data.size();++i) if(i<7 || i>10) inputs[i]=shaderInput(data[i].data(),data[i].size()); }
        double setupStarted=blockProfileNow();
        std::vector<ComPtr<ID3D12Resource>> blases,indexBuffers;
        std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instances;
        if(!reuse) {
        for(auto& part:partitions) {
            part.geometry.Triangles.VertexBuffer.StartAddress=inputs[0]->GetGPUVirtualAddress();
            if(hardware && part.mask!=16) {
                indexBuffers.push_back(upload(part.indices.data(),part.indices.size()*4));
                part.geometry.Triangles.IndexBuffer=indexBuffers.back()->GetGPUVirtualAddress();
            }
            blases.push_back(buffer(part.sizes.ResultDataMaxSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));
            D3D12_RAYTRACING_INSTANCE_DESC instance{};
            instance.Transform[0][0]=instance.Transform[1][1]=instance.Transform[2][2]=1;
            instance.InstanceID=part.first; instance.InstanceMask=hardware?part.mask:255;
            if(hardware) instance.Flags=D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE;
            if(part.twoSided) instance.Flags|=D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
            instance.AccelerationStructure=blases.back()->GetGPUVirtualAddress(); instances.push_back(instance);
        }
        }
        auto traversalBuffer=reuse?resident->traversalBuffer:shaderInput(traversalMetadata.data(),traversalMetadata.size());
        ComPtr<ID3D12Resource> instanceBuffer;
        if(!reuse) instanceBuffer=upload(instances.data(),instances.size()*sizeof(instances[0]));
        auto top=estimateTop; if(!reuse) top.InstanceDescs=instanceBuffer->GetGPUVirtualAddress();
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO topInfo=et;
        auto tlas=reuse?resident->tlas:buffer(topInfo.ResultDataMaxSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        ComPtr<ID3D12Resource> scratch;
        if(!reuse) scratch=buffer(std::max(scratchBytes,topInfo.ScratchDataSizeInBytes),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        ensurePipeline(shader);
        auto& root=gpuSession().root;
        auto& pipeline=gpuSession().pipeline;
        if(blockProfileEnabled()) std::cerr << "BLOCK_PROFILE resources_pipeline ms=" << blockProfileNow()-setupStarted << " bytes=0\n";
        double queueStarted=blockProfileNow();
        if(!session.queue) {
            D3D12_COMMAND_QUEUE_DESC queueDesc{}; queueDesc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
            check(device->CreateCommandQueue(&queueDesc,IID_PPV_ARGS(&session.queue)),"Create queue");
            for(size_t i=0;i<2;++i) {
                check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&session.allocators[i])),"Create allocator");
                check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,session.allocators[i].Get(),pipeline.Get(),IID_PPV_ARGS(&session.lists[i])),"Create commands");
            }
            check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&session.fence)),"Create fence"); session.fenceValue=0;
            D3D12_QUERY_HEAP_DESC queryDesc{}; queryDesc.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP; queryDesc.Count=4;
            check(device->CreateQueryHeap(&queryDesc,IID_PPV_ARGS(&session.timestamps)),"Create timestamps");
            session.timestampData=buffer(32,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        }
        auto& queue=session.queue; auto& allocators=session.allocators; auto& lists=session.lists;
        auto& fence=session.fence; auto& timestamps=session.timestamps; auto& timestampData=session.timestampData;
        if(fence->GetCompletedValue()<session.fenceValue) check(fence->SetEventOnCompletion(session.fenceValue,nullptr),"Wait for earlier work");
        check(device->GetDeviceRemovedReason(),"GPU device status");
        for(auto& list:lists) list->Close();
        check(allocators[0]->Reset(),"Reset allocator");
        check(lists[0]->Reset(allocators[0].Get(),pipeline.Get()),"Reset commands");
        ComPtr<ID3D12CommandAllocator> allocator=allocators[0]; ComPtr<ID3D12GraphicsCommandList4> commands=lists[0];
        uint32_t slot=0; uint64_t slotFence[2]{};
        auto barrier=[&]() { D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV; commands->ResourceBarrier(1,&b); };
        const uint32_t blockWidth=request?request->blockWidth:0;
        const bool adaptive=request && request->adaptiveMin && !direct && !probes && sliced && !researchRange &&
                            blockWidth && blockWidth<65536 && count%blockWidth==0 && request->adaptiveMin<spp &&
                            std::isfinite(request->adaptiveTol) && request->adaptiveTol>0;
        const uint32_t adaptiveMin=adaptive?request->adaptiveMin:0,tilesPerRow=adaptive?(blockWidth+3)/4:0;
        const uint32_t tiles=adaptive?tilesPerRow*((count/blockWidth+3)/4):0;
        uint32_t counterBytes=compareScene?65536:256;
        if(adaptive) counterBytes=65536+tiles*4;
        const bool luxelList=!direct && !probes && !reject && !researchRange && gpuOverride(L"VRAD3_GPU_LUXEL_LIST")!=L"0";
        const uint32_t maxSlice=researchRange?researchLuxels:static_cast<uint32_t>(gpuLimit(L"VRAD3_GPU_SLICE_LUXELS",count,1048576));
        const uint64_t listCapacity=luxelList?std::min(count,maxSlice):0;
        auto counterData=buffer(counterBytes+listCapacity*4,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        std::vector<uint8_t> zeros(counterBytes);
        auto counterZero=upload(zeros.data(),zeros.size());
        auto counterReadback=buffer(counterBytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        commands->CopyBufferRegion(counterData.Get(),0,counterZero.Get(),0,counterBytes);
        const bool dropDone=luxelList && adaptive;
        std::array<ComPtr<ID3D12Resource>,2> listStaging,flagReadback;
        std::array<uint32_t*,2> listMapped{}; std::array<uint64_t,2> stagingFence{},flagFence{};
        for(uint32_t k=0;k<(dropDone?2u:listCapacity?1u:0u);++k) {
            listStaging[k]=buffer(listCapacity*4,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
            void* mapped=nullptr; D3D12_RANGE none{0,0}; check(listStaging[k]->Map(0,&none,&mapped),"Map luxel list"); listMapped[k]=static_cast<uint32_t*>(mapped);
        }
        if(dropDone) for(auto& f:flagReadback) f=buffer(tiles*4ull,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_RESOURCE_BARRIER counterTransition{};
        counterTransition.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        counterTransition.Transition.pResource=counterData.Get();
        counterTransition.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        counterTransition.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST;
        counterTransition.Transition.StateAfter=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        commands->ResourceBarrier(1,&counterTransition);
        auto counterState=[&](D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
            counterTransition.Transition.StateBefore=before; counterTransition.Transition.StateAfter=after; commands->ResourceBarrier(1,&counterTransition);
        };
        auto recordCopies=[&]() {
            if(pendingCopies.empty()) return;
            std::vector<D3D12_RESOURCE_BARRIER> ready;
            for(auto& copy:pendingCopies) {
                commands->CopyResource(copy.target.Get(),copy.staging.Get());
                D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource=copy.target.Get();
                b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST; b.Transition.StateAfter=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                ready.push_back(b);
            }
            commands->ResourceBarrier(static_cast<UINT>(ready.size()),ready.data());
        };
        recordCopies();
        uint64_t& fenceValue=session.fenceValue; uint64_t frequency=0;
        check(queue->GetTimestampFrequency(&frequency),"Timestamp frequency");
        auto launch=[&]() {
            check(commands->Close(),"Close commands");
            ID3D12CommandList* submitted[]={commands.Get()}; queue->ExecuteCommandLists(1,submitted);
            check(queue->Signal(fence.Get(),++fenceValue),"Signal fence");
            return slotFence[slot]=fenceValue;
        };
        auto waitFor=[&](uint64_t value) {
            if(fence->GetCompletedValue()>=value) { check(device->GetDeviceRemovedReason(),"GPU device status"); return; }
            HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
            if(!event) throw std::runtime_error("Create event failed");
            HRESULT hr=fence->SetEventOnCompletion(value,event);
            DWORD wait=SUCCEEDED(hr) ? WaitForSingleObject(event,60000) : WAIT_FAILED;
            if(wait==WAIT_TIMEOUT) {
                std::cerr << "GPU lighting: no progress for 60 s; waiting for the submitted work before falling back\n" << std::flush;
                const ULONGLONG until=GetTickCount64()+600000;
                while(wait==WAIT_TIMEOUT && GetTickCount64()<until && device->GetDeviceRemovedReason()==S_OK) wait=WaitForSingleObject(event,1000);
                CloseHandle(event); check(device->GetDeviceRemovedReason(),"GPU device status");
                throw std::runtime_error("GPU lighting timed out");
            }
            CloseHandle(event); check(hr,"Set completion event");
            if(wait!=WAIT_OBJECT_0) throw std::runtime_error("GPU lighting timed out");
            check(device->GetDeviceRemovedReason(),"GPU device status");
        };
        auto submit=[&]() { waitFor(launch()); };
        auto useSlot=[&](uint32_t s) { slot=s; allocator=allocators[s]; commands=lists[s]; };
        auto reset=[&]() {
            waitFor(slotFence[slot]);
            check(allocator->Reset(),"Reset allocator");
            check(commands->Reset(allocator.Get(),pipeline.Get()),"Reset commands");
        };
        auto elapsed=[&](uint32_t s) {
            void* mapped=nullptr; D3D12_RANGE range{16ull*s,16ull*s+16};
            check(timestampData->Map(0,&range,&mapped),"Map timestamps");
            uint64_t ticks[2]; memcpy(ticks,static_cast<uint8_t*>(mapped)+16ull*s,16); timestampData->Unmap(0,nullptr);
            return (ticks[1]-ticks[0])*1000.0/frequency;
        };
        if(blockProfileEnabled()) std::cerr << "BLOCK_PROFILE queue_commands ms=" << blockProfileNow()-queueStarted << " bytes=0\n";
        double buildStarted=blockProfileNow();
        commands->EndQuery(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
        const bool splitBuilds=!usagePolicy().max;
        uint32_t buildSubmits=1; double maxBuildSubmitMs=0;
        if(!reuse) {
        build.ScratchAccelerationStructureData=scratch->GetGPUVirtualAddress();
        for(size_t i=0;i<partitions.size();++i) {
            build.Inputs=estimateBottom; build.Inputs.pGeometryDescs=&partitions[i].geometry;
            build.DestAccelerationStructureData=blases[i]->GetGPUVirtualAddress();
            commands->BuildRaytracingAccelerationStructure(&build,0,nullptr); barrier();
            if(splitBuilds) {
                const double submitted=blockProfileNow(); submit(); reset();
                maxBuildSubmitMs=std::max(maxBuildSubmitMs,blockProfileNow()-submitted); ++buildSubmits;
            }
        }
        build.Inputs=top; build.DestAccelerationStructureData=tlas->GetGPUVirtualAddress(); commands->BuildRaytracingAccelerationStructure(&build,0,nullptr); barrier();
        }
        commands->EndQuery(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);
        commands->ResolveQueryData(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,timestampData.Get(),0);
        { const double submitted=blockProfileNow(); submit(); maxBuildSubmitMs=std::max(maxBuildSubmitMs,blockProfileNow()-submitted); }
        pendingCopies.clear();
        double buildMs=elapsed(0),lightingMs=0,maxDispatchMs=0;
        if(!reuse) std::cout << "GPU scene build: submissions=" << buildSubmits << " longest_submission_ms=" << maxBuildSubmitMs << " build_ms=" << buildMs << '\n';
        if(persistent && !reuse) {
            ResidentScene retained;
            retained.hash=sceneHash; retained.inputs=inputs;
            for(size_t i=7;i<=10;++i) retained.inputs[i].Reset();
            retained.blases=blases; retained.indexBuffers=indexBuffers; retained.tlas=tlas; retained.traversalBuffer=traversalBuffer;
            retained.sceneRequired=sceneRequired; retained.boundsOffset=boundsOffset;
            retained.partitions=static_cast<uint32_t>(partitions.size());
            retained.hardware=hardware; retained.lightCulling=lightCulling; retained.compare=compareTraversal; retained.translucent=translucentScene;
            retained.pinnedBytes=sceneRequired-aligned(std::max(scratchBytes,et.ScratchDataSizeInBytes))
                -aligned(partitions.size()*64)-aligned(24);
            retained.lastUse=++session.useClock;
            while(session.scenes.size()>=kMaxResidentScenes) {
                auto oldest=std::min_element(session.scenes.begin(),session.scenes.end(),
                    [](const ResidentScene& x,const ResidentScene& y) { return x.lastUse<y.lastUse; });
                session.scenes.erase(oldest);
            }
            session.scenes.push_back(std::move(retained));
            resident=&session.scenes.back();
        }
        if(persistent) std::cout<<"GPU server scene: "<<(reuse?"reused":"uploaded")<<" bytes="<<sceneRequired<<" resident="<<session.scenes.size()<<'\n';
        if(blockProfileEnabled()) std::cerr << "BLOCK_PROFILE scene_build_wall ms=" << blockProfileNow()-buildStarted << " bytes=0\n";
        double dispatchStarted=blockProfileNow();
        constexpr uint64_t kMaxSlots=4194304;
        uint32_t dispatchCount=0;
        uint32_t dispatchConstants[30]{}; memcpy(dispatchConstants,constants,24*4);
        dispatchConstants[25]=(researchEnabled?1u:0u)|(gpuOverride(L"VRAD3_GPU_ESTIMATOR")==L"cs2" && gpuOverride(L"VRAD3_GPU_CS2_SHADOWS")==L"hla"?2u:0u)|(luxelList?counterBytes:0u);
        const bool lightPaths=!direct && !probes && !reject && gpuOverride(L"VRAD3_GPU_LIGHT_PATHS")==L"1";
        const uint32_t researchLightPaths=gpuOverride(L"VRAD3_GPU_RESEARCH_LIGHT_PATHS")==L"1"?1u:
                                           gpuOverride(L"VRAD3_GPU_RESEARCH_LIGHT_PATHS")==L"2"?2u:
                                           gpuOverride(L"VRAD3_GPU_RESEARCH_LIGHT_PATHS")==L"3"?3u:gpuOverride(L"VRAD3_GPU_RESEARCH_LIGHT_PATHS")==L"4"?4u:0u;
        dispatchConstants[28]=(hardwareScene?1u:0u)|(lightCullingScene?2u:0u)|(compareScene?4u:0u)|(adaptive?8u|(blockWidth<<16):0u)|(lightPaths?128u:0u)
            |(lightPaths?(researchLightPaths&7u)<<8:0u)
            |(direct && gpuOverride(L"VRAD3_GPU_LIGHT_LIMIT_REPORT")==L"1"?2048u:0u)|(hardwareScene && !translucentScene?4096u:0u)
            |(direct && gpuOverride(L"VRAD3_GPU_ESTIMATOR")==L"cs2" && gpuOverride(L"VRAD3_GPU_CS2_SLOTS")!=L"shared"?8192u:0u)
            |(!probes && !reject && gpuOverride(L"VRAD3_GPU_ESTIMATOR")==L"cs2" && gpuOverride(L"VRAD3_GPU_CS2_CLAMP")!=L"hla"?16384u:0u)
            |(!probes && !reject && gpuOverride(L"VRAD3_GPU_ESTIMATOR")==L"cs2" && gpuOverride(L"VRAD3_GPU_CS2_AO")!=L"hla"?32768u:0u);
        if(lightPaths) std::cout << "GPU sampling: light_paths=1\n";
        if(!probes && !reject && gpuOverride(L"VRAD3_GPU_ESTIMATOR")==L"cs2")
            std::cout << "GPU sampling: estimator=cs2" << (dispatchConstants[28]&16384u?" clamp_method=cs2":"") << (dispatchConstants[28]&32768u?" ao_method=cs2":"") << (dispatchConstants[25]&2u?" backface_shadows=hla":"") << "\n";
        // Adaptive rounds end at the minimum sample count, then every 32 more samples (Valve's rule), or every 16 passes for the CS2 estimator.
        const uint32_t adaptiveRound=gpuOverride(L"VRAD3_GPU_ESTIMATOR")==L"cs2"?16:32;
        auto roundEnd=[&](uint32_t done) { return done<adaptiveMin?adaptiveMin:adaptiveMin+((done-adaptiveMin)/adaptiveRound+1)*adaptiveRound; };
        if(adaptive) std::cout << "GPU adaptive samples: min=" << adaptiveMin << " max=" << spp << " tol=" << request->adaptiveTol
                               << " round=" << adaptiveRound
                               << " block_width=" << blockWidth << " tiles=" << tiles << '\n';
        uint32_t tileTests=0; uint64_t droppedLuxels=0;
        dispatchConstants[29]=boundsOffset;
        const bool forcedChunks=!direct && !gpuOverride(L"VRAD3_GPU_SAMPLE_CHUNK").empty();
        const bool cs2Direct=direct && gpuOverride(L"VRAD3_GPU_ESTIMATOR")==L"cs2";
        constexpr uint32_t kCs2Lanes=8;
        const uint32_t units=direct?(cs2Direct?1u:std::max(1u,constants[11])):spp;
        const bool adaptiveChunk=!forcedChunks;
        uint32_t sampleChunk=forcedChunks?static_cast<uint32_t>(gpuLimit(L"VRAD3_GPU_SAMPLE_CHUNK",units,units)):1u;
        const uint32_t luxelFloor=adaptiveChunk?4096u:64u;
        std::vector<double> unitCost(direct?units:0,0.0);
        double safety=1.0;
        const bool traceSchedule=gpuOverride(L"VRAD3_GPU_TRACE_SCHEDULE")==L"1";
        uint32_t batchLuxels=256u,chunkShrinks=0,chunkGrowths=0,passes=0;
        // Speed guard: estimates Valve's CPU pathtrace time from measured per-thread rates (the slower of two scenes) with a 50% margin, so an expensive CPU scene is not rejected on a cheaper scene's rate.
        const double cpuRate=std::min(65536.0*256/(32*11.16),262144.0*1024/(32*12.39));
        const double cpuEstimateMs=1500.0*count*spp/(cpuRate*usableCpuCount());
        const auto guardOverride=gpuOverride(L"VRAD3_GPU_SPEED_GUARD");
        if(!guardOverride.empty() && guardOverride!=L"0" && guardOverride!=L"1")
            throw std::runtime_error("VRAD3_GPU_SPEED_GUARD must be 0 or 1");
        const bool speedGuard=guardOverride!=L"0" && !direct && !reject;
        const double probeFactor=probes?std::max(1u,constants[15]):1;
        const double probeCpuEstimateMs=1500.0*(static_cast<double>(request?request->totalPaths:0)/probeFactor)/(static_cast<double>(gpuLimit(L"VRAD3_GPU_PROBE_CPU_RATE",200000,1000000000000ull))*usableCpuCount());
        auto offset=[&](uint32_t i) { uint32_t value; memcpy(&value,data[7].data()+4ull*i,4); return value; };
        uint64_t totalPaths=0,measuredPaths=0;
        double guardStarted=blockProfileEnabled()?blockProfileNow():0;
        if(sliced) totalPaths=request->totalPaths;
        else for(uint32_t i=0;i<count;++i) if(offset(i+1)>offset(i)) totalPaths+=spp;
        double guardMs=blockProfileEnabled()?blockProfileNow()-guardStarted:0;
        double measuredMs=0;
        std::cout << "GPU speed guard configuration: enabled=" << speedGuard << " total_paths=" << totalPaths
                  << " minimum_fraction=0.05 minimum_gpu_ms=250\n";
        uint32_t sliceCount=0;
        std::vector<float> dispatchTimes; double hostOverheadMs=0;
        GpuPacer pacer;
        const bool pipelined=!pacer.cap;
        struct Inflight { bool live=false; uint64_t fence=0; uint32_t slot=0,first=0,rangeLuxels=0,activeLuxels=0,sampleFirst=0,chunkSamples=0,passChunk=0,wanted=0,sliceLuxels=0,sliceFirst=0,span=0; };
        Inflight inflight;
        double fullMs=0; uint32_t fullLuxels=0,fullChunk=0;
        auto complete=[&]() {
            if(!inflight.live) return;
            const Inflight d=inflight; inflight.live=false;
            waitFor(d.fence);
            double ms=elapsed(d.slot); lightingMs+=ms; maxDispatchMs=std::max(maxDispatchMs,ms);
            dispatchTimes.push_back(static_cast<float>(ms));
            if(!researchRange) safety=ms>targetMs*1.5?std::max(0.25,safety*0.6):std::min(1.0,safety*1.03);
            if(traceSchedule) std::cout << "GPU schedule: " << (direct?"direct":probes?"probe":"trace") << " pass=" << passes << " units=" << d.sampleFirst << '+' << d.chunkSamples
                                        << " first=" << d.first << " luxels=" << d.rangeLuxels << " active=" << d.activeLuxels << " ms=" << ms << '\n';
            pacer.afterDispatch(ms);
            guardStarted=blockProfileEnabled()?blockProfileNow():0;
            if(direct && d.activeLuxels>=512)
                for(uint32_t u=d.sampleFirst;u<d.sampleFirst+d.chunkSamples;++u) unitCost[u]=std::max(ms/d.activeLuxels/d.chunkSamples,unitCost[u]*0.9);
            const uint64_t batchPaths=static_cast<uint64_t>(d.activeLuxels)*d.chunkSamples;
            measuredPaths+=batchPaths;
            if(request && request->progress)
                *request->progress=static_cast<uint32_t>(((static_cast<uint64_t>(d.sliceFirst)*units+static_cast<uint64_t>(d.sampleFirst)*d.sliceLuxels+
                    static_cast<uint64_t>(d.first+d.rangeLuxels)*d.chunkSamples*d.sliceLuxels/d.span)<<16)/(static_cast<uint64_t>(count)*units));
            if(batchPaths) measuredMs+=ms;
            ++dispatchCount;
            // Safety cap: a dispatch longer than 500 ms fails the GPU path.
            if(ms>500) throw GpuFailure("GPU dispatch exceeded 500 ms safety threshold; CPU fallback (ms="+
                std::to_string(ms)+" global_first="+std::to_string(d.sliceFirst+d.first)+
                " luxels="+std::to_string(d.rangeLuxels)+")",true,true);
            if(speedGuard && dispatchCount%4==0 && measuredPaths &&
               measuredPaths*(probes?10:20)>=totalPaths && measuredMs>=250) {
                double projectedMs=measuredMs*totalPaths/measuredPaths;
                bool slow=projectedMs>(probes?probeCpuEstimateMs:cpuEstimateMs);
                std::cout << "GPU speed guard: measured_luxels=" << d.sliceFirst+d.first << " dispatches=" << dispatchCount
                          << " measured_paths=" << measuredPaths << " measured_gpu_ms=" << measuredMs
                          << " projected_trace_ms=" << projectedMs << " cpu_estimate_ms=" << (probes?probeCpuEstimateMs:cpuEstimateMs)
                          << " decision=" << (slow?"CPU fallback":"continue GPU") << '\n';
                if(slow) throw GpuFailure("GPU speed guard: projected trace "+std::to_string(projectedMs)+
                    " ms exceeds conservative CPU estimate "+std::to_string(probes?probeCpuEstimateMs:cpuEstimateMs)+" ms; CPU fallback",true,true);
            }
            if(blockProfileEnabled()) guardMs+=blockProfileNow()-guardStarted;
            const bool full=d.chunkSamples==d.passChunk && (d.rangeLuxels==d.wanted || (d.rangeLuxels==d.span && d.wanted>=d.span));
            if(batchPaths && full) {
                const double ratio=std::clamp(targetMs*0.75/std::max(ms,0.01),0.25,4.0);
                const uint32_t ceiling=static_cast<uint32_t>(std::max<uint64_t>(64,(direct?kMaxSlots/4:kMaxSlots)/d.passChunk/64*64));
                batchLuxels=static_cast<uint32_t>(std::clamp<double>(std::floor(d.rangeLuxels*ratio/64)*64,64.0,ceiling));
                fullMs=ms; fullLuxels=d.wanted; fullChunk=d.passChunk;
            }
        };
        struct SliceInput { ComPtr<ID3D12Resource> staging,target; uint64_t capacity=0; uint8_t* mapped=nullptr; };
        std::array<SliceInput,4> sliceInputs;
        auto heldSliceBytes=[&]() { uint64_t total=0; for(auto& s:sliceInputs) if(s.capacity) total+=aligned(s.capacity); return total; };
        bool prefetched=false; uint32_t prefetchedFirst=0; uint64_t prefetchedBudget=0;
        try {
        for(uint32_t sliceFirst=researchFirst;sliceFirst<count;) {
            for(size_t i=7;i<=10;++i) inputs[i].Reset();
            uint32_t sliceLuxels=count;
            uint64_t currentSliceBudget=0;
            if(sliced) {
                if(prefetched && prefetchedFirst==sliceFirst) currentSliceBudget=prefetchedBudget;
                else {
                    check(gpuSession().adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&memory),"Query slice VRAM budget");
                    uint64_t currentBudget=std::min(budget,memory.Budget);
                    uint64_t remaining=currentBudget>memory.CurrentUsage?currentBudget-memory.CurrentUsage:0;
                    remaining+=heldSliceBytes();
                    currentSliceBudget=std::min(sliceBudget,remaining-remaining/10);
                    currentSliceBudget=currentSliceBudget>6*65536ull?currentSliceBudget-6*65536ull:0;
                }
                prefetched=false;
                GpuLightingSlice slice{}; char error[512]{};
                { BlockProfileScope profile("slice_reads");
                  if(request->readSlice(request->sliceContext,sliceFirst,std::min(maxSlice,count-sliceFirst),currentSliceBudget,&slice,error,sizeof(error)))
                      throw GpuFailure(error,false,true); }
                sliceLuxels=slice.luxels;
                if(!sliceLuxels || sliceLuxels>std::min(maxSlice,count-sliceFirst)) throw std::runtime_error("Invalid slice luxel count");
                for(size_t i=0;i<4;++i) {
                    if(!slice.buffers[i].data || !slice.buffers[i].bytes || slice.buffers[i].bytes>512ull*1024*1024)
                        throw std::runtime_error("Invalid slice buffer");
                    data[7+i]={static_cast<const uint8_t*>(slice.buffers[i].data),static_cast<size_t>(slice.buffers[i].bytes)};
                }
            }
            if(data[7].size()!=(sliceLuxels+1ull)*4 || (reject ? data[9].size()!=384 : data[8].size()!=data[9].size()) || data[10].size()%36)
                throw std::runtime_error("Inconsistent slice inputs");
            uint64_t outputBytes=sliceLuxels*static_cast<uint64_t>(outputStride),sliceRequired=aligned(outputBytes)*2;
            for(size_t i=7;i<=10;++i) sliceRequired+=aligned(data[i].size());
            if(sceneRequired+sliceRequired>usable) throw std::runtime_error("Slice exceeds local VRAM budget");
            requirePhysicalMemory(sliceRequired,"GPU slice upload/readback resources");
            peakResources=std::max(peakResources,sceneRequired+sliceRequired);
            { BlockProfileScope profile("slice_uploads");
              if(vramInputs) {
                  reset();
                  std::vector<D3D12_RESOURCE_BARRIER> ready;
                  for(size_t i=0;i<4;++i) {
                      const auto& bytes=data[7+i]; auto& s=sliceInputs[i];
                      if(s.capacity<bytes.size()) {
                          s=SliceInput{};
                          s.target=buffer(bytes.size(),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
                          s.staging=buffer(bytes.size(),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
                          void* mapped=nullptr; D3D12_RANGE none{0,0}; check(s.staging->Map(0,&none,&mapped),"Map slice staging");
                          s.mapped=static_cast<uint8_t*>(mapped); s.capacity=bytes.size();
                      }
                      memcpy(s.mapped,bytes.data(),bytes.size());
                      commands->CopyBufferRegion(s.target.Get(),0,s.staging.Get(),0,bytes.size());
                      D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource=s.target.Get();
                      b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                      b.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST; b.Transition.StateAfter=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                      ready.push_back(b);
                      inputs[7+i]=s.target;
                  }
                  commands->ResourceBarrier(static_cast<UINT>(ready.size()),ready.data());
                  submit();
              } else for(size_t i=7;i<=10;++i) inputs[i]=upload(data[i].data(),data[i].size()); }
            auto output=buffer(outputBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            auto readback=buffer(outputBytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
            check(gpuSession().adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&memory),"Query allocated local VRAM");
            peakLocalUsage=std::max(peakLocalUsage,memory.CurrentUsage);
            std::cout << "GPU slice resources: first=" << sliceFirst << " resource_bytes=" << sceneRequired+sliceRequired
                      << " local_usage_bytes=" << memory.CurrentUsage << '\n';
            dispatchConstants[0]=sliceLuxels; dispatchConstants[24]=sliceFirst;
            ++sliceCount;
            std::vector<uint32_t> activePrefix(sliceLuxels+1,0);
            for(uint32_t i=0;i<sliceLuxels;++i) activePrefix[i+1]=activePrefix[i]+(offset(i+1)>offset(i)?1u:0u);
            uint32_t listLuxels=luxelList?activePrefix[sliceLuxels]:0,rounds=0;
            std::vector<uint32_t> hostList,dropped;
            if(listLuxels) {
                for(uint32_t i=0,n=0;i<sliceLuxels;++i) if(activePrefix[i+1]>activePrefix[i]) listMapped[0][n++]=i;
                if(dropDone) hostList.assign(listMapped[0],listMapped[0]+listLuxels);
                reset();
                counterState(D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);
                commands->CopyBufferRegion(counterData.Get(),counterBytes,listStaging[0].Get(),0,listLuxels*4ull);
                counterState(D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                submit();
            }
            if(sliced && request->prefetchSlice && !researchRange && sliceFirst+sliceLuxels<count) {
                const uint32_t next=sliceFirst+sliceLuxels;
                if(!request->prefetchSlice(request->sliceContext,next,std::min(maxSlice,count-next),currentSliceBudget)) {
                    prefetched=true; prefetchedFirst=next; prefetchedBudget=currentSliceBudget;
                }
            }
            if(prefetched) for(size_t i=7;i<=10;++i) data[i]={nullptr,0};
            const double loopStarted=blockProfileNow(),loopGpuMs=lightingMs;
            for(uint32_t sampleFirst=0;sampleFirst<units;) {
                ++passes;
                const uint32_t passChunk=adaptive?std::min(sampleChunk,roundEnd(sampleFirst)-sampleFirst):sampleChunk;
                const uint32_t chunkSamples=std::min(passChunk,units-sampleFirst);
                dispatchConstants[26]=sampleFirst; dispatchConstants[27]=sampleFirst+chunkSamples;
                const uint32_t ceilingLuxels=static_cast<uint32_t>(std::max<uint64_t>(64,(direct?kMaxSlots/4:kMaxSlots)/passChunk/64*64));
                fullMs=0; fullLuxels=0;
                const bool listed=luxelList && sampleFirst>0;
                const uint32_t span=listed?listLuxels:sliceLuxels,floorLuxels=std::min(luxelFloor,(span+63)/64*64);
                dispatchConstants[3]=listed?constants[3]|0x08000000u:constants[3];
                for(uint32_t first=0;first<span;) {
                uint32_t wanted=std::min(ceilingLuxels,std::max(floorLuxels,batchLuxels));
                if(safety<1) wanted=static_cast<uint32_t>(std::max<double>(floorLuxels,std::floor(wanted*safety/64)*64));
                if(direct) {
                    double predicted=0; bool known=true;
                    for(uint32_t u=sampleFirst;u<sampleFirst+chunkSamples && known;++u) { known=unitCost[u]>0; predicted+=unitCost[u]; }
                    if(known && predicted>0)
                        wanted=std::min(wanted,static_cast<uint32_t>(std::max<double>(floorLuxels,std::floor(targetMs*0.75/predicted/64)*64)));
                }
                const uint32_t rangeLuxels=std::min(wanted,span-first);
                const uint32_t activeLuxels=listed?rangeLuxels:activePrefix[first+rangeLuxels]-activePrefix[first];
                if(!activeLuxels && sampleFirst>0) { first+=rangeLuxels; continue; }
                if(pipelined) useSlot(slot^1);
                reset();
                dispatchConstants[15]=rangeLuxels; dispatchConstants[23]=first;
                commands->SetComputeRootSignature(root.Get()); commands->SetComputeRootShaderResourceView(0,tlas->GetGPUVirtualAddress());
                for(UINT i=0;i<inputs.size();++i) commands->SetComputeRootShaderResourceView(i+1,inputs[i]->GetGPUVirtualAddress());
                commands->SetComputeRootUnorderedAccessView(14,output->GetGPUVirtualAddress());
                commands->SetComputeRootUnorderedAccessView(16,counterData->GetGPUVirtualAddress());
                commands->SetComputeRootShaderResourceView(17,traversalBuffer->GetGPUVirtualAddress());
                commands->SetComputeRoot32BitConstants(15,30,dispatchConstants,0);
                commands->EndQuery(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,2*slot);
                commands->Dispatch((dispatchConstants[15]*(cs2Direct?kCs2Lanes:1u)+63)/64,1,1); barrier();
                commands->EndQuery(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,2*slot+1);
                commands->ResolveQueryData(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,2*slot,2,timestampData.Get(),16ull*slot);
                const uint64_t submitted=launch();
                complete();
                inflight={true,submitted,slot,first,rangeLuxels,activeLuxels,sampleFirst,chunkSamples,passChunk,wanted,sliceLuxels,sliceFirst,span};
                if(!pipelined) complete();
                first+=rangeLuxels;
                }
                sampleFirst+=chunkSamples;
                if(adaptive && sampleFirst<units && sampleFirst>=adaptiveMin && (sampleFirst-adaptiveMin)%adaptiveRound==0) {
                    const uint32_t tileRowFirst=sliceFirst/blockWidth/4,tileRowLast=(sliceFirst+sliceLuxels-1)/blockWidth/4;
                    uint32_t testConstants[30]; memcpy(testConstants,dispatchConstants,sizeof(testConstants));
                    testConstants[3]=(constants[3]&~0xe0000000u)|0x20000000u;
                    testConstants[23]=tileRowFirst*tilesPerRow; testConstants[15]=(tileRowLast-tileRowFirst+1)*tilesPerRow;
                    memcpy(&testConstants[27],&request->adaptiveTol,4);
                    if(pipelined) useSlot(slot^1);
                    reset();
                    commands->SetComputeRootSignature(root.Get()); commands->SetComputeRootShaderResourceView(0,tlas->GetGPUVirtualAddress());
                    for(UINT i=0;i<inputs.size();++i) commands->SetComputeRootShaderResourceView(i+1,inputs[i]->GetGPUVirtualAddress());
                    commands->SetComputeRootUnorderedAccessView(14,output->GetGPUVirtualAddress());
                    commands->SetComputeRootUnorderedAccessView(16,counterData->GetGPUVirtualAddress());
                    commands->SetComputeRootShaderResourceView(17,traversalBuffer->GetGPUVirtualAddress());
                    commands->SetComputeRoot32BitConstants(15,30,testConstants,0);
                    constexpr uint32_t kDropFloor=32768; uint32_t listSlot=2;
                    if(dropDone && rounds && !hostList.empty()) {
                        const uint32_t previous=(rounds-1)&1; waitFor(flagFence[previous]);
                        void* mapped=nullptr; D3D12_RANGE range{0,tiles*4ull}; check(flagReadback[previous]->Map(0,&range,&mapped),"Map tile flags");
                        const uint32_t* flags=static_cast<const uint32_t*>(mapped); size_t kept=0;
                        for(uint32_t i:hostList) {
                            const uint32_t g=sliceFirst+i;
                            if(flags[(g/blockWidth/4)*tilesPerRow+(g%blockWidth)/4]) dropped.push_back(i); else hostList[kept++]=i;
                        }
                        D3D12_RANGE none{0,0}; flagReadback[previous]->Unmap(0,&none); hostList.resize(kept);
                        if(kept<listLuxels && (!kept || (kept>=kDropFloor && kept+listLuxels/32<=listLuxels))) {
                            listSlot=rounds&1; waitFor(stagingFence[listSlot]);
                            memcpy(listMapped[listSlot],hostList.data(),kept*4);
                        }
                    }
                    commands->Dispatch((testConstants[15]+63)/64,1,1); barrier();
                    if(dropDone) {
                        counterState(D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
                        commands->CopyBufferRegion(flagReadback[rounds&1].Get(),0,counterData.Get(),65536,tiles*4ull);
                        if(listSlot<2) {
                            counterState(D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
                            if(!hostList.empty()) commands->CopyBufferRegion(counterData.Get(),counterBytes,listStaging[listSlot].Get(),0,hostList.size()*4ull);
                            counterState(D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                        } else counterState(D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    }
                    const uint64_t tested=launch(); if(!pipelined) waitFor(tested);
                    if(dropDone) {
                        flagFence[rounds&1]=tested;
                        if(listSlot<2) { stagingFence[listSlot]=tested; listLuxels=static_cast<uint32_t>(hostList.size()); }
                    }
                    ++rounds; ++tileTests;
                }
                if(adaptiveChunk && sampleFirst<units && fullMs>0) {
                    const double ratio=std::clamp(targetMs*0.75/fullMs,0.25,4.0);
                    if(ratio>1.15 && fullLuxels>=span) {
                        const uint32_t limit=static_cast<uint32_t>(std::max<uint64_t>(1,kMaxSlots/span));
                        const uint32_t next=std::min({units,limit,static_cast<uint32_t>(fullChunk*ratio)});
                        if(next>passChunk) { sampleChunk=next; batchLuxels=span; ++chunkGrowths; }
                    } else if(ratio<0.87 && fullLuxels<=floorLuxels && fullChunk>1) {
                        sampleChunk=std::max(1u,static_cast<uint32_t>(fullChunk*ratio)); ++chunkShrinks;
                    }
                }
            }
            complete();
            hostOverheadMs+=blockProfileNow()-loopStarted-(lightingMs-loopGpuMs);
            BlockProfileScope readbackProfile("readback",outputBytes);
            reset();
            D3D12_RESOURCE_BARRIER transition{}; transition.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; transition.Transition.pResource=output.Get(); transition.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; transition.Transition.StateBefore=D3D12_RESOURCE_STATE_UNORDERED_ACCESS; transition.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE; commands->ResourceBarrier(1,&transition);
            commands->CopyResource(readback.Get(),output.Get()); submit();
            void* mapped=nullptr; D3D12_RANGE range{0,static_cast<SIZE_T>(outputBytes)}; check(readback->Map(0,&range,&mapped),"Map working records");
            memcpy(static_cast<uint8_t*>(result)+sliceFirst*static_cast<uint64_t>(outputStride),mapped,static_cast<size_t>(outputBytes)); readback->Unmap(0,nullptr);
            for(uint32_t i:dropped) memset(static_cast<uint8_t*>(result)+(sliceFirst+static_cast<uint64_t>(i))*204+144,0,60);
            droppedLuxels+=dropped.size();
            check(gpuSession().adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&memory),"Query completed slice VRAM");
            peakLocalUsage=std::max(peakLocalUsage,memory.CurrentUsage);
            if(researchRange) {
                reset();
                counterTransition.Transition.StateBefore=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                counterTransition.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
                commands->ResourceBarrier(1,&counterTransition);
                commands->CopyBufferRegion(counterReadback.Get(),0,counterData.Get(),0,counterBytes); submit();
                D3D12_RANGE counterRange{0,counterBytes};
                check(counterReadback->Map(0,&counterRange,&mapped),"Map research counters");
                uint32_t values[11]; memcpy(values,mapped,sizeof(values));
                const char* names[]={"paths","bounces","path_rays","shadow_rays","light_hits","lights_evaluated","lights_in_range","path_candidates","shadow_candidates","path_translucent","shadow_translucent"};
                std::cout << "GPU_RESEARCH first=" << sliceFirst << " luxels=" << sliceLuxels
                          << " spp=" << spp << " counters=" << researchEnabled << " gpu_ms=" << lightingMs;
                for(uint32_t i=0;i<11;++i) std::cout << ' ' << names[i] << '=' << values[i];
                std::cout << std::endl;
                if(compareTraversal) {
                    const uint32_t* words=static_cast<const uint32_t*>(mapped);
                    uint32_t mismatches=words[11];
                    std::cout << "GPU_TRAVERSAL_MISMATCH count=" << mismatches << '\n';
                    for(uint32_t i=0;i<std::min(mismatches,64u);++i) {
                        const uint32_t* record=words+64+i*20;
                        const float* floats=reinterpret_cast<const float*>(record);
                        std::cout << "GPU_TRAVERSAL_TIE hardware=" << record[0] << " legacy=" << record[1]
                                  << " hardware_t=" << floats[2] << " legacy_t=" << floats[3]
                                  << " t_bits=" << record[2] << ',' << record[3] << " mask=" << record[4] << " cull=" << record[5];
                        for(uint32_t j=6;j<16;++j) std::cout << " v" << j << '=' << floats[j];
                        std::cout << '\n';
                    }
                }
                counterReadback->Unmap(0,nullptr);
                throw std::runtime_error("Research range complete; partial working records withheld");
            }
            sliceFirst+=sliceLuxels;
        }
        } catch(...) { try { waitFor(fenceValue); } catch(...) {} throw; }
        std::cout << "GPU block slices: count=" << sliceCount << " peak_resource_bytes=" << peakResources << '\n';
        {
            std::sort(dispatchTimes.begin(),dispatchTimes.end());
            auto pct=[&](double p) { return dispatchTimes.empty()?0.0:static_cast<double>(dispatchTimes[std::min(dispatchTimes.size()-1,static_cast<size_t>(p*dispatchTimes.size()))]); };
            std::cout << "GPU dispatch times: count=" << dispatchTimes.size() << " p50_ms=" << pct(0.5) << " p95_ms=" << pct(0.95)
                      << " p99_ms=" << pct(0.99) << " max_ms=" << maxDispatchMs << " host_gap_ms=" << hostOverheadMs
                      << " paced_sleep_ms=" << pacer.slept << " tile_tests=" << tileTests << " dropped_luxels=" << droppedLuxels << " passes=" << passes << " chunk_growths=" << chunkGrowths << " chunk_shrinks=" << chunkShrinks << " final_chunk=" << sampleChunk << '\n';
        }
        std::cout << "GPU dispatch ranges: count=" << dispatchCount
                  << " luxels_per_dispatch=" << batchLuxels << " max_ms=" << maxDispatchMs
                  << " total_ms=" << lightingMs << '\n';
        std::wcout << L"GPU block result: adapter=" << gpuSession().adapterName << L" budget_bytes=" << budget
                   << L" batches=" << dispatchCount << L" threads=" << usableCpuCount() << L"\n";
        if(blockProfileEnabled()) std::cerr << "BLOCK_PROFILE dispatch_wall ms=" << blockProfileNow()-dispatchStarted << " bytes=0\n";
        if(blockProfileEnabled()) std::cerr << "BLOCK_PROFILE speed_guard ms=" << guardMs << " bytes=0\n";

        double hostMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
        gpuStats.vramPeak=std::max(gpuStats.vramPeak,peakLocalUsage);
        gpuStats.vramCurrent=memory.CurrentUsage; gpuStats.residentBytes=residentBytes(); gpuStats.scenes=static_cast<uint32_t>(session.scenes.size());
        return {buildMs,lightingMs,hostMs,sliceCount,peakResources,peakLocalUsage};
}

extern "C" GPU_EXPORT int BakeGpuLighting(const GpuLightingRequest* request,GpuLightingTiming* timing,char* error,uint32_t errorBytes) {
    bool keepSession=false;
    gpuLastDecision=false;
    try {
        if(!request || !timing || (request->version!=5 && request->version!=6) || !request->output ||
           (!request->shaderBytes && !request->shaderPath))
            throw std::runtime_error("Invalid GPU lighting request");
        const bool retained=request->version==6 && request->persistentSceneHash &&
                            GpuLightingHasScene(request->persistentSceneHash);
        std::vector<GpuInputView> data; data.reserve(13);
        { BlockProfileScope profile("api_input_views");
        for(size_t i=0;i<13;++i) {
            const auto& buffer=request->buffers[i];
            if(request->readSlice && i>=8 && i<=10) { data.push_back({nullptr,0}); continue; }
            if(retained && (i<7 || i>10)) { data.push_back({nullptr,buffer.bytes}); continue; }
            if(!buffer.data || buffer.bytes==0 || buffer.bytes>512ull*1024*1024) throw std::runtime_error("Invalid input buffer");
            const auto* first=static_cast<const uint8_t*>(buffer.data);
            data.push_back({first,static_cast<size_t>(buffer.bytes)});
        }
        }
        std::vector<uint8_t> shader;
        if(request->shaderBytes) {
            if(!request->shaderSize || request->shaderSize>512ull*1024*1024)
                throw std::runtime_error("Invalid shader size");
            shader.assign(request->shaderBytes,request->shaderBytes+static_cast<size_t>(request->shaderSize));
        } else shader=load(request->shaderPath);
        *timing=render(data,request->constants,shader,request->output,request->outputBytes,request);
        return 0;
    } catch(const GpuFailure& failure) {
        gpuLastDecision=failure.decision; keepSession=failure.keepSession;
        if(error && errorBytes) { strncpy_s(error,errorBytes,failure.what(),_TRUNCATE); }
    } catch(const std::exception& exception) {
        if(error && errorBytes) { strncpy_s(error,errorBytes,exception.what(),_TRUNCATE); }
    }
    const bool persistent=request && request->version==6 && request->persistentSceneHash;
    bool healthy=false;
    if(persistent && keepSession) {
        try { healthy=gpuSession().device && SUCCEEDED(gpuSession().device->GetDeviceRemovedReason()); } catch(...) { healthy=false; }
    }
    if(!healthy) ShutdownGpuLighting();
    return 1;
}

extern "C" GPU_EXPORT int PrepareGpuSession(const uint8_t* shader,uint64_t shaderSize,char* error,uint32_t errorBytes) {
    try {
        if(!shader || !shaderSize || shaderSize>512ull*1024*1024) throw std::runtime_error("Invalid shader");
        ensureDevice();
        ensurePipeline(std::vector<uint8_t>(shader,shader+static_cast<size_t>(shaderSize)));
        return 0;
    } catch(const std::exception& exception) {
        ShutdownGpuLighting();
        if(error && errorBytes) strncpy_s(error,errorBytes,exception.what(),_TRUNCATE);
        return 1;
    }
}

#ifndef VRAD3_STATIC_GPU
int wmain(int argc,wchar_t** argv) {
    try {
        if(argc!=5) throw std::runtime_error("Usage: gpu_lighting.exe <replay-folder> <shader.dxil> <working.bin> <timing.json>");
        auto started=std::chrono::steady_clock::now(); fs::path input=argv[1];
        auto shader=load(argv[2]),settings=load(input/L"settings.bin");
        if(settings.size()!=64) throw std::runtime_error("Invalid settings size");
        uint32_t constants[24]{}; memcpy(constants,settings.data(),64);
        const float transmission[5]={1,1,1,0.1f,0.01f};
        memcpy(constants+16,transmission,sizeof(transmission));
        const wchar_t* names[]={L"vertices.bin",L"uvs.bin",L"primitives.bin",L"colors.bin",L"materials.bin",L"texture-descriptors.bin",L"textures.bin",L"offsets.bin",L"indices.bin",L"cdf.bin",L"samples.bin",L"lights.bin"};
        std::vector<std::vector<uint8_t>> data;
        for(auto name:names) data.push_back(load(input/name));
        data.push_back(std::vector<uint8_t>(96));
        std::vector<uint8_t> result(static_cast<size_t>(constants[0])*204);
        auto timing=render(data,constants,shader,result.data(),result.size());
        std::ofstream file(argv[3],std::ios::binary); file.write(reinterpret_cast<const char*>(result.data()),result.size());
        if(!file) throw std::runtime_error("Failed to write working records"); file.close();
        double hostMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
        std::ofstream report(argv[4]); report << "{\n  \"samples\": " << constants[1] << ",\n  \"luxels\": " << constants[0] << ",\n  \"gpu_scene_build_ms\": " << timing.buildMs << ",\n  \"gpu_lighting_ms\": " << timing.lightingMs << ",\n  \"host_total_ms\": " << hostMs << ",\n  \"validated_against_vrad3\": false\n}\n";
        if(!report) throw std::runtime_error("Failed to write timing report");
        std::cout << "GPU lighting: " << timing.lightingMs << " ms; complete offline stage: " << hostMs << " ms\n";
        ShutdownGpuLighting();
        return 0;
    } catch(const std::exception& error) { std::cerr << error.what() << "\n"; ShutdownGpuLighting(); return 1; }
}
#endif
