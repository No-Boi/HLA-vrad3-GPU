// gpu_usage policy: keeps the PC responsive during a compile (below-normal priorities, short GPU dispatches, optional busy cap).
#pragma once

// gpu_usage: auto = short GPU work units and below-normal priority; 50..99 adds a GPU busy-time cap (percent); max = long units, normal priority.
struct UsagePolicy {
    bool max=false;
    unsigned cap=0;
};
static std::wstring usageSetting() {
    wchar_t value[32]{};
    DWORD n=GetEnvironmentVariableW(L"VRAD3_GPU_USAGE",value,32);
    if(!n || n>=32) return {};
    std::wstring text(value);
    for(auto& c:text) c=static_cast<wchar_t>(towlower(c));
    return text;
}
static UsagePolicy usagePolicy() {
    UsagePolicy policy; const auto text=usageSetting();
    if(text==L"max") { policy.max=true; return policy; }
    if(!text.empty() && text.find_first_not_of(L"0123456789")==std::wstring::npos) {
        const unsigned long value=std::wcstoul(text.c_str(),nullptr,10);
        if(value>=50 && value<100) policy.cap=static_cast<unsigned>(value);
    }
    return policy;
}
static std::string usageDescription() {
    const auto policy=usagePolicy();
    return policy.max?"max":policy.cap?"auto+cap"+std::to_string(policy.cap):"auto";
}
static std::wstring usageDescriptionW() { const auto text=usageDescription(); return std::wstring(text.begin(),text.end()); }
// The batch controller aims a dispatch at 75% of this; 10 ms keeps desktop hitches few without costing much compile time.
static constexpr unsigned kResponsiveBatchMs=10,kMaxSpeedBatchMs=100;
static unsigned defaultBatchMs() { return usagePolicy().max?kMaxSpeedBatchMs:kResponsiveBatchMs; }

static std::string responsiveStatus="unchanged (gpu_usage=max)";
static DWORD childPriorityFlag() { return usagePolicy().max?0:BELOW_NORMAL_PRIORITY_CLASS; }
static void lowerPriorities() noexcept {
    try {
        if(usagePolicy().max) return;
        const bool cpu=SetPriorityClass(GetCurrentProcess(),BELOW_NORMAL_PRIORITY_CLASS)!=0;
        long status=-1;
        // D3DKMT scheduling priority class 1 = below normal (d3dkmthk.h); resolved at run time so the exe gains no import.
        if(HMODULE gdi=LoadLibraryW(L"gdi32.dll")) {
            using SetClass=long (WINAPI*)(HANDLE,int);
            if(auto set=reinterpret_cast<SetClass>(GetProcAddress(gdi,"D3DKMTSetProcessSchedulingPriorityClass")))
                status=set(GetCurrentProcess(),1);
        }
        char text[96];
        sprintf_s(text,"cpu=%s gpu=%s",cpu?"below_normal":"failed",
                  status==0?"below_normal":"failed");
        if(status!=0) sprintf_s(text,"cpu=%s gpu=failed(0x%08lx)",cpu?"below_normal":"failed",static_cast<unsigned long>(status));
        responsiveStatus=text;
    } catch(...) {}
}

struct GpuPacer {
    unsigned cap=0; double owed=0,slept=0; HANDLE timer=nullptr;
    GpuPacer() { cap=usagePolicy().cap; }
    ~GpuPacer() { if(timer) CloseHandle(timer); }
    GpuPacer(const GpuPacer&)=delete; GpuPacer& operator=(const GpuPacer&)=delete;
    void afterDispatch(double ms) {
        if(!cap) return;
        owed+=ms*(100.0-cap)/cap;
        if(owed<1.0) return;
        LARGE_INTEGER begin,end,frequency; QueryPerformanceCounter(&begin);
        bool waited=false;
        // 0x2 = CREATE_WAITABLE_TIMER_HIGH_RESOLUTION: Sleep granularity is too coarse for short waits.
        if(!timer) timer=CreateWaitableTimerExW(nullptr,nullptr,0x2 ,TIMER_ALL_ACCESS);
        if(timer) {
            LARGE_INTEGER due; due.QuadPart=-static_cast<LONGLONG>(owed*10000.0);
            waited=SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE) && WaitForSingleObject(timer,5000)==WAIT_OBJECT_0;
        }
        if(!waited) Sleep(static_cast<DWORD>(owed+0.5));
        QueryPerformanceCounter(&end); QueryPerformanceFrequency(&frequency);
        const double actual=(end.QuadPart-begin.QuadPart)*1000.0/frequency.QuadPart;
        slept+=actual; owed-=actual;
        if(owed<-50) owed=-50;
    }
};
