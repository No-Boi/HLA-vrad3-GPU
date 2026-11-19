// Opt-in launch diagnostics (detailed logging): records the worker's process ancestry, job limits and std handles.
#pragma once
#include <tlhelp32.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")

// Unix milliseconds: FILETIME counts 100 ns from 1601; 116444736000000000 is the 1970 offset.
static uint64_t launchMillis() {
    FILETIME time{}; GetSystemTimeAsFileTime(&time);
    ULARGE_INTEGER value{}; value.LowPart=time.dwLowDateTime; value.HighPart=time.dwHighDateTime;
    return (value.QuadPart-116444736000000000ull)/10000;
}
static std::vector<std::wstring> launchTokens(const std::wstring& command) {
    int count=0; auto tokens=CommandLineToArgvW(command.c_str(),&count);
    std::vector<std::wstring> result;
    if(tokens) { for(int i=0;i<count;++i) result.emplace_back(tokens[i]); LocalFree(tokens); }
    return result;
}
static std::string launchHandle(HANDLE handle) {
    if(!handle || handle==INVALID_HANDLE_VALUE) return "null";
    SetLastError(0); DWORD type=GetFileType(handle),error=GetLastError(),mode=0;
    if(type==FILE_TYPE_PIPE) return "pipe";
    if(type==FILE_TYPE_DISK) return "file";
    if(type==FILE_TYPE_CHAR) return GetConsoleMode(handle,&mode)?"console":"character";
    return "unknown(error="+std::to_string(error)+")";
}
static std::string launchRead(const fs::path& path) {
    std::ifstream file(path); std::string line; std::getline(file,line); return line;
}
static int runLaunchProbe(const fs::path& folder) {
    try {
        fs::create_directories(folder);
        HANDLE lease=CreateFileW((folder.parent_path().parent_path()/L"active.lock").c_str(),
            GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        std::ofstream(folder/L"pid.txt")<<GetCurrentProcessId()<<'\n';
        ULONGLONG start=GetTickCount64();
        for(unsigned sequence=0;GetTickCount64()-start<60000;++sequence) {
            fs::path temporary=folder/L"heartbeat.tmp";
            { std::ofstream file(temporary); file<<GetCurrentProcessId()<<' '<<sequence<<' '<<launchMillis()<<'\n'; }
            if(!MoveFileExW(temporary.c_str(),(folder/L"heartbeat.txt").c_str(),MOVEFILE_REPLACE_EXISTING)) {
                if(lease!=INVALID_HANDLE_VALUE) CloseHandle(lease);
                return 1;
            }
            Sleep(1000);
        }
        std::ofstream(folder/L"end.txt")<<launchMillis()<<'\n';
        if(lease!=INVALID_HANDLE_VALUE) CloseHandle(lease);
        return 0;
    } catch(...) { return 1; }
}
struct LaunchDiagnostics {
    fs::path file;
    std::vector<std::pair<fs::path,std::string>> previous;
    void start(const fs::path& compile) noexcept {
        try {
            const auto now=launchMillis();
            fs::create_directories(compile/L"launch");
            file=compile/L"launch"/(std::to_wstring(GetCurrentProcessId())+L".txt");
            std::ofstream log(file);
            log<<"pid="<<GetCurrentProcessId()<<"\nstart_unix_ms="<<now
               <<"\ncommand="<<logField(logUtf8(GetCommandLineW()))
               <<"\ncwd="<<logField(logUtf8(fs::current_path().wstring()))<<'\n';
            std::map<DWORD,std::pair<DWORD,std::wstring>> processes;
            HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
            if(snapshot!=INVALID_HANDLE_VALUE) {
                PROCESSENTRY32W item{}; item.dwSize=sizeof(item);
                if(Process32FirstW(snapshot,&item)) do {
                    processes[item.th32ProcessID]={item.th32ParentProcessID,item.szExeFile};
                } while(Process32NextW(snapshot,&item));
                CloseHandle(snapshot);
            } else log<<"ancestry_error="<<GetLastError()<<'\n';
            DWORD ancestor=GetCurrentProcessId();
            for(const char* label:{"parent","grandparent"}) {
                auto own=processes.find(ancestor);
                ancestor=own==processes.end()?0:own->second.first;
                auto found=processes.find(ancestor);
                log<<label<<"_pid="<<ancestor<<"\n"<<label<<"_exe="
                   <<(found==processes.end()?"unavailable":logUtf8(found->second.second))<<'\n';
            }
            BOOL inJob=FALSE;
            bool jobKnown=IsProcessInJob(GetCurrentProcess(),nullptr,&inJob)!=FALSE;
            log<<"in_job="<<(jobKnown?(inJob?"1":"0"):"unknown")<<'\n';
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; bool limitsKnown=false;
            if(inJob) {
                limitsKnown=QueryInformationJobObject(nullptr,JobObjectExtendedLimitInformation,&limits,sizeof(limits),nullptr)!=FALSE;
                if(!limitsKnown) log<<"job_query_error="<<GetLastError()<<'\n';
                else {
                    DWORD flags=limits.BasicLimitInformation.LimitFlags;
                    log<<"job_limit_flags="<<flags
                       <<"\nkill_on_job_close="<<!!(flags&JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE)
                       <<"\nbreakaway_ok="<<!!(flags&JOB_OBJECT_LIMIT_BREAKAWAY_OK)
                       <<"\nsilent_breakaway_ok="<<!!(flags&JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK)
                       <<"\nactive_process_limit_enabled="<<!!(flags&JOB_OBJECT_LIMIT_ACTIVE_PROCESS)
                       <<"\nactive_process_limit="<<limits.BasicLimitInformation.ActiveProcessLimit<<'\n';
                }
            }
            log<<"stdin="<<launchHandle(GetStdHandle(STD_INPUT_HANDLE))
               <<"\nstdout="<<launchHandle(GetStdHandle(STD_OUTPUT_HANDLE))
               <<"\nstderr="<<launchHandle(GetStdHandle(STD_ERROR_HANDLE))<<'\n';
            auto current=launchTokens(GetCommandLineW());
            std::ifstream commands(fs::current_path()/L"lightmap.cmd");
            std::string line; unsigned number=0; bool matched=false;
            log<<"lightmap_cmd_present="<<bool(commands)<<'\n';
            while(std::getline(commands,line)) {
                ++number;
                int size=MultiByteToWideChar(CP_UTF8,0,line.data(),static_cast<int>(line.size()),nullptr,0);
                std::wstring wide(size,L'\0');
                MultiByteToWideChar(CP_UTF8,0,line.data(),static_cast<int>(line.size()),wide.data(),size);
                auto candidate=launchTokens(wide);
                if(candidate.size()!=current.size() || candidate.empty() ||
                   (_wcsicmp(fs::path(candidate[0]).filename().c_str(),L"vrad3.exe")!=0 &&
                    _wcsicmp(fs::path(candidate[0]).filename().c_str(),L"vrad3")!=0)) continue;
                if(!std::equal(candidate.begin()+1,candidate.end(),current.begin()+1)) continue;
                log<<"lightmap_cmd_line="<<number<<"\nlightmap_cmd_block="
                   <<logUtf8(argumentValue(std::vector<std::wstring>(current.begin()+1,current.end()),L"-blocknum"))<<'\n';
                matched=true; break;
            }
            log<<"lightmap_cmd_match="<<matched<<'\n';
            uint64_t latestEnd=0;
            for(const auto& item:fs::directory_iterator(compile/L"launch")) {
                if(item.path()==file || item.path().extension()!=L".txt") continue;
                std::ifstream input(item.path());
                while(std::getline(input,line)) if(line.rfind("end_unix_ms=",0)==0)
                    latestEnd=std::max(latestEnd,std::stoull(line.substr(12)));
            }
            if(latestEnd && latestEnd<=now) log<<"previous_end_unix_ms="<<latestEnd<<"\nprevious_end_to_start_ms="<<now-latestEnd<<'\n';
            if(fs::is_directory(compile/L"probes")) for(const auto& item:fs::directory_iterator(compile/L"probes")) {
                if(!item.is_directory()) continue;
                fs::path heartbeat=item.path()/L"heartbeat.txt";
                auto value=launchRead(heartbeat);
                log<<"previous_probe="<<logUtf8(item.path().filename().wstring())<<" heartbeat_start="<<value<<'\n';
                previous.emplace_back(heartbeat,value);
            }
            std::wstring module(32768,L'\0');
            DWORD length=GetModuleFileNameW(nullptr,module.data(),static_cast<DWORD>(module.size()));
            if(!length || length>=module.size()) return;
            module.resize(length);
            bool enabled=wrapperLogging==2;
            log<<"probe_enabled="<<enabled<<'\n';
            if(enabled) {
                fs::path folder=compile/L"probes"/std::to_wstring(GetCurrentProcessId());
                fs::create_directories(folder);
                std::wstring command=quoteArgument(module)+L" --launch-probe "+quoteArgument(folder.wstring());
                // Detached probe process that writes a heartbeat file, to show whether the process tree stays alive during a worker.
                DWORD flags=DETACHED_PROCESS|CREATE_NO_WINDOW|childPriorityFlag();
                if(limitsKnown && (limits.BasicLimitInformation.LimitFlags&JOB_OBJECT_LIMIT_BREAKAWAY_OK)) flags|=CREATE_BREAKAWAY_FROM_JOB;
                STARTUPINFOW startup{}; startup.cb=sizeof(startup); PROCESS_INFORMATION child{};
                log<<"probe_creation_flags="<<flags<<"\nprobe_inherit_handles=0\n";
                if(CreateProcessW(module.c_str(),command.data(),nullptr,nullptr,FALSE,flags,nullptr,nullptr,&startup,&child)) {
                    log<<"probe_pid="<<child.dwProcessId<<'\n'; CloseHandle(child.hThread); CloseHandle(child.hProcess);
                } else log<<"probe_launch_error="<<GetLastError()<<'\n';
            }
        } catch(const std::exception& error) {
            if(!file.empty()) { std::ofstream log(file,std::ios::app); log<<"diagnostics_error="<<error.what()<<'\n'; }
        }
    }
    ~LaunchDiagnostics() {
        if(file.empty()) return;
        try {
            std::ofstream log(file,std::ios::app);
            for(const auto& item:previous) {
                auto value=launchRead(item.first);
                log<<"previous_probe="<<logUtf8(item.first.parent_path().filename().wstring())
                   <<" heartbeat_end="<<value<<" advancing_during_worker="<<(!value.empty() && value!=item.second)<<'\n';
            }
            log<<"end_unix_ms="<<launchMillis()<<'\n';
        } catch(...) {}
    }
};
