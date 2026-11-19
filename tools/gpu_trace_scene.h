// Splits native triangles into traversal groups by ray class, and builds the light BVH and light grid metadata.
#pragma once
#include <array>
#include <map>

struct TracePartition {
    uint32_t mask=0, first=0;
    bool opaque=false, twoSided=false;
    std::vector<uint32_t> indices;
    D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO sizes{};
};
struct LightBoundsNode {
    float lower[3]; uint32_t escape;
    float upper[3]; uint32_t light;
};
static_assert(sizeof(LightBoundsNode)==32,"Shader bounds layout");

static std::vector<TracePartition> partitionTraceScene(const GpuInputView& primitives,
        const GpuInputView& materials,const uint32_t* constants,std::vector<uint8_t>& metadata) {
    const uint32_t triangles=static_cast<uint32_t>(primitives.size()/8);
    const bool probes=(constants[3]&0x40000000u)!=0;
    requirePhysicalMemory(static_cast<uint64_t>(triangles)*32,"Traversal partition staging");
    std::map<uint32_t,TracePartition> groups; uint32_t rejectOnlyTriangles=0;
    for(uint32_t i=0;i<triangles;++i) {
        uint32_t words[2]; memcpy(words,primitives.data()+i*8ull,8);
        if(static_cast<uint64_t>(words[1])*64+64>materials.size()) throw std::runtime_error("Invalid traversal material");
        float opacity; int32_t texture;
        memcpy(&opacity,materials.data()+words[1]*64ull+12,4);
        memcpy(&texture,materials.data()+words[1]*64ull+52,4);
        // Instance masks encode accepted ray classes rather than truncating Valve's 16-bit exclusion mask to DXR's 8 bits.
        const bool translucent=texture>=0 || opacity<1;
        uint32_t flags=words[0]|constants[14],mask=0;
        if(!(flags&constants[12])) mask|=1;
        if(probes ? !(flags&(translucent?constants[12]:constants[13])) : !(flags&constants[13])) mask|=translucent?4:2;
        if(!(flags&(constants[12]|1))) mask|=8;
        // Sample-reject rays only: triangles matching Valve's reject filter 0x40b1 that no other ray class uses.
        const bool rejectOnly=!mask && !(flags&0x40b1u);
        if(rejectOnly) { mask=32; ++rejectOnlyTriangles; }
        if(!mask) continue;
        bool twoSided=(flags&2)!=0;
        uint32_t key=mask|(twoSided?256:0)|(translucent?512:0)|(rejectOnly?(1u<<20):0);
        auto& group=groups[key]; group.mask=mask; group.opaque=!translucent; group.twoSided=twoSided;
        group.indices.insert(group.indices.end(),{i*3,i*3+1,i*3+2});
    }
    if(rejectOnlyTriangles) std::cout << "GPU traversal: sample-reject-only triangles=" << rejectOnlyTriangles << '\n';
    std::vector<TracePartition> result;
    for(auto& pair:groups) {
        auto& group=pair.second; group.first=static_cast<uint32_t>(metadata.size()/4);
        for(size_t i=0;i<group.indices.size();i+=3) {
            uint32_t id=group.indices[i]/3; size_t offset=metadata.size();
            metadata.resize(offset+4); memcpy(metadata.data()+offset,&id,4);
        }
        result.push_back(std::move(group));
    }
    if(result.empty()) throw std::runtime_error("No accepted traversal geometry");
    return result;
}

struct LightGridHeader {
    float lower[3]; uint32_t dimX;
    float scale[3]; uint32_t dimY;
    uint32_t dimZ,cells,lists,bounds;
};
static_assert(sizeof(LightGridHeader)==48,"Shader light grid layout");
// Each cell lists, in ascending order, every pathtrace-eligible light whose bounds overlap it; the shader tests each light's bounds as the BVH leaf does, so it meets the same lights in the same order.
static void appendLightGrid(const GpuInputView& lights,uint32_t count,std::vector<uint8_t>& metadata) {
    const size_t headerAt=metadata.size(); metadata.resize(headerAt+sizeof(LightGridHeader));
    LightGridHeader header{};
    std::vector<std::array<float,6>> boxes(count);
    std::vector<uint8_t> eligible(count);
    float lower[3]={INFINITY,INFINITY,INFINITY},upper[3]={-INFINITY,-INFINITY,-INFINITY};
    for(uint32_t i=0;i<count;++i) {
        memcpy(boxes[i].data(),lights.data()+(count+i)*112ull+16,12); memcpy(boxes[i].data()+3,lights.data()+(count+i)*112ull+32,12);
        for(int a=0;a<3;++a) { lower[a]=std::min(lower[a],boxes[i][a]); upper[a]=std::max(upper[a],boxes[i][3+a]); }
        uint32_t flags; memcpy(&flags,lights.data()+i*112ull+12,4); eligible[i]=flags&1;
    }
    bool usable=count>1;
    for(int a=0;a<3;++a) usable=usable && std::isfinite(upper[a]-lower[a]);
    constexpr uint64_t maximumEntries=1ull<<22;
    std::vector<uint32_t> starts,entries;
    for(double target=262144;usable && target>=8;target/=8) {
        double extent[3],largest=0;
        for(int a=0;a<3;++a) largest=std::max(largest,static_cast<double>(upper[a]-lower[a]));
        for(int a=0;a<3;++a) extent[a]=std::max(static_cast<double>(upper[a]-lower[a]),std::max(largest*1e-3,1e-6));
        const double side=std::cbrt(extent[0]*extent[1]*extent[2]/target);
        uint32_t dims[3]; float scale[3];
        for(int a=0;a<3;++a) {
            dims[a]=static_cast<uint32_t>(std::clamp(std::ceil(extent[a]/side),1.0,256.0));
            scale[a]=upper[a]>lower[a] ? static_cast<float>(dims[a]/static_cast<double>(upper[a]-lower[a])) : 0.0f;
        }
        auto cellRange=[&](const std::array<float,6>& box,int a,uint32_t& first,uint32_t& last) {
            // Shader cell is clamp(floor((p-lower)*scale)); the 0.01-cell margin covers rounding differences.
            const float low=std::floor((box[a]-lower[a])*scale[a]-0.01f),high=std::floor((box[3+a]-lower[a])*scale[a]+0.01f);
            first=static_cast<uint32_t>(std::clamp(low,0.0f,static_cast<float>(dims[a]-1)));
            last=static_cast<uint32_t>(std::clamp(high,0.0f,static_cast<float>(dims[a]-1)));
        };
        const uint64_t cells=static_cast<uint64_t>(dims[0])*dims[1]*dims[2];
        uint64_t total=0;
        for(uint32_t i=0;i<count;++i) {
            if(!eligible[i]) continue;
            uint32_t first[3],last[3];
            for(int a=0;a<3;++a) cellRange(boxes[i],a,first[a],last[a]);
            total+=static_cast<uint64_t>(last[0]-first[0]+1)*(last[1]-first[1]+1)*(last[2]-first[2]+1);
        }
        if(total>maximumEntries) continue;
        starts.assign(cells+1,0); entries.resize(total);
        for(int pass=0;pass<2;++pass) {
            for(uint32_t i=0;i<count;++i) {
                if(!eligible[i]) continue;
                uint32_t first[3],last[3];
                for(int a=0;a<3;++a) cellRange(boxes[i],a,first[a],last[a]);
                for(uint32_t z=first[2];z<=last[2];++z) for(uint32_t y=first[1];y<=last[1];++y) for(uint32_t x=first[0];x<=last[0];++x) {
                    const uint64_t cell=(static_cast<uint64_t>(z)*dims[1]+y)*dims[0]+x;
                    if(pass==0) ++starts[cell+1]; else entries[starts[cell]++]=i;
                }
            }
            if(pass==0) for(uint64_t c=0;c<cells;++c) starts[c+1]+=starts[c];
            else { for(uint64_t c=cells;c>0;--c) starts[c]=starts[c-1]; starts[0]=0; }
        }
        for(int a=0;a<3;++a) { header.lower[a]=lower[a]; header.scale[a]=scale[a]; }
        header.dimX=dims[0]; header.dimY=dims[1]; header.dimZ=dims[2];
        break;
    }
    if(header.dimZ) {
        auto append=[&](const void* bytes,size_t size) {
            const size_t at=(metadata.size()+15)&~size_t(15); metadata.resize(at+size); memcpy(metadata.data()+at,bytes,size);
            return static_cast<uint32_t>(at);
        };
        header.cells=append(starts.data(),starts.size()*4);
        header.lists=append(entries.data(),entries.size()*4);
        std::vector<float> bounds(count*8ull,0.0f);
        for(uint32_t i=0;i<count;++i) { memcpy(&bounds[i*8ull],boxes[i].data(),12); memcpy(&bounds[i*8ull+4],boxes[i].data()+3,12); }
        header.bounds=append(bounds.data(),bounds.size()*4);
        std::cout << "GPU light grid: " << header.dimX << 'x' << header.dimY << 'x' << header.dimZ << " cells, "
                  << entries.size() << " entries for " << std::count(eligible.begin(),eligible.end(),1) << " of " << count << " lights\n";
    }
    memcpy(metadata.data()+headerAt,&header,sizeof(header));
}

// Balanced BVH over contiguous original light ranges; leaves are visited in input order to preserve RNG consumption and accumulation order.
static uint32_t appendLightBounds(const GpuInputView& lights,uint32_t count,std::vector<uint8_t>& metadata,bool grid) {
    const uint32_t offset=static_cast<uint32_t>((metadata.size()+15)&~size_t(15));
    metadata.resize(offset);
    if(lights.size()<static_cast<uint64_t>(count)*224) throw std::runtime_error("Light culling requires native bounds");
    std::vector<LightBoundsNode> nodes;
    auto build=[&](auto&& self,uint32_t first,uint32_t end)->uint32_t {
        uint32_t index=static_cast<uint32_t>(nodes.size()); nodes.push_back({});
        LightBoundsNode node{}; node.light=UINT32_MAX;
        for(uint32_t axis=0;axis<3;++axis) { node.lower[axis]=INFINITY; node.upper[axis]=-INFINITY; }
        for(uint32_t i=first;i<end;++i) {
            const auto* meta=lights.data()+(count+i)*112ull;
            for(uint32_t axis=0;axis<3;++axis) {
                float lo,hi; memcpy(&lo,meta+16+axis*4,4); memcpy(&hi,meta+32+axis*4,4);
                if(!std::isfinite(lo) || !std::isfinite(hi) || lo>hi) throw std::runtime_error("Invalid light culling bounds");
                node.lower[axis]=std::min(node.lower[axis],lo); node.upper[axis]=std::max(node.upper[axis],hi);
            }
        }
        if(end-first==1) node.light=first;
        else { uint32_t mid=first+(end-first)/2; self(self,first,mid); self(self,mid,end); }
        node.escape=static_cast<uint32_t>(nodes.size()); nodes[index]=node; return index;
    };
    if(count) build(build,0,count);
    size_t bytes=nodes.size()*sizeof(LightBoundsNode);
    metadata.resize(offset+bytes);
    if(bytes) memcpy(metadata.data()+offset,nodes.data(),bytes);
    if(grid) appendLightGrid(lights,count,metadata);
    else { metadata.resize(metadata.size()+sizeof(LightGridHeader)); }
    return offset;
}
