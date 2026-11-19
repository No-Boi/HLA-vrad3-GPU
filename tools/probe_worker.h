#pragma once
// lpv_compute worker: runs Valve's worker under a debugger and replaces the lpv_indirect stage with the GPU
// when it can; otherwise Valve's CPU stage runs unchanged.
#ifdef VRAD3_STATIC_GPU
enum class ProbeSetting { Auto, On, Off };
static ProbeSetting probeSetting() {
    const auto value=environmentValue(L"VRAD3_GPU_PROBES");
    if(value.empty()) return ProbeSetting::Auto;
    if(value==L"1") return ProbeSetting::On;
    if(value==L"0") return ProbeSetting::Off;
    throw std::runtime_error("VRAD3_GPU_PROBES must be 0 or 1");
}
struct ProbeReport {
    bool gpu=false,server=false;
    std::string reason;
    double stageMs=0,gpuMs=0;
    uint64_t activeCells=0;
    uint32_t stripes=1;
    int childExit=-1;
    std::vector<uint8_t> candidate;
    fs::path cpuFile;
};
static ProbeReport probeReport;
// In auto mode, below this many requested sample paths the CPU stage is cheaper than GPU setup.
static uint64_t probeMinimumPaths() {
    const auto value=environmentValue(L"VRAD3_GPU_PROBE_MIN_PATHS");
    if(!value.empty() && value.find_first_not_of(L"0123456789")==std::wstring::npos && value.size()<=12) return std::stoull(value);
    static const uint64_t paths=(1ull<<15)*std::max(1u,usableCpuCount());
    return paths;
}
// The GPU samples one strategy where Valve's estimator combines two, so it takes this multiple of the
// requested maximum samples to leave no more noise than the CPU.
constexpr uint32_t probeSampleFactor=8,probeSampleLimit=32768;
static bool probeGpuStage(const CONTEXT& context,const std::vector<std::wstring>& forwarded,ProbeSetting setting) {
    progressWindow.status(L"GPU light probes");
    ULONGLONG started=GetTickCount64();
    ProbeArguments args=probeArguments(context);
    if(setting==ProbeSetting::Auto && emissiveSetting()<=0) {
        uint64_t cells=1; for(int i=0;i<3;++i) cells*=args.end[i]-args.start[i];
        if(cells*args.samples<probeMinimumPaths())
            throw std::runtime_error("small volume: CPU probes are cheaper than GPU setup (light_probes=on forces GPU)");
    }
    const uint32_t requested=args.samples;
    args.samples=std::min<uint32_t>(args.samples*probeSampleFactor,probeSampleLimit);
    const auto samples=environmentValue(L"VRAD3_PROBE_SPP");
    if(!samples.empty()) args.samples=static_cast<uint32_t>(std::stoul(samples));
    args.factor=std::max(1u,args.samples/requested);
    progressWindow.begin(L"Light Probes on GPU ("+std::to_wstring(args.samples)+L" spp)... ");
    struct Line { bool done=false; ~Line() { progressWindow.end(done); } } line;
    NativeLighting inputs; ProbePlan plan;
    std::vector<uint8_t> records; std::array<uint8_t,32> token{};
    GpuLightingTiming timing{}; char error[512]{};
    const auto seed=environmentValue(L"VRAD3_PROBE_SEED");
    auto prepare=[&](bool sceneResident) {
        { BlockProfileScope profile(sceneResident?"prepare_probe_small":"prepare_probe"); inputs=prepareProbe(args,plan,sceneResident); }
        if(!seed.empty()) inputs.constants[2]=static_cast<uint32_t>(std::stoul(seed));
        if(gpuServer::enabled) token=probeSceneToken(inputs,gpuServer::compileKey);
        const size_t cellCount=inputs.constants[0];
        if(records.size()!=cellCount*probeRecordBytes) { requirePhysicalMemory(cellCount*probeRecordBytes,"probe records"); records.assign(cellCount*probeRecordBytes,0); }
        GpuLightingRequest request{}; request.version=5;
        memcpy(request.constants,inputs.constants,sizeof(inputs.constants));
        for(size_t i=0;i<inputs.buffers.size();++i) request.buffers[i]={inputs.buffers[i].data(),inputs.buffers[i].size()};
        request.output=records.data(); request.outputBytes=records.size();
        request.shaderBytes=lightingShaderBytes(); request.shaderSize=lightingShaderSize();
        request.totalPaths=inputs.totalPaths; request.progress=progressWindow.progress();
        if(gpuServer::enabled) { request.quickToken=token.data(); request.sceneSkipped=sceneResident; }
        return request;
    };
    std::string reason; int outcome=0;
    if(gpuServer::enabled) {
        auto request=prepare(true);
        outcome=gpuServer::bakeForWorker(request,timing,reason);
        if(outcome==2) throw std::runtime_error(reason);
    }
    probeReport.server=outcome==1;
    if(outcome!=1) {
        auto request=prepare(false);
        if(outcome==3) {
            reason.clear();
            outcome=gpuServer::bakeForWorker(request,timing,reason);
            if(outcome==2) throw std::runtime_error(reason);
            probeReport.server=outcome==1;
        }
        if(!probeReport.server) {
            if(!reason.empty()) std::cout << "GPU server unavailable; using in-worker GPU: " << reason << '\n' << std::flush;
            request.quickToken=nullptr; request.sceneSkipped=false;
            if(BakeGpuLighting(&request,&timing,error,sizeof(error))) throw std::runtime_error(error);
        }
    }
    const auto raw=environmentValue(L"VRAD3_PROBE_RAW");
    fs::path folder=argumentValue(forwarded,L"-outdir");
    if(!raw.empty()) { fs::create_directories(raw); writeFile(fs::path(raw)/(fs::path(args.name).stem().wstring()+L".records"),records.data(),records.size()); }
    if(verifyItem) {
        std::vector<size_t> offsets;
        for(size_t face=0;face<6;++face) for(size_t c=0;c<3;++c) offsets.push_back(face*36+c*4);
        verifyCorrupt(records,probeRecordBytes,offsets);
    }
    auto packed=packProbe(records,args,inputs.buffers[7],plan.stripes);
    if(!folder.empty()) fs::create_directories(folder);
    if(verifyItem) { probeReport.candidate=std::move(packed); probeReport.cpuFile=folder/fs::path(args.name); }
    else writeFile(folder/fs::path(args.name),packed.data(),packed.size());
    probeReport.gpuMs=timing.lightingMs; probeReport.stageMs=static_cast<double>(GetTickCount64()-started);
    probeReport.activeCells=inputs.totalPaths/args.samples; probeReport.stripes=plan.stripes;
    if(!probeReport.server) { GpuLightingDiagnostics local; GetGpuLightingDiagnostics(&local); if(local.adapter[0]) hammerHardware=local; }
    line.done=true;
    std::cout << "Live GPU probes: " << args.name << " cells=" << probeReport.activeCells << " spp=" << args.samples << " stripes=" << plan.stripes
              << " " << timing.lightingMs << " ms GPU, " << probeReport.stageMs << " ms stage" << (probeReport.server?" (server)":"") << '\n' << std::flush;
    progressWindow.status(L"Valve CPU stages");
    return true;
}
static int runProbeWorker(const fs::path& real,const std::vector<std::wstring>& forwarded,ProbeSetting setting) {
    probeReport=ProbeReport{};
    emissiveApplied=false; hammerEmissive.clear(); gpuEmission.clear(); gpuEmissionMasks.clear();
    const bool researchCpu=environmentValue(L"VRAD3_PROBE_RESEARCH")==L"cpu";
    const bool research=researchCpu || !environmentValue(L"VRAD3_ISOLATE").empty() || !environmentValue(L"VRAD3_FORCE_OPAQUE").empty();
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
        Breakpoint entry,leave;
        if(research) outputPath=fs::absolute(argumentValue(forwarded,L"-outdir"));
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
                uint8_t prefix[8]; readMemory(address(0x3f190),prefix,sizeof(prefix));
                // The lpv_indirect stage Run function is at RVA 3f190; its first bytes (mov rax,r8; mov ecx,edx;
                // mov rdx,rax) are checked before the breakpoint is armed.
                const uint8_t expected[8]={0x49,0x8b,0xc0,0x8b,0xca,0x48,0x8b,0xd0};
                if(memcmp(prefix,expected,sizeof(prefix))) throw std::runtime_error("Unexpected lpv_indirect entry; refusing to patch");
                if(setting!=ProbeSetting::Off || research) entry.arm(address(0x3f190));
            } else if(event.dwDebugEventCode==LOAD_DLL_DEBUG_EVENT) {
                if(event.u.LoadDll.hFile) CloseHandle(event.u.LoadDll.hFile);
            } else if(event.dwDebugEventCode==EXCEPTION_DEBUG_EVENT) {
                auto exception=event.u.Exception.ExceptionRecord;
                uintptr_t hit=reinterpret_cast<uintptr_t>(exception.ExceptionAddress);
                if(exception.ExceptionCode==EXCEPTION_BREAKPOINT && leave.armed && hit==leave.location) {
                    HANDLE thread=OpenThread(THREAD_GET_CONTEXT|THREAD_SET_CONTEXT,FALSE,event.dwThreadId);
                    if(!thread) fail("Open verify probe thread");
                    CONTEXT context{}; context.ContextFlags=CONTEXT_FULL;
                    if(!GetThreadContext(thread,&context)) fail("Get verify probe context");
                    leave.restore(); context.Rip=hit;
                    try {
                        auto cpu=loadFile(probeReport.cpuFile); auto& gpu=probeReport.candidate;
                        if(cpu.size()!=gpu.size() || cpu.size()%24) throw std::runtime_error("Verify probe size mismatch");
                        std::ostringstream line; line<<std::setprecision(6)<<"VRAD3 GPU verify: "<<verifyLabel;
                        bool ok=true; size_t cells=cpu.size()/24;
                        auto decode=[](const uint8_t* p,size_t c) {
                            if(verifyWord(p)==0x80808080u) return 0.0;
                            return std::ldexp(double(static_cast<int8_t>(p[c]))/127,static_cast<int8_t>(p[3]));
                        };
                        for(size_t face=0;face<6;++face) {
                            VerifyMetric metric;
                            for(size_t cell=0;cell<cells;++cell) {
                                auto c=cpu.data()+(face*cells+cell)*4,g=gpu.data()+(face*cells+cell)*4;
                                for(size_t channel=0;channel<3;++channel) metric.add(decode(c,channel),decode(g,channel));
                            }
                            std::string name="face"+std::to_string(face); metric.print(line,name.c_str()); ok=ok && metric.ok();
                        }
                        line<<" -> "<<(ok?"OK":"CHECK")<<" (CPU result written)";
                        verifyLine=line.str(); std::cout<<verifyLine<<'\n'<<std::flush;
                        gpu.clear(); gpu.shrink_to_fit();
                    } catch(const std::exception& error) { verifyUnavailable(error.what()); }
                    if(!SetThreadContext(thread,&context)) fail("Set verify probe context");
                    CloseHandle(thread);
                } else if(exception.ExceptionCode==EXCEPTION_BREAKPOINT && entry.armed && hit==entry.location) {
                    HANDLE thread=OpenThread(THREAD_GET_CONTEXT|THREAD_SET_CONTEXT,FALSE,event.dwThreadId);
                    if(!thread) fail("OpenThread");
                    CONTEXT context{}; context.ContextFlags=CONTEXT_FULL;
                    if(!GetThreadContext(thread,&context)) fail("GetThreadContext");
                    entry.restore(); context.Rip=hit;
                    applyEmissiveMaterials(real,fs::current_path());
                    bool skipped=false;
                    try {
                        if(research) isolatePathtrace(true);
                        if(researchCpu || setting==ProbeSetting::Off) throw std::runtime_error(researchCpu?"research CPU run":"light_probes=off");
                        skipped=probeGpuStage(context,forwarded,setting);
                    } catch(const std::exception& failure) {
                        probeReport.reason=failure.what();
                        if(verifyItem) verifyUnavailable(failure.what());
                        std::cerr << "GPU probes unavailable; continuing with CPU: " << failure.what() << '\n' << std::flush;
                    }
                    if(skipped && verifyItem) { leave.arm(readValue<uintptr_t>(context.Rsp)); skipped=false; probeReport.reason="CPU verified"; }
                    if(skipped) {
                        // Skip the stage: return straight to its caller (the stage has not run its prologue).
                        context.Rip=readValue<uintptr_t>(context.Rsp); context.Rsp+=sizeof(uintptr_t);
                        probeReport.gpu=true;
                    }
                    if(!SetThreadContext(thread,&context)) fail("SetThreadContext");
                    CloseHandle(thread);
                } else if(exception.ExceptionCode!=EXCEPTION_BREAKPOINT) {
                    disposition=DBG_EXCEPTION_NOT_HANDLED;
                    if(!event.u.Exception.dwFirstChance) std::cerr << "Unhandled exception 0x" << std::hex << exception.ExceptionCode << std::dec << "\n";
                }
            } else if(event.dwDebugEventCode==EXIT_PROCESS_DEBUG_EVENT) {
                exitCode=event.u.ExitProcess.dwExitCode; exited=true; probeReport.childExit=static_cast<int>(exitCode);
                progressWindow.childExited();
                if(!ContinueDebugEvent(event.dwProcessId,event.dwThreadId,disposition)) fail("Continue child exit");
                break;
            }
            if(!ContinueDebugEvent(event.dwProcessId,event.dwThreadId,disposition)) fail("ContinueDebugEvent");
        }
        CloseHandle(info.hThread); CloseHandle(info.hProcess); info={};
        cleanupGpu(&ShutdownGpuLighting);
        if(exitCode==0 && verifyItem && !verifyLine.empty()) {
            try {
                auto file=fs::path(argumentValue(forwarded,L"-outdir"))/argumentValue(forwarded,L"-indirectfile");
                auto status=compileTempRoot()/L"verify-probes"/file.filename();
                fs::create_directories(status.parent_path());
                std::ofstream(status)<<logUtf8(fs::absolute(file).wstring())<<'\n'<<fileSha256(file)<<'\n'<<verifyLine<<'\n';
            } catch(const std::exception& error) { std::cerr<<"GPU verify status unavailable: "<<error.what()<<'\n'; }
        }
        return static_cast<int>(exitCode);
    } catch(const std::exception& error) {
        std::cerr << error.what() << "\n";
        if(info.hProcess) { if(!exited) TerminateProcess(info.hProcess,1); CloseHandle(info.hProcess); CloseHandle(info.hThread); }
        cleanupGpu(&ShutdownGpuLighting);
        return exited && exitCode ? static_cast<int>(exitCode) : 1;
    }
}
#endif
