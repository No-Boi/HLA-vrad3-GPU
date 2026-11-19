#pragma once
// Reads the paused child's scene, lights, sky and footprint arrays and packs them into the GPU lighting
// buffers and constants.
#include "gpu_lighting_api.h"
#include <array>
#include <algorithm>
#include <thread>
#include <mutex>
#include <exception>
#include <intrin.h>
#include <map>
#include <functional>

static constexpr const char* emptyBlockReason="Empty block: no lightmap samples to trace";
static bool emptyBlock(const std::string& reason) { return reason.rfind(emptyBlockReason,0)==0; }

static std::map<uint64_t,std::array<float,4>> gpuEmission;
struct GpuEmissionMask { uint32_t width=0,height=0; std::vector<uint8_t> rgba; std::array<float,3> average{}; };
static std::map<uint64_t,GpuEmissionMask> gpuEmissionMasks;

struct SliceStorage {
    virtual ~SliceStorage()=default;
    virtual uint8_t* allocate(size_t index,size_t bytes)=0;
};
struct NativeLighting {
    std::array<std::vector<uint8_t>,13> buffers;
    uint32_t constants[24]{};
    std::array<std::vector<uint8_t>,4> sliceBuffers;
    uint64_t totalPaths=0;
    SliceStorage* storage=nullptr;
    uint64_t facts[8]{};
    uint32_t width=0,adaptiveMin=0; float adaptiveTol=0;
};
static uint32_t pathtraceMinSamples=0; static float pathtraceTolerance=0;
static bool cs2SamplesEnabled() {
    wchar_t value[8]{};
    if(GetEnvironmentVariableW(L"VRAD3_GPU_ESTIMATOR",value,8)!=3 || _wcsicmp(value,L"cs2")!=0) return false;
    return !(GetEnvironmentVariableW(L"VRAD3_GPU_CS2_SAMPLES",value,8)==1 && value[0]==L'0');
}
template<class Function> static void parallelRange(size_t count,size_t minimumPerThread,Function fn) {
    static const unsigned cpus=usableCpuCount();
    size_t threads=std::min<size_t>(cpus,std::max<size_t>(1,count/std::max<size_t>(1,minimumPerThread)));
    if(threads<=1) { if(count) fn(size_t(0),count); return; }
    std::vector<std::thread> pool; std::vector<std::exception_ptr> errors(threads);
    auto run=[&](size_t t) {
        try { fn(count*t/threads,count*(t+1)/threads); } catch(...) { errors[t]=std::current_exception(); }
    };
    try { for(size_t t=1;t<threads;++t) pool.emplace_back(run,t); }
    catch(...) { for(auto& worker:pool) worker.join(); throw; }
    run(0);
    for(auto& worker:pool) worker.join();
    for(auto& error:errors) if(error) std::rethrow_exception(error);
}
static inline uint32_t popcount64(uint64_t value) {
    value=value-((value>>1)&0x5555555555555555ull);
    value=(value&0x3333333333333333ull)+((value>>2)&0x3333333333333333ull);
    value=(value+(value>>4))&0x0f0f0f0f0f0f0f0full;
    return static_cast<uint32_t>((value*0x0101010101010101ull)>>56);
}
static std::vector<uint8_t> copyArray(uintptr_t pointer,size_t count,size_t stride,const char* label="read_array") {
    if(count>134217728 || stride>4096 || count*stride>1024ull*1024*1024 || (count && !pointer))
        throw std::runtime_error("Native input bounds exceeded: count="+std::to_string(count)+" stride="+std::to_string(stride));
    requirePhysicalMemory(count*stride,label);
    std::vector<uint8_t> bytes(count*stride);
    { BlockProfileScope profile(label,bytes.size()); readMemory(pointer,bytes.data(),bytes.size()); } return bytes;
}
static void copyArrayInto(uint8_t* destination,uintptr_t pointer,size_t count,size_t stride,const char* label) {
    if(count>134217728 || stride>4096 || count*stride>1024ull*1024*1024 || (count && !pointer))
        throw std::runtime_error("Native input bounds exceeded: count="+std::to_string(count)+" stride="+std::to_string(stride));
    BlockProfileScope profile(label,count*stride); readMemory(pointer,destination,count*stride);
}
template<class T> static void appendValue(std::vector<uint8_t>& bytes,const T& value) {
    size_t offset=bytes.size(); bytes.resize(offset+sizeof(T)); memcpy(bytes.data()+offset,&value,sizeof(T));
}
// Sky buffer: header width/height at 0/4, padded 3x4 transform at 16, clamp limits at 64, angular constants
// (1, 1/2pi, 1/pi, 1/2) at 76; three float planes at 96. Source: sky image at scene+0x70, plane pointers
// at image+0x20, strides at image+0x2a0.
static std::vector<uint8_t> prepareNativeSky(uintptr_t scene) {
    std::vector<uint8_t> packed(96);
    uintptr_t image=readValue<uintptr_t>(scene+0x70);
    if(!image) return packed;
    uint32_t width=readValue<uint32_t>(image),height=readValue<uint32_t>(image+4);
    uint64_t planeBytes=static_cast<uint64_t>(width)*height*4;
    if(!width || !height || width>8192 || height>8192 || 96+planeBytes*3>512ull*1024*1024)
        throw std::runtime_error("Native sky dimensions exceeded");
    requirePhysicalMemory(96+planeBytes*3,"environment sky");
    packed.resize(static_cast<size_t>(96+planeBytes*3));
    memcpy(packed.data(),&width,4); memcpy(packed.data()+4,&height,4);
    readMemory(scene+0x78,packed.data()+16,48);
    readMemory(address(0x31fcc0),packed.data()+64,12);
    // RVAs of Valve's angular constants.
    const uintptr_t fields[]={0xd5930,0xd58f8,0xd5910,0xd591c};
    for(uint32_t i=0;i<4;++i) readMemory(address(fields[i]),packed.data()+76+i*4,4);
    for(uint32_t c=0;c<3;++c) {
        uintptr_t plane=readValue<uintptr_t>(image+0x20+c*8);
        uint64_t stride=readValue<uint64_t>(image+0x2a0+c*8);
        if(!plane || stride<width*4ull || stride>8192*16ull || readValue<uint64_t>(image+0x1a0+c*8)!=4)
            throw std::runtime_error("Unsupported native sky plane stride");
        for(uint32_t y=0;y<height;++y)
            readMemory(plane+y*stride,packed.data()+96+c*planeBytes+y*width*4ull,width*4ull);
    }
    for(size_t offset=16;offset<packed.size();offset+=4) {
        float value; memcpy(&value,packed.data()+offset,4);
        if(!std::isfinite(value)) throw std::runtime_error("Nonfinite native environment sky");
    }
    const float* limits=reinterpret_cast<const float*>(packed.data()+64);
    for(uint32_t i=0;i<3;++i) if(limits[i]<=0 || limits[i]>1)
        throw std::runtime_error("Unsupported native sky clamp limits");
    const float* angles=reinterpret_cast<const float*>(packed.data()+76);
    if(angles[0]!=1 || std::abs(angles[1]-0.159154943f)>1e-7f ||
       std::abs(angles[2]-0.318309886f)>1e-7f || angles[3]!=0.5f)
        throw std::runtime_error("Unsupported native sky angular constants");
    return packed;
}
// Cookie tree at scene+0x20 (root index at +0x30): 24-byte nodes with a uint32 key at +8 and an image
// pointer at +16.
static std::map<uint32_t,uintptr_t> nativeCookieImages(uintptr_t scene) {
    std::map<uint32_t,uintptr_t> images;
    std::vector<uint16_t> pending{readValue<uint16_t>(scene+0x30)};
    std::set<uint16_t> visited;
    uintptr_t nodes=readValue<uintptr_t>(scene+0x20);
    while(!pending.empty()) {
        uint16_t index=pending.back(); pending.pop_back();
        if(index==0xffff) continue;
        if(!nodes || !visited.insert(index).second) throw std::runtime_error("Invalid native cookie tree");
        uintptr_t node=nodes+static_cast<uintptr_t>(index)*24;
        uint32_t key=readValue<uint32_t>(node+8);
        uintptr_t image=readValue<uintptr_t>(node+16);
        if(!image) throw std::runtime_error("Null native cookie resource");
        if(!images.emplace(key,image).second)
            throw std::runtime_error("Duplicate native cookie key");
        pending.push_back(readValue<uint16_t>(node)); pending.push_back(readValue<uint16_t>(node+2));
    }
    return images;
}
// Cookies share the environment buffer after the sky planes: header words 2/3 give their count and the
// buffer length; each starts with a 16-byte width/height header and three packed planes.
static uint32_t appendNativeCookie(std::vector<uint8_t>& packed,uintptr_t image) {
    uint32_t width=readValue<uint32_t>(image),height=readValue<uint32_t>(image+4);
    uint64_t planeBytes=static_cast<uint64_t>(width)*height*4;
    size_t offset=(packed.size()+15)&~size_t(15);
    if(!width || !height || width>8192 || height>8192 || offset+16+planeBytes*3>512ull*1024*1024)
        throw std::runtime_error("Native cookie dimensions/memory exceeded");
    requirePhysicalMemory(16+planeBytes*3,"projected cookie");
    packed.resize(static_cast<size_t>(offset+16+planeBytes*3));
    memcpy(packed.data()+offset,&width,4); memcpy(packed.data()+offset+4,&height,4);
    for(uint32_t channel=0;channel<3;++channel) {
        uintptr_t plane=readValue<uintptr_t>(image+0x20+channel*8);
        uint64_t stride=readValue<uint64_t>(image+0x2a0+channel*8);
        if(!plane || stride<width*4ull || stride>8192*16ull || readValue<uint64_t>(image+0x1a0+channel*8)!=4)
            throw std::runtime_error("Unsupported native cookie plane stride");
        for(uint32_t y=0;y<height;++y)
            readMemory(plane+y*stride,packed.data()+offset+16+channel*planeBytes+y*width*4ull,width*4ull);
    }
    const float* pixels=reinterpret_cast<const float*>(packed.data()+offset+16);
    for(size_t i=0;i<planeBytes*3/4;++i) if(!std::isfinite(pixels[i]) || pixels[i]<0)
        throw std::runtime_error("Invalid native cookie radiance");
    uint32_t count; memcpy(&count,packed.data()+8,4); ++count;
    uint32_t bytes=static_cast<uint32_t>(packed.size());
    memcpy(packed.data()+8,&count,4); memcpy(packed.data()+12,&bytes,4);
    return static_cast<uint32_t>(offset);
}
struct FlatScene {
    uint32_t triangles=0;
    std::vector<uint8_t> vertices,uvs,colors;
    std::vector<uint32_t> flags;
    std::vector<uint64_t> keys;
    std::vector<uint8_t> uvless;
};
enum class FlatRead { Counts, Geometry, All };
// Hammer's preview scene is instanced; a compile's scene_info.dat is one flat mesh. Mesh 0 is the root, children
// at mesh+0xd8 (count) / +0xe0 (0x50-byte entries: +0 mesh, +0x10 3x4 transform, +0x48 material override).
// Valve flattens them (0x140091730): transform = parent x child, flags |= child mesh+0xa0, nearest override wins.
static bool sceneInstanced(uintptr_t scene) {
    if(readValue<uint32_t>(scene+0x180)!=1) return true;
    return readValue<uint32_t>(readValue<uintptr_t>(readValue<uintptr_t>(scene+0x188))+0xd8)!=0;
}
static FlatScene flattenScene(uintptr_t scene,FlatRead read) {
    const uint32_t meshes=readValue<uint32_t>(scene+0x180);
    if(!meshes || meshes>65536) throw std::runtime_error("Unexpected scene mesh count");
    const uintptr_t root=readValue<uintptr_t>(readValue<uintptr_t>(scene+0x188));
    struct Placement { uintptr_t mesh=0; float xf[12]{1,0,0,0,0,1,0,0,0,0,1,0}; uintptr_t overrides=0; uint32_t flags=0; bool moved=false; };
    std::vector<Placement> placed;
    Placement top; top.mesh=root; top.flags=readValue<uint32_t>(root+0xa0);
    if(readValue<uint32_t>(root+0x10)) placed.push_back(top);
    std::function<void(const Placement&,int)> walk=[&](const Placement& parent,int depth) {
        const uint32_t children=readValue<uint32_t>(parent.mesh+0xd8);
        if(!children) return;
        if(depth>=64 || children>(1u<<20)) throw std::runtime_error("Unsupported scene instance tree");
        const auto entries=copyArray(readValue<uintptr_t>(parent.mesh+0xe0),children,0x50,"read_scene_instances");
        for(uint32_t i=0;i<children;++i) {
            const uint8_t* entry=entries.data()+i*0x50ull;
            Placement child; memcpy(&child.mesh,entry,8); child.moved=true;
            float local[12]; memcpy(local,entry+0x10,sizeof(local));
            for(float value:local) if(!std::isfinite(value)) throw std::runtime_error("Nonfinite scene instance transform");
            for(int r=0;r<3;++r) for(int c=0;c<4;++c) {
                const float* p=parent.xf+r*4;
                child.xf[r*4+c]=p[2]*local[8+c]+p[1]*local[4+c]+p[0]*local[c]+(c==3?p[3]:0.0f);
            }
            uintptr_t overrides; memcpy(&overrides,entry+0x48,8);
            child.overrides=overrides?overrides:parent.overrides;
            child.flags=parent.flags|readValue<uint32_t>(child.mesh+0xa0);
            walk(child,depth+1);
            if(!readValue<uint32_t>(child.mesh+0x10)) continue;
            if(placed.size()>=(1u<<20)) throw std::runtime_error("Too many scene instances");
            placed.push_back(child);
        }
    };
    walk(top,0);
    uint64_t total=0;
    for(const auto& p:placed) {
        const uint32_t vertices=readValue<uint32_t>(p.mesh+0x10),triangles=readValue<uint32_t>(p.mesh+0x28),keys=readValue<uint32_t>(p.mesh+0x58);
        if(!triangles || vertices!=static_cast<uint64_t>(triangles)*3) throw std::runtime_error("Unsupported scene mesh layout");
        const uint32_t colors=readValue<uint32_t>(p.mesh+0x70),uvs=readValue<uint32_t>(p.mesh+0x88);
        if(read==FlatRead::All && ((uvs && uvs<vertices) || (keys && keys<triangles)))
            throw std::runtime_error("Unsupported scene mesh attributes: "+std::to_string(triangles)+" triangles, "+std::to_string(colors)
                                     +" colors, "+std::to_string(uvs)+" uvs, "+std::to_string(keys)+" material keys");
        total+=triangles;
    }
    if(!total || total>(1u<<26)) throw std::runtime_error("Unsupported flattened scene size");
    FlatScene flat; flat.triangles=static_cast<uint32_t>(total);
    if(read==FlatRead::Counts) return flat;
    requirePhysicalMemory(total*(read==FlatRead::All?(48+4+24+12+8+1):(48+4)),"flattened scene");
    flat.vertices.resize(total*48); flat.flags.resize(total);
    if(read==FlatRead::All) { flat.uvs.resize(total*24); flat.colors.resize(total*12); flat.keys.resize(total); flat.uvless.resize(total); }
    std::map<uintptr_t,std::pair<std::vector<uint8_t>,std::vector<uint8_t>>> overrideKeys;
    uint8_t defaultUv[8]{},defaultColor[12]{};
    if(read==FlatRead::All) { readMemory(address(0x32dbf0),defaultUv,8); readMemory(address(0x31fce0),defaultColor,12); }
    uint64_t at=0;
    for(const auto& p:placed) {
        const uint32_t triangles=readValue<uint32_t>(p.mesh+0x28),vertices=triangles*3;
        const auto corners=copyArray(readValue<uintptr_t>(p.mesh+0x18),vertices,16,"read_scene_vertices");
        const auto flags=copyArray(readValue<uintptr_t>(p.mesh+0x30),triangles,2,"read_scene_flags");
        const float* m=p.xf;
        const bool mirror=p.moved && (m[0]*(m[5]*m[10]-m[6]*m[9])-m[1]*(m[4]*m[10]-m[6]*m[8])+m[2]*(m[4]*m[9]-m[5]*m[8]))<0;
        // A mirroring instance has corners 1 and 2 swapped so the winding matches Valve's object-space culling.
        const uint32_t order[3]={0,mirror?2u:1u,mirror?1u:2u};
        uint8_t* outCorners=flat.vertices.data()+at*48;
        for(uint32_t t=0;t<triangles;++t) for(uint32_t k=0;k<3;++k) {
            const uint8_t* source=corners.data()+(t*3ull+order[k])*16; uint8_t* target=outCorners+(t*3ull+k)*16;
            if(!p.moved) { memcpy(target,source,16); continue; }
            float v[3]; memcpy(v,source,12);
            const float world[4]={m[0]*v[0]+m[1]*v[1]+m[2]*v[2]+m[3],m[4]*v[0]+m[5]*v[1]+m[6]*v[2]+m[7],m[8]*v[0]+m[9]*v[1]+m[10]*v[2]+m[11],1.0f};
            memcpy(target,world,16);
        }
        for(uint32_t t=0;t<triangles;++t) { uint16_t value; memcpy(&value,flags.data()+t*2ull,2); flat.flags[at+t]=p.flags|value; }
        if(read==FlatRead::All) {
            if(!readValue<uint32_t>(p.mesh+0x88)) {
                for(uint64_t c=0;c<vertices;++c) memcpy(flat.uvs.data()+(at*3+c)*8,defaultUv,8);
                std::fill_n(flat.uvless.data()+at,triangles,uint8_t(1));
            } else {
                const auto uvs=copyArray(readValue<uintptr_t>(p.mesh+0x90),vertices,8,"read_scene_uvs");
                for(uint32_t t=0;t<triangles;++t) for(uint32_t k=0;k<3;++k)
                    memcpy(flat.uvs.data()+((at+t)*3+k)*8,uvs.data()+(t*3ull+order[k])*8,8);
            }
            const uint32_t colored=std::min(readValue<uint32_t>(p.mesh+0x70),triangles);
            if(colored) {
                const auto colors=copyArray(readValue<uintptr_t>(p.mesh+0x78),colored,12,"read_scene_colors");
                memcpy(flat.colors.data()+at*12,colors.data(),colors.size());
            }
            for(uint32_t t=colored;t<triangles;++t) memcpy(flat.colors.data()+(at+t)*12,defaultColor,12);
            if(readValue<uint32_t>(p.mesh+0x58)) {
                const auto keys=copyArray(readValue<uintptr_t>(p.mesh+0x60),triangles,8,"read_scene_material_keys");
                memcpy(flat.keys.data()+at,keys.data(),keys.size());
            }
            if(p.overrides) {
                auto found=overrideKeys.find(p.overrides);
                if(found==overrideKeys.end()) {
                    const int32_t n=readValue<int32_t>(p.overrides);
                    if(n>65536) throw std::runtime_error("Unsupported scene material override");
                    std::pair<std::vector<uint8_t>,std::vector<uint8_t>> pair;
                    if(n>0) { pair.first=copyArray(readValue<uintptr_t>(p.overrides+8),n,8,"read_scene_overrides");
                              pair.second=copyArray(readValue<uintptr_t>(p.overrides+0x20),n,8,"read_scene_overrides"); }
                    found=overrideKeys.emplace(p.overrides,std::move(pair)).first;
                }
                const uint64_t* from=reinterpret_cast<const uint64_t*>(found->second.first.data());
                const uint64_t* to=reinterpret_cast<const uint64_t*>(found->second.second.data());
                const size_t n=found->second.first.size()/8;
                for(uint32_t t=0;t<triangles;++t) {
                    uint64_t& key=flat.keys[at+t];
                    for(size_t i=0;i<n;++i) if(from[i]==key) { key=to[i]; break; }
                }
            }
        }
        at+=triangles;
    }
    return flat;
}
constexpr uint32_t kMaxGlobalSamples=1u<<28;
static int nativeLightPathEmitters=-1;
static const char* nativeLightPathTree="";
static NativeLighting prepareNative(uint32_t spp,uint32_t directLights=0,uint32_t probeCells=0,bool sceneResident=false,
                                    uint32_t rejectSamples=0) {
    const bool direct=directLights!=0;
    NativeLighting input;
    // Valve globals (RVAs): scene 32aa20; luxel count 32ab88; block width/height 32aba8/32abac; footprint
    // offsets 32ab00, indices 32acd8, cdf 32ab58; samples 32ad30 (72-byte records), count 32ad28.
    uint32_t count=probeCells?probeCells:rejectSamples?rejectSamples:countAt(0x32ab88);
    if((probeCells?count>1048576:rejectSamples?count>(1u<<22):!validGpuBlock(count,countAt(0x32aba8),countAt(0x32abac))) || !spp || spp>4096)
        throw std::runtime_error("Unsupported native block settings");
    uintptr_t scene=pointerAt(0x32aa20);
    uintptr_t mesh=readValue<uintptr_t>(readValue<uintptr_t>(scene+0x188));
    const bool instanced=sceneInstanced(scene);
    FlatScene flat;
    if(instanced) flat=flattenScene(scene,sceneResident?FlatRead::Counts:FlatRead::All);
    uint32_t vertexCount=instanced?flat.triangles*3:readValue<uint32_t>(mesh+0x10),triangles=instanced?flat.triangles:readValue<uint32_t>(mesh+0x28);
    if(!instanced && (vertexCount!=static_cast<uint64_t>(triangles)*3 || !triangles ||
       readValue<uint32_t>(mesh+0x58)<triangles || readValue<uint32_t>(mesh+0x70)<triangles ||
       readValue<uint32_t>(mesh+0x88)<vertexCount))
        throw std::runtime_error("Unsupported native mesh layout");
    auto& data=input.buffers;
    input.facts[0]=vertexCount; input.facts[1]=triangles;
    if(uintptr_t sky=readValue<uintptr_t>(scene+0x70)) { input.facts[4]=readValue<uint32_t>(sky); input.facts[5]=readValue<uint32_t>(sky+4); }
    uintptr_t materialObject=scene+0x128;
    input.facts[2]=readValue<uint32_t>(materialObject+0x10); input.facts[3]=readValue<uint32_t>(materialObject+0x40);
    if(!sceneResident) {
    data[12]=prepareNativeSky(scene);
    data[0]=instanced?std::move(flat.vertices):copyArray(readValue<uintptr_t>(mesh+0x18),vertexCount,16,"read_vertices");
    data[1]=instanced?std::move(flat.uvs):copyArray(readValue<uintptr_t>(mesh+0x90),vertexCount,8,"read_uvs");
    data[3]=instanced?std::move(flat.colors):copyArray(readValue<uintptr_t>(mesh+0x78),triangles,12,"read_colors");
    uintptr_t table=readValue<uintptr_t>(materialObject);
    uint32_t capacity=readValue<uint32_t>(materialObject+8),used=readValue<uint32_t>(materialObject+0x10);
    uint32_t textureCount=readValue<uint32_t>(materialObject+0x40);
    if(capacity>65536 || textureCount>65536) throw std::runtime_error("Native resource counts exceeded");
    std::map<uint64_t,uint32_t> dense; std::vector<const GpuEmissionMask*> masks;
    std::map<uint64_t,uint32_t> averaged; std::vector<uint8_t> averagedPacked;
    for(uint32_t i=0;i<capacity;++i) {
        uintptr_t entry=table+static_cast<uintptr_t>(i)*140;
        if(readValue<uint32_t>(entry)&0x80000000u) continue;
        uintptr_t material=entry+12;
        float numeric[10]; readMemory(material,numeric,sizeof(numeric));
        for(float value:numeric) if(!std::isfinite(value)) throw std::runtime_error("Nonfinite native material");
        int32_t slots[4]={-1,-1,-1,readValue<int32_t>(material+88)};
        for(uint32_t slot=0;slot<3;++slot) {
            if(!readValue<uint64_t>(material+40+slot*16)) continue;
            slots[slot]=readValue<int32_t>(material+52+slot*16);
            if(slots[slot]<0 || static_cast<uint32_t>(slots[slot])>=textureCount)
                throw std::runtime_error("Native material texture out of bounds");
        }
        if(slots[2]>=0 && (slots[3]<0 || slots[3]>3))
            throw std::runtime_error("Native mask channel out of bounds");
        if(numeric[9]<0) throw std::runtime_error("Native opacity out of bounds");
        const uint64_t key=readValue<uint64_t>(entry+4);
        dense[key]=static_cast<uint32_t>(dense.size());
        float packed[12]={numeric[0],numeric[1],numeric[2],numeric[9],numeric[3],numeric[4],numeric[5],0,
                          numeric[6],numeric[7],numeric[8],0};
        if(auto glow=gpuEmission.find(key); glow!=gpuEmission.end()) std::copy(glow->second.begin(),glow->second.end(),packed+8);
        if(auto mask=gpuEmissionMasks.find(key); mask!=gpuEmissionMasks.end()) {
            if(instanced) {
                float copy[12]; std::copy(packed,packed+12,copy); std::copy(mask->second.average.begin(),mask->second.average.end(),copy+8);
                averaged[key]=used+static_cast<uint32_t>(averaged.size());
                for(float value:copy) appendValue(averagedPacked,value);
                for(int32_t value:slots) appendValue(averagedPacked,value);
            }
            masks.push_back(&mask->second); packed[7]=static_cast<float>(textureCount+masks.size());
        }
        for(float value:packed) appendValue(data[4],value);
        for(int32_t value:slots) appendValue(data[4],value);
    }
    if(dense.size()!=used || dense.empty()) throw std::runtime_error("Native material occupancy mismatch");
    data[4].insert(data[4].end(),averagedPacked.begin(),averagedPacked.end());
    std::vector<uint8_t> flagBytes,keys;
    if(!instanced) {
        flagBytes=copyArray(readValue<uintptr_t>(mesh+0x30),triangles,2,"read_flags");
        keys=copyArray(readValue<uintptr_t>(mesh+0x60),triangles,8,"read_material_keys");
    }
    for(uint32_t i=0;i<triangles;++i) {
        uint32_t flags; uint64_t key;
        if(instanced) { flags=flat.flags[i]; key=flat.keys[i]; }
        else { uint16_t value; memcpy(&value,flagBytes.data()+i*2,2); flags=value; memcpy(&key,keys.data()+i*8,8); }
        auto found=dense.find(key); if(found==dense.end()) throw std::runtime_error("Native primitive material missing");
        uint32_t material=found->second;
        if(instanced && flat.uvless[i]) if(auto copy=averaged.find(key); copy!=averaged.end()) material=copy->second;
        appendValue(data[2],flags); appendValue(data[2],material);
    }
    uintptr_t textures=readValue<uintptr_t>(materialObject+0x48);
    for(uint32_t i=0;i<textureCount;++i) {
        uintptr_t texture=textures+static_cast<uintptr_t>(i)*32;
        uint32_t width=readValue<uint32_t>(texture+0x10),height=readValue<uint32_t>(texture+0x14);
        if(!width || !height || width>8192 || height>8192 || readValue<uint32_t>(texture+0x18)!=4)
            throw std::runtime_error("Native GPU requires RGBA8 textures");
        appendValue(data[5],static_cast<uint32_t>(data[6].size())); appendValue(data[5],width);
        appendValue(data[5],height); appendValue(data[5],uint32_t(4));
        auto bytes=copyArray(readValue<uintptr_t>(texture+8),static_cast<size_t>(width)*height,4);
        if(data[6].size()+bytes.size()>512ull*1024*1024) throw std::runtime_error("Native texture memory exceeded");
        data[6].insert(data[6].end(),bytes.begin(),bytes.end());
    }
    for(const GpuEmissionMask* mask:masks) {
        appendValue(data[5],static_cast<uint32_t>(data[6].size())); appendValue(data[5],mask->width);
        appendValue(data[5],mask->height); appendValue(data[5],uint32_t(4));
        if(data[6].size()+mask->rgba.size()>512ull*1024*1024) throw std::runtime_error("Native texture memory exceeded");
        data[6].insert(data[6].end(),mask->rgba.begin(),mask->rgba.end());
    }
    if(data[5].empty()) data[5].resize(16);
    if(data[6].empty()) data[6].resize(4);
    }
    if(!probeCells && !rejectSamples) {
        input.width=countAt(0x32aba8);
        if(!direct && cs2SamplesEnabled() && pathtraceMinSamples && pathtraceMinSamples<spp && input.width && input.width<65536) {
            input.adaptiveMin=pathtraceMinSamples; input.adaptiveTol=pathtraceTolerance;
        }
    }
    if(rejectSamples) {
        data[7].resize((count+1ull)*4); uint32_t* ranks=reinterpret_cast<uint32_t*>(data[7].data());
        for(uint32_t i=0;i<=count;++i) ranks[i]=i;
        data[9].resize(384);
    }
    else if(!probeCells) data[7]=copyArray(pointerAt(0x32ab00),count+1,4,"read_offsets");
    else { data[7].resize((count+1ull)*4); data[8].resize(count*12ull); data[9].resize(count*12ull); data[10].resize(count*36ull); }
    const uint32_t* offsets=reinterpret_cast<const uint32_t*>(data[7].data());
    uint32_t entries=offsets[count],globalSampleCount=probeCells?count:countAt(0x32ad28);
    if(offsets[0]==0 && ((!probeCells && !entries) || !globalSampleCount)) throw std::runtime_error(emptyBlockReason);
    if(offsets[0]!=0 || globalSampleCount>kMaxGlobalSamples) throw std::runtime_error("Invalid native footprint");
    for(uint32_t luxel=0;luxel<count;++luxel) {
        if(offsets[luxel]>offsets[luxel+1] || offsets[luxel+1]>entries)
            throw std::runtime_error("Invalid native CSR");
        if(offsets[luxel]!=offsets[luxel+1]) input.totalPaths+=spp;
    }
    if(!sceneResident) for(uint32_t slot:{0u,1u,3u}) {
        const float* values=reinterpret_cast<const float*>(data[slot].data());
        for(size_t i=0;i<data[slot].size()/4;++i)
            if(!std::isfinite(values[i])) throw std::runtime_error("Nonfinite native input");
    }
    uint32_t lightCount=readValue<uint32_t>(scene);
    if(lightCount>65536) throw std::runtime_error("Native light count exceeded");
    uintptr_t lightTable=readValue<uintptr_t>(scene+8);
    auto cookieImages=nativeCookieImages(scene);
    std::map<uintptr_t,uint32_t> cookieOffsets;
    std::vector<uintptr_t> pathList;
    uint32_t cookieLights=0; std::vector<uintptr_t> directList;
    for(uint32_t i=0;i<lightCount;++i) {
        uintptr_t light=lightTable+static_cast<uintptr_t>(i)*264;
        uint32_t type=readValue<uint32_t>(light);
        if(type<1 || type>4) throw std::runtime_error("Native GPU light type "+std::to_string(type)+" not implemented");
        if(!readValue<uint8_t>(light+0x84) || !(readValue<uint32_t>(light+0x104)&(direct?2:1))) continue;
        if(direct) directList.push_back(light);
        else pathList.push_back(light);
        uint32_t cookie=0; float projectionScale=0;
        auto found=cookieImages.find(readValue<uint32_t>(light+0x38));
        if(type==4 && readValue<uint32_t>(light+0x38) && found!=cookieImages.end()) {
            auto cached=cookieOffsets.find(found->second);
            if(cached==cookieOffsets.end()) cached=cookieOffsets.emplace(found->second,sceneResident?0u:appendNativeCookie(data[12],found->second)).first;
            cookie=cached->second;
            const float extent=std::max(readValue<float>(light+0x98),readValue<float>(light+0x9c));
            if(!std::isfinite(extent) || extent<=0) throw std::runtime_error("Unsupported native ortho cookie size");
            projectionScale=2/extent;
            ++cookieLights;
        }
        if(type==3 && readValue<uint32_t>(light+0x38) && found!=cookieImages.end()) {
            auto cached=cookieOffsets.find(found->second);
            if(cached==cookieOffsets.end()) cached=cookieOffsets.emplace(found->second,sceneResident?0u:appendNativeCookie(data[12],found->second)).first;
            cookie=cached->second;
            float halfFov=readValue<float>(light+0x34);
            if(!std::isfinite(halfFov) || halfFov<=0 || halfFov>=90 || readValue<float>(light+0xd4)<=0)
                throw std::runtime_error("Unsupported native cookie projection angle");
            projectionScale=1/std::tan(halfFov*0.01745329252f);
            ++cookieLights;
        }
        appendValue(data[11],type); appendValue(data[11],uint32_t(readValue<uint32_t>(light+0x8c)==0));
        appendValue(data[11],readValue<uint32_t>(light+0xe4)); appendValue(data[11],readValue<uint32_t>(light+0x104));
        // Offsets in Valve's 264-byte light record copied into the 112-byte GPU record (0xffffffff = zero).
        const uint32_t fields[24]={0x10,0x14,0x18,0x1c,0xac,0xb0,0xb4,0xe0,0xb8,0xbc,0xc0,0x90,
                                  0x24,0x28,0x2c,0xffffffff,0xd4,0xdc,0x20,0xffffffff,0xc4,0xc8,0xcc,0xffffffff};
        for(uint32_t j=0;j<24;++j) {
            uint32_t field=fields[j];
            if(direct && j<3) field=4+j*4;
            // light_ortho keeps its width and height (+0x98, +0x9c) where spots keep their cone.
            if(type==4 && (j==16 || j==17)) field=j==16?0x98:0x9c;
            float value=field==0xffffffff?0:readValue<float>(light+field);
            if(j==19) value=static_cast<float>(cookie);
            if(j==23) value=projectionScale;
            if(!std::isfinite(value)) throw std::runtime_error("Nonfinite native light");
            appendValue(data[11],value);
        }
    }
    wchar_t lightPathsSetting[8]{};
    GetEnvironmentVariableW(L"VRAD3_GPU_LIGHT_PATHS",lightPathsSetting,8);
    if(!direct && !probeCells) {
        nativeLightPathEmitters=0; nativeLightPathTree=sceneResident?"resident":"none";
        for(uintptr_t light:pathList) { const uint32_t type=readValue<uint32_t>(light); if(type==1 || type==3 || type==4) ++nativeLightPathEmitters; }
    }
    if(!sceneResident && !direct && !probeCells && std::wstring(lightPathsSetting)==L"1") {
        // Valve's emitter importance tree (scene+0xd0): 4-wide SoA nodes (0xf0 bytes) and leaf packets (0xe0);
        // the table maps light x 16x16 emission cells to 2-bit child trails (0x140087030 samples, 0x140087880 evaluates).
        const uintptr_t tree=scene+0xd0;
        const int32_t nodes=readValue<int32_t>(tree),records=readValue<int32_t>(tree+0x18),cells=readValue<int32_t>(tree+0x30);
        if(nodes<0 || nodes>(1<<20) || records<=0 || records>(1<<20) || cells!=static_cast<int64_t>(lightCount)*256)
            std::cout << "GPU light paths: no usable Valve emitter tree (nodes=" << nodes << " records=" << records << " cells=" << cells << ")\n";
        else {
            std::vector<uint32_t> valveToGpu(lightCount,0xffffffffu),gpuToValve;
            for(uintptr_t light:pathList) {
                const uint32_t index=static_cast<uint32_t>((light-lightTable)/264);
                valveToGpu[index]=static_cast<uint32_t>(gpuToValve.size()); gpuToValve.push_back(index);
            }
            auto& packed=data[12];
            const size_t base=(packed.size()+15)&~size_t(15);
            const size_t nodeBytes=nodes*240ull,recordBytes=records*224ull,cellBytes=cells*4ull;
            const size_t nodesAt=base+48,recordsAt=nodesAt+nodeBytes,cellsAt=recordsAt+recordBytes,
                         mapAt=cellsAt+cellBytes,reverseAt=mapAt+lightCount*4ull,end=reverseAt+gpuToValve.size()*4;
            packed.resize(end);
            const uint32_t header[12]={0x45525456u,uint32_t(nodes),uint32_t(records),uint32_t(cells),lightCount,uint32_t(gpuToValve.size()),
                                       uint32_t(nodesAt),uint32_t(recordsAt),uint32_t(cellsAt),uint32_t(mapAt),uint32_t(reverseAt),0};
            memcpy(packed.data()+base,header,48);
            if(nodes) readMemory(readValue<uintptr_t>(tree+8),packed.data()+nodesAt,nodeBytes);
            readMemory(readValue<uintptr_t>(tree+0x20),packed.data()+recordsAt,recordBytes);
            readMemory(readValue<uintptr_t>(tree+0x38),packed.data()+cellsAt,cellBytes);
            memcpy(packed.data()+mapAt,valveToGpu.data(),lightCount*4ull);
            if(!gpuToValve.empty()) memcpy(packed.data()+reverseAt,gpuToValve.data(),gpuToValve.size()*4);
            const uint32_t offset=static_cast<uint32_t>(base); memcpy(packed.data()+92,&offset,4);
            std::cout << "GPU light paths: Valve emitter tree nodes=" << nodes << " packets=" << records << '\n';
            nativeLightPathTree="read";
        }
    }
    input.facts[6]=cookieLights; input.facts[7]=cookieOffsets.size();
    if(cookieLights) std::cout << "Native projected cookies: lights=" << cookieLights << " images=" << cookieOffsets.size() << '\n';
    input.constants[0]=count; input.constants[1]=spp; input.constants[2]=countAt(0x32aae8); input.constants[3]=0;
    readMemory(scene+0x54,input.constants+4,12); readMemory(scene+0x44,input.constants+7,12);
    for(uint32_t i=4;i<10;++i) { float value; memcpy(&value,input.constants+i,4); if(!std::isfinite(value)) throw std::runtime_error("Nonfinite native sky"); }
    input.constants[10]=readValue<uint8_t>(scene+0x6c)!=0;
    input.constants[11]=static_cast<uint32_t>(data[11].size()/112);
    // Pathtrace ray masks: nearest hit 0xc0b0, shadow rays 0xc0b1.
    input.constants[12]=0xc0b0; input.constants[13]=0xc0b1; input.constants[14]=instanced?0:readValue<uint32_t>(mesh+0xa0);
    // Valve globals (RVAs): transmission W at 31fcc0 (3 floats), epsilon d58f0, min cosine d58e4,
    // roulette ceiling d5928, directional connection distance d5968.
    readMemory(address(0x31fcc0),input.constants+16,12);
    input.constants[19]=readValue<uint32_t>(address(0xd58f0));
    input.constants[20]=readValue<uint32_t>(address(0xd58e4));
    const float* transmission=reinterpret_cast<const float*>(input.constants+16);
    for(uint32_t i=0;i<5;++i) if(!std::isfinite(transmission[i]))
        throw std::runtime_error("Nonfinite native transmission constants");
    if(transmission[3]<=0 || transmission[4]<=0)
        throw std::runtime_error("Invalid native transmission constants");
    std::cout << "Native transmission constants: W=" << transmission[0] << ',' << transmission[1] << ','
              << transmission[2] << " epsilon=" << transmission[3] << " minCos=" << transmission[4] << '\n';
    input.constants[21]=readValue<uint32_t>(address(0xd5928));
    float roulette=readValue<float>(address(0xd5928));
    if(!std::isfinite(roulette) || roulette<=0 || roulette>=1)
        throw std::runtime_error("Invalid native roulette ceiling");
    std::cout << "Native roulette ceiling: " << roulette << '\n';
    input.constants[22]=readValue<uint32_t>(address(0xd5968));
    float directionalDistance=readValue<float>(address(0xd5968));
    if(!std::isfinite(directionalDistance) || directionalDistance<=0)
        throw std::runtime_error("Invalid native directional connection distance");
    std::cout << "Native input sizes: width=" << countAt(0x32aba8) << " height=" << countAt(0x32abac)
              << " luxels=" << count << " spp=" << spp << " footprint_entries=" << entries
              << " global_samples=" << globalSampleCount << " buffer_bytes=";
    for(const auto& bytes:data) std::cout << bytes.size() << ',';
    std::cout << '\n';
    if(direct) {
        // Directlight mode: top bit selects it, low byte is the stored-light limit.
        input.constants[3]=0x80000000u|directLights;
        input.constants[12]=readValue<uint32_t>(address(0x17ae40));
        // Only the directlight pass-2 mask 0x60b0 (global 17ae40; pass 1 adds bit 0) is verified.
        if(input.constants[12]!=0x60b0u) throw std::runtime_error("Unverified directlight visibility mask");
        input.constants[16]=readValue<uint32_t>(address(0xd5954));
        float expansion; memcpy(&expansion,input.constants+16,4);
        if(!std::isfinite(expansion) || expansion<0) throw std::runtime_error("Invalid directlight bounds expansion");
        for(uintptr_t light:directList) {
            uint8_t meta[112]{};
            int32_t id=readValue<int32_t>(light+0x78); memcpy(meta,&id,4);
            uint32_t shadow=readValue<uint8_t>(light+0x44)!=0; memcpy(meta+4,&shadow,4);
            uint32_t noExtent=readValue<float>(light+0x90)==0 && readValue<float>(light+0x94)==0; memcpy(meta+8,&noExtent,4);
            readMemory(light+0xec,meta+16,12); readMemory(light+0xf8,meta+32,12);
            readMemory(light+0x7c,meta+76,4);
            if(id>255) throw std::runtime_error("Native directlight runtime light ID out of byte range");
            const float* bounds=reinterpret_cast<const float*>(meta+16);
            for(uint32_t j: {0u,1u,2u,4u,5u,6u}) if(!std::isfinite(bounds[j])) throw std::runtime_error("Nonfinite directlight bounds");
            for(uint32_t j=0;j<3;++j) if(bounds[j]>bounds[j+4]) throw std::runtime_error("Invalid directlight bounds");
            float divisor; memcpy(&divisor,meta+76,4);
            if(id>=0 && (!std::isfinite(divisor) || divisor<=0)) throw std::runtime_error("Invalid directlight scalar divisor");
            data[11].insert(data[11].end(),meta,meta+112);
        }
    }
    if(!direct) {
        for(uintptr_t light:pathList) {
            uint8_t meta[112]{};
            readMemory(light+0xec,meta+16,12); readMemory(light+0xf8,meta+32,12);
            data[11].insert(data[11].end(),meta,meta+112);
        }
    }
    if(data[11].empty()) data[11].resize(112);
    return input;
}

static int readNativeSlice(void* context,uint32_t first,uint32_t maxLuxels,uint64_t budget,
                           GpuLightingSlice* slice,char* error,uint32_t errorBytes) {
    try {
        auto& input=*static_cast<NativeLighting*>(context);
        input.sliceBuffers={};
        auto& data=input.sliceBuffers;
        uint8_t* slicePointer[4]{}; size_t sliceSize[4]{};
        auto allocate=[&](size_t index,size_t bytes) -> uint8_t* {
            requirePhysicalMemory(bytes,"slice array");
            if(input.storage) slicePointer[index]=input.storage->allocate(index,bytes);
            else { data[index].assign(bytes,0); slicePointer[index]=data[index].data(); }
            sliceSize[index]=bytes; return slicePointer[index];
        };
        uint32_t count=input.constants[0],globalSamples=countAt(0x32ad28);
        if(first>=count || !maxLuxels) throw std::runtime_error("Invalid native slice range");
        const auto* offsets=reinterpret_cast<const uint32_t*>(input.buffers[7].data());
        MEMORYSTATUSEX memory{}; memory.dwLength=sizeof(memory);
        if(!GlobalMemoryStatusEx(&memory)) throw std::runtime_error("Cannot query slice RAM");
        const uint64_t usable=usableMemory(memory),ramBudget=usable>260ull*1024*1024?usable-260ull*1024*1024:0;
        budget=std::min(budget,ramBudget/2);
        constexpr uint64_t kSliceBufferBytes=512ull*1024*1024,kSliceEntries=kSliceBufferBytes/4;
        uint32_t n=0;
        while(n<maxLuxels && first+n<count) {
            uint64_t entries=offsets[first+n+1]-offsets[first];
            uint64_t bytes=entries*44+(n+2ull)*4+(n+1ull)*408;
            if(entries>kSliceEntries || bytes>budget) break;
            ++n;
        }
        if(!n) throw std::runtime_error("A single luxel footprint exceeds slice VRAM/RAM budget");
        auto tileRows=[&] {
            if(!input.adaptiveMin || first+n>=count) return;
            const uint64_t row=4ull*input.width,end=(first+n)/row*row;
            if(end>first) n=static_cast<uint32_t>(end-first);
        };
        tileRows();
        uint32_t base=offsets[first],entries=offsets[first+n]-base;
        requirePhysicalMemory(entries*48ull+(n+1ull)*4+4ull*1024*1024,"slice gather");
        copyArrayInto(allocate(0,(n+1ull)*4),pointerAt(0x32ab00)+first*4ull,n+1,4,"read_slice_offsets");
        for(uint32_t i=0;i<=n;++i) {
            uint32_t actual; memcpy(&actual,slicePointer[0]+i*4ull,4);
            if(actual!=offsets[first+i]) throw std::runtime_error("Paused child CSR changed");
            uint32_t value=offsets[first+i]-base;
            memcpy(slicePointer[0]+i*4ull,&value,4);
        }
        if(entries>134217728) throw std::runtime_error("Native input bounds exceeded: count="+std::to_string(entries)+" stride=4");
        { uint8_t* targets[2]={allocate(1,std::max<size_t>(4,entries*4ull)),allocate(2,std::max<size_t>(4,entries*4ull))};
          const uintptr_t sources[2]={pointerAt(0x32acd8)+base*4ull,pointerAt(0x32ab58)+base*4ull};
          if(entries && (!sources[0] || !sources[1])) throw std::runtime_error("Native footprint arrays missing");
          constexpr size_t piece=1u<<20; const size_t pieces=(static_cast<size_t>(entries)+piece-1)/piece;
          BlockProfileScope profile("read_slice_arrays",entries*8ull);
          parallelRange(pieces*2,1,[&](size_t from,size_t to) {
              for(size_t k=from;k<to;++k) {
                  const size_t array=k/pieces,start=(k%pieces)*piece,count=std::min<size_t>(piece,entries-start);
                  readMemory(sources[array]+start*4,targets[array]+start*4,count*4);
              }
          }); }
        auto* indices=reinterpret_cast<uint32_t*>(slicePointer[1]);
        const auto* cdf=reinterpret_cast<const float*>(slicePointer[2]);
        std::vector<uint32_t> ids,prefix; std::vector<uint64_t> bits;
        uint32_t firstId=0;
        for(;;) {
            { BlockProfileScope profile("gather_sample_ids",entries*4ull);
              if(entries) {
                  std::mutex lock; uint32_t lowest=UINT32_MAX,highest=0;
                  parallelRange(entries,1u<<16,[&](size_t from,size_t to) {
                      auto bounds=std::minmax_element(indices+from,indices+to);
                      std::lock_guard<std::mutex> guard(lock);
                      lowest=std::min(lowest,*bounds.first); highest=std::max(highest,*bounds.second);
                  });
                  firstId=lowest;
                  if(highest>=globalSamples) throw std::runtime_error("Native footprint index out of bounds");
                  uint64_t span=static_cast<uint64_t>(highest)-firstId+1,words=(span+63)/64;
                  requirePhysicalMemory(words*12,"slice sample bitmap");
                  bits.assign(static_cast<size_t>(words),0);
                  parallelRange(entries,1u<<16,[&](size_t from,size_t to) {
                      for(size_t i=from;i<to;++i) {
                          uint32_t value=indices[i]-firstId;
                          _InterlockedOr64(reinterpret_cast<volatile __int64*>(&bits[value>>6]),static_cast<__int64>(1ull<<(value&63)));
                      }
                  });
                  prefix.resize(static_cast<size_t>(words)+1);
                  uint32_t running=0;
                  for(size_t w=0;w<bits.size();++w) { prefix[w]=running; running+=popcount64(bits[w]); }
                  prefix[bits.size()]=running;
                  ids.resize(running);
                  parallelRange(bits.size(),1u<<14,[&](size_t from,size_t to) {
                      for(size_t w=from;w<to;++w) {
                          uint32_t at=prefix[w];
                          for(uint64_t word=bits[w];word;word&=word-1) {
                              unsigned long bit; _BitScanForward64(&bit,word);
                              ids[at++]=firstId+static_cast<uint32_t>(w*64+bit);
                          }
                      }
                  });
              } }
            if(ids.size()*36ull<=kSliceBufferBytes || n==1) break;
            n=std::max(1u,static_cast<uint32_t>(n*(kSliceBufferBytes*0.9/36/ids.size())));
            tileRows();
            entries=offsets[first+n]-base;
            sliceSize[0]=(n+1ull)*4; sliceSize[1]=sliceSize[2]=std::max<size_t>(4,entries*4ull);
            ids.clear(); prefix.clear(); bits.clear();
        }
        if(!ids.empty() && ids.back()>=globalSamples) throw std::runtime_error("Native footprint index out of bounds");
        allocate(3,std::max<size_t>(1,ids.size())*36);
        constexpr uint32_t chunkRecords=32768,maxGap=8;
        { BlockProfileScope profile("read_pack_slice_samples",ids.size()*72ull);
        std::vector<size_t> runs;
        for(size_t i=0;i<ids.size();) {
            runs.push_back(i);
            size_t end=i+1;
            while(end<ids.size() && ids[end]-ids[i]<chunkRecords && ids[end]-ids[end-1]<=maxGap) ++end;
            i=end;
        }
        runs.push_back(ids.size());
        const uintptr_t pack=pointerAt(0x32ad30);
        parallelRange(runs.size()-1,4,[&](size_t from,size_t to) {
            std::vector<uint8_t> staging(chunkRecords*72);
            for(size_t r=from;r<to;++r) {
                const size_t i=runs[r],end=runs[r+1];
                const uint32_t records=ids[end-1]-ids[i]+1;
                readMemory(pack+ids[i]*72ull,staging.data(),records*72ull);
                for(size_t j=i;j<end;++j)
                    memcpy(slicePointer[3]+j*36,staging.data()+(ids[j]-ids[i])*72ull,36);
            }
        }); }
        { BlockProfileScope profile("validate_remap_slice",entries*8ull);
        parallelRange(n,256,[&](size_t firstLuxel,size_t lastLuxel) {
            for(size_t luxel=firstLuxel;luxel<lastLuxel;++luxel) {
                float previous=0;
                uint32_t begin=offsets[first+luxel]-base,end=offsets[first+luxel+1]-base;
                for(uint32_t i=begin;i<end;++i) {
                    if(!std::isfinite(cdf[i]) || cdf[i]<previous) throw std::runtime_error("Invalid native footprint CDF");
                    previous=cdf[i];
                    uint32_t value=indices[i]-firstId;
                    indices[i]=prefix[value>>6]+popcount64(bits[value>>6]&((1ull<<(value&63))-1));
                }
                if(begin!=end && previous<=0) throw std::runtime_error("Native footprint has zero mass");
            }
        });
        const float* vectors=reinterpret_cast<const float*>(slicePointer[3]);
        parallelRange(ids.size(),1u<<14,[&](size_t from,size_t to) {
            for(size_t i=from*9;i<to*9;++i) if(!std::isfinite(vectors[i])) throw std::runtime_error("Nonfinite native sample");
            for(size_t i=from;i<to;++i) for(uint32_t offset:{3u,6u}) {
                const float* normal=vectors+i*9+offset;
                if(std::abs(normal[0]*normal[0]+normal[1]*normal[1]+normal[2]*normal[2]-1)>0.002f)
                    throw std::runtime_error("Invalid native sample normal");
            }
        }); }
        slice->luxels=n;
        for(size_t i=0;i<4;++i) slice->buffers[i]={slicePointer[i],sliceSize[i]};
        std::cout << "GPU slice input: first=" << first << " luxels=" << n
                  << " entries=" << entries << " samples=" << ids.size() << " budget_bytes=" << budget << '\n';
        return 0;
    } catch(const std::exception& exception) {
        if(error && errorBytes) strncpy_s(error,errorBytes,exception.what(),_TRUNCATE);
        return 1;
    }
}

static void surveyNative(uint32_t spp) {
    std::set<std::string> failures, errors;
    auto check=[&](bool bad,const char* reason) { if(bad) failures.insert(reason); };
    auto section=[&](auto action) { try { action(); } catch(const std::exception& e) { errors.insert(e.what()); } };
    uintptr_t scene=pointerAt(0x32aa20);
    uint32_t meshes=readValue<uint32_t>(scene+0x180);
    uintptr_t sky=readValue<uintptr_t>(scene+0x70);
    std::ostringstream meshJson,textureJson,lightJson,lightFlagsJson,sizeJson;
    uint32_t opaque=0,translucent=0,opacityTextures=0,masks=0,emissive=0,materials=0;
    uint32_t skyWidth=0,skyHeight=0;
    check(!validGpuBlock(countAt(0x32ab88),countAt(0x32aba8),countAt(0x32abac)) || !spp || spp>4096,
          "Unsupported native block settings");
    check(meshes!=1,"Native GPU requires one mesh");
    section([&] {
        if(meshes>65536) throw std::runtime_error("Survey mesh count exceeded");
        uintptr_t table=readValue<uintptr_t>(scene+0x188);
        for(uint32_t i=0;i<meshes;++i) {
            uintptr_t mesh=readValue<uintptr_t>(table+i*sizeof(uintptr_t));
            uint32_t vertices=readValue<uint32_t>(mesh+0x10),triangles=readValue<uint32_t>(mesh+0x28);
            uint32_t children=readValue<uint32_t>(mesh+0xd8);
            uint32_t materialKeys=readValue<uint32_t>(mesh+0x58),colors=readValue<uint32_t>(mesh+0x70),uvs=readValue<uint32_t>(mesh+0x88);
            if(i) meshJson << ',';
            meshJson << "{\"vertices\":" << vertices << ",\"triangles\":" << triangles
                     << ",\"children\":" << children << ",\"flags\":" << readValue<uint32_t>(mesh+0xa0)
                     << ",\"vertex_bytes\":" << vertices*16ull << ",\"uv_bytes\":" << vertices*8ull
                     << ",\"color_bytes\":" << triangles*12ull << ",\"flag_bytes\":" << triangles*2ull
                     << ",\"material_key_bytes\":" << triangles*8ull
                     << ",\"layout_checks\":{\"children\":{\"actual\":" << children << ",\"expected\":0,\"pass\":" << (children==0?"true":"false")
                     << "},\"vertices\":{\"actual\":" << vertices << ",\"expected\":" << triangles*3ull << ",\"pass\":" << (vertices==triangles*3ull?"true":"false")
                     << "},\"triangles_nonzero\":{\"actual\":" << triangles << ",\"pass\":" << (triangles?"true":"false")
                     << "},\"count_0x58\":{\"comparison\":\"at_least\",\"actual\":" << materialKeys << ",\"expected\":" << triangles << ",\"pass\":" << (materialKeys>=triangles?"true":"false")
                     << "},\"count_0x70\":{\"comparison\":\"at_least\",\"actual\":" << colors << ",\"expected\":" << triangles << ",\"pass\":" << (colors>=triangles?"true":"false")
                     << "},\"count_0x88\":{\"comparison\":\"at_least\",\"actual\":" << uvs << ",\"expected\":" << vertices << ",\"pass\":" << (uvs>=vertices?"true":"false") << "}}}";
            check(children || vertices!=static_cast<uint64_t>(triangles)*3 || !triangles ||
                  materialKeys<triangles || colors<triangles || uvs<vertices,"Unsupported native mesh layout/instances");
        }
    });
    section([&] { if(sky) { skyWidth=readValue<uint32_t>(sky); skyHeight=readValue<uint32_t>(sky+4);
        auto packed=prepareNativeSky(scene); writeFile(outputPath/L"native-sky.bin",packed.data(),packed.size()); } });
    section([&] {
        uintptr_t object=scene+0x128,table=readValue<uintptr_t>(object);
        uint32_t capacity=readValue<uint32_t>(object+8),used=readValue<uint32_t>(object+0x10);
        uint32_t textures=readValue<uint32_t>(object+0x40);
        check(capacity>65536 || textures>65536,"Native resource counts exceeded");
        if(capacity>65536 || textures>65536) return;
        std::set<uint64_t> keys;
        for(uint32_t i=0;i<capacity;++i) {
            uintptr_t entry=table+static_cast<uintptr_t>(i)*140;
            if(readValue<uint32_t>(entry)&0x80000000u) continue;
            ++materials; keys.insert(readValue<uint64_t>(entry+4));
            uintptr_t m=entry+12; float numeric[10]; readMemory(m,numeric,sizeof(numeric));
            for(float v:numeric) check(!std::isfinite(v),"Nonfinite native material");
            bool op=readValue<uint64_t>(m+56)!=0,mask=readValue<uint64_t>(m+72)!=0;
            opaque+=numeric[9]==1 && !op; translucent+=numeric[9]<1;
            opacityTextures+=op; masks+=mask;
            emissive+=numeric[6]>0 || numeric[7]>0 || numeric[8]>0;
            check(numeric[9]<0,"Native opacity out of bounds");
            check(mask && (readValue<int32_t>(m+88)<0 || readValue<int32_t>(m+88)>3),"Native mask channel out of bounds");
            for(uint32_t slot=0;slot<3;++slot) if(readValue<uint64_t>(m+40+slot*16)) {
                int32_t index=readValue<int32_t>(m+52+slot*16);
                check(index<0 || static_cast<uint32_t>(index)>=textures,"Native material texture out of bounds");
            }
        }
        check(keys.size()!=used || keys.empty(),"Native material occupancy mismatch");
        uintptr_t meshTable=readValue<uintptr_t>(scene+0x188);
        if(meshes<=65536) for(uint32_t i=0;i<meshes;++i) section([&] {
            uintptr_t mesh=readValue<uintptr_t>(meshTable+i*sizeof(uintptr_t));
            uint32_t vertices=readValue<uint32_t>(mesh+0x10),triangles=readValue<uint32_t>(mesh+0x28);
            auto primitiveKeys=copyArray(readValue<uintptr_t>(mesh+0x60),triangles,8);
            for(uint32_t t=0;t<triangles;++t) { uint64_t key; memcpy(&key,primitiveKeys.data()+t*8,8);
                check(!keys.count(key),"Native primitive material missing"); }
            for(auto field: {std::array<uint32_t,3>{0x18,vertices,16}, {0x90,vertices,8}, {0x78,triangles,12}}) {
                auto bytes=copyArray(readValue<uintptr_t>(mesh+field[0]),field[1],field[2]);
                for(size_t n=0;n<bytes.size();n+=4) { float v; memcpy(&v,bytes.data()+n,4); check(!std::isfinite(v),"Nonfinite native input"); }
            }
        });
        uintptr_t textureTable=readValue<uintptr_t>(object+0x48);
        std::map<std::array<uint32_t,3>,uint32_t> groups;
        uint64_t textureBytes=0;
        for(uint32_t i=0;i<textures;++i) {
            uintptr_t t=textureTable+static_cast<uintptr_t>(i)*32;
            uint32_t w=readValue<uint32_t>(t+0x10),h=readValue<uint32_t>(t+0x14),bpp=readValue<uint32_t>(t+0x18);
            ++groups[{bpp,w,h}];
            check(!w || !h || w>8192 || h>8192 || bpp!=4,"Native GPU requires RGBA8 textures");
            textureBytes+=static_cast<uint64_t>(w)*h*4;
            check(textureBytes>512ull*1024*1024,"Native texture memory exceeded");
            check(static_cast<uint64_t>(w)*h>64000000 || (w && h && !readValue<uintptr_t>(t+8)),"Native input bounds exceeded");
        }
        bool first=true;
        for(const auto& group:groups) {
            if(!first) textureJson << ','; first=false;
            textureJson << "{\"bytes_per_pixel\":" << group.first[0] << ",\"width\":" << group.first[1]
                        << ",\"height\":" << group.first[2] << ",\"count\":" << group.second << '}';
        }
    });
    section([&] {
        uint32_t count=readValue<uint32_t>(scene);
        check(count>65536,"Native light count exceeded"); if(count>65536) return;
        uintptr_t table=readValue<uintptr_t>(scene+8); std::map<uint32_t,uint32_t> types;
        std::map<std::array<uint32_t,4>,uint32_t> flags;
        for(uint32_t i=0;i<count;++i) {
            uintptr_t light=table+static_cast<uintptr_t>(i)*264; uint32_t type=readValue<uint32_t>(light); ++types[type];
            ++flags[{type,readValue<uint8_t>(light+0x84),readValue<uint32_t>(light+0x8c),readValue<uint32_t>(light+0x104)}];
            check(type<1 || type>4,"Native GPU light type not implemented");
            if(readValue<uint8_t>(light+0x84)) for(uint32_t field:{0x10u,0x14u,0x18u,0x1cu,0xacu,0xb0u,0xb4u,0xe0u,0xb8u,0xbcu,0xc0u,0x90u,0x24u,0x28u,0x2cu,0xd4u,0xdcu,0x20u,0xc4u,0xc8u,0xccu})
                check(!std::isfinite(readValue<float>(light+field)),"Nonfinite native light");
        }
        bool first=true; for(const auto& type:types) { if(!first) lightJson << ','; first=false; lightJson << '"' << type.first << "\":" << type.second; }
        first=true; for(const auto& group:flags) {
            if(!first) lightFlagsJson << ','; first=false;
            lightFlagsJson << "{\"type\":" << group.first[0] << ",\"enabled\":" << group.first[1]
                           << ",\"source_size_flag\":" << group.first[2] << ",\"flags\":" << group.first[3]
                           << ",\"count\":" << group.second << '}';
        }
        for(uint32_t field:{0x54u,0x58u,0x5cu,0x44u,0x48u,0x4cu}) check(!std::isfinite(readValue<float>(scene+field)),"Nonfinite native sky");
    });
    section([&] {
        uint32_t count=countAt(0x32ab88),global=countAt(0x32ad28);
        uint32_t entries=readValue<uint32_t>(pointerAt(0x32ab00)+static_cast<uintptr_t>(count)*4);
        sizeJson << "\"width\":" << countAt(0x32aba8) << ",\"height\":" << countAt(0x32abac)
                 << ",\"luxels\":" << count << ",\"spp\":" << spp
                 << ",\"footprint_entries\":" << entries << ",\"csr_bytes\":" << (count+1ull)*4
                 << ",\"index_bytes\":" << entries*4ull << ",\"cdf_bytes\":" << entries*4ull
                 << ",\"global_sample_records\":" << global << ",\"global_raw_sample_bytes\":" << global*72ull
                 << ",\"working_bytes\":" << count*204ull;
        if(!entries) return;
        constexpr uint32_t chunkEntries=1048576;
        auto chunk=copyArray(pointerAt(0x32acd8),std::min(entries,chunkEntries),4,"survey_index_chunk");
        uint32_t firstSample=UINT32_MAX,lastSample=0;
        for(uint64_t first=0;first<entries;first+=chunkEntries) {
            uint32_t n=static_cast<uint32_t>(std::min<uint64_t>(chunkEntries,entries-first));
            if(first) readMemory(pointerAt(0x32acd8)+first*4,chunk.data(),n*4ull);
            const auto* values=reinterpret_cast<const uint32_t*>(chunk.data());
            auto bounds=std::minmax_element(values,values+n);
            firstSample=std::min(firstSample,*bounds.first); lastSample=std::max(lastSample,*bounds.second);
        }
        uint64_t compact=static_cast<uint64_t>(lastSample)-firstSample+1;
        sizeJson << ",\"first_sample\":" << firstSample << ",\"last_sample\":" << lastSample
                 << ",\"compact_sample_records\":" << compact << ",\"referenced_raw_sample_bytes\":" << compact*72
                 << ",\"packed_sample_bytes\":" << compact*36;
        check(lastSample>=global,"Native footprint index out of bounds");
    });
    section([&] {
        uint32_t count=countAt(0x32ab88),global=countAt(0x32ad28);
        auto offsetBytes=copyArray(pointerAt(0x32ab00),static_cast<size_t>(count)+1,4);
        const auto* offsets=reinterpret_cast<const uint32_t*>(offsetBytes.data()); uint32_t entries=offsets[count];
        check(offsets[0]!=0 || !entries || !global || global>kMaxGlobalSamples,"Invalid native footprint");
        if(!entries) return;
        auto indexBytes=copyArray(pointerAt(0x32acd8),entries,4),cdfBytes=copyArray(pointerAt(0x32ab58),entries,4);
        const auto* indices=reinterpret_cast<const uint32_t*>(indexBytes.data()); const auto* cdf=reinterpret_cast<const float*>(cdfBytes.data());
        auto bounds=std::minmax_element(indices,indices+entries);
        check(*bounds.second>=global,"Native footprint index out of bounds");
        uint64_t sampleCount=static_cast<uint64_t>(*bounds.second)-*bounds.first+1;
        check(sampleCount>14000000,"Native compact sample count exceeded");
        if(sampleCount<=14000000 && *bounds.second<global) {
            auto records=copyArray(pointerAt(0x32ad30)+static_cast<uintptr_t>(*bounds.first)*72,static_cast<size_t>(sampleCount),72);
            for(size_t i=0;i<sampleCount;++i) {
                float v[9]; memcpy(v,records.data()+i*72,36);
                for(float value:v) check(!std::isfinite(value),"Nonfinite native input");
                for(uint32_t n:{3u,6u}) check(std::abs(v[n]*v[n]+v[n+1]*v[n+1]+v[n+2]*v[n+2]-1)>0.002f,"Invalid native sample normal");
            }
        }
        for(uint32_t i=0;i<count;++i) {
            bool invalid=offsets[i]>offsets[i+1] || offsets[i+1]>entries;
            check(invalid,"Invalid native CSR"); if(invalid) continue;
            float previous=0;
            for(uint32_t j=offsets[i];j<offsets[i+1];++j) {
                check(!std::isfinite(cdf[j]) || cdf[j]<previous,"Invalid native footprint index/CDF"); previous=cdf[j];
            }
            check(offsets[i]!=offsets[i+1] && previous<=0,"Native footprint has zero mass");
        }
    });
    auto strings=[](std::ostream& out,const std::set<std::string>& values) {
        bool first=true; for(const auto& value:values) { if(!first) out << ','; first=false; out << '"';
            for(char c:value) { if(c=='"' || c=='\\') out << '\\'; if(c=='\n') out << "\\n"; else if(c=='\r') out << "\\r"; else out << c; } out << '"'; }
    };
    std::ofstream out(outputPath/L"scene-survey.json");
    out << "{\"complete\":" << (errors.empty()?"true":"false") << ",\"mesh_count\":" << meshes << ",\"meshes\":[" << meshJson.str()
        << "],\"materials\":{\"count\":" << materials << ",\"opaque\":" << opaque << ",\"opacity_lt_one\":" << translucent
        << ",\"opacity_texture\":" << opacityTextures << ",\"mask_texture\":" << masks << ",\"emissive\":" << emissive
        << "},\"textures\":[" << textureJson.str() << "],\"lights_by_type\":{" << lightJson.str()
        << "},\"light_flags\":[" << lightFlagsJson.str() << "],\"input_sizes\":{" << sizeJson.str()
        << "},\"sky\":{\"environment_image\":" << (sky?"true":"false") << ",\"width\":" << skyWidth << ",\"height\":" << skyHeight
        << "},\"prepare_native_failures\":["; strings(out,failures); out << "],\"survey_errors\":["; strings(out,errors); out << "]}\n";
    if(!out) throw std::runtime_error("Scene survey write failed");
    std::cout << "Scene survey: meshes=" << meshes << " materials=" << materials << " unsupported_reasons=" << failures.size()
              << " complete=" << errors.empty() << '\n' << std::flush;
}
