#pragma once
// Replaces Valve's sample stage from Reject Invalid on (cs2 estimator): GPU reject, then the footprint,
// normal, tangent and confidence data are computed on all CPU threads and written into the child.
#ifdef VRAD3_STATIC_GPU
#include <thread>
#include <functional>
#include <atomic>
static bool sampleStageWanted() {
    return environmentValue(L"VRAD3_GPU_ESTIMATOR")==L"cs2";
}
// RVA of the end of Valve's sample stage; the skipped code is jumped over to here.
constexpr uintptr_t sampleStageEndRva=0xace1;
static std::string sampleStageSummary;
static const char* sampleStageStep="";
struct StageSample { float u,v; int32_t chart; float w40,w44,w; float n[3]; float t[4]; };
static void stageParallel(uint32_t count,const std::function<void(uint32_t,uint32_t)>& body) {
    const uint32_t threads=std::max(1u,std::min<uint32_t>(usableCpuCount(),64));
    std::atomic<uint32_t> next{0}; const uint32_t step=std::max(1u,count/(threads*16));
    std::vector<std::thread> pool;
    std::exception_ptr error; std::mutex errorLock;
    for(uint32_t t=0;t<threads;++t) pool.emplace_back([&] {
        try { for(uint32_t first;(first=next.fetch_add(step))<count;) body(first,std::min(count,first+step)); }
        catch(...) { std::lock_guard<std::mutex> lock(errorLock); if(!error) error=std::current_exception(); }
    });
    for(auto& thread:pool) thread.join();
    if(error) std::rethrow_exception(error);
}
template<class T> static uintptr_t stageAllocate(size_t count,const char* what) {
    void* memory=VirtualAllocEx(processHandle,nullptr,std::max<size_t>(1,count)*sizeof(T),MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    if(!memory) fail(what);
    return reinterpret_cast<uintptr_t>(memory);
}
static void stageWrite(uintptr_t at,const void* data,size_t bytes,const char* what) {
    SIZE_T written=0;
    if(bytes && (!WriteProcessMemory(processHandle,reinterpret_cast<void*>(at),data,bytes,&written) || written!=bytes)) fail(what);
}
// CUtlVector header {i32 count; T* ptr; i32 cap; u32 flags} at an RVA. Flags 0x80000000 mark memory
// Valve does not own, so it never frees it.
static void stageVector(uintptr_t rva,uintptr_t pointer,int32_t count) {
    uint8_t header[24]{}; const uint32_t flags=0x80000000u;
    memcpy(header,&count,4); memcpy(header+8,&pointer,8); memcpy(header+16,&count,4); memcpy(header+20,&flags,4);
    stageWrite(address(rva),header,24,"Write sample stage vector");
}
struct StageAllocations {
    std::vector<uintptr_t> owned;
    template<class T> uintptr_t add(size_t count,const char* what) { owned.push_back(stageAllocate<T>(count,what)); return owned.back(); }
    ~StageAllocations() { for(const uintptr_t at:owned) VirtualFreeEx(processHandle,reinterpret_cast<void*>(at),0,MEM_RELEASE); }
};
static bool sampleStageAtCall(CONTEXT& context,const fs::path& realExe,bool& changed) {
    changed=false;
    const ULONGLONG started=GetTickCount64();
    const int32_t count=readValue<int32_t>(address(0x32ad28));
    const uintptr_t samples=pointerAt(0x32ad30);
    if(count<=0 || !samples) return false;
    const uint32_t mask=(readValue<uint32_t>(address(0x17ae44))&readValue<uint32_t>(address(0x17ae40)))|1u;
    if(mask!=0x40b1u) throw std::runtime_error("unverified sample reject mask");
    // Block globals (RVAs): 32aba0 x0,y0; 32aba8 width,height; 32ab88 luxel count.
    const int32_t x0=readValue<int32_t>(address(0x32aba0)),y0=readValue<int32_t>(address(0x32aba4));
    const int32_t bw=readValue<int32_t>(address(0x32aba8)),bh=readValue<int32_t>(address(0x32abac));
    const int32_t luxels=readValue<int32_t>(address(0x32ab88));
    if(bw<=0 || bh<=0 || luxels!=bw*bh) throw std::runtime_error("unexpected block size");
    if(readValue<int32_t>(address(0x32ac60))<luxels)
        throw std::runtime_error("luxel info has "+std::to_string(readValue<int32_t>(address(0x32ac60)))+" entries for "+std::to_string(luxels)+" luxels");
    sampleStageStep="luxel info"; const auto luxelInfo=copyArray(pointerAt(0x32ac68),static_cast<size_t>(luxels),5,"read_stage_luxels");
    const int32_t charts=readValue<int32_t>(address(0x32ac30));
    sampleStageStep="charts"; const auto chartRecords=copyArray(pointerAt(0x32ac38),static_cast<size_t>(std::max(charts,0)),25,"read_stage_charts");
    // Luxel info: 5-byte entries, chart id first. Chart records are 25 bytes; flag bit 0 / 1 at +0x18
    // collapses the kernel window to one column / row.
    auto chartFlags=[&](int32_t chart)->uint8_t { return chart>=0 && chart<charts ? chartRecords[chart*25ull+0x18] : 0; };
    float directions[96]; rejectDirections(realExe,directions);
    bool sceneResident=gpuServer::active();
    std::vector<StageSample> kept; kept.reserve(static_cast<size_t>(count));
    std::vector<uint8_t> keepSample(static_cast<size_t>(count),0);
    uint64_t gpuMsTotal=0,dropped=0; double gpuMs=0; RejectInputs rejectInputs;
    for(uint32_t first=0;first<static_cast<uint32_t>(count);first+=rejectChunk) {
        const uint32_t n=std::min<uint32_t>(rejectChunk,count-first);
        sampleStageStep="reject"; const auto records=copyArray(samples+first*static_cast<uintptr_t>(rejectStride),n,rejectStride,"read_stage_samples");
        const auto result=rejectChunkOnGpu(records,n,mask,directions,sceneResident,gpuMs,rejectInputs);
        for(uint32_t i=0;i<n;++i) {
            const uint8_t* record=records.data()+i*static_cast<size_t>(rejectStride);
            const uint32_t* r=result.data()+i*4ull;
            // Sample record: +0c normal, +18 face normal, +24 tangent and sign (4 floats), +34 u, +38 v, +3c chart,
            // +40 and +44 weights.
            int32_t chart; float w40,w44; memcpy(&chart,record+0x3c,4); memcpy(&w40,record+0x40,4); memcpy(&w44,record+0x44,4);
            if(static_cast<int>(r[0])>13) chart=-1;
            const int m=32-static_cast<int>(r[1]);
            if(m<8) w40=static_cast<float>(m)*0.125f;
            if(chart==-1 || w40==0.0f || w44==0.0f) continue;
            float shading[3],face[3]; memcpy(shading,record+0x0c,12); memcpy(face,record+0x18,12);
            const float ns=shading[0]*shading[0]+shading[1]*shading[1]+shading[2]*shading[2];
            const float nf=face[0]*face[0]+face[1]*face[1]+face[2]*face[2];
            if(!std::isfinite(w40*w44) || !(std::fabs(ns-1.0f)<=0.002f) || !(std::fabs(nf-1.0f)<=0.002f)) { ++dropped; continue; }
            StageSample s{}; float f[12]; memcpy(f,record+0x0c,12); memcpy(f+3,record+0x24,16);
            memcpy(&s.u,record+0x34,4); memcpy(&s.v,record+0x38,4); s.chart=chart; s.w40=w40; s.w44=w44; s.w=w40*w44;
            memcpy(s.n,f,12); memcpy(s.t,f+3,16);
            kept.push_back(s); keepSample[first+i]=1;
        }
    }
    gpuMsTotal=static_cast<uint64_t>(gpuMs);
    const int32_t keptCount=static_cast<int32_t>(kept.size());
    const ULONGLONG rejected=GetTickCount64();
    sampleStageStep="footprint";
    const int32_t gx0=x0-2,gy0=y0-2,gw=bw+4,gh=bh+4;
    std::vector<uint32_t> bucketStart(static_cast<size_t>(gw)*gh+1,0),bucketed;
    auto bucketOf=[&](const StageSample& s)->int64_t {
        const int32_t tx=static_cast<int32_t>(std::floor(s.u))-gx0,ty=static_cast<int32_t>(std::floor(s.v))-gy0;
        return tx<0 || ty<0 || tx>=gw || ty>=gh ? -1 : static_cast<int64_t>(ty)*gw+tx;
    };
    for(const auto& s:kept) { const int64_t b=bucketOf(s); if(b>=0) ++bucketStart[b+1]; }
    for(size_t b=1;b<bucketStart.size();++b) bucketStart[b]+=bucketStart[b-1];
    bucketed.resize(bucketStart.back());
    { std::vector<uint32_t> cursor(bucketStart.begin(),bucketStart.end()-1);
      for(uint32_t i=0;i<kept.size();++i) { const int64_t b=bucketOf(kept[i]); if(b>=0) bucketed[cursor[b]++]=i; } }
    auto visit=[&](int32_t lx,int32_t ly,auto&& use) {
        uint32_t chart; memcpy(&chart,luxelInfo.data()+(static_cast<size_t>(ly)*bw+lx)*5,4);
        if(chart==0xffffffffu) return;
        const int32_t tx=x0+lx,ty=y0+ly;
        for(int32_t dy=-2;dy<=2;++dy) for(int32_t dx=-2;dx<=2;++dx) {
            const size_t b=static_cast<size_t>(ty+dy-gy0)*gw+(tx+dx-gx0);
            for(uint32_t j=bucketStart[b];j<bucketStart[b+1];++j) {
                const uint32_t i=bucketed[j]; const StageSample& s=kept[i];
                if(static_cast<uint32_t>(s.chart)!=chart) continue;
                const uint8_t flags=chartFlags(s.chart);
                if((flags&1) && dx) continue;
                if((flags&2) && dy) continue;
                const float du=s.u-(static_cast<float>(tx)+0.5f),dv=s.v-(static_cast<float>(ty)+0.5f),d2=du*du+dv*dv;
                // Valve's kernel: 0.7978846*expf(-2*d^4) over the 5x5 texels around the sample, d^2 <= 4.
                const float k=d2<=4.0f ? expf(d2*d2*-2.0f)*0.7978846f : 0.0f;
                const int32_t nx=lx+dx,ny=ly+dy; uint32_t owner=0xffffffffu;
                if(nx>=0 && ny>=0 && nx<bw && ny<bh) memcpy(&owner,luxelInfo.data()+(static_cast<size_t>(ny)*bw+nx)*5,4);
                const float pick=owner==chart && d2<=4.0f ? expf(d2*-2.0f)*0.7978846f*s.w44 : 0.0f;
                if(k!=0.0f || pick!=0.0f) use(i,k,pick);
            }
        }
    };
    std::vector<int32_t> offsets(static_cast<size_t>(luxels)+1,0);
    stageParallel(static_cast<uint32_t>(luxels),[&](uint32_t a,uint32_t b) {
        for(uint32_t l=a;l<b;++l) { int32_t c=0; visit(static_cast<int32_t>(l%bw),static_cast<int32_t>(l/bw),[&](uint32_t,float,float pick) { if(pick!=0.0f) ++c; }); offsets[l+1]=c; }
    });
    for(size_t l=1;l<offsets.size();++l) offsets[l]+=offsets[l-1];
    const int64_t entries=offsets.back();
    if(entries>INT32_MAX) throw std::runtime_error("sample stage: too many footprint entries");
    StageAllocations allocations;
    const uintptr_t offsetsAt=allocations.add<int32_t>(offsets.size(),"Allocate stage offsets");
    const uintptr_t indicesAt=allocations.add<int32_t>(static_cast<size_t>(entries),"Allocate stage indices");
    const uintptr_t cdfAt=allocations.add<float>(static_cast<size_t>(entries),"Allocate stage cdf");
    stageWrite(offsetsAt,offsets.data(),offsets.size()*4,"Write stage offsets");
    const int32_t bandRows=std::max(1,std::min(bh,65536/std::max(bw,1)));
    std::vector<float> outputWords(static_cast<size_t>(luxels)*11,0.0f);
    std::vector<int32_t> bandIndices; std::vector<float> bandCdf; std::atomic<bool> reported{false};
    for(int32_t row=0;row<bh;row+=bandRows) {
        const int32_t rows=std::min(bandRows,bh-row);
        const uint32_t firstLuxel=static_cast<uint32_t>(row*bw),bandLuxels=static_cast<uint32_t>(rows*bw);
        const int64_t base=offsets[firstLuxel],bandEntries=offsets[firstLuxel+bandLuxels]-base;
        bandIndices.assign(static_cast<size_t>(bandEntries),0); bandCdf.assign(static_cast<size_t>(bandEntries),0);
        stageParallel(bandLuxels,[&](uint32_t a,uint32_t b) {
            std::vector<std::pair<float,uint32_t>> list; std::vector<std::pair<uint32_t,float>> kernel,picks;
            for(uint32_t q=a;q<b;++q) {
                const uint32_t l=firstLuxel+q;
                float N[3]={0,0,0},T[4]={0,0,0,0},sum=0; list.clear(); kernel.clear(); picks.clear();
                visit(static_cast<int32_t>(l%bw),static_cast<int32_t>(l/bw),[&](uint32_t i,float k,float pick) {
                    if(k!=0.0f) kernel.push_back({i,k});
                    if(pick!=0.0f) picks.push_back({i,pick}); });
                std::sort(kernel.begin(),kernel.end(),[](const auto& p,const auto& r) { return p.first<r.first; });
                for(const auto& [i,k]:kernel) {
                    const StageSample& s=kept[i];
                    for(int c=0;c<3;++c) N[c]+=k*s.n[c];
                    for(int c=0;c<4;++c) T[c]+=k*s.t[c];
                    sum+=k; list.push_back({k*s.w,i});
                }
                float cs2Confidence=0;
                {
                    float total=0,weighted=0;
                    for(const auto& [k,i]:list) { total+=k; weighted+=k*kept[i].w40; }
                    cs2Confidence=total>0 ? weighted/total : 0;
                    list.clear(); for(const auto& [i,pick]:picks) list.push_back({pick,i});
                }
                std::sort(list.begin(),list.end(),[](const auto& p,const auto& r) { return p.first>r.first || (p.first==r.first && p.second<r.second); });
                const size_t at=static_cast<size_t>(offsets[l]-base);
                float total=0; for(const auto& e:list) total+=e.first;
                float running=0,previous=0;
                for(size_t j=0;j<list.size();++j) {
                    running+=list[j].first;
                    const float c=j+1==list.size() ? 1.0f : running/total;
                    if(!(c>=previous) && !reported.exchange(true)) {
                        std::ostringstream d; d<<"GPU sample stage: CDF check failed luxel="<<l<<" entry="<<j<<'/'<<list.size()<<" total="<<total<<" c="<<c<<" previous="<<previous<<" weights:";
                        for(size_t e=0;e<list.size() && e<12;++e) d<<' '<<list[e].first<<"(w40="<<kept[list[e].second].w40<<" w="<<kept[list[e].second].w<<')';
                        std::cerr<<d.str()<<'\n'<<std::flush;
                    }
                    bandIndices[at+j]=static_cast<int32_t>(list[j].second); bandCdf[at+j]=c;
                    previous=c;
                }
                float out[11]={0,0,0,0,0,0,0,0,0,0,0};
                if(sum!=0) {
                    const float r=1.0f/sum;
                    for(int c=0;c<3;++c) N[c]*=r;
                    for(int c=0;c<4;++c) T[c]*=r;
                    auto normalize=[](float* v) { const float length=std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);
                        if(length>0) { const float inverse=1.0f/length; for(int c=0;c<3;++c) v[c]*=inverse; } else v[0]=v[1]=v[2]=0;
                        return length; };
                    const float Lt=std::sqrt(T[0]*T[0]+T[1]*T[1]+T[2]*T[2]),w=T[3];
                    const float L=normalize(N);
                    for(int c=0;c<3;++c) out[c]=N[c];
                    out[3]=std::sqrt((1-L)/L);
                    normalize(T);
                    for(int c=0;c<3;++c) out[4+c]=T[c];
                    out[7]=w>=0 ? 1.0f : -1.0f;
                    out[8]=static_cast<float>(90-static_cast<int>(w*w*90.0f))+std::sqrt((1-Lt)/Lt);
                    out[9]=sum;
                }
                out[10]=std::min(1.0f,std::max(0.0f,cs2Confidence));
                memcpy(outputWords.data()+static_cast<size_t>(l)*11,out,44);
            }
        });
        stageWrite(indicesAt+static_cast<uintptr_t>(base)*4,bandIndices.data(),bandIndices.size()*4,"Write stage indices");
        stageWrite(cdfAt+static_cast<uintptr_t>(base)*4,bandCdf.data(),bandCdf.size()*4,"Write stage cdf");
    }
    sampleStageStep="commit";
    {
        uint32_t written=0;
        for(uint32_t first=0;first<static_cast<uint32_t>(count);first+=rejectChunk) {
            const uint32_t n=std::min<uint32_t>(rejectChunk,count-first);
            auto records=copyArray(samples+first*static_cast<uintptr_t>(rejectStride),n,rejectStride,"read_stage_samples");
            uint32_t keep=0;
            for(uint32_t i=0;i<n;++i) {
                if(!keepSample[first+i]) continue;
                uint8_t* record=records.data()+keep*static_cast<size_t>(rejectStride);
                if(keep!=i) memmove(record,records.data()+i*static_cast<size_t>(rejectStride),rejectStride);
                memcpy(record+0x40,&kept[written+keep].w40,4);
                ++keep;
            }
            changed=true;
            stageWrite(samples+written*static_cast<uintptr_t>(rejectStride),records.data(),keep*static_cast<size_t>(rejectStride),"Write compacted samples");
            written+=keep;
        }
        if(written!=kept.size()) throw std::runtime_error("sample stage: compacted count changed");
    }
    stageWrite(address(0x32ad28),&keptCount,4,"Write kept sample count");
    const uintptr_t outputs=pointerAt(0x32ab90);
    for(int32_t row=0;row<bh;row+=bandRows) {
        const int32_t rows=std::min(bandRows,bh-row);
        const uint32_t firstLuxel=static_cast<uint32_t>(row*bw),bandLuxels=static_cast<uint32_t>(rows*bw);
        // Per-luxel output records are 0x8c bytes; words 24-34 (+0x60) are normal, spread, tangent, tangent sign,
        // 90-degree term, weight sum and confidence.
        auto records=copyArray(outputs+firstLuxel*0x8cull,bandLuxels,0x8c,"read_stage_outputs");
        for(uint32_t q=0;q<bandLuxels;++q) memcpy(records.data()+q*0x8cull+0x60,outputWords.data()+static_cast<size_t>(firstLuxel+q)*11,44);
        stageWrite(outputs+firstLuxel*0x8cull,records.data(),records.size(),"Write stage outputs");
    }
    allocations.owned.clear();
    // Valve's footprint offsets, indices and cdf vectors.
    stageVector(0x32aaf8,offsetsAt,static_cast<int32_t>(offsets.size()));
    stageVector(0x32acd0,indicesAt,static_cast<int32_t>(entries));
    stageVector(0x32ab50,cdfAt,static_cast<int32_t>(entries));
    // Empty Valve's Map list local and restore the callee-saved registers the skipped code would have
    // restored (frame offset and saved registers are per build).
    const uint8_t empty[24]{}; stageWrite(context.Rbp+valveBuild->stageMapLocal,empty,24,"Clear sample stage local");
    for(const SavedRegister& saved:valveBuild->stageRestores) {
        if(saved.gpr) readMemory(context.Rsp+saved.rsp,&(context.*saved.gpr),8);
        else readMemory(context.Rsp+saved.rsp,&(&context.Xmm0)[saved.xmm],16);
    }
    context.Rip=address(sampleStageEndRva);
    std::ostringstream line;
    line<<"GPU sample stage: samples="<<count<<" kept="<<keptCount<<" degenerate="<<dropped<<" entries="<<entries<<" footprint=cs2 gpu_ms="<<gpuMsTotal
        <<" reject_ms="<<rejected-started<<" footprint_ms="<<GetTickCount64()-rejected<<" stage_ms="<<GetTickCount64()-started;
    std::cout<<line.str()<<'\n'<<std::flush;
    sampleStageSummary="kept="+std::to_string(keptCount)+" entries="+std::to_string(entries)+" stage_ms="+std::to_string(GetTickCount64()-started);
    return true;
}
#endif
