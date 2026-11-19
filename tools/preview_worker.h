// GPU side of the Hammer preview: hooks Valve's vrad3 commands (path_trace_triangles, lpv_*) and lights them on the GPU.
#pragma once
#ifdef VRAD3_STATIC_GPU
enum class PreviewSetting { Auto, On, Off };
static PreviewSetting previewSetting() {
    const auto value=environmentValue(L"VRAD3_GPU_PREVIEW_GPU");
    if(value.empty()) return PreviewSetting::Auto;
    if(value==L"1") return PreviewSetting::On;
    if(value==L"0") return PreviewSetting::Off;
    throw std::runtime_error("VRAD3_GPU_PREVIEW_GPU must be 0 or 1");
}
struct PreviewSamples { uint32_t min=0,max=0; float error=0; };
static PreviewSamples previewSamples() {
    const auto quality=environmentValue(L"VRAD3_GPU_PREVIEW_QUALITY");
    if(!quality.empty() && quality!=L"1" && quality!=L"2") throw std::runtime_error("VRAD3_GPU_PREVIEW_QUALITY must be 1 or 2");
    PreviewSamples result=quality==L"2"?PreviewSamples{256,1024,0.025f}:PreviewSamples{128,256,0.05f};
    if(cs2SamplesEnabled()) { result.min=std::min(result.min*2,4096u); result.max=std::min(result.max*4,4096u); }
    auto count=[](const wchar_t* name,uint32_t& target) {
        const auto value=environmentValue(name);
        if(value.empty()) return;
        if(value.size()>4 || value.find_first_not_of(L"0123456789")!=std::wstring::npos || !std::stoul(value) || std::stoul(value)>4096)
            throw std::runtime_error(logUtf8(name)+" must be 1..4096");
        target=static_cast<uint32_t>(std::stoul(value));
    };
    count(L"VRAD3_GPU_PREVIEW_MIN",result.min); count(L"VRAD3_GPU_PREVIEW_MAX",result.max);
    const auto error=environmentValue(L"VRAD3_GPU_PREVIEW_ERROR");
    if(!error.empty()) {
        size_t used=0; float value=-1;
        try { value=std::stof(error,&used); } catch(...) { used=0; }
        if(!used || used!=error.size() || !std::isfinite(value) || value<0 || value>1) throw std::runtime_error("VRAD3_GPU_PREVIEW_ERROR must be 0..1");
        result.error=value;
    }
    result.min=std::min(result.min,result.max);
    return result;
}
static uint64_t previewMinimumPaths() {
    const auto value=environmentValue(L"VRAD3_GPU_PREVIEW_MIN_PATHS");
    if(!value.empty() && value.find_first_not_of(L"0123456789")==std::wstring::npos && value.size()<=12) return std::stoull(value);
    // Valve's CPU stage traces roughly 350k paths per second per thread.
    return 350000ull*std::max<uint32_t>(1,usableCpuCount());
}
enum class PreviewCorners { Valve, Cs2 };
static PreviewCorners previewCorners() {
    const auto value=environmentValue(L"VRAD3_GPU_PREVIEW_CORNERS");
    if(value.empty()) return environmentValue(L"VRAD3_GPU_ESTIMATOR")==L"cs2"?PreviewCorners::Cs2:PreviewCorners::Valve;
    if(value==L"cs2") return PreviewCorners::Cs2;
    if(value==L"valve") return PreviewCorners::Valve;
    throw std::runtime_error("VRAD3_GPU_PREVIEW_CORNERS must be cs2 or valve");
}
static float previewSubdivide() {
    const auto value=environmentValue(L"VRAD3_GPU_PREVIEW_SUBDIVIDE");
    if(value.empty()) return 64.0f;
    if(value.size()>4 || value.find_first_not_of(L"0123456789")!=std::wstring::npos || (std::stoul(value) && (std::stoul(value)<8 || std::stoul(value)>4096)))
        throw std::runtime_error("VRAD3_GPU_PREVIEW_SUBDIVIDE must be 0 or 8..4096");
    return static_cast<float>(std::stoul(value));
}
constexpr uint32_t kTrianglePoints=16;
constexpr uint32_t kTriangleChunk=65536;
struct PreviewReport {
    uint32_t gpuCalls=0,cpuCalls=0,probeGpu=0,probeCpu=0,voxelVolumes=0;
    uint64_t gpuTriangles=0,paths=0,splitTriangles=0,subTriangles=0;
    float splitEdge=0;
    double gpuMs=0,stageMs=0,voxelMs=0;
    std::string reason;
    int childExit=-1;
};
static PreviewReport previewReport;
struct TriangleCpuCall : std::runtime_error { using std::runtime_error::runtime_error; };
struct TriangleArguments { std::string input,output; uint64_t start=0; uint32_t corners=0; };
static TriangleArguments triangleArguments(const CONTEXT& context) {
    if(static_cast<uint32_t>(context.Rdx)!=5 || !context.R8) throw TriangleCpuCall("path_trace_triangles needs exactly 4 arguments");
    auto pointers=copyArray(context.R8,5,8);
    auto args=reinterpret_cast<const uintptr_t*>(pointers.data());
    auto token=[&](uint32_t i) {
        std::string value;
        for(size_t j=0;j<1024;++j) { char c=readValue<char>(args[i]+j); if(!c) return value; value+=c; }
        throw TriangleCpuCall("path_trace_triangles argument too long");
    };
    auto number=[&](uint32_t i) {
        std::string text=token(i); size_t used=0; unsigned long long value=0;
        if(text.empty() || text.find_first_not_of("0123456789")!=std::string::npos) throw TriangleCpuCall("Unsupported path_trace_triangles number");
        try { value=std::stoull(text,&used); } catch(...) { used=0; }
        if(!used || value>0x7fffffffull) throw TriangleCpuCall("Unsupported path_trace_triangles number");
        return static_cast<uint64_t>(value);
    };
    TriangleArguments result; result.input=token(1); result.start=number(2);
    result.corners=static_cast<uint32_t>(number(3)); result.output=token(4);
    if(!result.corners || result.corners%3) throw TriangleCpuCall("path_trace_triangles vertex count must be a positive multiple of 3");
    return result;
}
struct TriangleBatch {
    NativeLighting* inputs=nullptr;
    const float* corners=nullptr;
    std::vector<float> normals;
    uint32_t triangles=0;
    std::array<std::vector<uint8_t>,4> slice;
};
// Valve's 16 points per triangle: s = sqrt(i/4+1/8), t = j/4+1/8 with i = point/4, j = point%4; weights (1-s, (1-t)s, st).
static void triangleBarycentrics(uint32_t point,float& u,float& v,float& w) {
    const float s=std::sqrt(static_cast<float>(point/4)*0.25f+0.125f),t=static_cast<float>(point%4)*0.25f+0.125f;
    u=1.0f-s; v=(1.0f-t)*s; w=s*t;
}
static int readTriangleSlice(void* context,uint32_t first,uint32_t maxLuxels,uint64_t budget,
                             GpuLightingSlice* slice,char* error,uint32_t errorBytes) {
    try {
        auto& batch=*static_cast<TriangleBatch*>(context);
        batch.slice={};
        const uint32_t count=batch.triangles*kTrianglePoints;
        if(first>=count || first%kTrianglePoints || !maxLuxels) throw std::runtime_error("Invalid triangle slice range");
        uint64_t n=std::min<uint64_t>({maxLuxels,count-first,budget/(48+4+408)});
        if(first+n<count) n=n/kTrianglePoints*kTrianglePoints;
        if(!n) throw std::runtime_error("A triangle exceeds the slice VRAM/RAM budget");
        requirePhysicalMemory(n*52+4,"triangle slice");
        auto& data=batch.slice;
        data[0].assign((n+1)*4,0);
        uint32_t* offsets=reinterpret_cast<uint32_t*>(data[0].data());
        uint32_t entries=0;
        for(uint64_t k=0;k<n;++k) {
            offsets[k]=entries;
            if(!std::isnan(batch.normals[(first+k)/kTrianglePoints*3])) ++entries;
        }
        offsets[n]=entries;
        data[1].assign(std::max<size_t>(4,entries*4ull),0); data[2].assign(std::max<size_t>(4,entries*4ull),0);
        data[3].assign(std::max<size_t>(1,entries)*36ull,0);
        uint32_t* indices=reinterpret_cast<uint32_t*>(data[1].data());
        float* cdf=reinterpret_cast<float*>(data[2].data());
        float* samples=reinterpret_cast<float*>(data[3].data());
        uint32_t at=0;
        for(uint64_t k=0;k<n;++k) {
            const uint32_t unit=static_cast<uint32_t>(first+k),triangle=unit/kTrianglePoints;
            const float* normal=batch.normals.data()+triangle*3ull;
            if(std::isnan(normal[0])) continue;
            const float* a=batch.corners+triangle*9ull; const float* b=a+3; const float* c=a+6;
            float u,v,w; triangleBarycentrics(unit%kTrianglePoints,u,v,w);
            float* sample=samples+at*9ull;
            for(int axis=0;axis<3;++axis) {
                sample[axis]=(b[axis]*v+a[axis]*u)+c[axis]*w;
                sample[3+axis]=normal[axis]; sample[6+axis]=normal[axis];
            }
            indices[at]=at; cdf[at]=1.0f; ++at;
        }
        slice->luxels=static_cast<uint32_t>(n);
        for(size_t i=0;i<4;++i) slice->buffers[i]={data[i].data(),data[i].size()};
        return 0;
    } catch(const std::exception& exception) {
        if(error && errorBytes) strncpy_s(error,errorBytes,exception.what(),_TRUNCATE);
        return 1;
    }
}
static std::vector<float> triangleNormals(const float* corners,uint32_t triangles) {
    std::vector<float> normals(triangles*3ull,std::numeric_limits<float>::quiet_NaN());
    parallelRange(triangles,4096,[&](size_t from,size_t to) {
        for(size_t t=from;t<to;++t) {
            const float* a=corners+t*9; const float* b=a+3; const float* c=a+6;
            bool finite=true; for(int i=0;i<9;++i) finite=finite && std::isfinite(a[i]);
            if(!finite) continue;
            const float e1[3]={b[0]-a[0],b[1]-a[1],b[2]-a[2]},e2[3]={c[0]-a[0],c[1]-a[1],c[2]-a[2]};
            // Valve's face normal: normalize(cross(b-a, c-a)), in Valve's operation order.
            const float n[3]={e2[2]*e1[1]-e2[1]*e1[2],e2[0]*e1[2]-e2[2]*e1[0],e2[1]*e1[0]-e2[0]*e1[1]};
            const float length=std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
            if(!(length>0) || !std::isfinite(length)) continue;
            for(int i=0;i<3;++i) normals[t*3+i]=n[i]/length;
        }
    });
    return normals;
}
static float triangleLongestEdge(const float* a) {
    float longest=0;
    for(int i=0;i<3;++i) {
        const float* p=a+i*3; const float* q=a+(i+1)%3*3;
        longest=std::max(longest,std::sqrt((q[0]-p[0])*(q[0]-p[0])+(q[1]-p[1])*(q[1]-p[1])+(q[2]-p[2])*(q[2]-p[2])));
    }
    return longest;
}
static std::array<uint8_t,32> triangleSceneHash{};
static bool triangleSceneUploaded=false;
static uint32_t triangleCallIndex=0;
static std::vector<float> triangleGpuStage(const TriangleArguments& args,const std::vector<float>& corners,const PreviewSamples& samples,
                                           float subdivide,PreviewCorners method,double& gpuMs) {
    SetEnvironmentVariableW(L"VRAD3_GPU_LIGHT_PATHS",L"0");
    // 0x17ae44 (RVA in vrad3) holds the ray mask path_trace_triangles uses; only 0xc0b0 is supported.
    const uint32_t mask=readValue<uint32_t>(address(0x17ae44));
    if(mask!=0xc0b0u) throw std::runtime_error("unverified path_trace_triangles ray mask");
    const uint32_t perPointMax=(samples.max+kTrianglePoints-1)/kTrianglePoints,perPointMin=(samples.min+kTrianglePoints-1)/kTrianglePoints;
    const bool adaptive=perPointMin<perPointMax && samples.error>0;
    const uint32_t triangles=args.corners/3;
    std::vector<float> output(args.corners*3ull,0.0f);
    std::vector<uint8_t> records;
    const uint32_t call=triangleCallIndex++;
    const std::vector<float> normals=triangleNormals(corners.data(),triangles);
    std::vector<uint32_t> level(triangles,1);
    uint64_t subTriangles=0;
    if(subdivide>0) {
        std::vector<float> longest(triangles,0.0f);
        parallelRange(triangles,4096,[&](size_t from,size_t to) {
            for(size_t t=from;t<to;++t) if(!std::isnan(normals[t*3])) longest[t]=triangleLongestEdge(corners.data()+t*9);
        });
        const uint64_t budget=std::max<uint64_t>(131072,2ull*triangles);
        for(;;) {
            subTriangles=0;
            for(uint32_t t=0;t<triangles;++t) {
                const float k=std::ceil(longest[t]/subdivide*0.9999f);
                level[t]=k>1?static_cast<uint32_t>(std::min(k,64.0f)):1;
                if(level[t]>1) subTriangles+=level[t]*level[t];
            }
            if(subTriangles<=budget) break;
            subdivide*=1.25f;
        }
    }
    std::vector<uint32_t> whole; std::vector<float> wholeCorners,wholeNormals;
    std::vector<float> subCorners,subNormals,subBary; std::vector<uint32_t> subOwner,splitTriangles; std::vector<size_t> firstSub;
    const uint32_t splitSamples=perPointMax;
    auto subPerPoint=[&](uint32_t k) { return std::max(perPointMin,(splitSamples+k*k-1)/(k*k)); };
    std::vector<uint32_t> rangePerPoint; std::vector<size_t> rangeFirst;
    if(subTriangles) {
        for(uint32_t t=0;t<triangles;++t)
            if(level[t]>1 && std::find(rangePerPoint.begin(),rangePerPoint.end(),subPerPoint(level[t]))==rangePerPoint.end())
                rangePerPoint.push_back(subPerPoint(level[t]));
        std::sort(rangePerPoint.begin(),rangePerPoint.end());
        requirePhysicalMemory(subTriangles*(204ull+192)+triangles*52ull,"split triangles");
        subCorners.reserve(subTriangles*9); subNormals.reserve(subTriangles*3); subBary.reserve(subTriangles*9); subOwner.reserve(subTriangles);
        for(uint32_t t=0;t<triangles;++t) if(level[t]==1) {
            const float* a=corners.data()+t*9ull; const float* n=normals.data()+t*3ull;
            whole.push_back(t); wholeCorners.insert(wholeCorners.end(),a,a+9); wholeNormals.insert(wholeNormals.end(),n,n+3);
        }
        for(const uint32_t perPoint:rangePerPoint) for(uint32_t t=0,first=1;t<triangles;++t) {
            if(level[t]==1 || subPerPoint(level[t])!=perPoint) continue;
            if(first) { rangeFirst.push_back(subOwner.size()); first=0; }
            const float* a=corners.data()+t*9ull; const float* n=normals.data()+t*3ull;
            splitTriangles.push_back(t); firstSub.push_back(subOwner.size());
            const uint32_t k=level[t]; const float step=1.0f/static_cast<float>(k);
            auto add=[&](uint32_t u0,uint32_t v0,uint32_t u1,uint32_t v1,uint32_t u2,uint32_t v2) {
                for(const auto& [u,v]:{std::pair{u0,v0},std::pair{u1,v1},std::pair{u2,v2}}) {
                    const float bary[3]={1.0f-static_cast<float>(u+v)*step,static_cast<float>(u)*step,static_cast<float>(v)*step};
                    for(int axis=0;axis<3;++axis) subCorners.push_back(a[axis]*bary[0]+a[3+axis]*bary[1]+a[6+axis]*bary[2]);
                    subBary.insert(subBary.end(),bary,bary+3);
                }
                subNormals.insert(subNormals.end(),n,n+3); subOwner.push_back(t);
            };
            for(uint32_t u=0;u<k;++u) for(uint32_t v=0;u+v<k;++v) {
                add(u,v,u+1,v,u,v+1);
                if(u+v+1<k) add(u+1,v+1,u,v+1,u+1,v);
            }
        }
        firstSub.push_back(subOwner.size()); rangeFirst.push_back(subOwner.size());
    }
    auto trace=[&](const float* tileCorners,const float* tileNormals,uint32_t tiles,uint32_t perPoint,bool early,uint32_t stream,const auto& combine) {
        for(uint32_t firstTriangle=0;firstTriangle<tiles;firstTriangle+=kTriangleChunk) {
            TriangleBatch batch; batch.triangles=std::min(kTriangleChunk,tiles-firstTriangle);
            batch.corners=tileCorners+firstTriangle*9ull;
            batch.normals.assign(tileNormals+firstTriangle*3ull,tileNormals+(firstTriangle+batch.triangles)*3ull);
            const uint32_t units=batch.triangles*kTrianglePoints;
            NativeLighting inputs;
            bool resident=triangleSceneUploaded && GpuLightingHasScene(triangleSceneHash.data());
            { BlockProfileScope profile(resident?"prepare_triangles_small":"prepare_triangles"); inputs=prepareNative(perPoint,0,units,resident); }
            const auto token=sceneToken(inputs,L"ptt:"+std::to_wstring(GetProcessId(processHandle))+L":"+std::to_wstring(pointerAt(0x32aa20)),"triangles-v1");
            if(resident && token!=triangleSceneHash) {
                resident=false;
                BlockProfileScope profile("prepare_triangles"); inputs=prepareNative(perPoint,0,units,false);
            }
            inputs.constants[12]=mask; inputs.constants[13]=mask|1;
            inputs.constants[2]^=(call*0x10000u+stream+firstTriangle/kTriangleChunk+1)*0x9e3779b9u;
            uint64_t traced=0; for(uint32_t t=0;t<batch.triangles;++t) if(!std::isnan(batch.normals[t*3ull])) ++traced;
            inputs.totalPaths=traced*kTrianglePoints*perPoint;
            batch.inputs=&inputs;
            if(!traced) continue;
            requirePhysicalMemory(units*204ull,"triangle records");
            records.assign(units*204ull,0);
            GpuLightingRequest request{}; request.version=6;
            memcpy(request.constants,inputs.constants,sizeof(inputs.constants));
            for(size_t i=0;i<inputs.buffers.size();++i) request.buffers[i]={inputs.buffers[i].data(),inputs.buffers[i].size()};
            request.output=records.data(); request.outputBytes=records.size();
            request.shaderBytes=lightingShaderBytes(); request.shaderSize=lightingShaderSize();
            request.readSlice=readTriangleSlice; request.sliceContext=&batch; request.totalPaths=inputs.totalPaths;
            request.persistentSceneHash=token.data(); request.progress=progressWindow.progress();
            if(early) { request.adaptiveMin=perPointMin; request.blockWidth=4; request.adaptiveTol=samples.error; }
            GpuLightingTiming timing{}; char error[512]{};
            if(BakeGpuLighting(&request,&timing,error,sizeof(error))) { triangleSceneUploaded=false; throw std::runtime_error(error); }
            triangleSceneHash=token; triangleSceneUploaded=true;
            gpuMs+=timing.lightingMs; previewReport.paths+=inputs.totalPaths;
            combine(firstTriangle,batch.triangles);
        }
    };
    float weights[kTrianglePoints][3],sums[3]={0,0,0};
    for(uint32_t p=0;p<kTrianglePoints;++p) {
        triangleBarycentrics(p,weights[p][0],weights[p][1],weights[p][2]);
        for(int k=0;k<3;++k) sums[k]+=weights[p][k];
    }
    const float scale[3]={1.0f/sums[0],1.0f/sums[1],1.0f/sums[2]};
    const bool reject=method==PreviewCorners::Cs2;
    auto tileMeans=[&](const uint8_t* record,float mean[kTrianglePoints][3],bool drop[kTrianglePoints]) {
        float tile[3]={0,0,0};
        for(uint32_t p=0;p<kTrianglePoints;++p) {
            uint32_t samplesTaken; memcpy(&samplesTaken,record+p*204ull+32,4);
            mean[p][0]=mean[p][1]=mean[p][2]=0;
            if(samplesTaken) memcpy(mean[p],record+p*204ull,12);
            for(int channel=0;channel<3;++channel) tile[channel]+=mean[p][channel];
        }
        for(int channel=0;channel<3;++channel) tile[channel]=std::max(0.0f,tile[channel]*0.0625f)*8.0f;
        for(uint32_t p=0;p<kTrianglePoints;++p) {
            for(int channel=0;channel<3;++channel) mean[p][channel]=std::max(0.0f,mean[p][channel]);
            drop[p]=reject && (mean[p][0]>tile[0] || mean[p][1]>tile[1] || mean[p][2]>tile[2]);
        }
    };
    const bool split=subTriangles!=0;
    trace(split?wholeCorners.data():corners.data(),split?wholeNormals.data():normals.data(),split?static_cast<uint32_t>(whole.size()):triangles,
          perPointMax,adaptive,0,[&](uint32_t first,uint32_t count) {
        parallelRange(count,4096,[&](size_t from,size_t to) {
            for(size_t t=from;t<to;++t) {
                float* out=output.data()+(split?whole[first+t]:first+t)*9ull;
                const uint8_t* record=records.data()+t*kTrianglePoints*204ull;
                if(!reject) {
                    float corner[3][3]{};
                    for(uint32_t p=0;p<kTrianglePoints;++p) {
                        uint32_t samplesTaken; memcpy(&samplesTaken,record+p*204ull+32,4);
                        float mean[3]={0,0,0}; if(samplesTaken) memcpy(mean,record+p*204ull,12);
                        for(int k=0;k<3;++k) for(int channel=0;channel<3;++channel) corner[k][channel]+=mean[channel]*weights[p][k];
                    }
                    for(int k=0;k<3;++k) for(int channel=0;channel<3;++channel) out[k*3+channel]=corner[k][channel]*scale[k];
                    continue;
                }
                float mean[kTrianglePoints][3]; bool drop[kTrianglePoints];
                tileMeans(record,mean,drop);
                float corner[3][3]{},weight[3]{};
                for(uint32_t p=0;p<kTrianglePoints;++p) {
                    if(drop[p]) continue;
                    for(int k=0;k<3;++k) {
                        weight[k]+=weights[p][k];
                        for(int channel=0;channel<3;++channel) corner[k][channel]+=mean[p][channel]*weights[p][k];
                    }
                }
                for(int k=0;k<3;++k) for(int channel=0;channel<3;++channel) out[k*3+channel]=corner[k][channel]/std::max(weight[k],1.0f);
            }
        });
    });
    if(split) {
        std::vector<float> pointMeans(subOwner.size()*kTrianglePoints*3ull,0.0f);
        for(size_t range=0;range<rangePerPoint.size();++range) {
            const size_t base=rangeFirst[range];
            trace(subCorners.data()+base*9,subNormals.data()+base*3,static_cast<uint32_t>(rangeFirst[range+1]-base),rangePerPoint[range],false,
                  0x8000+static_cast<uint32_t>(range)*0x100,[&](uint32_t first,uint32_t count) {
                parallelRange(count,4096,[&](size_t from,size_t to) {
                    for(size_t t=from;t<to;++t) {
                        const size_t sub=base+first+t;
                        const uint8_t* record=records.data()+t*kTrianglePoints*204ull;
                        float mean[kTrianglePoints][3]; bool drop[kTrianglePoints];
                        tileMeans(record,mean,drop);
                        memcpy(pointMeans.data()+sub*kTrianglePoints*3ull,mean,sizeof(mean));
                    }
                });
            });
        }
        parallelRange(splitTriangles.size(),256,[&](size_t from,size_t to) {
            for(size_t i=from;i<to;++i) {
                float* out=output.data()+splitTriangles[i]*9ull;
                double cell[kTrianglePoints][4]{};
                for(size_t s=firstSub[i];s<firstSub[i+1];++s) {
                    const float* bary=subBary.data()+s*9ull; const float* mean=pointMeans.data()+s*kTrianglePoints*3ull;
                    for(uint32_t p=0;p<kTrianglePoints;++p) {
                        float u,v,w; triangleBarycentrics(p,u,v,w);
                        const double b1=static_cast<double>(bary[1])*u+static_cast<double>(bary[4])*v+static_cast<double>(bary[7])*w;
                        const double b2=static_cast<double>(bary[2])*u+static_cast<double>(bary[5])*v+static_cast<double>(bary[8])*w;
                        const double sq=std::clamp(b1+b2,0.0,1.0);
                        const int row=std::min(3,static_cast<int>(4*sq*sq)),column=sq>0?std::clamp(static_cast<int>(4*b2/sq),0,3):0;
                        double* c=cell[row*4+column];
                        for(int channel=0;channel<3;++channel) c[channel]+=mean[p*3+channel];
                        c[3]+=1;
                    }
                }
                double tile[3]={0,0,0}; int filled=0;
                for(auto& c:cell) if(c[3]>0) {
                    for(int channel=0;channel<3;++channel) { c[channel]/=c[3]; tile[channel]+=c[channel]; }
                    ++filled;
                }
                for(double& value:tile) value=std::max(0.0,value/std::max(filled,1))*8.0;
                double corner[3][3]{},weight[3]{};
                for(uint32_t c=0;c<kTrianglePoints;++c) {
                    if(!(cell[c][3]>0) || (reject && (cell[c][0]>tile[0] || cell[c][1]>tile[1] || cell[c][2]>tile[2]))) continue;
                    for(int k=0;k<3;++k) {
                        weight[k]+=weights[c][k];
                        for(int channel=0;channel<3;++channel) corner[k][channel]+=cell[c][channel]*weights[c][k];
                    }
                }
                for(int k=0;k<3;++k) for(int channel=0;channel<3;++channel)
                    out[k*3+channel]=static_cast<float>(reject?corner[k][channel]/std::max(weight[k],1.0):weight[k]>0?corner[k][channel]/weight[k]:0);
            }
        });
        previewReport.splitTriangles+=splitTriangles.size(); previewReport.subTriangles+=subOwner.size();
        previewReport.splitEdge=std::max(previewReport.splitEdge,subdivide);
    }
    for(float value:output) if(!std::isfinite(value)) throw std::runtime_error("Nonfinite GPU triangle lighting");
    return output;
}
static fs::path triangleFile(const std::vector<std::wstring>& forwarded,const wchar_t* folder,const std::string& name) {
    const fs::path file=fs::u8path(name);
    if(file.is_absolute()) return file;
    const auto base=argumentValue(forwarded,folder);
    return base.empty()?file:fs::path(base)/file;
}
static bool triangleGpuCall(const CONTEXT& context,const std::vector<std::wstring>& forwarded,const PreviewSamples& samples,float subdivide,
                            PreviewCorners method,bool smallJob) {
    const ULONGLONG started=GetTickCount64();
    const TriangleArguments args=triangleArguments(context);
    const fs::path input=triangleFile(forwarded,L"-indir",args.input),output=triangleFile(forwarded,L"-outdir",args.output);
    std::ifstream file(input,std::ios::binary);
    if(!file) throw TriangleCpuCall("Cannot read "+args.input);
    file.seekg(0,std::ios::end); const uint64_t size=static_cast<uint64_t>(file.tellg());
    if(size<(args.start+args.corners)*12ull) throw TriangleCpuCall(args.input+" is shorter than the requested vertices");
    requirePhysicalMemory(args.corners*24ull,"triangle corners");
    std::vector<float> corners(args.corners*3ull);
    file.seekg(static_cast<std::streamoff>(args.start*12ull)); file.read(reinterpret_cast<char*>(corners.data()),args.corners*12ll);
    if(!file) throw TriangleCpuCall("Cannot read "+args.input);
    if(smallJob && gpuEmission.empty()) {
        bool big=false;
        for(uint32_t t=0;subdivide>0 && !big && t<args.corners/3;++t) big=triangleLongestEdge(corners.data()+t*9ull)>subdivide;
        if(!big) throw TriangleCpuCall("small job: Valve's CPU is quicker");
    }
    progressWindow.status(L"GPU preview triangles");
    progressWindow.begin(L"Preview triangles on GPU ("+std::to_wstring(args.corners/3)+L")... ");
    struct Line { bool done=false; ~Line() { progressWindow.end(done); } } line;
    double gpuMs=0;
    const uint64_t splitBefore=previewReport.splitTriangles,subBefore=previewReport.subTriangles;
    const auto lighting=triangleGpuStage(args,corners,samples,subdivide,method,gpuMs);
    writeFile(output,lighting.data(),lighting.size()*sizeof(float));
    const double stageMs=static_cast<double>(GetTickCount64()-started);
    ++previewReport.gpuCalls; previewReport.gpuTriangles+=args.corners/3; previewReport.gpuMs+=gpuMs; previewReport.stageMs+=stageMs;
    GpuLightingDiagnostics local; GetGpuLightingDiagnostics(&local); if(local.adapter[0]) hammerHardware=local;
    line.done=true;
    std::cout << "Live GPU triangles: " << args.input << " triangles=" << args.corners/3 << " samples=" << samples.min << '-' << samples.max
              << " error=" << samples.error << " corners=" << (method==PreviewCorners::Cs2?"cs2":"valve")
              << " split=" << previewReport.splitTriangles-splitBefore << " into " << previewReport.subTriangles-subBefore
              << ' ' << gpuMs << " ms GPU, " << stageMs << " ms stage\n" << std::flush;
    progressWindow.status(L"Valve CPU stages");
    return true;
}
// Placeholder voxel file left by the shim: ceil(cells/32) zero words, then this marker, a NUL and the flood_fill byte (0 or 1).
static constexpr char kPendingVoxels[]="vrad3-preview-voxels-v1";
static uint64_t pendingVoxelBytes(const fs::path& file,bool* flood=nullptr) {
    constexpr uint64_t trailer=sizeof(kPendingVoxels)+1;
    std::error_code error; const uint64_t size=fs::file_size(file,error);
    if(error || size<=trailer) return 0;
    std::ifstream input(file,std::ios::binary); input.seekg(static_cast<std::streamoff>(size-trailer));
    char tail[trailer]{}; input.read(tail,trailer);
    if(!input || memcmp(tail,kPendingVoxels,sizeof(kPendingVoxels)) || static_cast<uint8_t>(tail[trailer-1])>1) return 0;
    if(flood) *flood=tail[trailer-1]==1;
    return size-trailer;
}
struct PreviewScript { uint64_t corners=0; uint32_t probes=0,voxels=0; };
static PreviewScript scanPreviewScript(const std::vector<std::wstring>& forwarded,const std::wstring& entrypoint) {
    std::ifstream input(argumentValue(forwarded,L"-script")); std::string line; bool inside=false;
    const std::string label=logUtf8(entrypoint)+":";
    PreviewScript result;
    while(std::getline(input,line)) {
        std::istringstream tokens(line); std::string command;
        if(!(tokens>>command)) continue;
        if(command.back()==':') { inside=command==label; continue; }
        std::string first,start,count;
        if(inside && command=="path_trace_triangles" && (tokens>>first>>start>>count))
            try { result.corners+=std::stoull(count); } catch(...) {}
        if(inside && command=="lpv_indirect") ++result.probes;
        if(command=="lpv_load_solid_voxels" && (tokens>>first) && pendingVoxelBytes(triangleFile(forwarded,L"-indir",first))) ++result.voxels;
    }
    return result;
}
static std::unique_ptr<VoxelScene> previewVoxelScene;
static const VoxelScene& readVoxelScene() {
    if(previewVoxelScene) return *previewVoxelScene;
    // vrad3 RVAs: 0x32aa20 is the loaded scene pointer.
    const uintptr_t scene=pointerAt(0x32aa20);
    if(!scene) throw std::runtime_error("No scene loaded before lpv_load_solid_voxels");
    auto flat=flattenScene(scene,FlatRead::Geometry);
    auto result=std::make_unique<VoxelScene>();
    result->corners.resize(flat.triangles*9ull); result->flags=std::move(flat.flags);
    for(uint64_t v=0;v<flat.triangles*3ull;++v) memcpy(result->corners.data()+v*3,flat.vertices.data()+v*16,12);
    previewVoxelScene=std::move(result);
    return *previewVoxelScene;
}
static void previewVoxelCall(const CONTEXT& context,const std::vector<std::wstring>& forwarded) {
    if(static_cast<uint32_t>(context.Rdx)<2 || !context.R8) return;
    const uintptr_t argument=readValue<uintptr_t>(context.R8+8);
    std::string name;
    for(size_t j=0;;++j) {
        if(j==1024) throw std::runtime_error("lpv_load_solid_voxels argument too long");
        const char c=readValue<char>(argument+j); if(!c) break; name+=c;
    }
    const fs::path file=triangleFile(forwarded,L"-indir",name);
    bool flood=false; const uint64_t bytes=pendingVoxelBytes(file,&flood);
    if(!bytes) return;
    const ULONGLONG started=GetTickCount64();
    VoxelVolume volume; volume.flood=flood;
    uint64_t cells=1;
    // vrad3 RVAs: lpv cell counts at 0x1de868, box at 0x1de850, local->world matrix at 0x32adf0.
    for(uint32_t a=0;a<3;++a) { volume.cells[a]=countAt(0x1de868+a*4); cells*=volume.cells[a]; }
    readMemory(address(0x1de850),volume.box,sizeof(volume.box)); readMemory(address(0x32adf0),volume.matrix,sizeof(volume.matrix));
    for(float v:volume.box) if(!std::isfinite(v)) throw std::runtime_error("Invalid probe volume box");
    for(float v:volume.matrix) if(!std::isfinite(v)) throw std::runtime_error("Invalid probe volume transform");
    if(!cells || (cells+31)/32*4!=bytes) throw std::runtime_error(name+" doesn't match lpv_cells");
    progressWindow.status(L"Preview probe voxels");
    const auto result=voxelizeVolume(readVoxelScene(),volume,std::max<uint32_t>(1,usableCpuCount()));
    writeFile(file,result.words.data(),result.words.size()*4);
    const double ms=static_cast<double>(GetTickCount64()-started);
    ++previewReport.voxelVolumes; previewReport.voxelMs+=ms;
    std::cout << "Preview voxels (" << kPendingVoxels << "): " << name << ' ' << volume.cells[0] << 'x' << volume.cells[1] << 'x' << volume.cells[2]
              << " solid=" << result.solid << " flood=" << (flood?1:0) << ' ' << ms << " ms" << (result.note.empty()?"":" ("+result.note+")") << '\n' << std::flush;
    progressWindow.status(L"Valve CPU stages");
}
static bool previewProbeCall(const CONTEXT& context,const std::vector<std::wstring>& forwarded,ProbeSetting setting) {
    ProbeArguments args;
    try { args=probeArguments(context); }
    catch(const std::exception& error) { throw TriangleCpuCall(error.what()); }
    uint64_t cells=1; for(int i=0;i<3;++i) cells*=args.end[i]-args.start[i];
    const bool gpuReady=previewReport.gpuCalls || previewReport.probeGpu;
    if(setting==ProbeSetting::Auto && gpuEmission.empty() && cells*args.samples<(gpuReady?probeMinimumPaths():previewMinimumPaths()))
        throw TriangleCpuCall("small probe volume: Valve's CPU is quicker");
    probeReport=ProbeReport{};
    if(!probeGpuStage(context,forwarded,ProbeSetting::On)) return false;
    ++previewReport.probeGpu; previewReport.gpuMs+=probeReport.gpuMs;
    return true;
}
struct PreviewPlan {
    bool triangles=false;
    bool smallTriangles=false;
    PreviewSamples samples;
    float subdivide=0;
    PreviewCorners corners=PreviewCorners::Valve;
    ProbeSetting probes=ProbeSetting::Off;
    bool voxels=false;
    bool emissive=false;
};
static int runPreviewWorker(const fs::path& real,const std::vector<std::wstring>& forwarded,const PreviewPlan& plan) {
    previewReport=PreviewReport{}; triangleSceneUploaded=false; triangleCallIndex=0; previewVoxelScene.reset();
    emissiveApplied=false; hammerEmissive.clear(); gpuEmission.clear(); gpuEmissionMasks.clear();
    PROCESS_INFORMATION info{};
    bool exited=false; DWORD exitCode=1;
    try {
        STARTUPINFOW startup{}; startup.cb=sizeof(startup); startup.dwFlags=STARTF_USESTDHANDLES;
        startup.hStdInput=GetStdHandle(STD_INPUT_HANDLE); startup.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE); startup.hStdError=GetStdHandle(STD_ERROR_HANDLE);
        std::wstring command=hammerCommand;
        DWORD flags=DEBUG_ONLY_THIS_PROCESS|CREATE_NO_WINDOW|childPriorityFlag();
        progressWindow.launch(startup,flags);
        if(!CreateProcessW(real.c_str(),command.data(),nullptr,nullptr,TRUE,flags,nullptr,nullptr,&startup,&info)) fail("CreateProcessW");
        processHandle=info.hProcess;
        DebugSetProcessKillOnExit(TRUE);
        enum { kTriangles,kVoxels,kProbes,kHooks };
        struct Hook { uintptr_t rva; std::vector<uint8_t> prologue; const char* name; bool wanted,gpu; uintptr_t at=0; bool rearm=false; Breakpoint entry; };
        // Entry RVAs of Valve's commands in vrad3_real.exe, with their first bytes to verify the build.
        Hook hooks[kHooks]={
            {0x58f20,{0x40,0x55,0x53,0x48,0x8d,0xac,0x24,0x48,0xff,0xff,0xff,0x48,0x81,0xec,0xb8,0x01},"path_trace_triangles",plan.triangles || plan.emissive || plan.smallTriangles,plan.triangles || plan.smallTriangles},
            {0x3d680,{0x48,0x81,0xec,0x88,0x00,0x00,0x00,0x83,0xfa,0x02,0x7d,0x1f},"lpv_load_solid_voxels",plan.voxels,false},
            {0x3f190,{0x49,0x8b,0xc0,0x8b,0xca,0x48,0x8b,0xd0},"lpv_indirect",plan.probes!=ProbeSetting::Off || plan.emissive,plan.probes!=ProbeSetting::Off},
        };
        Breakpoint leave;
        bool gpuUsable=true;
        DEBUG_EVENT event{};
        for(;;) {
            if(!WaitForDebugEvent(&event,1000)) {
                if(GetLastError()==ERROR_SEM_TIMEOUT) continue;
                fail("Child debug event wait failed");
            }
            DWORD disposition=DBG_CONTINUE;
            if(event.dwDebugEventCode==CREATE_PROCESS_DEBUG_EVENT) {
                imageBase=reinterpret_cast<uintptr_t>(event.u.CreateProcessInfo.lpBaseOfImage);
                if(event.u.CreateProcessInfo.hFile) CloseHandle(event.u.CreateProcessInfo.hFile);
                for(auto& hook:hooks) {
                    if(!hook.wanted) continue;
                    std::string problem;
                    try {
                        hook.at=address(hook.rva);
                        std::vector<uint8_t> prefix(hook.prologue.size()); readMemory(hook.at,prefix.data(),prefix.size());
                        if(prefix!=hook.prologue) problem=std::string("unexpected ")+hook.name+" entry";
                    } catch(const std::exception& error) { problem=error.what(); }
                    if(problem.empty()) { hook.entry.arm(hook.at); continue; }
                    hook.wanted=false; previewReport.reason=problem;
                    if(&hook==&hooks[kVoxels]) throw std::runtime_error("Preview probe voxels unavailable: "+problem);
                }
            } else if(event.dwDebugEventCode==LOAD_DLL_DEBUG_EVENT) {
                if(event.u.LoadDll.hFile) CloseHandle(event.u.LoadDll.hFile);
            } else if(event.dwDebugEventCode==EXCEPTION_DEBUG_EVENT) {
                auto exception=event.u.Exception.ExceptionRecord;
                const uintptr_t hit=reinterpret_cast<uintptr_t>(exception.ExceptionAddress);
                Hook* entered=nullptr;
                for(auto& hook:hooks) if(hook.entry.armed && hit==hook.entry.location) entered=&hook;
                if(exception.ExceptionCode==EXCEPTION_BREAKPOINT && (entered || (leave.armed && hit==leave.location))) {
                    HANDLE thread=OpenThread(THREAD_GET_CONTEXT|THREAD_SET_CONTEXT,FALSE,event.dwThreadId);
                    if(!thread) fail("OpenThread");
                    CONTEXT context{}; context.ContextFlags=CONTEXT_FULL;
                    if(!GetThreadContext(thread,&context)) fail("GetThreadContext");
                    context.Rip=hit;
                    if(!entered) {
                        leave.restore();
                        for(auto& hook:hooks) if(hook.rearm) { hook.rearm=false; if(hook.wanted) hook.entry.arm(hook.at); }
                    } else {
                        entered->entry.restore();
                        bool skipped=false;
                        if(entered==&hooks[kVoxels]) previewVoxelCall(context,forwarded);
                        else applyEmissiveMaterials(real,fs::current_path());
                        if(entered!=&hooks[kVoxels] && entered->gpu && gpuUsable) {
                            try {
                                skipped=entered==&hooks[kTriangles]?triangleGpuCall(context,forwarded,plan.samples,plan.subdivide,plan.corners,!plan.triangles)
                                                                   :previewProbeCall(context,forwarded,plan.probes);
                            } catch(const TriangleCpuCall& call) {
                                previewReport.reason=call.what();
                                std::cerr << "GPU preview: this " << entered->name << " on CPU: " << call.what() << '\n' << std::flush;
                            } catch(const std::exception& failure) {
                                previewReport.reason=failure.what();
                                gpuUsable=false;
                                std::cerr << "GPU preview unavailable; continuing with CPU: " << failure.what() << '\n' << std::flush;
                            }
                        }
                        if(skipped) {
                            context.Rip=readValue<uintptr_t>(context.Rsp); context.Rsp+=sizeof(uintptr_t);
                            entered->entry.arm(entered->at);
                        } else {
                            if(entered==&hooks[kTriangles]) ++previewReport.cpuCalls;
                            if(entered==&hooks[kProbes]) ++previewReport.probeCpu;
                            if(entered==&hooks[kVoxels] || (entered->gpu && gpuUsable)) {
                                const uintptr_t back=readValue<uintptr_t>(context.Rsp);
                                if(leave.armed && leave.location!=back) leave.restore();
                                if(!leave.armed) leave.arm(back);
                                entered->rearm=true;
                            }
                        }
                    }
                    if(!SetThreadContext(thread,&context)) fail("SetThreadContext");
                    CloseHandle(thread);
                } else if(exception.ExceptionCode!=EXCEPTION_BREAKPOINT) {
                    disposition=DBG_EXCEPTION_NOT_HANDLED;
                    if(!event.u.Exception.dwFirstChance) std::cerr << "Unhandled exception 0x" << std::hex << exception.ExceptionCode << std::dec << "\n";
                }
            } else if(event.dwDebugEventCode==EXIT_PROCESS_DEBUG_EVENT) {
                exitCode=event.u.ExitProcess.dwExitCode; exited=true; previewReport.childExit=static_cast<int>(exitCode);
                progressWindow.childExited();
                if(!ContinueDebugEvent(event.dwProcessId,event.dwThreadId,disposition)) fail("Continue child exit");
                break;
            }
            if(!ContinueDebugEvent(event.dwProcessId,event.dwThreadId,disposition)) fail("ContinueDebugEvent");
        }
        CloseHandle(info.hThread); CloseHandle(info.hProcess); info={};
        previewVoxelScene.reset();
        cleanupGpu(&ShutdownGpuLighting);
        return static_cast<int>(exitCode);
    } catch(const std::exception& error) {
        std::cerr << error.what() << "\n";
        previewReport.reason=error.what();
        if(info.hProcess) { if(!exited) TerminateProcess(info.hProcess,1); CloseHandle(info.hProcess); CloseHandle(info.hThread); }
        previewVoxelScene.reset();
        cleanupGpu(&ShutdownGpuLighting);
        return exited && exitCode ? static_cast<int>(exitCode) : 1;
    }
}
#endif
