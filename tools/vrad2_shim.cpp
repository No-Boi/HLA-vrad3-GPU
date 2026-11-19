// vrad2.exe shim: bakes Hammer's lighting preview with vrad3's path tracer, falling back to Valve's vrad2.
#define NOMINMAX
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
namespace fs=std::filesystem;
#pragma warning(push)
#pragma warning(disable:4505)
#include "wrapper_settings.h"
#include "preview_scene.h"
#pragma warning(pop)

static std::string logText;
static std::string narrow(const std::wstring& text) {
    const int n=WideCharToMultiByte(CP_UTF8,0,text.c_str(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    std::string out(static_cast<size_t>(n),'\0');
    WideCharToMultiByte(CP_UTF8,0,text.c_str(),static_cast<int>(text.size()),out.data(),n,nullptr,nullptr);
    return out;
}
static std::string timestamp() {
    SYSTEMTIME t; GetLocalTime(&t); char text[32];
    sprintf_s(text,"%04u-%02u-%02u %02u:%02u:%02u",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond);
    return text;
}
static void say(const std::string& line) {
    std::cout<<"vrad2 shim: "<<line<<'\n'<<std::flush;
    logText+=timestamp()+" "+line+"\n";
}
static std::wstring environmentValue(const wchar_t* name) {
    std::wstring value(32768,L'\0');
    const DWORD n=GetEnvironmentVariableW(name,value.data(),static_cast<DWORD>(value.size()));
    value.resize(n<value.size()?n:0);
    return value;
}
static bool readFile(const fs::path& path,std::string& bytes) {
    std::ifstream input(path,std::ios::binary);
    if(!input) return false;
    bytes.assign(std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>());
    return !input.bad();
}
static bool writeFile(const fs::path& path,const void* data,size_t size) {
    std::ofstream output(path,std::ios::binary|std::ios::trunc);
    output.write(static_cast<const char*>(data),static_cast<std::streamsize>(size));
    output.close();
    return !output.fail();
}

struct Command { int id=0; size_t begin=0,end=0; std::vector<std::string> names; int64_t value=0; };
// commands.dat: int32 id + payload per command, as Valve's vrad2 dispatcher reads them (vrad2.exe 0x328e0).
static bool parseCommands(const std::string& data,std::vector<Command>& commands,std::string& error) {
    size_t at=0;
    auto need=[&](size_t n) { return data.size()-at>=n; };
    auto int32=[&]() { int32_t v; memcpy(&v,data.data()+at,4); at+=4; return v; };
    auto name=[&](Command& c) {
        const auto nul=data.find('\0',at);
        if(nul==std::string::npos) return false;
        c.names.push_back(data.substr(at,nul-at)); at=nul+1;
        return true;
    };
    while(at<data.size()) {
        Command c; c.begin=at;
        if(!need(4)) { error="commands.dat ends inside a command id"; return false; }
        c.id=int32(); bool ok=true;
        switch(c.id) {
        case 0x0: ok=need(0x20); at+=ok?0x20:0; break;
        case 0x1: case 0x2: case 0x3: case 0x4: case 0x5: case 0x6:
        case 0x7: case 0x8: case 0x9: case 0xb: ok=name(c); break;
        case 0xa: ok=name(c) && name(c); break;
        case 0xc: ok=need(8); if(ok) { memcpy(&c.value,data.data()+at,8); at+=8; } break;
        case 0xd: ok=need(8); if(ok) { c.value=int32(); at+=4; } break;
        case 0xe: case 0x10: ok=need(4); if(ok) c.value=int32(); break;
        case 0xf: case 0x11: ok=need(0x5c); at+=ok?0x5c:0; break;
        case 0x12: case 0x13: break;
        default: error="unknown command id "+std::to_string(c.id)+" at byte "+std::to_string(c.begin); return false;
        }
        if(!ok) { error="commands.dat ends inside command "+std::to_string(c.id); return false; }
        c.end=at; commands.push_back(std::move(c));
    }
    return true;
}

// float -> half, round to nearest even.
static uint16_t toHalf(float value) {
    uint32_t f; memcpy(&f,&value,4);
    const uint32_t sign=f&0x80000000u; f^=sign;
    uint16_t out;
    if(f>=(127u+16)<<23) out=f>255u<<23?0x7e00:0x7c00;
    else if(f<113u<<23) {
        const uint32_t magicBits=((127u-15)+(23-10)+1)<<23; float magic; memcpy(&magic,&magicBits,4);
        float shifted; memcpy(&shifted,&f,4); shifted+=magic;
        uint32_t bits; memcpy(&bits,&shifted,4); out=static_cast<uint16_t>(bits-magicBits);
    } else {
        const uint32_t odd=(f>>13)&1;
        f+=(static_cast<uint32_t>(15-127)<<23)+0xfff+odd;
        out=static_cast<uint16_t>(f>>13);
    }
    return static_cast<uint16_t>(out|sign>>16);
}

static HANDLE jobObject=nullptr;
static bool runProcess(const fs::path& exe,const std::wstring& arguments,const std::vector<wchar_t>* environment,int& exitCode) {
    std::wstring command=L"\""+exe.wstring()+L"\""+arguments;
    std::vector<wchar_t> line(command.begin(),command.end()); line.push_back(0);
    STARTUPINFOW startup{}; startup.cb=sizeof(startup); PROCESS_INFORMATION process{};
    const DWORD flags=CREATE_SUSPENDED|(environment?CREATE_UNICODE_ENVIRONMENT:0);
    if(!CreateProcessW(exe.c_str(),line.data(),nullptr,nullptr,TRUE,flags,
                       environment?const_cast<wchar_t*>(environment->data()):nullptr,nullptr,&startup,&process)) {
        say("could not start "+narrow(exe.wstring())+" (error "+std::to_string(GetLastError())+")");
        return false;
    }
    if(jobObject) AssignProcessToJobObject(jobObject,process.hProcess);
    ResumeThread(process.hThread);
    WaitForSingleObject(process.hProcess,INFINITE);
    DWORD code=1; GetExitCodeProcess(process.hProcess,&code);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    exitCode=static_cast<int>(code);
    return true;
}
static std::wstring argumentTail() {
    const wchar_t* p=GetCommandLineW(); bool quoted=false;
    while(*p && (quoted || (*p!=L' ' && *p!=L'\t'))) { if(*p==L'"') quoted=!quoted; ++p; }
    return p;
}
static std::vector<wchar_t> environmentBlock() {
    std::vector<wchar_t> block;
    if(LPWCH all=GetEnvironmentStringsW()) {
        const wchar_t* p=all; while(*p) p+=wcslen(p)+1;
        block.assign(static_cast<const wchar_t*>(all),p); FreeEnvironmentStringsW(all);
    }
    block.push_back(0); if(block.size()<2) block.push_back(0);
    return block;
}

struct Job {
    fs::path self,real,vrad3;
    std::wstring arguments;
    std::vector<wchar_t> environment;
    std::string original;
    bool commandsChanged=false;
    std::vector<fs::path> temporary;
    void cleanUp() {
        if(commandsChanged && writeFile(L"commands.dat",original.data(),original.size())) commandsChanged=false;
        for(const auto& path:temporary) { std::error_code error; fs::remove(path,error); }
        temporary.clear();
    }
    int vrad2(const std::string& why) {
        cleanUp();
        say(why+": running Valve's vrad2 (vrad2_real.exe) on the whole job");
        int code=1;
        if(!runProcess(real,arguments,&environment,code)) return 1;
        say("vrad2_real.exe exit code "+std::to_string(code));
        return code;
    }
};

struct Triplet { size_t first; std::string triangles,irradiance; int32_t count=0; };
struct Volume { size_t first; std::string exr; float matrix[12],box[6]; int32_t cells[3]; bool flood=false; uint64_t words=0; };
// Marker string present only in the GPU vrad3.exe; Valve's vrad3 would read the placeholder voxel file as an empty volume.
static constexpr char kPendingVoxels[]="vrad3-preview-voxels-v1";
static bool makesPreviewVoxels(const fs::path& vrad3) {
    std::string bytes;
    return readFile(vrad3,bytes) && bytes.find(kPendingVoxels)!=std::string::npos;
}
// Hammer's probe volume descriptor: 3x4 local->world matrix at 0, box mins/maxs at 0x30, cell counts at 0x48,
// flood_fill byte at 0x54; then int32 flags (bit0 direct, bit1 indirect).
static bool readVolume(const std::string& data,const Command& command,Volume& volume,std::string& why) {
    const char* p=data.data()+command.begin+4; int32_t flags=0;
    memcpy(volume.matrix,p,48); memcpy(volume.box,p+0x30,24); memcpy(volume.cells,p+0x48,12); memcpy(&flags,p+0x58,4);
    volume.flood=p[0x54]!=0;
    if(flags!=3) { why="flags "+std::to_string(flags)+" (vrad3 bakes direct + indirect)"; return false; }
    for(float v:volume.matrix) if(!std::isfinite(v)) { why="a bad transform"; return false; }
    for(int a=0;a<3;++a) {
        if(!std::isfinite(volume.box[a]) || !std::isfinite(volume.box[3+a]) || volume.box[3+a]<=volume.box[a]) { why="a bad box"; return false; }
        if(volume.cells[a]<1 || volume.cells[a]>128) { why="cells out of range"; return false; }
    }
    volume.words=(static_cast<uint64_t>(volume.cells[0])*volume.cells[1]*volume.cells[2]+31)/32;
    return true;
}

static fs::path mapPath(const std::wstring& arguments) {
    std::wstring lower=arguments;
    for(auto& c:lower) c=towlower(c);
    const size_t end=lower.rfind(L".vmap"),colon=end==std::wstring::npos?end:lower.rfind(L':',end);
    if(colon==std::wstring::npos || !colon) return {};
    return fs::path(arguments.substr(colon-1,end+5-(colon-1)));
}
static fs::path shareAddonFolder(const fs::path& map) {
    for(fs::path folder=map.parent_path();!map.empty() && folder.has_parent_path() && folder!=folder.parent_path();folder=folder.parent_path()) {
        if(_wcsicmp(folder.parent_path().filename().c_str(),L"hlvr_addons")) continue;
        const fs::path game=folder.parent_path().parent_path().parent_path()/L"game"/L"hlvr_addons"/folder.filename();
        std::error_code error;
        if(!fs::is_directory(game,error)) return {};
        SetEnvironmentVariableW(L"VRAD3_GPU_PREVIEW_ADDON",game.c_str());
        return game;
    }
    return {};
}

static std::string fixScene(const std::string& scene,const fs::path& map,const fs::path& addon) {
    if(map.empty() || addon.empty()) { say("scene: Hammer's (the map isn't in an addon)"); return {}; }
    const ULONGLONG started=GetTickCount64();
    std::string report; bool fixed=false;
    try {
        EmissiveFiles files; files.addon=addon;
        const fs::path game=addon.parent_path().parent_path();
        for(const fs::path& path:{addon/L"pak01_dir.vpk",game/L"hlvr"/L"pak01_dir.vpk",game/L"core"/L"pak01_dir.vpk"}) {
            EmissivePak pak; std::error_code error;
            if(fs::is_regular_file(path,error) && pak.open(path)) files.paks.push_back(std::move(pak));
        }
        const auto emissive=environmentValue(L"VRAD3_GPU_EMISSIVE");
        const bool uvs=!emissive.empty() && emissive!=L"0" && emissive!=L"off" &&
            environmentValue(L"VRAD3_GPU_ESTIMATOR")==L"cs2" && environmentValue(L"VRAD3_GPU_EMISSIVE_MASK")!=L"average";
        fixed=fixPreviewScene(fs::u8path(scene),map,files,uvs,L"vrad2shim_scene.dat",report);
    } catch(const std::exception& failure) { report=std::string("can't fix it: ")+failure.what(); }
    char seconds[32]; sprintf_s(seconds,"%.2f",(GetTickCount64()-started)/1000.0);
    say(std::string("scene: ")+(fixed?"the compile's":"Hammer's")+" ("+report+", "+seconds+" s)");
    return fixed?"vrad2shim_scene.dat":std::string();
}

static int bake(Job& job) {
    const auto mode=settingLower(environmentValue(L"VRAD3_GPU_HAMMER_PREVIEW"));
    if(mode==L"vrad2") return job.vrad2("hammer_preview = vrad2");
    if(!readFile(L"commands.dat",job.original)) return job.vrad2("no commands.dat in the job folder");
    std::vector<Command> commands; std::string error;
    if(!parseCommands(job.original,commands,error)) return job.vrad2(error);

    std::vector<Triplet> triplets; std::vector<Volume> volumes; std::vector<std::string> scenes;
    std::vector<bool> taken(commands.size(),false);
    size_t firstWork=commands.size(),lastScene=0;
    const bool volumesToVrad3=settingLower(environmentValue(L"VRAD3_GPU_PREVIEW_PROBE_VOLUMES"))!=L"vrad2";
    int voxelMaker=-1;
    for(size_t i=0;i<commands.size();++i) {
        if(commands[i].id==1) { scenes.push_back(commands[i].names[0]); lastScene=i; }
        if(i+2<commands.size() && commands[i].id==4 && commands[i+1].id==0xe && commands[i+1].value==3 && commands[i+2].id==6) {
            triplets.push_back({i,commands[i].names[0],commands[i+2].names[0]});
            taken[i]=taken[i+1]=taken[i+2]=true; firstWork=std::min(firstWork,i);
            i+=2;
        } else if(volumesToVrad3 && i+1<commands.size() && commands[i].id==0xf && commands[i+1].id==8) {
            if(voxelMaker<0) {
                voxelMaker=makesPreviewVoxels(job.vrad3)?1:0;
                if(!voxelMaker) say("probe volumes stay with vrad2: vrad3.exe beside vrad2.exe isn't the GPU vrad3.exe (it makes their voxels)");
            }
            Volume volume; volume.first=i; volume.exr=commands[i+1].names[0]; std::string why;
            if(voxelMaker && readVolume(job.original,commands[i],volume,why)) { volumes.push_back(volume); taken[i]=taken[i+1]=true; firstWork=std::min(firstWork,i); }
            else if(voxelMaker) say("probe volume "+volume.exr+" stays with vrad2: "+why);
            i+=1;
        }
    }
    if(triplets.empty() && volumes.empty()) return job.vrad2("no triangle-mode vertex lighting or probe volumes for vrad3 in this job");
    if(scenes.size()!=1 || lastScene>firstWork) return job.vrad2(std::to_string(scenes.size())+" scene files, or the scene after the work (expected one before)");
    if(scenes[0].empty() || scenes[0].find_first_of(" \t\"")!=std::string::npos) return job.vrad2("scene file name vrad3's script can't hold: "+scenes[0]);
    std::error_code fileError;
    if(!fs::is_regular_file(fs::path(scenes[0]),fileError)) return job.vrad2("scene file "+scenes[0]+" is missing");
    if(!fs::is_regular_file(job.vrad3,fileError)) return job.vrad2("vrad3.exe is missing beside vrad2.exe");

    auto scratch=[&](const fs::path& file) { std::error_code removeError; fs::remove(file,removeError); job.temporary.push_back(file); };
    std::ostringstream script,sections;
    size_t corners=0;
    for(size_t k=0;k<triplets.size();++k) {
        auto& t=triplets[k]; std::string bytes;
        if(!readFile(fs::path(t.triangles),bytes) || bytes.size()<4) return job.vrad2("can't read "+t.triangles);
        memcpy(&t.count,bytes.data(),4);
        if(t.count<0 || t.count%3 || static_cast<uint64_t>(t.count)>0x7fffffffull/12 || bytes.size()<4+12ull*t.count)
            return job.vrad2(t.triangles+": "+std::to_string(t.count)+" corners in "+std::to_string(bytes.size())+" bytes");
        corners+=t.count;
        if(!t.count) continue;
        const fs::path in=L"vrad2shim_in_"+std::to_wstring(k)+L".raw";
        job.temporary.push_back(in); scratch(L"vrad2shim_out_"+std::to_wstring(k)+L".raw");
        if(!writeFile(in,bytes.data()+4,12ull*t.count)) return job.vrad2("can't write "+narrow(in.wstring()));
        script<<"    path_trace_triangles vrad2shim_in_"<<k<<".raw 0 "<<t.count<<" vrad2shim_out_"<<k<<".raw\n";
    }
    const bool quality2=environmentValue(L"VRAD3_GPU_PREVIEW_QUALITY")==L"2";
    for(size_t k=0;k<volumes.size();++k) {
        const auto& v=volumes[k]; const std::string name="vrad2shim_lpv_"+std::to_string(k);
        for(const char* suffix:{"_voxels.dat","_direct.dat","_indirect.dat",".exr"}) scratch(fs::u8path(name+suffix));
        char range[64]; sprintf_s(range," 0 0 0 %d %d %d",v.cells[0],v.cells[1],v.cells[2]);
        script<<"    run "<<name<<"\n"
              <<"    lpv_direct "<<name<<"_direct.dat"<<range<<"\n"
              <<"    lpv_indirect "<<name<<"_indirect.dat"<<range<<(quality2?" 256 1024":" 128 512")<<" 0.010000\n"
              <<"    run "<<name<<"\n"
              <<"    lpv_image\n"
              <<"    lpv_direct_load "<<name<<"_direct.dat"<<range<<"\n"
              <<"    lpv_indirect_load "<<name<<"_indirect.dat"<<range<<"\n"
              <<"    lpv_image_ambient_occlusion\n"
              <<"    lpv_image_median_filter 3\n"
              <<"    lpv_image_resolve\n"
              <<"    lpv_image_write_ambientcube "<<name<<".exr\n";
        char text[512];
        sprintf_s(text,"    lpv_cells %d %d %d\n    lpv_box %.9g %.9g %.9g %.9g %.9g %.9g\n",v.cells[0],v.cells[1],v.cells[2],
                  v.box[0],v.box[1],v.box[2],v.box[3],v.box[4],v.box[5]);
        sections<<"\n"<<name<<":\n"<<text<<"    lpv_local_to_world";
        for(float m:v.matrix) { sprintf_s(text," %.9g",m); sections<<text; }
        sections<<"\n    lpv_init\n    lpv_load_solid_voxels "<<name<<"_voxels.dat\n    done\n";
    }
    script<<"    done\n"<<sections.str();
    const fs::path scriptFile=L"vrad2shim.vrad3";
    job.temporary.push_back(scriptFile);
    const fs::path map=mapPath(job.arguments),addon=shareAddonFolder(map);
    scratch(L"vrad2shim_scene.dat");
    std::string scene=fixScene(scenes[0],map,addon);
    if(scene.empty()) scene=scenes[0];
    auto prepare=[&]() {
        const auto text="vrad2shim:\n    sceneinfo "+scene+" 0\n"+script.str();
        if(!writeFile(scriptFile,text.data(),text.size())) return std::string("can't write the vrad3 script");
        for(size_t k=0;k<volumes.size();++k) {
            const auto& v=volumes[k]; const std::string name="vrad2shim_lpv_"+std::to_string(k)+"_voxels.dat";
            std::string voxels(v.words*4,'\0'); voxels.append(kPendingVoxels,sizeof(kPendingVoxels)); voxels.push_back(v.flood?1:0);
            if(!writeFile(name,voxels.data(),voxels.size())) return "can't write "+name;
        }
        return std::string();
    };
    if(const auto why=prepare(); !why.empty()) return job.vrad2(why);

    say("vrad3: "+std::to_string(triplets.size())+" batch(es), "+std::to_string(corners/3)+" triangles, "+
        std::to_string(volumes.size())+" probe volume(s), scene "+scene);
    const ULONGLONG started=GetTickCount64();
    std::string failure;
    const fs::path report=L"vrad2shim_report.txt";
    scratch(report);
    std::error_code reportError; const fs::path reportPath=fs::absolute(report,reportError);
    for(int attempt=0;(corners || !volumes.empty()) && attempt<2;++attempt) {
        int code=1; failure.clear();
        const std::wstring arguments=L" -script vrad2shim.vrad3 -entrypoint vrad2shim -threads "+
            std::to_wstring(std::max<DWORD>(1,GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)));
        { std::error_code removeError; fs::remove(report,removeError); }
        SetEnvironmentVariableW(L"VRAD3_GPU_PREVIEW_REPORT",reportError?nullptr:reportPath.c_str());
        const bool ran=runProcess(job.vrad3,arguments,nullptr,code);
        SetEnvironmentVariableW(L"VRAD3_GPU_PREVIEW_REPORT",nullptr);
        if(std::string line; ran && readFile(report,line)) {
            while(!line.empty() && (line.back()=='\n' || line.back()=='\r')) line.pop_back();
            say("vrad3: "+line);
        } else if(ran) say("vrad3: no report (Valve's own vrad3.exe, or an older GPU vrad3.exe)");
        if(!ran) failure="vrad3 didn't start";
        else if(code) failure="vrad3 failed (exit code "+std::to_string(code)+")";
        if(failure.empty() || scene==scenes[0]) break;
        say(failure+" on the shim's scene: again on Hammer's");
        scene=scenes[0];
        if(const auto why=prepare(); !why.empty()) return job.vrad2(why);
    }
    if(!failure.empty()) say(failure+": keeping the outputs it finished");
    size_t kept=0;
    for(size_t k=0;k<triplets.size();++k) {
        const auto& t=triplets[k]; std::string bytes,why;
        if(t.count && (!readFile(L"vrad2shim_out_"+std::to_wstring(k)+L".raw",bytes) || bytes.size()!=12ull*t.count))
            why="vrad3's output is missing or the wrong size";
        std::vector<uint16_t> out(2+3ull*t.count);
        memcpy(out.data(),&t.count,4);
        for(size_t i=0;why.empty() && i<3ull*t.count;++i) {
            float v; memcpy(&v,bytes.data()+4*i,4);
            if(!std::isfinite(v) || v<-1e-3f) why="vrad3's output has a bad value ("+std::to_string(v)+")";
            out[2+i]=toHalf(std::min(std::max(v,0.f),65504.f));
        }
        if(why.empty() && !writeFile(fs::path(t.irradiance),out.data(),out.size()*2)) why="can't write it";
        if(why.empty()) { ++kept; continue; }
        say(t.irradiance+" goes to vrad2: "+why);
        taken[t.first]=taken[t.first+1]=taken[t.first+2]=false;
    }
    for(const auto& v:volumes) {
        const std::string name="vrad2shim_lpv_"+std::to_string(&v-volumes.data());
        std::error_code copyError;
        const bool made=fs::file_size(name+"_voxels.dat",copyError)==v.words*4 && !copyError && fs::file_size(name+".exr",copyError)>0 && !copyError;
        if(made) fs::copy_file(name+".exr",fs::path(v.exr),fs::copy_options::overwrite_existing,copyError);
        if(made && !copyError) { ++kept; continue; }
        say("probe volume "+v.exr+" goes to vrad2: vrad3's "+(made?"exr couldn't be copied there ("+copyError.message()+")":"voxels or exr are missing"));
        taken[v.first]=taken[v.first+1]=false;
    }
    if(!failure.empty() && !kept) return job.vrad2(failure);
    char seconds[32]; sprintf_s(seconds,"%.1f",(GetTickCount64()-started)/1000.0);
    say("vrad3 done in "+std::string(seconds)+" s");

    std::string rest; bool work=false,photons=false;
    for(size_t i=0;i<commands.size();++i) {
        if(taken[i]) continue;
        rest.append(job.original,commands[i].begin,commands[i].end-commands[i].begin);
        const int id=commands[i].id;
        photons=photons || id==0xc || id==5 || id==2;
        work=work || (id!=0 && id!=1 && id!=0xc && id!=5 && id!=2);
    }
    if(photons && !work) say("photon map skipped: nothing left for vrad2 uses it (vrad3 traces every bounce)");
    int code=0;
    if(work) {
        say("vrad2_real.exe for the commands vrad3 doesn't cover");
        job.commandsChanged=true;
        if(!writeFile(L"commands.dat",rest.data(),rest.size())) return job.vrad2("can't write the filtered commands.dat");
        if(!runProcess(job.real,job.arguments,&job.environment,code)) return job.vrad2("vrad2_real.exe didn't start");
        if(code) return job.vrad2("vrad2_real.exe failed on the other commands (exit code "+std::to_string(code)+")");
    }
    job.cleanUp();
    say("done");
    return code;
}

static fs::path localFolder(const wchar_t* setting,const wchar_t* name) {
    const auto chosen=environmentValue(setting);
    if(!chosen.empty()) {
        wchar_t expanded[32768]{}; ExpandEnvironmentStringsW(chosen.c_str(),expanded,32768);
        return fs::absolute(expanded);
    }
    const auto local=environmentValue(L"LOCALAPPDATA");
    return local.empty()?fs::path():fs::path(local)/L"vrad3-gpu"/name;
}
static void copyJob(const fs::path& to) {
    std::error_code error;
    fs::create_directories(to,error);
    fs::copy(fs::current_path(),to,fs::copy_options::recursive|fs::copy_options::overwrite_existing,error);
    if(error) say("capture: copying the job to "+narrow(to.wstring())+" failed: "+error.message());
}

int wmain(int,wchar_t**) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    Job job;
    job.arguments=argumentTail();
    job.environment=environmentBlock();
    std::wstring module(32768,L'\0');
    const DWORD length=GetModuleFileNameW(nullptr,module.data(),static_cast<DWORD>(module.size()));
    module.resize(length<module.size()?length:0);
    job.self=module; const auto bin=job.self.parent_path();
    loadWrapperSettings();
    const auto realOverride=environmentValue(L"VRAD2_REAL_EXE"),vrad3Override=environmentValue(L"VRAD2_SHIM_VRAD3_EXE");
    job.real=realOverride.empty()?bin/L"vrad2_real.exe":fs::path(realOverride);
    job.vrad3=vrad3Override.empty()?bin/L"vrad3.exe":fs::path(vrad3Override);
    std::error_code error;
    if(!fs::is_regular_file(job.real,error) || fs::equivalent(job.real,job.self,error)) {
        say("vrad2_real.exe is missing. Rename Valve's original vrad2.exe to vrad2_real.exe beside this one (or set VRAD2_REAL_EXE).");
        return 1;
    }
    // Job object with kill-on-close: child processes die with the shim.
    if(HANDLE handle=CreateJobObjectW(nullptr,nullptr)) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if(SetInformationJobObject(handle,JobObjectExtendedLimitInformation,&limits,sizeof(limits))) jobObject=handle;
        else CloseHandle(handle);
    }
    say("job "+narrow(fs::current_path().wstring())+", arguments:"+narrow(job.arguments));

    fs::path capture;
    if(environmentValue(L"VRAD3_GPU_PREVIEW_CAPTURE")==L"1") {
        const auto root=localFolder(L"VRAD3_GPU_PREVIEW_CAPTURE_FOLDER",L"captures");
        SYSTEMTIME t; GetLocalTime(&t); wchar_t name[64];
        swprintf_s(name,L"vrad2-%04u%02u%02u-%02u%02u%02u-%lu",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,GetCurrentProcessId());
        if(!root.empty()) { capture=root/name; copyJob(capture/L"input"); say("capture: "+narrow(capture.wstring())); }
    }
    int code=1;
    try { code=bake(job); }
    catch(const std::exception& failure) { code=job.vrad2(std::string("shim error: ")+failure.what()); }
    if(!capture.empty()) {
        copyJob(capture/L"output");
        std::ofstream(capture/L"vrad2-shim.log",std::ios::binary)<<logText;
    }
    if(wrapperLogging) {
        const auto folder=localFolder(L"VRAD3_GPU_LOG_FOLDER",L"logs");
        if(!folder.empty()) fs::create_directories(folder,error);
        if(!folder.empty()) std::ofstream(folder/L"vrad2-shim.log",std::ios::binary|std::ios::app)<<logText<<"exit code "<<code<<"\n\n";
    }
    return code;
}
