// Process-isolated block prefetching: a background coordinator bakes the other lightmap blocks ahead of the workers.
// A held, share-denied claim file is the authority to bake a block; ready folders appear only after verification.
#pragma once
#include <psapi.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "dxgi.lib")

namespace prefetch {
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    Handle()=default;
    explicit Handle(HANDLE h):value(h) {}
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
    Handle(Handle&& other) noexcept:value(other.value) { other.value=INVALID_HANDLE_VALUE; }
    Handle& operator=(Handle&& other) noexcept { reset(); value=other.value; other.value=INVALID_HANDLE_VALUE; return *this; }
    ~Handle() { reset(); }
    explicit operator bool() const { return value && value!=INVALID_HANDLE_VALUE; }
    void reset() { if(*this) CloseHandle(value); value=INVALID_HANDLE_VALUE; }
};
struct ServerStatus { bool valid=false; DWORD pid=0; uint64_t privateBytes=0,vramCurrent=0,vramPeak=0,residentBytes=0; };
// GPU server status file: "VRAD3_GPU_SERVER_1 pid privateBytes vramCurrent vramPeak residentBytes tick"; valid only while the server process lives and the tick is under 10 s old.
static ServerStatus serverStatus() {
    ServerStatus status;
    try {
        std::ifstream file(compileTempRoot()/L"gpu-server.status"); std::string version; ULONGLONG tick=0;
        if(!(file>>version>>status.pid>>status.privateBytes>>status.vramCurrent>>status.vramPeak>>status.residentBytes>>tick) ||
           version!="VRAD3_GPU_SERVER_1" || GetTickCount64()-tick>10000) return {};
        HANDLE process=OpenProcess(SYNCHRONIZE,FALSE,status.pid);
        if(!process) return {};
        status.valid=WaitForSingleObject(process,0)==WAIT_TIMEOUT; CloseHandle(process);
    } catch(...) { return {}; }
    return status;
}
static std::wstring wide(const std::string& text) {
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
    if(!n && !text.empty()) throw std::runtime_error("Invalid UTF-8 commands");
    std::wstring result(n,L'\0');
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),result.data(),n);
    return result;
}
static fs::path modulePath() {
    std::wstring name(32768,L'\0'); auto n=GetModuleFileNameW(nullptr,name.data(),static_cast<DWORD>(name.size()));
    if(!n || n>=name.size()) throw std::runtime_error("Cannot locate wrapper");
    name.resize(n); return name;
}
static fs::path rootPath() {
    return compileTempRoot();
}
static void atomicText(const fs::path& path,const std::string& text) {
    fs::path temp=path; temp+=L".tmp-"+std::to_wstring(GetCurrentProcessId());
    { std::ofstream file(temp,std::ios::binary); file<<text; if(!file) throw std::runtime_error("Cannot write prefetch metadata"); }
    if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) fail("Publish prefetch metadata");
}
static void atomicCopy(const fs::path& from,const fs::path& to) {
    fs::create_directories(to.parent_path());
    fs::path temp=to; temp+=L".tmp-"+std::to_wstring(GetCurrentProcessId());
    fs::copy_file(from,temp,fs::copy_options::overwrite_existing);
    if(!MoveFileExW(temp.c_str(),to.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) fail("Publish prefetched file");
}
// Renaming a folder fails while anything inside is open; a scanner or just-exited worker can hold a file briefly, so retry.
static bool publishDirectory(const fs::path& from,const fs::path& to) {
    for(unsigned attempt=0;attempt<60;++attempt) {
        if(MoveFileExW(from.c_str(),to.c_str(),MOVEFILE_WRITE_THROUGH)) return true;
        const DWORD error=GetLastError();
        if(error!=ERROR_ACCESS_DENIED && error!=ERROR_SHARING_VIOLATION) break;
        Sleep(50);
    }
    return false;
}
static bool plainChildren(const fs::path& folder) {
    WIN32_FIND_DATAW item{};
    HANDLE find=FindFirstFileExW((folder/L"*").c_str(),FindExInfoBasic,&item,FindExSearchNameMatch,nullptr,0);
    if(find==INVALID_HANDLE_VALUE) { const DWORD error=GetLastError(); return error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND; }
    bool plain=true;
    do {
        const std::wstring name=item.cFileName; if(name==L"." || name==L"..") continue;
        if((item.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) ||
           ((item.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) && !plainChildren(folder/name))) { plain=false; break; }
    } while(FindNextFileW(find,&item));
    FindClose(find); return plain;
}
static bool plainTree(const fs::path& path) {
    const DWORD attributes=GetFileAttributesW(path.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES || (attributes&FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    return plainChildren(path);
}
static void removeTree(const fs::path& path) {
    if(path.parent_path()!=rootPath() || !fs::exists(path/L"prefetch.marker") || !plainTree(path)) return;
    fs::remove_all(path);
}
static void cleanupOld(const fs::path& current) {
    const auto root=rootPath(); fs::create_directories(root);
    for(const auto& item:fs::directory_iterator(root)) {
        if(!item.is_directory() || !fs::exists(item.path()/L"prefetch.marker") ||
           item.path()==current) continue;
        Handle lease(CreateFileW((item.path()/L"coordinator.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,0,nullptr));
        if(!lease) continue;
        Handle active(CreateFileW((item.path()/L"active.lock").c_str(),GENERIC_READ,0,nullptr,OPEN_ALWAYS,0,nullptr));
        if(!active) continue;
        active.reset(); lease.reset(); removeTree(item.path());
    }
}
static std::vector<std::wstring> replaceArgument(std::vector<std::wstring> args,const std::wstring& key,const std::wstring& value) {
    std::vector<std::wstring> result;
    for(size_t i=0;i<args.size();++i) {
        if(args[i]==key) { if(++i>=args.size()) throw std::runtime_error("Missing argument value"); continue; }
        if(args[i].rfind(key+L"=",0)==0) continue;
        result.push_back(args[i]);
    }
    result.push_back(key); result.push_back(value); return result;
}
static unsigned number(const std::wstring& text) {
    if(text.empty() || text.find_first_not_of(L"0123456789")!=std::wstring::npos) throw std::runtime_error("Invalid block number");
    auto value=std::stoull(text); if(value>UINT32_MAX) throw std::runtime_error("Block number overflow"); return static_cast<unsigned>(value);
}
struct Block { unsigned id; std::wstring file; std::vector<std::wstring> args; std::wstring direct; uint64_t cells=0; };
struct Plan { std::vector<Block> blocks; unsigned width=0,height=0,side=0; fs::path folder; bool probes=false; };
static Plan discover(const std::vector<std::wstring>& current,bool requireMatch) {
    Plan plan; std::ifstream commands(L"lightmap.cmd"); if(!commands) throw std::runtime_error("Missing lightmap.cmd");
    std::string line; std::set<unsigned> ids; bool match=false;
    std::vector<std::wstring> closest; size_t closestScore=SIZE_MAX;
    while(std::getline(commands,line)) {
        if(!line.empty() && line.back()=='\r') line.pop_back();
        if(line.empty() || line=="@echo off" || line=="setlocal") continue;
        auto tokens=launchTokens(wide(line));
        if(tokens.empty() || (_wcsicmp(fs::path(tokens[0]).filename().c_str(),L"vrad3") &&
           _wcsicmp(fs::path(tokens[0]).filename().c_str(),L"vrad3.exe"))) throw std::runtime_error("Unrecognized lightmap.cmd line");
        tokens.erase(tokens.begin());
        if(tokens==current) match=true;
        if(requireMatch) {
            size_t score=std::max(tokens.size(),current.size());
            for(size_t i=0;i<std::min(tokens.size(),current.size());++i) if(tokens[i]==current[i]) --score;
            if(argumentValue(tokens,L"-blocknum")!=argumentValue(current,L"-blocknum")) score+=current.size()+tokens.size();
            if(score<closestScore) { closest=tokens; closestScore=score; }
        }
        if(argumentValue(tokens,L"-entrypoint")!=L"lm_compute" || argumentValue(tokens,L"-script")!=L"script.vrad3" ||
           argumentValue(tokens,L"-outdir").empty()) throw std::runtime_error("Unsupported block command");
        unsigned id=number(argumentValue(tokens,L"-blocknum")); auto file=argumentValue(tokens,L"-blockfile");
        if(file.empty() || fs::path(file).filename()!=fs::path(file) || !ids.insert(id).second) throw std::runtime_error("Invalid/duplicate block command");
        plan.blocks.push_back({id,file,tokens});
    }
    if(plan.blocks.empty()) throw std::runtime_error("Empty lightmap.cmd");
    if(requireMatch && !match) {
        std::ostringstream reason; reason<<"lightmap.cmd mismatch";
        for(size_t i=0;i<std::max(current.size(),closest.size());++i) {
            auto actual=i<current.size()?current[i]:L"<missing>";
            auto expected=i<closest.size()?closest[i]:L"<missing>";
            if(actual==expected) continue;
            reason<<"; argument["<<i+1<<"]";
            if(i && i-1<closest.size() && closest[i-1].rfind(L"-",0)==0) reason<<' '<<logUtf8(closest[i-1]);
            reason<<" worker="<<logUtf8(actual)<<" expected="<<logUtf8(expected);
        }
        throw std::runtime_error(reason.str());
    }
    std::ifstream script(L"script.vrad3"); if(!script) throw std::runtime_error("Missing script.vrad3");
    std::string section; std::map<unsigned,std::wstring> combine; bool load=false;
    unsigned cw=0,ch=0,cs=0;
    while(std::getline(script,line)) {
        std::istringstream stream(line); std::string op; if(!(stream>>op) || op.rfind("//",0)==0) continue;
        if(op.back()==':') { section=op; continue; }
        if(section=="lm_compute:" && op=="lightmap_size") {
            if(!(stream>>plan.width>>plan.height>>plan.side)) throw std::runtime_error("Invalid compute dimensions");
        }
        if(section=="lm_compute:" && op=="lightmap_load_block") {
            std::string file,index; int variant=-1;
            if(!(stream>>file>>index>>variant) || file!="lightmap_info.dat" || index!="$blocknum" || variant!=0) throw std::runtime_error("Unsupported load block");
            load=true;
        }
        if(section=="lm_combine:" && op=="lightmap_size") stream>>cw>>ch>>cs;
        if(section=="lm_combine:" && op=="lightmap_read_block") {
            std::string file,index,extra;
            if(!(stream>>file>>index) || (stream>>extra)) throw std::runtime_error("Invalid combine block");
            if(!combine.emplace(number(wide(index)),wide(file)).second) throw std::runtime_error("Duplicate combine block");
        }
    }
    if(!load || !plan.side || plan.side>1024 || plan.width%plan.side || plan.height%plan.side ||
       cw!=plan.width || ch!=plan.height || cs!=plan.side || combine.size()!=plan.blocks.size() ||
       uint64_t(plan.width/plan.side)*(plan.height/plan.side)!=plan.blocks.size()) throw std::runtime_error("Block geometry/list mismatch");
    for(const auto& block:plan.blocks) if(combine[block.id]!=block.file || block.id>=combine.size()) throw std::runtime_error("Combine block set mismatch");
    return plan;
}
static uint64_t probeCells(const std::vector<std::wstring>& args) {
    uint64_t cells=1;
    for(const auto axis:{L"x",L"y",L"z"}) {
        const auto start=argumentValue(args,std::wstring(L"-start")+axis),end=argumentValue(args,std::wstring(L"-end")+axis);
        const unsigned from=number(start),to=number(end);
        if(to<=from || to-from>4096) throw std::runtime_error("Invalid probe volume range");
        cells*=to-from;
    }
    return cells;
}
static Plan discoverProbes(const std::vector<std::wstring>& current,bool requireMatch) {
    Plan plan; plan.probes=true; std::ifstream commands(L"lightprobevolumes.cmd"); if(!commands) throw std::runtime_error("Missing lightprobevolumes.cmd");
    std::string line; std::set<std::wstring> files; bool match=false;
    while(std::getline(commands,line)) {
        if(!line.empty() && line.back()=='\r') line.pop_back();
        if(line.empty() || line=="@echo off" || line=="setlocal") continue;
        auto tokens=launchTokens(wide(line));
        if(tokens.empty() || (_wcsicmp(fs::path(tokens[0]).filename().c_str(),L"vrad3") &&
           _wcsicmp(fs::path(tokens[0]).filename().c_str(),L"vrad3.exe"))) throw std::runtime_error("Unrecognized lightprobevolumes.cmd line");
        tokens.erase(tokens.begin());
        if(tokens==current) match=true;
        if(argumentValue(tokens,L"-entrypoint")!=L"lpv_compute" || argumentValue(tokens,L"-script")!=L"lpv.vrad3" ||
           argumentValue(tokens,L"-outdir").empty()) throw std::runtime_error("Unsupported probe command");
        auto direct=argumentValue(tokens,L"-directfile"),indirect=argumentValue(tokens,L"-indirectfile");
        for(const auto& file:{direct,indirect})
            if(file.empty() || fs::path(file).filename()!=fs::path(file) || !files.insert(file).second) throw std::runtime_error("Invalid/duplicate probe command");
        Block block{static_cast<unsigned>(plan.blocks.size()),indirect,tokens,direct,probeCells(tokens)};
        plan.blocks.push_back(std::move(block));
    }
    if(plan.blocks.empty()) throw std::runtime_error("Empty lightprobevolumes.cmd");
    if(requireMatch && !match) {
        std::ostringstream reason; reason<<"lightprobevolumes.cmd mismatch";
        const auto target=argumentValue(current,L"-indirectfile");
        auto same=std::find_if(plan.blocks.begin(),plan.blocks.end(),[&](const Block& b) { return b.file==target; });
        if(same==plan.blocks.end()) reason<<"; no line for "<<logUtf8(target);
        else for(size_t i=0;i<std::max(current.size(),same->args.size());++i) {
            auto actual=i<current.size()?current[i]:L"<missing>",expected=i<same->args.size()?same->args[i]:L"<missing>";
            if(actual!=expected) { reason<<"; argument["<<i+1<<"] worker="<<logUtf8(actual)<<" expected="<<logUtf8(expected); break; }
        }
        throw std::runtime_error(reason.str());
    }
    return plan;
}
static std::string cachedFileSha256(const fs::path& file) {
    try {
        const auto root=rootPath();
        if(!fs::exists(root/L"vrad3-temp.marker")) return fileSha256(file);
        std::ostringstream id;
        id<<logUtf8(fs::absolute(file).wstring())<<'\t'<<fs::file_size(file)<<'\t'<<fs::last_write_time(file).time_since_epoch().count()<<'\t';
        const std::string prefix=id.str();
        { std::ifstream cache(root/L"hash-cache.txt"); std::string line;
          while(std::getline(cache,line)) if(line.size()==prefix.size()+64 && line.compare(0,prefix.size(),prefix)==0) return line.substr(prefix.size()); }
        const std::string digest=fileSha256(file);
        std::ofstream(root/L"hash-cache.txt",std::ios::app)<<prefix<<digest<<'\n';
        return digest;
    } catch(...) { return fileSha256(file); }
}
// Compile key: hash of the compile's input files (size, time, content) and every VRAD3_ setting, so any change starts a fresh prefetch folder.
static std::string key(const fs::path& real,bool probes=false) {
    std::ostringstream identity; identity<<(probes?"probe-v1\n":"prefetch-v1\n")
        <<logUtf8(fs::canonical(fs::current_path()).wstring())<<'\n'<<usableCpuCount()<<'\n';
    std::vector<fs::path> files=probes?std::vector<fs::path>{fs::path(L"lpv.vrad3"),fs::path(L"scene_info.dat"),fs::absolute(real),modulePath()}
        :std::vector<fs::path>{fs::path(L"script.vrad3"),fs::path(L"scene_info.dat"),fs::path(L"lightmap_packing_geometry.dat"),
                         fs::path(L"lightmap_info.dat"),fs::path(L"lightmap.cmd"),fs::absolute(real),modulePath()};
    if(probes && fs::exists(L"lightprobevolumes.cmd")) files.push_back(fs::path(L"lightprobevolumes.cmd"));
    if(fs::exists(rootPath()) && !fs::is_empty(rootPath()) && !fs::exists(rootPath()/L"vrad3-temp.marker"))
        throw std::runtime_error("Compile temp folder is not owned by the wrapper");
    fs::create_directories(rootPath());
    if(!plainTree(rootPath())) throw std::runtime_error("Compile temp folder contains a reparse point");
    atomicText(rootPath()/L"vrad3-temp.marker","VRAD3 compile temp v1\n");
    for(const auto& file:files) {
        identity<<logUtf8(file.wstring())<<'\n'<<fs::file_size(file)<<'\n'<<fs::last_write_time(file).time_since_epoch().count()<<'\n'<<cachedFileSha256(file)<<'\n';
    }
    std::map<std::wstring,std::wstring> settings;
    auto environment=GetEnvironmentStringsW(); if(!environment) throw std::runtime_error("Cannot read environment");
    for(auto p=environment;*p;p+=wcslen(p)+1) {
        std::wstring value=p;
        if(value.rfind(L"VRAD3_",0)==0) { auto split=value.find(L'='); if(split!=std::wstring::npos) settings[value.substr(0,split)]=value.substr(split+1); }
    }
    FreeEnvironmentStringsW(environment);
    for(const auto& setting:settings) identity<<logUtf8(setting.first)<<'='<<logUtf8(setting.second)<<'\n';
    auto temp=rootPath()/(L"key-"+std::to_wstring(GetCurrentProcessId())+L".tmp");
    { std::ofstream file(temp,std::ios::binary); file<<identity.str(); if(!file) throw std::runtime_error("Cannot write compile identity"); }
    auto result=fileSha256(temp); fs::remove(temp); return result;
}
static void prepareCompile(const fs::path& real,bool probes=false) noexcept {
    try {
        auto identity=key(real,probes); const auto root=rootPath();
        cleanupOld(root/wide(identity));
        std::ifstream previous(root/L"compile-key.txt"); std::string old; previous>>old; previous.close();
        if(!old.empty() && old!=identity) {
            for(const auto& item:fs::directory_iterator(root))
                if(item.is_directory() && fs::exists(item.path()/L"prefetch.marker"))
                    throw std::runtime_error("Earlier compile still active");
            if(!plainTree(root)) throw std::runtime_error("Compile temp folder contains a reparse point");
            fs::remove_all(root); fs::create_directories(root);
            atomicText(root/L"vrad3-temp.marker","VRAD3 compile temp v1\n");
        }
        atomicText(root/L"compile-key.txt",identity);
    }
    catch(const std::exception& error) { std::cerr<<"Compile temp cleanup unavailable: "<<error.what()<<'\n'; }
}
// Opened share-read only: a second opener gets a sharing violation, and Windows releases the claim if the owner dies.
static HANDLE claim(const fs::path& folder,unsigned block) {
    return CreateFileW((folder/(L"claim-"+std::to_wstring(block))).c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,
        nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
}
static void claimOwner(HANDLE handle) {
    std::string text=std::to_string(GetCurrentProcessId()); DWORD written=0;
    SetFilePointer(handle,0,nullptr,FILE_BEGIN); SetEndOfFile(handle);
    if(!WriteFile(handle,text.data(),static_cast<DWORD>(text.size()),&written,nullptr) || written!=text.size()) fail("Write block claim");
    FlushFileBuffers(handle);
}
static bool verified(const fs::path& file,unsigned side,HammerTiming* timing=nullptr) {
    if(!fs::is_regular_file(file)) return false;
    std::ifstream status(blockStatusPath(file)); std::string version,hash; HammerTiming t;
    if(!(status>>version>>hash>>t.gpuBlocks>>t.cpuBlocks>>t.dispatchMs>>t.hostMs>>t.stageMs>>t.processMs
        >>t.directGpuBlocks>>t.directCpuBlocks>>t.directMs) || version!="VRAD3_GPU_STATUS_1" ||
        t.gpuBlocks+t.cpuBlocks!=1) return false;
    std::vector<uint8_t> bytes; if(hash!=blockSha256(file,&bytes)) return false;
    // A block file is one raw LZ4 block of side*side 140-byte luxel records; this walks the tokens to validate it without decompressing.
    uint64_t decoded=0,expected=uint64_t(side)*side*140; size_t p=0;
    auto length=[&](unsigned initial,uint64_t& n) {
        n=initial; if(initial!=15) return true;
        unsigned b=255; while(b==255) { if(p==bytes.size()) return false; b=bytes[p++]; n+=b; if(n>expected) return false; } return true;
    };
    while(p<bytes.size()) {
        unsigned token=bytes[p++]; uint64_t literals=0,match=0;
        if(!length(token>>4,literals) || literals>bytes.size()-p || literals>expected-decoded) return false;
        p+=static_cast<size_t>(literals); decoded+=literals;
        if(p==bytes.size()) break;
        if(bytes.size()-p<2) return false;
        unsigned offset=bytes[p]|(unsigned(bytes[p+1])<<8); p+=2;
        if(!offset || offset>decoded || !length(token&15,match) || match+4>expected-decoded) return false;
        decoded+=match+4;
    }
    if(decoded!=expected) return false;
    if(timing) *timing=t; return true;
}
static fs::path blockDir(const Plan& plan,const Block& block) { return plan.folder/(L"block-"+std::to_wstring(block.id)); }
static bool itemVerified(const Plan& plan,const fs::path& folder,const Block& block,HammerTiming* timing=nullptr) {
    if(!plan.probes) return verified(folder/block.file,plan.side,timing);
    std::error_code error;
    return fs::is_regular_file(folder/block.file,error) && fs::file_size(folder/block.file,error)==block.cells*24 &&
           fs::is_regular_file(folder/block.direct,error) && fs::file_size(folder/block.direct,error)>0;
}
static std::string itemDigest(const Plan& plan,const fs::path& folder,const Block& block) {
    if(!plan.probes) return blockSha256(folder/block.file);
    return fileSha256(folder/block.file)+' '+fileSha256(folder/block.direct);
}
static bool ready(const Plan& plan,const Block& block,HammerTiming* timing=nullptr) {
    auto dir=blockDir(plan,block)/L"ready";
    if(!plan.probes) return fs::exists(dir/L"done.txt") && verified(dir/block.file,plan.side,timing);
    std::ifstream done(dir/L"done.txt"); std::string digest; std::getline(done,digest);
    return done && itemVerified(plan,dir,block) && digest==itemDigest(plan,dir,block);
}
static DWORD parentPid() {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0)); PROCESSENTRY32W item{}; item.dwSize=sizeof(item);
    if(snapshot && Process32FirstW(snapshot.value,&item)) do {
        if(item.th32ProcessID==GetCurrentProcessId()) return item.th32ParentProcessID;
    } while(Process32NextW(snapshot.value,&item));
    return 0;
}
static bool mayDetach(DWORD& flags) {
    BOOL inJob=FALSE; if(!IsProcessInJob(GetCurrentProcess(),nullptr,&inJob)) return false;
    flags=DETACHED_PROCESS|CREATE_NO_WINDOW|childPriorityFlag();
    if(!inJob) return true;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    if(!QueryInformationJobObject(nullptr,JobObjectExtendedLimitInformation,&limits,sizeof(limits),nullptr)) return false;
    if(limits.BasicLimitInformation.LimitFlags&JOB_OBJECT_LIMIT_BREAKAWAY_OK) { flags|=CREATE_BREAKAWAY_FROM_JOB; return true; }
    return (limits.BasicLimitInformation.LimitFlags&JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK)!=0;
}
static bool cpuFallback(const fs::path& block,const HammerTiming& timing) {
    if(!timing.cpuBlocks) return false;
    std::ifstream status(blockStatusPath(block)); std::string line;
    while(std::getline(status,line)) if(line.rfind("VRAD3 GPU verify:",0)==0 && line.find("(CPU result written)")!=std::string::npos) return false;
    return true;
}
struct Foreground {
    Plan plan; Block block{}; Handle owner,lease; bool enabled=false,served=false;
    void start(const std::vector<std::wstring>& original,const fs::path& real,bool gpu,UserWorkerLog& worker,bool probes=false) noexcept {
        try {
            DWORD flags=0;
            worker.prefetchDecision="off:GPU off";
            if(!gpu) return;
            worker.prefetchDecision="off:VRAD3_GPU_PREFETCH=0";
            if(environmentValue(L"VRAD3_GPU_PREFETCH")==L"0") return;
            worker.prefetchDecision="off:no breakaway (job query failed or job forbids detachment)";
            if(!mayDetach(flags)) return;
            if(probes) { plan=discoverProbes(original,true); block=*std::find_if(plan.blocks.begin(),plan.blocks.end(),[&](const Block& b) { return b.args==original; }); }
            else { plan=discover(original,true); block=*std::find_if(plan.blocks.begin(),plan.blocks.end(),[&](const Block& b) { return b.id==number(argumentValue(original,L"-blocknum")); }); }
            plan.folder=rootPath()/wide(key(real,probes)); cleanupOld(plan.folder); fs::create_directories(plan.folder);
            atomicText(plan.folder/L"prefetch.marker","VRAD3 prefetch v1\n");
            lease=Handle(CreateFileW((plan.folder/L"active.lock").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_ALWAYS,0,nullptr));
            if(!lease) throw std::runtime_error("Cannot lease prefetch compile");
            enabled=true;
            while(!ready(plan,block)) {
                owner=Handle(claim(plan.folder,block.id));
                if(owner) { claimOwner(owner.value); break; }
                if(GetLastError()!=ERROR_SHARING_VIOLATION) throw std::runtime_error("Cannot claim block");
                Sleep(50);
            }
            if(ready(plan,block)) {
                auto dir=blockDir(plan,block)/L"ready"; auto destination=fs::path(argumentValue(original,L"-outdir"))/block.file;
                atomicCopy(dir/block.file,destination);
                if(probes) atomicCopy(dir/block.direct,destination.parent_path()/block.direct);
                else {
                    atomicCopy(blockStatusPath(dir/block.file),blockStatusPath(destination));
                    std::ofstream status(blockStatusPath(destination),std::ios::app); status<<"prefetched=1\n";
                }
                if(fs::exists(dir/L"worker.log")) { std::ifstream log(dir/L"worker.log",std::ios::binary); std::cout<<log.rdbuf()<<std::flush; }
                served=true; worker.prefetchDecision="served"; return;
            }
            if(mayDetach(flags)) {
                if(probes) fs::remove(plan.folder/L"stop.txt");
                auto module=modulePath(); std::wstring command=quoteArgument(module.wstring())+(probes?L" --probe-coordinator ":L" --prefetch-coordinator ")+
                    quoteArgument(plan.folder.wstring())+L" "+std::to_wstring(parentPid())+L" "+quoteArgument(fs::absolute(real).wstring());
                STARTUPINFOW startup{}; startup.cb=sizeof(startup); PROCESS_INFORMATION child{};
                if(CreateProcessW(module.c_str(),command.data(),nullptr,nullptr,FALSE,flags,nullptr,nullptr,&startup,&child)) {
                    CloseHandle(child.hThread); CloseHandle(child.hProcess);
                    worker.prefetchDecision="on";
                } else {
                    worker.prefetchDecision="off:coordinator launch error "+std::to_string(GetLastError());
                    std::cerr<<"Prefetch unavailable: "<<worker.prefetchDecision<<'\n';
                }
            } else worker.prefetchDecision="off:no breakaway (job query failed or job forbids detachment)";
        } catch(const std::exception& error) {
            enabled=false; owner.reset(); lease.reset(); worker.prefetchDecision="off:"+std::string(error.what());
            std::cerr<<"Prefetch unavailable: "<<error.what()<<'\n';
        }
    }
    void finish(const std::vector<std::wstring>& args,int result,const UserWorkerLog& worker,bool probeFallback=false) noexcept {
        if(!enabled || served || !owner) return;
        try {
            auto dir=blockDir(plan,block); const fs::path outdir=argumentValue(args,L"-outdir"); auto source=outdir/block.file;
            HammerTiming timing;
            if(result || !itemVerified(plan,outdir,block,&timing)) { atomicText(dir/L"failed.txt","Foreground result failed verification\n"); return; }
            fs::create_directories(dir/L"pending");
            atomicCopy(source,dir/L"pending"/block.file);
            if(plan.probes) atomicCopy(outdir/block.direct,dir/L"pending"/block.direct);
            else atomicCopy(blockStatusPath(source),blockStatusPath(dir/L"pending"/block.file));
            std::ostringstream metrics; metrics<<worker.hardware.budget<<' '<<hammerPeakResources<<' '<<(plan.probes?probeFallback:cpuFallback(source,timing))<<' '<<GetTickCount64()-worker.started<<' '
                <<worker.hardware.adapterLuidLow<<' '<<worker.hardware.adapterLuidHigh<<'\n';
            atomicText(dir/L"pending"/L"metrics.txt",metrics.str());
            atomicText(dir/L"pending"/L"done.txt",itemDigest(plan,dir/L"pending",block));
            if(!publishDirectory(dir/L"pending",dir/L"ready")) fail("Publish foreground block");
        } catch(const std::exception& error) { std::cerr<<"Prefetch result unavailable: "<<error.what()<<'\n'; }
    }
};
struct TreeMemory { uint64_t ram=0,commit=0; };
// Per process with all descendants: resident RAM and commit (private bytes). Commit counts against RAM + page file and fails allocations at the limit; running short of RAM only pages.
static std::map<DWORD,TreeMemory> processMemory(DWORD exclude=0) {
    std::map<DWORD,DWORD> parents; std::map<DWORD,TreeMemory> memory;
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0)); PROCESSENTRY32W item{}; item.dwSize=sizeof(item);
    if(snapshot && Process32FirstW(snapshot.value,&item)) do { parents[item.th32ProcessID]=item.th32ParentProcessID; } while(Process32NextW(snapshot.value,&item));
    for(const auto& process:parents) {
        Handle handle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_VM_READ,FALSE,process.first)); PROCESS_MEMORY_COUNTERS counters{};
        if(handle && GetProcessMemoryInfo(handle.value,&counters,sizeof(counters))) memory[process.first]={counters.WorkingSetSize,counters.PagefileUsage};
    }
    auto total=memory;
    for(const auto& process:memory) {
        if(exclude && process.first==exclude) continue;
        DWORD ancestor=process.first; std::set<DWORD> seen;
        while(parents.count(ancestor) && seen.insert(ancestor).second) {
            ancestor=parents[ancestor]; if(ancestor==process.first) break;
            total[ancestor].ram+=process.second.ram; total[ancestor].commit+=process.second.commit;
        }
    }
    return total;
}
struct Bake { Block block; Handle claim,process; ULONGLONG started; };
static int coordinator(const fs::path& folder,DWORD parent,const fs::path& real,bool probes=false) {
    try {
        Plan plan=probes?discoverProbes({},false):discover({},false); plan.folder=folder;
        if(folder!=rootPath()/wide(key(real,probes))) throw std::runtime_error("Coordinator inputs changed");
        Handle election(CreateFileW((folder/L"coordinator.lock").c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_ALWAYS,0,nullptr));
        if(!election) return 0; claimOwner(election.value);
        Handle active(CreateFileW((folder/L"active.lock").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_ALWAYS,0,nullptr));
        Handle launcher(OpenProcess(SYNCHRONIZE,FALSE,parent)); if(!launcher) throw std::runtime_error("Cannot watch launching process");
        Handle job(CreateJobObjectW(nullptr,nullptr)); JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if(!job || !SetInformationJobObject(job.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits))) fail("Create coordinator job");
        std::ofstream log; if(wrapperLogging==2) log.open(folder/L"coordinator.log",std::ios::app); log<<"start pid="<<GetCurrentProcessId()<<" parent="<<parent<<" threads="<<usableCpuCount()<<'\n'<<std::flush;
        std::vector<Bake> bakes; std::map<unsigned,uint64_t> peaks,commitPeaks; std::set<unsigned> measured,failed;
        unsigned adaptive=2,previousSlots=1,completed=0; uint64_t completedCells=0; bool serial=false;
        double previousRate=0; ULONGLONG epoch=GetTickCount64(),activity=epoch;
        const auto started=epoch;
        uint64_t ram=0,commit=0,vram=0,budget=0,serverRamPeak=0,serverVramPeak=0; unsigned cap=usableCpuCount();
        Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
        Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter;
        if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) throw std::runtime_error("Cannot query prefetch adapter");
        const auto vramLimit=environmentValue(L"VRAD3_GPU_VRAM_MB");
        const uint64_t configuredBudget=vramLimit.empty()?UINT64_MAX:uint64_t(number(vramLimit))*1048576;
        auto configured=environmentValue(L"VRAD3_GPU_PREFETCH_MAX"); if(!configured.empty()) cap=std::max(1u,std::min(cap,number(configured)));
        auto ramOverride=environmentValue(L"VRAD3_GPU_PREFETCH_RAM_MB");
        const uint64_t ramLimit=ramOverride.empty()?UINT64_MAX:uint64_t(number(ramOverride))*1048576;
        auto commitOverride=environmentValue(L"VRAD3_GPU_PREFETCH_COMMIT_MB");
        const uint64_t commitLimit=commitOverride.empty()?UINT64_MAX:uint64_t(number(commitOverride))*1048576;
        const auto reserveSetting=environmentValue(L"VRAD3_GPU_MEMORY_RESERVE_GB");
        const bool ramGate=reserveSetting!=L"off";
        const uint64_t reserveGb=reserveSetting.empty() || !ramGate?0:uint64_t(number(reserveSetting));
        while(true) {
            if(WaitForSingleObject(launcher.value,0)==WAIT_OBJECT_0) { log<<"stop launcher exited\n"; break; }
            if(probes && fs::exists(folder/L"stop.txt")) { log<<"stop lpv_finish_all\n"; break; }
            auto memory=processMemory(probes?serverStatus().pid:0); unsigned done=0,foreground=0;
            for(const auto& block:plan.blocks) {
                auto dir=blockDir(plan,block);
                if(fs::exists(dir/L"ready"/L"done.txt")) {
                    ++done;
                    if(measured.insert(block.id).second) {
                        uint64_t b=0,v=0; unsigned cpu=0; double ms=0;
                        LUID luid{};
                        std::ifstream metrics(dir/L"ready"/L"metrics.txt"); metrics>>b>>v>>cpu>>ms>>luid.LowPart>>luid.HighPart;
                        ram=std::max(ram,peaks[block.id]); commit=std::max(commit,commitPeaks[block.id]); vram=std::max(vram,v);
                        if(!cpu && metrics && !adapter && (!probes || luid.LowPart || luid.HighPart) && FAILED(factory->EnumAdapterByLuid(luid,IID_PPV_ARGS(&adapter))))
                            throw std::runtime_error("Cannot locate measured prefetch adapter");
                        if(cpu || !metrics) { serial=true; log<<"serial CPU fallback or missing metrics block="<<block.id<<'\n'; }
                        ++completed; completedCells+=block.cells; activity=GetTickCount64();
                    }
                    continue;
                }
                Handle check(claim(folder,block.id));
                if(!check) {
                    std::ifstream info(folder/(L"claim-"+std::to_wstring(block.id))); DWORD pid=0; info>>pid;
                    if(pid!=GetCurrentProcessId()) {
                        peaks[block.id]=std::max(peaks[block.id],memory[pid].ram); commitPeaks[block.id]=std::max(commitPeaks[block.id],memory[pid].commit);
                        ++foreground; activity=GetTickCount64();
                    }
                }
            }
            for(auto it=bakes.begin();it!=bakes.end();) {
                const auto usage=memory[GetProcessId(it->process.value)];
                peaks[it->block.id]=std::max(peaks[it->block.id],usage.ram); commitPeaks[it->block.id]=std::max(commitPeaks[it->block.id],usage.commit);
                if(WaitForSingleObject(it->process.value,0)!=WAIT_OBJECT_0) { ++it; continue; }
                DWORD code=1; GetExitCodeProcess(it->process.value,&code); auto dir=blockDir(plan,it->block); HammerTiming timing;
                if(!code && itemVerified(plan,dir/L"pending",it->block,&timing)) {
                    atomicText(dir/L"pending"/L"done.txt",itemDigest(plan,dir/L"pending",it->block));
                    if(!publishDirectory(dir/L"pending",dir/L"ready")) {
                        const DWORD publishError=GetLastError();
                        serial=true; failed.insert(it->block.id); atomicText(dir/L"failed.txt","Publish failed: "+std::to_string(publishError));
                        log<<"failed block="<<it->block.id<<" publish error "<<publishError<<'\n';
                    } else {
                        if(!probes && cpuFallback(dir/L"ready"/it->block.file,timing)) serial=true;
                        log<<"ready block="<<it->block.id<<" ms="<<GetTickCount64()-it->started<<" peak_ram="<<peaks[it->block.id]<<'\n';
                    }
                } else { serial=true; failed.insert(it->block.id); atomicText(dir/L"failed.txt","Background child failed: "+std::to_string(code)); log<<"failed block="<<it->block.id<<" exit="<<code<<'\n'; }
                it=bakes.erase(it); activity=GetTickCount64();
            }
            if(done==plan.blocks.size()) { log<<"stop all blocks ready\n"; break; }
            const auto now=GetTickCount64();
            if(now-activity>60000 && bakes.empty()) { log<<"stop idle timeout\n"; break; }
            if(completed>=std::max(2u,previousSlots) && (!probes || now-epoch>=3000)) {
                if(probes) {
                    double rate=completedCells*1000.0/std::max<ULONGLONG>(1,now-epoch);
                    if(previousRate && rate<previousRate*0.85) adaptive=std::max(1u,adaptive-1);
                    else if(rate>=previousRate) adaptive=std::min(cap,adaptive+1);
                    previousRate=rate; previousSlots=adaptive;
                    log<<"feedback cells_per_s="<<rate<<" adaptive="<<adaptive<<'\n';
                } else {
                    double rate=completed*1000.0/std::max<ULONGLONG>(1,now-epoch);
                    if(previousRate && rate<=previousRate) { adaptive=std::max(1u,adaptive-1); cap=std::min(cap,adaptive); }
                    else { previousRate=rate; previousSlots=adaptive; ++adaptive; }
                    log<<"feedback rate="<<rate<<" adaptive="<<adaptive<<'\n';
                }
                completed=0; completedCells=0; epoch=now;
            }
            MEMORYSTATUSEX state{}; state.dwLength=sizeof(state); if(!GlobalMemoryStatusEx(&state)) fail("Query coordinator RAM");
            const uint64_t available=std::min(state.ullAvailPhys,ramLimit);
            const uint64_t commitAvailable=std::min(state.ullAvailPageFile,commitLimit);
            DXGI_QUERY_VIDEO_MEMORY_INFO gpuMemory{};
            if(adapter) {
                if(FAILED(adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&gpuMemory)))
                    throw std::runtime_error("Cannot query prefetch VRAM");
                budget=std::min(configuredBudget,gpuMemory.Budget);
            }
            const uint64_t reserve=!ramGate?0:reserveSetting.empty()?std::max<uint64_t>(4ull<<30,state.ullTotalPhys*15/100):reserveGb<<30;
            uint64_t commitFree=0;
            unsigned slots=1;
            const auto server=serverStatus();
            if(server.valid) { serverRamPeak=std::max(serverRamPeak,server.privateBytes); serverVramPeak=std::max({serverVramPeak,server.vramPeak,server.vramCurrent}); }
            const uint64_t serverVram=server.valid?serverVramPeak+serverVramPeak/5:0;
            const uint64_t serverGrowth=server.valid && serverRamPeak>server.privateBytes?serverRamPeak-server.privateBytes:0;
            if(ram && !serial && (probes || ((vram || server.valid) && budget))) {
                uint64_t cost=ram+ram/5; unsigned live=foreground+static_cast<unsigned>(bakes.size());
                uint64_t running=0; for(const auto& block:plan.blocks) if(peaks.count(block.id) && !measured.count(block.id)) running+=std::min(cost,peaks[block.id]);
                uint64_t growth=cost*live>running?cost*live-running:0;
                growth+=serverGrowth;
                uint64_t free=available>reserve+growth?available-reserve-growth:0;
                unsigned extra=ramGate?static_cast<unsigned>(std::min<uint64_t>(cap,free/cost)):cap;
                if(ramGate && commit) {
                    const uint64_t commitCost=commit+commit/5;
                    uint64_t commitRunning=0;
                    for(const auto& block:plan.blocks) if(commitPeaks.count(block.id) && !measured.count(block.id)) commitRunning+=std::min(commitCost,commitPeaks[block.id]);
                    const uint64_t commitGrowth=(commitCost*live>commitRunning?commitCost*live-commitRunning:0)+serverGrowth+(1ull<<30);
                    commitFree=commitAvailable>commitGrowth?commitAvailable-commitGrowth:0;
                    extra=std::min<unsigned>(extra,static_cast<unsigned>(std::min<uint64_t>(cap,commitFree/commitCost)));
                }
                uint64_t gpuFree=budget>gpuMemory.CurrentUsage+serverVram?budget-gpuMemory.CurrentUsage-serverVram:0;
                unsigned gpuCap=vram?static_cast<unsigned>(std::min<uint64_t>(cap,(gpuFree-gpuFree/10)/(vram+vram/5))):cap;
                slots=std::max(1u,std::min({cap,adaptive,gpuCap,live+extra}));
            }
            log<<"sample ms="<<now-started<<" slots="<<slots<<" foreground="<<foreground<<" background="<<bakes.size()
               <<" available="<<available<<" actual_available="<<state.ullAvailPhys<<" reserve="<<reserve<<" measured_ram="<<ram<<" measured_vram="<<vram
               <<" commit_available="<<commitAvailable<<" commit_free="<<commitFree<<" measured_commit="<<commit
               <<" adapter_budget="<<budget<<" adapter_usage="<<gpuMemory.CurrentUsage
               <<" server="<<(server.valid?1:0)<<" server_ram="<<server.privateBytes<<" server_vram="<<server.vramCurrent<<" server_vram_reserved="<<serverVram<<'\n'<<std::flush;
            unsigned allowed=slots>1 && slots>foreground?slots-foreground:0;
            while(bakes.size()<allowed && !serial) {
                auto candidate=std::find_if(plan.blocks.begin(),plan.blocks.end(),[&](const Block& b) { return !failed.count(b.id) && !fs::exists(blockDir(plan,b)/L"ready"/L"done.txt"); });
                bool launched=false;
                for(;candidate!=plan.blocks.end();++candidate) {
                    if(failed.count(candidate->id) || fs::exists(blockDir(plan,*candidate)/L"ready"/L"done.txt")) continue;
                    Handle ownership(claim(folder,candidate->id)); if(!ownership) continue;
                    claimOwner(ownership.value); auto dir=blockDir(plan,*candidate); fs::create_directories(dir/L"pending");
                    const uint64_t outputBytes=plan.probes?candidate->cells*24*4:uint64_t(plan.side)*plan.side*140*2;
                    if(fs::space(folder).available<outputBytes) { serial=true; log<<"serial disk space\n"; break; }
                    auto args=replaceArgument(candidate->args,L"-outdir",(dir/L"pending").wstring());
                    std::wstring command=quoteArgument(modulePath().wstring())+L" --prefetch-bake";
                    for(const auto& arg:args) command+=L" "+quoteArgument(arg);
                    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
                    Handle output(CreateFileW((dir/L"pending"/L"worker.log").c_str(),GENERIC_WRITE,FILE_SHARE_READ,&security,CREATE_ALWAYS,0,nullptr));
                    Handle input(CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,nullptr));
                    STARTUPINFOW startup{}; startup.cb=sizeof(startup); startup.dwFlags=STARTF_USESTDHANDLES;
                    startup.hStdInput=input.value; startup.hStdOutput=startup.hStdError=output.value; PROCESS_INFORMATION child{};
                    auto old=environmentValue(L"VRAD3_GPU_VRAM_MB");
                    const auto serving=serverStatus();
                    const uint64_t serverHold=serving.valid?(serverVramPeak+serverVramPeak/5):0;
                    const uint64_t gpuFree=budget>gpuMemory.CurrentUsage+serverHold?budget-gpuMemory.CurrentUsage-serverHold:0;
                    auto partition=std::to_wstring(std::max<uint64_t>(1,(gpuFree-gpuFree/10)/slots/1048576));
                    SetEnvironmentVariableW(L"VRAD3_GPU_USER_VRAM_MB",old.empty()?L"none":old.c_str());
                    SetEnvironmentVariableW(L"VRAD3_GPU_LAUNCHER_PID",std::to_wstring(parent).c_str());
                    if(!probes || budget) SetEnvironmentVariableW(L"VRAD3_GPU_VRAM_MB",partition.c_str());
                    BOOL created=CreateProcessW(modulePath().c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_SUSPENDED|CREATE_NO_WINDOW|childPriorityFlag(),nullptr,nullptr,&startup,&child);
                    SetEnvironmentVariableW(L"VRAD3_GPU_VRAM_MB",old.empty()?nullptr:old.c_str());
                    SetEnvironmentVariableW(L"VRAD3_GPU_LAUNCHER_PID",nullptr);
                    SetEnvironmentVariableW(L"VRAD3_GPU_USER_VRAM_MB",nullptr);
                    if(!created) { serial=true; failed.insert(candidate->id); log<<"serial CreateProcess failure\n"; break; }
                    Handle process(child.hProcess),thread(child.hThread);
                    if(!AssignProcessToJobObject(job.value,process.value) || ResumeThread(thread.value)==DWORD(-1)) {
                        TerminateProcess(process.value,1); WaitForSingleObject(process.value,5000); serial=true; log<<"serial child job failure\n"; break;
                    }
                    log<<"launch block="<<candidate->id<<" pid="<<child.dwProcessId<<" slots="<<slots<<" vram_mb="<<logUtf8(partition)<<'\n';
                    bakes.push_back({*candidate,std::move(ownership),std::move(process),now}); launched=true; break;
                }
                if(!launched) break;
            }
            Sleep(100);
        }
        job.reset(); for(auto& bake:bakes) WaitForSingleObject(bake.process.value,5000);
        log<<"exit\n"<<std::flush; return 0;
    } catch(const std::exception& error) {
        if(wrapperLogging==2) std::ofstream(folder/L"coordinator.log",std::ios::app)<<"error "<<error.what()<<'\n';
        return 1;
    }
}
static void removeCompile(const fs::path& real) noexcept {
    try {
        const auto root=rootPath();
        if(!fs::exists(root/L"vrad3-temp.marker") || !plainTree(root)) return;
        auto folder=root/wide(key(real));
        Handle election,active;
        if(fs::exists(folder)) {
            for(unsigned retry=0;retry<100;++retry) {
                election=Handle(CreateFileW((folder/L"coordinator.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,0,nullptr));
                if(election) break;
                Sleep(50);
            }
            if(!election) throw std::runtime_error("Coordinator still active after combine");
            active=Handle(CreateFileW((folder/L"active.lock").c_str(),GENERIC_READ,0,nullptr,OPEN_ALWAYS,0,nullptr));
            if(!active) throw std::runtime_error("Worker still active after combine");
            if(activeWorkerLog && !activeWorkerLog->directory.empty() && fs::exists(folder/L"coordinator.log"))
                atomicCopy(folder/L"coordinator.log",activeWorkerLog->directory/L"prefetch-coordinator.log");
            election.reset(); active.reset(); removeTree(folder);
        }
        if(activeWorkerLog && !activeWorkerLog->directory.empty() && fs::exists(root/L"gpu-server.log"))
            atomicCopy(root/L"gpu-server.log",activeWorkerLog->directory/L"gpu-server.log");
        cleanupOld({});
        for(const auto& item:fs::directory_iterator(root))
            if(item.is_directory() && fs::exists(item.path()/L"prefetch.marker"))
                throw std::runtime_error("Another compile is still active");
        fs::remove_all(root);
    } catch(const std::exception& error) { std::cerr<<"Prefetch cleanup unavailable: "<<error.what()<<'\n'; }
}
static void removeProbeCompile(const fs::path& real) noexcept {
    try {
        const auto root=rootPath();
        if(!fs::exists(root/L"vrad3-temp.marker") || !plainTree(root)) return;
        std::ifstream current(root/L"compile-key.txt"); std::string old; current>>old; current.close();
        if(old.empty() || old!=key(real,true)) return;
        if(const auto folder=root/wide(old); fs::exists(folder)) {
            atomicText(folder/L"stop.txt","lpv_finish_all\n");
            Handle election,active;
            for(unsigned retry=0;retry<100 && !election;++retry) {
                election=Handle(CreateFileW((folder/L"coordinator.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,0,nullptr));
                if(!election) Sleep(50);
            }
            if(!election) throw std::runtime_error("Probe coordinator still active after lpv_finish_all");
            active=Handle(CreateFileW((folder/L"active.lock").c_str(),GENERIC_READ,0,nullptr,OPEN_ALWAYS,0,nullptr));
            if(!active) throw std::runtime_error("Probe worker still active after lpv_finish_all");
            if(activeWorkerLog && !activeWorkerLog->directory.empty() && fs::exists(folder/L"coordinator.log"))
                atomicCopy(folder/L"coordinator.log",activeWorkerLog->directory/L"probe-coordinator.log");
            election.reset(); active.reset(); removeTree(folder);
        }
        if(activeWorkerLog && !activeWorkerLog->directory.empty() && fs::exists(root/L"gpu-server.log"))
            atomicCopy(root/L"gpu-server.log",activeWorkerLog->directory/L"gpu-server.log");
        cleanupOld({});
        for(const auto& item:fs::directory_iterator(root))
            if(item.is_directory() && fs::exists(item.path()/L"prefetch.marker"))
                throw std::runtime_error("Another compile is still active");
        fs::remove_all(root);
    } catch(const std::exception& error) { std::cerr<<"Probe cleanup unavailable: "<<error.what()<<'\n'; }
}
}
