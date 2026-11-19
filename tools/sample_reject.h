#pragma once
// GPU version of Valve's "Generate Samples - Reject Invalid" 32-ray test. Samples the GPU cannot decide
// safely are re-run on Valve's own CPU code.
#ifdef VRAD3_STATIC_GPU
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
enum class RejectSetting { Off, On, Verify };
static RejectSetting rejectSetting() {
    return environmentValue(L"VRAD3_GPU_SAMPLE_REJECT_VERIFY")==L"1"?RejectSetting::Verify:RejectSetting::On;
}
// RVAs in vrad3_real.exe: Valve's reject dispatcher call and the instruction after it.
// Sample records are 0x48 bytes: +00 position, +0c normal, +18 face normal, +3c chart id (-1 = invalid), +40 weight.
constexpr uintptr_t rejectCallRva=0xa0b0,rejectAfterRva=0xa0b5,rejectDispatchRva=0x2b8c0;
constexpr uint32_t rejectChunk=1u<<22,rejectStride=0x48;
struct RejectBatch { uint32_t first=0,n=0; std::vector<uint32_t> uncertain; std::vector<uint8_t> records; };
struct RejectPipeline {
    std::thread worker; std::mutex lock; std::condition_variable changed;
    std::deque<RejectBatch> ready; bool done=false; std::exception_ptr error;
    uint64_t certain=0; double gpuMs=0;
    ~RejectPipeline() { if(worker.joinable()) worker.join(); }
};
struct RejectRun {
    bool active=false,verify=false,fallback=false;
    uintptr_t samples=0,temp=0; int32_t count=0; size_t tempBytes=0;
    std::shared_ptr<RejectPipeline> pipeline;
    RejectBatch batch;
    CONTEXT call{};
    std::array<uint8_t,0x88> job{}; uint32_t counter=0;
    uint64_t uncertain=0; uint32_t batches=0;
    std::vector<uint8_t> gpuInvalid,gpuCertain,gpuM;
    std::vector<uint32_t> gpuRaw;
    uint64_t certain=0; double gpuMs=0,stageMs=0; ULONGLONG started=0;
    std::string note;
};
static RejectRun rejectRun;
static std::string rejectSummary;
static std::string rejectLogText;
struct RejectReasons { uint64_t hit=0,facing=0,limit=0,margin=0,open=0; } rejectReasons;
// Valve's 32 hemisphere directions: radical inverses base 3 (u) and 2 (v), phi=2*pi*v, with tier0's
// V_sincosf so they match Valve's bit for bit.
static void rejectDirections(const fs::path& realExe,float* out) {
    static float table[96]; static bool ready=false;
    if(!ready) {
        HMODULE tier0=LoadLibraryExW((realExe.parent_path()/L"tier0.dll").c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
        if(!tier0) throw std::runtime_error("tier0.dll not loadable for V_sincosf");
        using SinCos=void (*)(float,float*,float*);
        auto sincos=reinterpret_cast<SinCos>(GetProcAddress(tier0,"V_sincosf"));
        if(!sincos) throw std::runtime_error("tier0.dll has no V_sincosf");
        auto radical=[](uint32_t i,uint32_t base) {
            const float f0=1.0f/static_cast<float>(base); float f=f0,s=0;
            while(i) { s+=static_cast<float>(i%base)*f; f*=f0; i/=base; }
            return s;
        };
        for(uint32_t i=0;i<32;++i) {
            const float u=radical(i,3),v=radical(i,2),phi=v*6.2831855f;
            float sine=0,cosine=0; sincos(phi,&sine,&cosine);
            const float r=std::sqrt(u);
            table[3*i]=cosine*r; table[3*i+1]=sine*r; table[3*i+2]=std::sqrt(std::max(1.0f-u,0.0f));
        }
        ready=true;
    }
    memcpy(out,table,sizeof(table));
}
struct RejectPacked { std::vector<uint8_t> samples,basis; };
static RejectPacked packReject(const std::vector<uint8_t>& records,uint32_t n) {
    BlockProfileScope packProfile("pack_reject");
    RejectPacked packed; packed.samples.resize(n*36ull); packed.basis.resize(std::max<size_t>(96,n*3ull)*4);
    for(uint32_t i=0;i<n;++i) {
        const uint8_t* record=records.data()+i*static_cast<size_t>(rejectStride);
        float s[9]; memcpy(s,record,36);
        const float nx=s[3],ny=s[4],nz=s[5];
        // Valve's basis terms, in its float operation order.
        const float sign=nz>=0 ? 1.0f : -1.0f, a=-1.0f/(nz+sign), b=(nx*ny)*a;
        const float terms[3]={b,(nx*(nx*sign))*a+1.0f,(ny*ny)*a+sign};
        memcpy(packed.samples.data()+i*36ull,s,36); memcpy(packed.basis.data()+i*12ull,terms,12);
    }
    return packed;
}
struct RejectInputs { NativeLighting inputs; std::array<uint8_t,32> token{}; uint32_t n=0; bool small=false; };
static std::vector<uint32_t> requestReject(RejectPacked& packed,uint32_t n,uint32_t mask,const float* directions,
                                           bool& sceneResident,double& gpuMs,RejectInputs& kept) {
    std::vector<uint32_t> result(n*4ull);
    for(int attempt=0;attempt<2;++attempt) {
        if(!(kept.small && sceneResident && kept.n==n)) {
            BlockProfileScope profile(sceneResident?"prepare_reject_small":"prepare_reject");
            kept=RejectInputs{}; kept.inputs=prepareNative(1,0,0,sceneResident,n); kept.n=n; kept.small=sceneResident;
            if(gpuServer::active()) kept.token=gpuServer::lightmapToken(kept.inputs,0);
            kept.inputs.constants[3]=0x10000000u; memcpy(kept.inputs.constants+8,&mask,4); kept.inputs.totalPaths=n;
            memcpy(kept.inputs.buffers[9].data(),directions,384);
        }
        NativeLighting& inputs=kept.inputs;
        std::swap(inputs.buffers[8],packed.basis); std::swap(inputs.buffers[10],packed.samples);
        struct Lend { NativeLighting& inputs; RejectPacked& packed;
            ~Lend() { std::swap(inputs.buffers[8],packed.basis); std::swap(inputs.buffers[10],packed.samples); } } lend{inputs,packed};
        GpuLightingRequest request{}; request.version=5;
        memcpy(request.constants,inputs.constants,sizeof(inputs.constants));
        for(size_t i=0;i<inputs.buffers.size();++i) request.buffers[i]={inputs.buffers[i].data(),inputs.buffers[i].size()};
        request.output=reinterpret_cast<uint8_t*>(result.data()); request.outputBytes=result.size()*4;
        request.shaderBytes=lightingShaderBytes(); request.shaderSize=lightingShaderSize(); request.totalPaths=n;
        GpuLightingTiming timing{}; std::string reason; int outcome=0;
        BlockProfileScope requestProfile("request_reject");
        if(gpuServer::active()) {
            request.quickToken=kept.token.data(); request.sceneSkipped=sceneResident;
            outcome=gpuServer::bakeForWorker(request,timing,reason);
            if(outcome==2) throw std::runtime_error(reason);
            if(outcome==3) { sceneResident=false; continue; }
        }
        if(outcome!=1) {
            if(sceneResident) { sceneResident=false; continue; }
            request.quickToken=nullptr; request.sceneSkipped=false;
            char error[512]{};
            if(BakeGpuLighting(&request,&timing,error,sizeof(error))) throw std::runtime_error(error);
        }
        sceneResident=true; gpuMs+=timing.lightingMs;
        return result;
    }
    throw std::runtime_error("GPU sample reject: scene upload did not settle");
}
static std::vector<uint32_t> rejectChunkOnGpu(const std::vector<uint8_t>& records,uint32_t n,uint32_t mask,
                                              const float* directions,bool& sceneResident,double& gpuMs,RejectInputs& kept) {
    auto packed=packReject(records,n);
    return requestReject(packed,n,mask,directions,sceneResident,gpuMs,kept);
}
static void testRejectChunks(int32_t count,uintptr_t samples,uint32_t mask,const float* directions,bool verify,
                             uint64_t& certain,double& gpuMs,const std::function<void(RejectBatch&&)>& publish) {
    bool sceneResident=gpuServer::active();
    struct Chunk { std::vector<uint8_t> records; RejectPacked packed; };
    auto readChunk=[&](uint32_t first) {
        Chunk chunk; const uint32_t n=std::min<uint32_t>(rejectChunk,count-first);
        chunk.records=copyArray(samples+first*static_cast<uintptr_t>(rejectStride),n,rejectStride,"read_reject_samples");
        chunk.packed=packReject(chunk.records,n);
        return chunk;
    };
    RejectInputs kept;
    std::future<Chunk> next=std::async(std::launch::async,readChunk,0u);
    for(uint32_t first=0;first<static_cast<uint32_t>(count);first+=rejectChunk) {
        const uint32_t n=std::min<uint32_t>(rejectChunk,count-first);
        Chunk chunk=next.get();
        if(first+rejectChunk<static_cast<uint32_t>(count)) next=std::async(std::launch::async,readChunk,first+rejectChunk);
        auto& records=chunk.records;
        const auto result=requestReject(chunk.packed,n,mask,directions,sceneResident,gpuMs,kept);
        chunk.packed=RejectPacked{};
        RejectBatch batch; batch.first=first; batch.n=n;
        for(uint32_t i=0;i<n;++i) {
            const uint32_t* r=result.data()+i*4ull;
            const int back=static_cast<int>(r[0]),shortRays=static_cast<int>(r[1]),ub=static_cast<int>(r[2]);
            const int us=static_cast<int>(r[3]&0xffff),uh=static_cast<int>(r[3]>>16);
            // Valve: invalid if more than 13 of 32 rays hit back faces; fewer than 8 open rays scales the weight by open/8.
            // Certain only if one more ray going either way (e.g. a hit Embree finds and the GPU misses at an edge) cannot change that.
            const bool alwaysInvalid=back-ub-uh-1>13,neverInvalid=back+ub+uh+1<=13;
            const int m=32-shortRays,mLow=32-std::min(32,shortRays+us+uh+1);
            const bool isCertain=alwaysInvalid || (neverInvalid && mLow>=8);
            const bool invalid=back>13;
            if(!isCertain) {
                if(uh) ++rejectReasons.hit; else if(ub) ++rejectReasons.facing; else if(us) ++rejectReasons.limit;
                else if(!(back-1>13) && !(back+1<=13)) ++rejectReasons.margin; else ++rejectReasons.open;
            }
            if(verify) { rejectRun.gpuRaw[first+i]=uint32_t(back)|uint32_t(shortRays)<<8|uint32_t(ub)<<16|uint32_t(std::min(255,us+uh))<<24; rejectRun.gpuInvalid[first+i]=invalid; rejectRun.gpuCertain[first+i]=isCertain; rejectRun.gpuM[first+i]=static_cast<uint8_t>(m); continue; }
            uint8_t* record=records.data()+i*static_cast<size_t>(rejectStride);
            if(!isCertain) {
                batch.uncertain.push_back(first+i);
                batch.records.insert(batch.records.end(),record,record+rejectStride);
                continue;
            }
            ++certain;
            if(invalid) { const int32_t none=-1; memcpy(record+0x3c,&none,4); }
            if(m<8) { const float weight=static_cast<float>(m)*0.125f; memcpy(record+0x40,&weight,4); }
        }
        if(verify) continue;
        { BlockProfileScope writeProfile("write_reject"); SIZE_T written=0;
          if(!WriteProcessMemory(processHandle,reinterpret_cast<void*>(samples+first*static_cast<uintptr_t>(rejectStride)),records.data(),records.size(),&written) || written!=records.size())
              throw std::runtime_error("Write GPU sample reject results failed"); }
        if(!batch.uncertain.empty()) publish(std::move(batch));
    }
}
static bool nextRejectBatch() {
    auto& pipeline=*rejectRun.pipeline;
    std::unique_lock<std::mutex> hold(pipeline.lock);
    pipeline.changed.wait(hold,[&] { return !pipeline.ready.empty() || pipeline.done; });
    if(pipeline.error || pipeline.ready.empty()) return false;
    rejectRun.batch=std::move(pipeline.ready.front()); pipeline.ready.pop_front();
    return true;
}
static void runRejectBatch() {
    const auto& batch=rejectRun.batch;
    const size_t bytes=batch.records.size();
    if(bytes>rejectRun.tempBytes) {
        if(rejectRun.temp) VirtualFreeEx(processHandle,reinterpret_cast<void*>(rejectRun.temp),0,MEM_RELEASE);
        rejectRun.temp=0; rejectRun.tempBytes=0;
        void* temp=VirtualAllocEx(processHandle,nullptr,bytes,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
        if(!temp) fail("Allocate CPU sample reject copy");
        rejectRun.temp=reinterpret_cast<uintptr_t>(temp); rejectRun.tempBytes=bytes;
    }
    SIZE_T written=0;
    if(!WriteProcessMemory(processHandle,reinterpret_cast<void*>(rejectRun.temp),batch.records.data(),bytes,&written) || written!=bytes) fail("Write CPU sample reject copy");
    const int32_t uncertainCount=static_cast<int32_t>(batch.uncertain.size());
    // Valve's stage reads its samples through two globals: count at 0x32ad28, array pointer at 0x32ad30.
    if(!WriteProcessMemory(processHandle,reinterpret_cast<void*>(address(0x32ad28)),&uncertainCount,4,&written) ||
       !WriteProcessMemory(processHandle,reinterpret_cast<void*>(address(0x32ad30)),&rejectRun.temp,8,&written)) fail("Point sample reject at the copy");
    rejectRun.uncertain+=batch.uncertain.size(); ++rejectRun.batches;
}
static void mergeRejectBatch() {
    const auto& batch=rejectRun.batch;
    std::vector<uint8_t> copy(batch.records.size()); readMemory(rejectRun.temp,copy.data(),copy.size());
    const uintptr_t base=rejectRun.samples+batch.first*static_cast<uintptr_t>(rejectStride);
    auto records=copyArray(base,batch.n,rejectStride,"read_reject_merge");
    for(size_t k=0;k<batch.uncertain.size();++k)
        memcpy(records.data()+(batch.uncertain[k]-batch.first)*static_cast<size_t>(rejectStride)+0x3c,copy.data()+k*rejectStride+0x3c,8);
    SIZE_T written=0;
    if(!WriteProcessMemory(processHandle,reinterpret_cast<void*>(base),records.data(),records.size(),&written) || written!=records.size())
        fail("Write CPU sample reject results");
}
static void finishRejectWorker() {
    auto pipeline=rejectRun.pipeline;
    if(pipeline->worker.joinable()) pipeline->worker.join();
    rejectRun.certain=pipeline->certain; rejectRun.gpuMs=pipeline->gpuMs;
    if(pipeline->error) std::rethrow_exception(pipeline->error);
}
// Puts the thread back at the dispatcher call. The dispatcher consumes the job's item counter ([rsp+6c])
// and fields of [rbp+b8,rbp+140), so a repeat needs them restored.
static void repeatRejectCall(CONTEXT& context) {
    context=rejectRun.call; SIZE_T written=0;
    if(!WriteProcessMemory(processHandle,reinterpret_cast<void*>(context.Rbp+0xb8),rejectRun.job.data(),rejectRun.job.size(),&written) || written!=rejectRun.job.size() ||
       !WriteProcessMemory(processHandle,reinterpret_cast<void*>(context.Rsp+0x6c),&rejectRun.counter,4,&written)) fail("Restore sample reject job");
}
static bool rejectAtCall(CONTEXT& context,const fs::path& realExe,RejectSetting setting) {
    rejectRun=RejectRun{}; rejectRun.started=GetTickCount64(); rejectRun.verify=setting==RejectSetting::Verify;
    const int32_t count=readValue<int32_t>(address(0x32ad28));
    const uintptr_t samples=pointerAt(0x32ad30);
    if(count<=0 || !samples) return false;
    uint64_t minimum=2000000;
    const auto forced=environmentValue(L"VRAD3_GPU_SAMPLE_REJECT_MIN");
    if(!forced.empty() && forced.find_first_not_of(L"0123456789")==std::wstring::npos && forced.size()<=10) minimum=std::stoull(forced);
    if(!rejectRun.verify && static_cast<uint64_t>(count)<minimum) { rejectSummary="cpu: "+std::to_string(count)+" samples (small)"; return false; }
    rejectRun.count=count; rejectRun.samples=samples;
    // Ray mask from Valve's globals 0x17ae44 and 0x17ae40; the GPU scene is built for exactly 0x40b1.
    const uint32_t mask=(readValue<uint32_t>(address(0x17ae44))&readValue<uint32_t>(address(0x17ae40)))|1u;
    if(mask!=0x40b1u) throw std::runtime_error("unverified sample reject mask");
    std::array<float,96> directions; rejectDirections(realExe,directions.data());
    if(rejectRun.verify) {
        rejectRun.gpuInvalid.resize(count); rejectRun.gpuCertain.resize(count); rejectRun.gpuM.resize(count); rejectRun.gpuRaw.resize(count);
        testRejectChunks(count,samples,mask,directions.data(),true,rejectRun.certain,rejectRun.gpuMs,[](RejectBatch&&) {});
        rejectRun.active=true; return false;
    }
    auto pipeline=std::make_shared<RejectPipeline>();
    rejectRun.pipeline=pipeline; rejectRun.call=context;
    readMemory(context.Rbp+0xb8,rejectRun.job.data(),rejectRun.job.size()); readMemory(context.Rsp+0x6c,&rejectRun.counter,4);
    pipeline->worker=std::thread([pipeline,count,samples,mask,directions] {
        try {
            testRejectChunks(count,samples,mask,directions.data(),false,pipeline->certain,pipeline->gpuMs,[&](RejectBatch&& batch) {
                std::lock_guard<std::mutex> hold(pipeline->lock); pipeline->ready.push_back(std::move(batch)); pipeline->changed.notify_one();
            });
        } catch(...) { std::lock_guard<std::mutex> hold(pipeline->lock); pipeline->error=std::current_exception(); }
        std::lock_guard<std::mutex> hold(pipeline->lock); pipeline->done=true; pipeline->changed.notify_one();
    });
    if(nextRejectBatch()) { runRejectBatch(); rejectRun.active=true; return false; }
    finishRejectWorker();
    rejectRun.stageMs=static_cast<double>(GetTickCount64()-rejectRun.started);
    return true;
}
static bool rejectAfterCall(CONTEXT& context) {
    if(!rejectRun.active) return false;
    rejectRun.active=false;
    std::ostringstream line;
    if(rejectRun.verify) {
        uint64_t agree=0,disagree=0,uncertain=0,uncertainAgree=0; std::vector<uint32_t> examples;
        for(uint32_t first=0;first<static_cast<uint32_t>(rejectRun.count);first+=rejectChunk) {
            const uint32_t n=std::min<uint32_t>(rejectChunk,rejectRun.count-first);
            const auto records=copyArray(rejectRun.samples+first*static_cast<uintptr_t>(rejectStride),n,rejectStride,"read_reject_verify");
            for(uint32_t i=0;i<n;++i) {
                int32_t chart; float weight; memcpy(&chart,records.data()+i*rejectStride+0x3c,4); memcpy(&weight,records.data()+i*rejectStride+0x40,4);
                const uint32_t g=first+i; const int m=rejectRun.gpuM[g];
                const bool cpuInvalid=chart==-1;
                const bool same=cpuInvalid==(rejectRun.gpuInvalid[g]!=0) &&
                                (cpuInvalid || (m<8 ? weight==static_cast<float>(m)*0.125f : weight==1.0f));
                if(!rejectRun.gpuCertain[g]) { ++uncertain; uncertainAgree+=same; continue; }
                if(same) ++agree;
                else {
                    ++disagree;
                    if(examples.size()<8) {
                        examples.push_back(g);
                        float s[9]; memcpy(s,records.data()+i*rejectStride,36);
                        std::ostringstream detail; detail<<"GPU sample reject mismatch "<<g<<": cpu chart="<<chart<<" weight="<<weight<<" gpu invalid="<<int(rejectRun.gpuInvalid[g])
                                 <<" m="<<m<<" back="<<(rejectRun.gpuRaw[g]&255)<<" short="<<((rejectRun.gpuRaw[g]>>8)&255)
                                 <<" unsure="<<((rejectRun.gpuRaw[g]>>16)&255)<<'/'<<(rejectRun.gpuRaw[g]>>24)<<" pos=("<<s[0]<<','<<s[1]<<','<<s[2]<<") n=("<<s[3]<<','<<s[4]<<','<<s[5]<<") F=("<<s[6]<<','<<s[7]<<','<<s[8]<<")\n";
                        std::cout<<detail.str(); rejectLogText+=detail.str();
                    }
                }
            }
        }
        uint64_t nearBack=0,lowOpen=0;
        for(int32_t g=0;g<rejectRun.count;++g) {
            const uint32_t back=rejectRun.gpuRaw[g]&255,m=32-((rejectRun.gpuRaw[g]>>8)&255);
            if(back==13 || back==14) ++nearBack;
            if(back<13 && m<=8) ++lowOpen;
        }
        line<<"GPU sample reject margins: back13or14="<<nearBack<<" valid_open_le8="<<lowOpen<<'\n';
        line<<"GPU sample reject verify: samples="<<rejectRun.count<<" certain_agree="<<agree<<" certain_disagree="<<disagree
            <<" uncertain="<<uncertain<<" (gpu guess right "<<uncertainAgree<<") gpu_ms="<<rejectRun.gpuMs;
        for(uint32_t g:examples) line<<" mismatch="<<g;
        rejectSummary="verify certain_disagree="+std::to_string(disagree)+" uncertain="+std::to_string(uncertain);
    } else if(rejectRun.fallback) {
        try { finishRejectWorker(); } catch(const std::exception& error) { rejectRun.note=error.what(); }
        if(rejectRun.temp) VirtualFreeEx(processHandle,reinterpret_cast<void*>(rejectRun.temp),0,MEM_RELEASE);
        std::cerr << "GPU sample reject unavailable; Valve's stage ran on every sample: " << rejectRun.note << '\n' << std::flush;
        rejectSummary="CPU: "+rejectRun.note; rejectRun=RejectRun{};
        return false;
    } else {
        mergeRejectBatch();
        if(nextRejectBatch()) {
            runRejectBatch(); repeatRejectCall(context); rejectRun.active=true;
            return true;
        }
        SIZE_T written=0;
        if(rejectRun.pipeline->error) {
            if(!WriteProcessMemory(processHandle,reinterpret_cast<void*>(address(0x32ad28)),&rejectRun.count,4,&written) ||
               !WriteProcessMemory(processHandle,reinterpret_cast<void*>(address(0x32ad30)),&rejectRun.samples,8,&written)) fail("Restore sample reject globals");
            repeatRejectCall(context); rejectRun.active=true; rejectRun.fallback=true;
            return true;
        }
        finishRejectWorker();
        if(!WriteProcessMemory(processHandle,reinterpret_cast<void*>(address(0x32ad28)),&rejectRun.count,4,&written) ||
           !WriteProcessMemory(processHandle,reinterpret_cast<void*>(address(0x32ad30)),&rejectRun.samples,8,&written)) fail("Restore sample reject globals");
        VirtualFreeEx(processHandle,reinterpret_cast<void*>(rejectRun.temp),0,MEM_RELEASE);
        line<<"GPU sample reject: samples="<<rejectRun.count<<" gpu="<<rejectRun.certain<<" cpu="<<rejectRun.uncertain<<" batches="<<rejectRun.batches<<" gpu_ms="<<rejectRun.gpuMs;
        rejectSummary="gpu="+std::to_string(rejectRun.certain)+" cpu="+std::to_string(rejectRun.uncertain);
    }
    line<<" stage_ms="<<GetTickCount64()-rejectRun.started<<" why: hit="<<rejectReasons.hit<<" facing="<<rejectReasons.facing
        <<" limit="<<rejectReasons.limit<<" margin="<<rejectReasons.margin<<" open="<<rejectReasons.open;
    rejectSummary+=" stage_ms="+std::to_string(GetTickCount64()-rejectRun.started)+" why=hit:"+std::to_string(rejectReasons.hit)+",facing:"+
        std::to_string(rejectReasons.facing)+",limit:"+std::to_string(rejectReasons.limit)+",margin:"+std::to_string(rejectReasons.margin)+",open:"+std::to_string(rejectReasons.open);
    std::cout<<line.str()<<'\n'<<std::flush; rejectLogText+=line.str()+"\n";
    rejectRun=RejectRun{};
    return false;
}
#endif
