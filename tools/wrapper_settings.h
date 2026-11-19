// Reads vrad3_gpu.ini, validates each setting and translates it into the wrapper's environment controls.
#pragma once
#include <cwctype>

static int wrapperLogging=0;
static std::wstring settingTrim(std::wstring text) {
    const auto first=text.find_first_not_of(L" \t\r\n");
    if(first==std::wstring::npos) return {};
    return text.substr(first,text.find_last_not_of(L" \t\r\n")-first+1);
}
static std::wstring settingLower(std::wstring text) {
    for(auto& c:text) c=static_cast<wchar_t>(towlower(c));
    return text;
}
static std::vector<std::wstring> settingWarnings;
static void settingWarn(const std::wstring& text) {
    std::wcerr<<L"vrad3_gpu.ini: "<<text<<L"\n";
    settingWarnings.push_back(text);
}
// The ini as Notepad may save it: UTF-8 with or without a BOM, or UTF-16; else ANSI.
static std::wstring settingFileText(const fs::path& path) {
    std::ifstream input(path,std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>());
    auto at=[&](size_t i) { return static_cast<unsigned char>(bytes[i]); };
    if(bytes.size()>=2 && ((at(0)==0xff && at(1)==0xfe) || (at(0)==0xfe && at(1)==0xff))) {
        const bool big=at(0)==0xfe; std::wstring text;
        for(size_t i=2;i+1<bytes.size();i+=2) text+=static_cast<wchar_t>(big?at(i)<<8|at(i+1):at(i+1)<<8|at(i));
        return text;
    }
    size_t first=bytes.size()>=3 && at(0)==0xef && at(1)==0xbb && at(2)==0xbf ? 3 : 0;
    if(first==bytes.size()) return {};
    const int size=static_cast<int>(bytes.size()-first);
    UINT page=CP_UTF8; int count=MultiByteToWideChar(page,MB_ERR_INVALID_CHARS,bytes.data()+first,size,nullptr,0);
    if(!count) { page=CP_ACP; count=MultiByteToWideChar(page,0,bytes.data()+first,size,nullptr,0); }
    std::wstring text(static_cast<size_t>(count),L'\0');
    MultiByteToWideChar(page,0,bytes.data()+first,size,text.data(),count);
    return text;
}
static bool settingParse(const std::wstring& name,const std::wstring& value,bool valid,std::wstring& env,std::wstring& translated,
                         std::map<std::wstring,std::wstring>& controls) {
    const auto lower=settingLower(value);
    if(name==L"enabled") {
        env=L"VRAD3_GPU_ENABLED"; valid=valid && (lower==L"true" || lower==L"false"); translated=lower==L"true"?L"1":L"0";
    } else if(name==L"parallel_compiling") {
        env=L"VRAD3_GPU_PREFETCH"; translated=lower==L"off"?L"0":L"1";
        bool numeric=!value.empty() && value.find_first_not_of(L"0123456789")==std::wstring::npos;
        if(numeric) {
            try { auto n=std::stoull(value); numeric=n>0 && n<=UINT32_MAX; } catch(...) { numeric=false; }
        }
        valid=valid && (lower==L"auto" || lower==L"off" || numeric);
        if(valid && numeric) controls[L"VRAD3_GPU_PREFETCH_MAX"]=value;
    } else if(name==L"memory_reserve") {
        env=L"VRAD3_GPU_MEMORY_RESERVE_GB"; translated=lower==L"auto"?L"":lower;
        bool numeric=!value.empty() && value.size()<=4 && value.find_first_not_of(L"0123456789")==std::wstring::npos;
        valid=valid && (lower==L"auto" || lower==L"off" || numeric);
    } else if(name==L"emissive") {
        env=L"VRAD3_GPU_EMISSIVE"; translated=lower==L"on"?L"1":lower==L"off"?L"0":lower;
        bool numeric=false;
        if(lower!=L"on" && lower!=L"off" && !value.empty() && value.size()<=8 && value.find_first_not_of(L"0123456789.")==std::wstring::npos) {
            try { const float scale=std::stof(value); numeric=scale>0 && scale<=1000; } catch(...) { numeric=false; }
        }
        valid=valid && (lower==L"on" || lower==L"off" || numeric);
    } else if(name==L"emissive_fallback") {
        env=L"VRAD3_GPU_EMISSIVE_FALLBACK"; valid=valid && (lower==L"cpu" || lower==L"stop"); translated=lower==L"stop"?L"stop":L"";
    } else if(name==L"gpu_usage") {
        env=L"VRAD3_GPU_USAGE"; translated=lower==L"auto"?L"":lower;
        bool numeric=!value.empty() && value.size()<=3 && value.find_first_not_of(L"0123456789")==std::wstring::npos;
        if(numeric) { const auto n=std::stoul(value); numeric=n>=50 && n<=100; }
        valid=valid && (lower==L"auto" || lower==L"max" || numeric);
    } else if(name==L"light_paths") {
        env=L"VRAD3_GPU_LIGHT_PATHS"; valid=valid && (lower==L"auto" || lower==L"on" || lower==L"off");
        translated=lower==L"auto"?L"":lower==L"on"?L"1":L"0";
    } else if(name==L"estimator") {
        env=L"VRAD3_GPU_ESTIMATOR"; valid=valid && (lower==L"hla" || lower==L"cs2"); translated=lower;
    } else if(name==L"clamp_method") {
        env=L"VRAD3_GPU_CS2_CLAMP"; valid=valid && (lower==L"hla" || lower==L"cs2"); translated=lower==L"hla"?L"hla":L"";
    } else if(name==L"ao_method") {
        env=L"VRAD3_GPU_CS2_AO"; valid=valid && (lower==L"hla" || lower==L"cs2"); translated=lower==L"hla"?L"hla":L"";
    } else if(name==L"backface_shadows") {
        env=L"VRAD3_GPU_CS2_SHADOWS"; valid=valid && (lower==L"hla" || lower==L"cs2"); translated=lower==L"hla"?L"hla":L"";
    } else if(name==L"emissive_mask") {
        env=L"VRAD3_GPU_EMISSIVE_MASK"; valid=valid && (lower==L"texel" || lower==L"average"); translated=lower==L"average"?L"average":L"";
    } else if(name==L"sample_grid") {
        env=L"VRAD3_GPU_SAMPLE_GRID"; valid=valid && (lower==L"auto" || lower==L"5" || lower==L"3" || lower==L"2" || lower==L"1");
        translated=lower==L"auto"?L"":lower;
    } else if(name==L"light_limit_report") {
        env=L"VRAD3_GPU_LIGHT_LIMIT_REPORT"; valid=valid && (lower==L"on" || lower==L"off"); translated=lower==L"on"?L"1":L"0";
    } else if(name==L"cs2_fireflies") {
        env=L"VRAD3_GPU_CS2_FIREFLIES"; valid=valid && (lower==L"on" || lower==L"off"); translated=lower==L"on"?L"1":L"0";
    } else if(name==L"cs2_samples") {
        env=L"VRAD3_GPU_CS2_SAMPLES"; valid=valid && (lower==L"on" || lower==L"off"); translated=lower==L"on"?L"1":L"0";
    } else if(name==L"progress_window") {
        env=L"VRAD3_GPU_PROGRESS_WINDOW"; valid=valid && (lower==L"on" || lower==L"off" || lower==L"visible");
        translated=lower==L"on"?L"1":lower==L"visible"?L"2":L"0";
    } else if(name==L"direct_light" || name==L"light_probes") {
        env=name==L"direct_light"?L"VRAD3_GPU_DIRECTLIGHT":L"VRAD3_GPU_PROBES";
        valid=valid && (lower==L"auto" || lower==L"on" || lower==L"off");
        translated=lower==L"auto"?L"":lower==L"on"?L"1":L"0";
    } else if(name==L"verify") {
        env=L"VRAD3_GPU_VERIFY"; valid=valid && (lower==L"off" || lower==L"on"); translated=lower==L"on"?L"1":L"0";
    } else if(name==L"verify_blocks" || name==L"verify_probes") {
        env=name==L"verify_blocks"?L"VRAD3_GPU_VERIFY_BLOCKS":L"VRAD3_GPU_VERIFY_PROBES"; translated=value;
        bool numeric=!value.empty() && value.find_first_not_of(L"0123456789")==std::wstring::npos;
        try { numeric=numeric && std::stoull(value)>0 && std::stoull(value)<=UINT32_MAX; } catch(...) { numeric=false; }
        valid=valid && numeric;
    } else if(name==L"logging") {
        env=L"VRAD3_GPU_LOGGING"; translated=lower==L"on"?L"summary":lower;
        valid=valid && (lower==L"off" || lower==L"on" || lower==L"summary" || lower==L"detailed");
    } else if(name==L"log_folder") { env=L"VRAD3_GPU_LOG_FOLDER"; translated=value; }
    else if(name==L"hammer_preview") {
        env=L"VRAD3_GPU_HAMMER_PREVIEW"; valid=valid && (lower==L"vrad3" || lower==L"vrad2"); translated=lower;
    } else if(name==L"preview_capture") {
        env=L"VRAD3_GPU_PREVIEW_CAPTURE"; valid=valid && (lower==L"on" || lower==L"off"); translated=lower==L"on"?L"1":L"0";
    } else if(name==L"preview_capture_folder") { env=L"VRAD3_GPU_PREVIEW_CAPTURE_FOLDER"; translated=value; }
    else if(name==L"preview_probe_volumes") {
        env=L"VRAD3_GPU_PREVIEW_PROBE_VOLUMES"; valid=valid && (lower==L"vrad3" || lower==L"vrad2"); translated=lower;
    } else if(name==L"preview_gpu") {
        env=L"VRAD3_GPU_PREVIEW_GPU"; valid=valid && (lower==L"auto" || lower==L"on" || lower==L"off");
        translated=lower==L"auto"?L"":lower==L"on"?L"1":L"0";
    } else if(name==L"preview_quality") {
        env=L"VRAD3_GPU_PREVIEW_QUALITY"; valid=valid && (lower==L"1" || lower==L"2"); translated=lower;
    } else if(name==L"preview_min_samples" || name==L"preview_max_samples") {
        env=name==L"preview_min_samples"?L"VRAD3_GPU_PREVIEW_MIN":L"VRAD3_GPU_PREVIEW_MAX"; translated=lower==L"auto"?L"":value;
        bool numeric=lower==L"auto" || (!value.empty() && value.size()<=4 && value.find_first_not_of(L"0123456789")==std::wstring::npos);
        try { numeric=numeric && (lower==L"auto" || (std::stoul(value)>=1 && std::stoul(value)<=4096)); } catch(...) { numeric=false; }
        valid=valid && numeric;
    } else if(name==L"preview_error") {
        env=L"VRAD3_GPU_PREVIEW_ERROR"; translated=lower==L"auto"?L"":value;
        bool numeric=lower==L"auto";
        if(!numeric) try { size_t used=0; const float error=std::stof(value,&used); numeric=used==value.size() && error>=0 && error<=1; } catch(...) { numeric=false; }
        valid=valid && numeric;
    } else if(name==L"preview_subdivide") {
        env=L"VRAD3_GPU_PREVIEW_SUBDIVIDE"; translated=lower==L"auto"?L"":lower==L"off"?L"0":value;
        bool numeric=lower==L"auto" || lower==L"off";
        if(!numeric) try { size_t used=0; const unsigned long units=std::stoul(value,&used); numeric=used==value.size() && units>=8 && units<=4096; } catch(...) { numeric=false; }
        valid=valid && numeric;
    } else if(name==L"preview_corners") {
        env=L"VRAD3_GPU_PREVIEW_CORNERS"; translated=lower==L"auto"?L"":lower;
        valid=valid && (lower==L"auto" || lower==L"cs2" || lower==L"valve");
    } else if(name==L"gpu") {
        env=L"VRAD3_GPU_ADAPTER"; translated=lower==L"auto"?L"":value;
        valid=valid && !value.empty();
    } else valid=false;
    return valid;
}
static void loadWrapperSettings() noexcept {
    try {
        wchar_t module[32768]{};
        if(!GetModuleFileNameW(nullptr,module,32768)) return;
        std::wistringstream input(settingFileText(fs::path(module).parent_path()/L"vrad3_gpu.ini"));
        std::wstring line; bool section=true,app=false;
        std::map<std::wstring,std::wstring> controls;
        while(std::getline(input,line)) {
            auto comment=line.find_first_of(L";#");
            if(comment!=std::wstring::npos) line.resize(comment);
            line=settingTrim(line); if(line.empty()) continue;
            if(line.front()==L'[' && line.back()==L']') {
                const auto name=settingLower(settingTrim(line.substr(1,line.size()-2)));
                section=name==L"vrad3_gpu"; app=name==L"vrad3_settings";
                if(!section && !app) settingWarn(L"unknown section "+line);
                continue;
            }
            if(app) continue;
            auto split=line.find(L'=');
            auto name=settingLower(settingTrim(line.substr(0,split)));
            auto value=split==std::wstring::npos?L"":settingTrim(line.substr(split+1));
            std::wstring env,translated;
            const bool valid=settingParse(name,value,section && split!=std::wstring::npos,env,translated,controls);
            if(!valid) { settingWarn(L"ignored unknown key/value "+name+L" = "+value); continue; }
            controls[env]=translated;
        }
        // An environment variable already set wins over the ini.
        for(const auto& control:controls) {
            if(!GetEnvironmentVariableW(control.first.c_str(),nullptr,0) && !control.second.empty())
                SetEnvironmentVariableW(control.first.c_str(),control.second.c_str());
        }
        wchar_t logging[32]{}; GetEnvironmentVariableW(L"VRAD3_GPU_LOGGING",logging,32);
        auto mode=settingLower(logging);
        wrapperLogging=mode==L"detailed"?2:mode==L"summary"?1:0;
        wchar_t limits[8]{};
        if(!wrapperLogging && GetEnvironmentVariableW(L"VRAD3_GPU_LIGHT_LIMIT_REPORT",limits,8)==1 && limits[0]==L'1') wrapperLogging=1;
        if(mode.empty() && GetEnvironmentVariableW(L"VRAD3_GPU_CACHE",nullptr,0)) wrapperLogging=2;
        wchar_t verify[8]{}; GetEnvironmentVariableW(L"VRAD3_GPU_VERIFY",verify,8);
        if(std::wstring(verify)==L"1" && !wrapperLogging) wrapperLogging=1;
    } catch(...) { std::cerr<<"vrad3_gpu.ini: could not read settings; keeping defaults\n"; }
}
static void resolveLightPathsAuto() noexcept {
    try {
        if(!GetEnvironmentVariableW(L"VRAD3_GPU_ESTIMATOR",nullptr,0)) {
            wchar_t value[8]{};
            const bool hlaFeature=GetEnvironmentVariableW(L"VRAD3_GPU_LIGHT_PATHS",value,8)==1 && value[0]==L'1';
            SetEnvironmentVariableW(L"VRAD3_GPU_ESTIMATOR",hlaFeature?L"hla":L"cs2");
        }
        wchar_t estimator[8]{};
        if(GetEnvironmentVariableW(L"VRAD3_GPU_ESTIMATOR",estimator,8)==3)
            SetEnvironmentVariableW(L"VRAD3_GPU_ESTIMATOR",_wcsicmp(estimator,L"cs2")==0?L"cs2":_wcsicmp(estimator,L"hla")==0?L"hla":estimator);
        if(GetEnvironmentVariableW(L"VRAD3_GPU_ESTIMATOR",estimator,8)==3 && _wcsicmp(estimator,L"cs2")==0) {
            wchar_t value[8]{};
            if(GetEnvironmentVariableW(L"VRAD3_GPU_LIGHT_PATHS",value,8)==1 && value[0]==L'1')
                settingWarn(L"estimator = cs2 turns VRAD3_GPU_LIGHT_PATHS off");
            SetEnvironmentVariableW(L"VRAD3_GPU_LIGHT_PATHS",L"0");
            return;
        }
        if(GetEnvironmentVariableW(L"VRAD3_GPU_LIGHT_PATHS",nullptr,0)) return;
        std::ifstream script(fs::current_path()/L"script.vrad3");
        std::string line;
        while(std::getline(script,line)) {
            const auto at=line.find("lightmap_pathtrace");
            if(at==std::string::npos) continue;
            std::istringstream words(line.substr(at+18));
            double minimum=0,maximum=0,tolerance=0;
            if(!(words>>minimum>>maximum>>tolerance) || !std::isfinite(tolerance) || tolerance<=0) return;
            // Final-quality compiles (lightmap_pathtrace tolerance <= 0.025) get light paths on, faster previews off.
            SetEnvironmentVariableW(L"VRAD3_GPU_LIGHT_PATHS",tolerance<=0.025+1e-6?L"1":L"0");
            SetEnvironmentVariableW(L"VRAD3_GPU_LIGHT_PATHS_AUTO",L"1");
            return;
        }
    } catch(...) {}
}
static fs::path compileTempRoot() {
    auto path=(fs::current_path()/L"vrad3_gpu_tmp").lexically_normal().wstring();
    // Extended-length path prefix so deep compile paths exceed MAX_PATH.
    if(path.rfind(L"\\\\?\\",0)!=0) path=path.rfind(L"\\\\",0)==0?L"\\\\?\\UNC\\"+path.substr(2):L"\\\\?\\"+path;
    return path;
}
