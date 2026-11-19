// Per-worker user logs: a one-line summary per run in vrad3-gpu.log, and detailed per-compile folders.
#pragma once
#include "version.h"
#include <algorithm>
#include <chrono>

struct UserLogLock {
    HANDLE handle=INVALID_HANDLE_VALUE;
    // Exclusive lock file; a sharing violation means another worker holds it, so retry.
    explicit UserLogLock(const fs::path& root) {
        for(unsigned retry=0;retry<500;++retry) {
            handle=CreateFileW((root/L"summary.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(handle!=INVALID_HANDLE_VALUE) return;
            if(GetLastError()!=ERROR_SHARING_VIOLATION) break;
            Sleep(10);
        }
        throw std::runtime_error("Cannot lock user log root");
    }
    ~UserLogLock() { if(handle!=INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
static std::string logUtf8(const std::wstring& text) {
    int size=WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    std::string result(size,'\0');
    WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),result.data(),size,nullptr,nullptr);
    return result;
}
static std::string logField(std::string text) {
    for(char& c:text) if(c=='\n' || c=='\r' || c=='\t') c=' ';
    return text;
}
static std::wstring logStamp(const SYSTEMTIME& time,bool iso=false) {
    wchar_t text[64];
    if(iso) swprintf_s(text,L"%04u-%02u-%02uT%02u:%02u:%02u",time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond);
    else swprintf_s(text,L"%04u%02u%02u-%02u%02u%02u",time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond);
    return text;
}
static bool lightPathsLogged() {
    wchar_t value[8]{};
    return GetEnvironmentVariableW(L"VRAD3_GPU_LIGHT_PATHS",value,8)==1 && value[0]==L'1';
}
static bool lightPathsAutomatic() {
    wchar_t value[8]{};
    return GetEnvironmentVariableW(L"VRAD3_GPU_LIGHT_PATHS_AUTO",value,8)==1 && value[0]==L'1';
}
static std::string scriptLightmapSize(const fs::path& script) {
    std::ifstream input(script);
    for(std::string line;std::getline(input,line);) {
        std::istringstream tokens(line); std::string command; unsigned width=0,height=0,block=0;
        if(tokens>>command && command=="lightmap_size" && tokens>>width>>height>>block)
            return std::to_string(width)+"x"+std::to_string(height)+" (blocks "+std::to_string(block)+"x"+std::to_string(block)+")";
    }
    return {};
}
static fs::path userLogRoot;
#include "launch_diagnostics.h"
struct UserWorkerLog;
static UserWorkerLog* activeWorkerLog=nullptr;
struct UserWorkerLog {
    LaunchDiagnostics launch;
    fs::path directory,workdir;
    std::wstring entry,block;
    std::string lightmapSize;
    std::string result="CPU stage",reason,prefetchDecision="off:not a lightmap worker",serverDecision="off:not a GPU worker";
    GpuLightingDiagnostics hardware{};
    double traceMs=0,directMs=0;
    int exitCode=-1;
    ULONGLONG started=GetTickCount64();
    HANDLE lease=INVALID_HANDLE_VALUE;
    bool written=false;
    void start(const std::vector<std::wstring>& args) {
        workdir=fs::canonical(fs::current_path());
        entry=argumentValue(args,L"-entrypoint"); block=argumentValue(args,L"-blocknum");
        if(!wrapperLogging) return;
        auto overrideRoot=environmentValue(L"VRAD3_GPU_LOG_FOLDER");
        if(overrideRoot.empty()) overrideRoot=environmentValue(L"VRAD3_GPU_CACHE");
        if(!overrideRoot.empty()) { userLogRoot=fs::absolute(overrideRoot); fs::create_directories(userLogRoot); }
        else {
            auto local=environmentValue(L"LOCALAPPDATA");
            if(local.empty()) throw std::runtime_error("LocalAppData unavailable; set log_folder");
            userLogRoot=fs::path(local)/L"vrad3-gpu"/L"logs"; fs::create_directories(userLogRoot);
        }
        lightmapSize=scriptLightmapSize(fs::path(argumentValue(args,L"-script")));
        if(wrapperLogging==1) return;
        UserLogLock lock(userLogRoot);
        SYSTEMTIME stamp{}; GetLocalTime(&stamp);
        fs::path script=argumentValue(args,L"-script");
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if(GetFileAttributesExW(script.c_str(),GetFileExInfoStandard,&attributes)) {
            FILETIME local{}; FileTimeToLocalFileTime(&attributes.ftLastWriteTime,&local); FileTimeToSystemTime(&local,&stamp);
        }
        std::wstring name=(workdir.filename()==L"_vrad3"?workdir.parent_path().filename():workdir.filename()).wstring();
        for(auto& c:name) if(c<L' ' || std::wstring(L"<>:\"/\\|?*").find(c)!=std::wstring::npos) c=L'_';
        // FNV-1a hash of the working directory: maps with the same name and timestamp get separate folders.
        uint64_t identity=14695981039346656037ull;
        for(wchar_t c:workdir.wstring()) { identity^=static_cast<uint64_t>(towlower(c)); identity*=1099511628211ull; }
        std::wostringstream suffix; suffix<<std::hex<<identity;
        fs::path compile=userLogRoot/(logStamp(stamp)+L"-"+name+L"-"+suffix.str());
        fs::create_directories(compile);
        std::ofstream(compile/L"vrad3-compile.marker") << "VRAD3 detailed logs\n";
        lease=CreateFileW((compile/L"active.lock").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(lease==INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot lease compile logs");
        std::vector<fs::directory_entry> folders;
        for(const auto& item:fs::directory_iterator(userLogRoot)) {
            if(item.path()==compile || !item.is_directory() ||
               (GetFileAttributesW(item.path().c_str())&FILE_ATTRIBUTE_REPARSE_POINT) ||
               !fs::is_regular_file(item.path()/L"vrad3-compile.marker")) continue;
            folders.push_back(item);
        }
        std::sort(folders.begin(),folders.end(),[](const auto& a,const auto& b) { return a.last_write_time()>b.last_write_time(); });
        for(size_t i=0;i<folders.size();++i) {
            auto& item=folders[i];
            if(i<4 && fs::file_time_type::clock::now()-item.last_write_time()<=std::chrono::hours(24*14)) continue;
            HANDLE idle=CreateFileW((item.path()/L"active.lock").c_str(),GENERIC_READ,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(idle==INVALID_HANDLE_VALUE) continue;
            CloseHandle(idle);
            bool linked=false;
            for(const auto& nested:fs::recursive_directory_iterator(item.path()))
                if(GetFileAttributesW(nested.path().c_str())&FILE_ATTRIBUTE_REPARSE_POINT) { linked=true; break; }
            if(!linked) fs::remove_all(item.path());
        }
        directory=compile/(block.empty()?entry:L"block-"+block);
        fs::create_directories(directory);
        activeWorkerLog=this;
        installFaultDiagnostics(directory);
        launch.start(compile);
    }
    void append(bool crash=false) noexcept {
        if(written || userLogRoot.empty()) return;
        try {
            UserLogLock lock(userLogRoot);
            fs::path summary=userLogRoot/L"vrad3-gpu.log";
            // Rotate the summary log at 5 MB.
            if(fs::exists(summary) && fs::file_size(summary)>=5ull*1024*1024) {
                bool rotated=false;
                for(unsigned retry=0;retry<500;++retry) {
                    if(MoveFileExW(summary.c_str(),(userLogRoot/L"vrad3-gpu.old.log").c_str(),MOVEFILE_REPLACE_EXISTING)) { rotated=true; break; }
                    if(GetLastError()!=ERROR_SHARING_VIOLATION && GetLastError()!=ERROR_ACCESS_DENIED) break;
                    Sleep(10);
                }
                if(!rotated) throw std::runtime_error("Cannot rotate summary log (Win32 "+std::to_string(GetLastError())+")");
            }
            SYSTEMTIME time{}; GetLocalTime(&time);
            auto driver=hardware.driver;
            std::ostringstream line;
            line<<logUtf8(logStamp(time,true))<<"\tversion="<<VRAD3_VERSION
                <<"\tmap="<<logField(logUtf8(workdir.wstring()))<<"\tentry="<<logUtf8(entry)<<"\tblock="<<logUtf8(block)<<"\tlightmap_size="<<lightmapSize
                <<"\tadapter="<<(hardware.adapter[0]?logField(logUtf8(hardware.adapter)):"unavailable")
                <<"\tdriver="<<(driver>>48)<<'.'<<((driver>>32)&65535)<<'.'<<((driver>>16)&65535)<<'.'<<(driver&65535)
                <<"\tvram_budget_bytes="<<hardware.budget<<"\tthreads="<<usableCpuCount()
                <<"\tresult="<<(crash?"wrapper crash":result)<<"\treason="<<logField(reason)
                <<"\tprefetch="<<logField(prefetchDecision)<<"\tgpu_server="<<logField(serverDecision)
                <<"\tgpu_usage="<<usageDescription()<<"\tpriority="<<responsiveStatus
                <<"\tprogress_window="<<(ProgressWindow::mode()==2?"visible":ProgressWindow::enabled()?"on":"off")
                <<"\testimator="<<(environmentValue(L"VRAD3_GPU_ESTIMATOR")==L"cs2"?"cs2":"hla")
                <<"\tsample_grid="<<logUtf8(environmentValue(L"VRAD3_GPU_SAMPLE_GRID"))
                <<"\tlight_paths="<<(lightPathsAutomatic()?(lightPathsLogged()?"auto-on":"auto-off"):lightPathsLogged()?"on":"off")
                <<"\tlight_path_emitters="<<(nativeLightPathEmitters<0?std::string():std::to_string(nativeLightPathEmitters))
                <<"\tsample_reject="<<logField(rejectSummaryText())
                <<"\temitter_tree="<<(lightPathsLogged()?nativeLightPathTree:"")
                <<"\tgpu_trace_ms="<<traceMs<<"\tgpu_direct_ms="<<directMs<<"\ttotal_ms="<<GetTickCount64()-started
                <<"\tchild_exit="<<(crash || exitCode==-1?"unavailable":std::to_string(exitCode))
                <<"\tcrash_dump="<<(!directory.empty() && (crash || fs::exists(directory/L"wrapper-fault.dmp"))?logUtf8((directory/L"wrapper-fault.dmp").wstring()):"")<<'\n';
            std::ofstream file(summary,std::ios::binary|std::ios::app); file<<line.str(); if(!verifyLine.empty()) file<<verifyLine<<'\n'; file.close();
            if(!file) throw std::runtime_error("Cannot append user summary");
            if(!rejectLogContents().empty()) {
                std::ofstream rejects(userLogRoot/L"sample-reject.log",std::ios::binary|std::ios::app);
                rejects<<logUtf8(logStamp(time,true))<<"  map="<<logUtf8(workdir.wstring())<<"  block="<<logUtf8(block)<<'\n'<<rejectLogContents()<<'\n';
            }
            if(!lightLimitText.empty()) {
                std::ofstream limits(userLogRoot/L"light-limits.log",std::ios::binary|std::ios::app);
                limits<<logUtf8(logStamp(time,true))<<"  map="<<logUtf8(workdir.wstring())<<'\n'<<lightLimitText<<'\n';
            }
            written=true;
        } catch(const std::exception& error) { std::cerr<<"User log unavailable: "<<error.what()<<'\n'; }
    }
    ~UserWorkerLog() { append(); if(lease!=INVALID_HANDLE_VALUE) CloseHandle(lease); activeWorkerLog=nullptr; }
};
static void summarizeWorkerFault() noexcept { if(activeWorkerLog) activeWorkerLog->append(true); }
