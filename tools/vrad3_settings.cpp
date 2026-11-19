// vrad3_settings.exe: a small window for editing vrad3_gpu.ini, validated with the same parser vrad3.exe uses.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <dxgi.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <shobjidl.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>
namespace fs=std::filesystem;
#pragma warning(disable:4505)
#include "wrapper_settings.h"
#include "settings_template.h"
#pragma comment(lib,"user32.lib")
#pragma comment(lib,"gdi32.lib")
#pragma comment(lib,"comctl32.lib")
#pragma comment(lib,"shell32.lib")
#pragma comment(lib,"dxgi.lib")
#pragma comment(lib,"dwmapi.lib")
#pragma comment(lib,"uxtheme.lib")
#pragma comment(lib,"advapi32.lib")
#pragma comment(lib,"ole32.lib")
#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

enum Group { Basic, Cs2, Hla, Both, Preview, Reports, GroupCount };
static const wchar_t* const kTabNames[GroupCount]={L"Basic",L"CS2 mode",L"HLA mode",L"Both modes",L"Hammer preview",L"Reports and testing"};
struct Setting {
    std::wstring key,name,defaultValue,title,description; Group group=Basic;
    std::vector<std::wstring> choices; bool editable=true;
    std::wstring value,saved;
    HWND label{},combo{},note{},browse{}; int row=0; bool invalid=false; std::wstring why;
    bool text=false;
};
static const struct { const wchar_t* key; const wchar_t* name; const wchar_t* choices; bool editable; } kChoices[]={
    {L"enabled",L"GPU Baking",L"true|false",false},{L"estimator",L"Estimator Mode",L"cs2|hla",false},
    {L"emissive",L"Emissive Lighting",L"off|on|0.5|2",true},{L"gpu_usage",L"GPU Usage",L"auto|max|90|75|50",true},
    {L"parallel_compiling",L"Parallel Compiling",L"auto|off|1|2|4",true},{L"memory_reserve",L"Memory Reserve",L"auto|off|2|4|8",true},
    {L"gpu",L"GPU",L"auto",true},{L"progress_window",L"Progress Window",L"off|on|visible",false},
    {L"logging",L"Logging",L"off|summary|detailed",false},{L"log_folder",L"Log Folder",L"",true},
    {L"cs2_samples",L"CS2 Sample Counts",L"on|off",false},{L"cs2_fireflies",L"CS2 Firefly Removal",L"on|off",false},
    {L"clamp_method",L"Clamp Method",L"cs2|hla",false},{L"ao_method",L"AO Method",L"cs2|hla",false},
    {L"backface_shadows",L"Backface Shadows",L"cs2|hla",false},{L"emissive_mask",L"Emissive Mask",L"texel|average",false},
    {L"light_paths",L"Light Paths",L"auto|on|off",false},
    {L"sample_grid",L"Sample Grid",L"auto|5|3|2|1",false},{L"emissive_fallback",L"Emissive Fallback",L"cpu|stop",false},
    {L"direct_light",L"GPU Direct Light",L"auto|on|off",false},{L"light_probes",L"GPU Light Probes",L"auto|on|off",false},
    {L"light_limit_report",L"Light Limit Report",L"off|on",false},
    {L"verify",L"Verify",L"off|on",false},{L"verify_blocks",L"Verify Blocks",L"1|2|4",true},{L"verify_probes",L"Verify Probes",L"1|2|4",true},
    {L"hammer_preview",L"Hammer Preview",L"vrad3|vrad2",false},{L"preview_probe_volumes",L"Preview Probe Volumes",L"vrad3|vrad2",false},
    {L"preview_gpu",L"Preview GPU",L"auto|on|off",false},{L"preview_quality",L"Preview Quality",L"1|2",false},
    {L"preview_min_samples",L"Preview Min Samples",L"auto|64|128|256|512",true},{L"preview_max_samples",L"Preview Max Samples",L"auto|128|256|1024|4096",true},
    {L"preview_error",L"Preview Error",L"auto|0.1|0.05|0.025|0",true},{L"preview_subdivide",L"Preview Subdivide",L"auto|off|32|64|128|256",true},
    {L"preview_corners",L"Preview Corners",L"auto|cs2|valve",false},
    {L"preview_capture",L"Preview Capture",L"off|on",false},{L"preview_capture_folder",L"Preview Capture Folder",L"",true},
};
static std::vector<Setting> settings;
static fs::path iniPath;
static bool iniExists=false;
static HWND window,tab,description,ramLabel,ramCombo,ramText,status,pathText,themeLabel,themeCombo,defaults,saveButton,closeButton;
static HFONT font,monoFont,boldFont;
static double scale=1;
static SIZE minimum{};
static int S(int v) { return static_cast<int>(std::lround(v*scale)); }
enum { IdTab=10,IdDescription,IdRamCombo,IdRamText,IdDefaults,IdSave,IdClose,IdStatus,IdPath,IdTheme,IdCombo=1000,IdLabel=2000,IdNote=3000,IdBrowse=4000 };

static const wchar_t* const kThemes[]={L"system",L"light",L"dark"};
static std::wstring theme=L"system",themeSaved=L"system";
static bool dark=false;
static const COLORREF kDarkWindow=RGB(32,32,32),kDarkField=RGB(43,43,43),kDarkTabOff=RGB(45,45,45),kDarkBorder=RGB(85,85,85),
                      kDarkText=RGB(232,232,232),kDarkGrey=RGB(150,150,150),kDarkRed=RGB(255,110,110);
static HBRUSH darkWindowBrush,darkFieldBrush,darkTabOffBrush,darkBorderBrush;
static HBRUSH windowBrush() { return dark?darkWindowBrush:GetSysColorBrush(COLOR_WINDOW); }

static std::wstring widen(const char* text) {
    const int n=MultiByteToWideChar(CP_UTF8,0,text,-1,nullptr,0);
    std::wstring out(static_cast<size_t>(n),L'\0'); MultiByteToWideChar(CP_UTF8,0,text,-1,out.data(),n);
    if(!out.empty() && out.back()==L'\0') out.pop_back();
    return out;
}
static std::vector<std::wstring> splitLines(const std::wstring& text) {
    std::vector<std::wstring> lines; std::wistringstream in(text); std::wstring line;
    while(std::getline(in,line)) { if(!line.empty() && line.back()==L'\r') line.pop_back(); lines.push_back(line); }
    return lines;
}
static Setting* findSetting(const std::wstring& key) {
    for(auto& s:settings) if(s.key==key) return &s;
    return nullptr;
}
static std::wstring valueOf(const wchar_t* key) {
    const Setting* s=findSetting(key);
    return s?settingLower(s->invalid?s->defaultValue:s->value):L"";
}

static void loadTemplate() {
    Group group=Basic; std::wstring title,text; bool inDescription=false;
    for(const auto& line:splitLines(widen(kSettingsTemplate))) {
        // Template format: "; --- a, b ---" starts the description of the keys that follow; "; BASIC SETTINGS" style banners name the group.
        if(line.rfind(L"; ---",0)==0) {
            title=settingTrim(line.substr(5)); title=title.substr(0,title.find(L" ---"));
            text.clear(); inDescription=true; continue;
        }
        if(line.rfind(L";",0)==0) {
            if(line.rfind(L";  BASIC SETTINGS",0)==0) group=Basic;
            else if(line.rfind(L";  CS2 mode only",0)==0) group=Cs2;
            else if(line.rfind(L";  HLA mode only",0)==0) group=Hla;
            else if(line.rfind(L";  Both modes",0)==0) group=Both;
            else if(line.rfind(L";  Hammer preview",0)==0) group=Preview;
            else if(line.rfind(L";  Reports and tester",0)==0) group=Reports;
            else if(inDescription) { auto d=line.substr(1); if(!d.empty() && d[0]==L' ') d.erase(0,1); text+=d+L"\r\n"; }
            continue;
        }
        const auto trimmed=settingTrim(line); const auto eq=trimmed.find(L'=');
        if(trimmed.empty() || trimmed[0]==L'[' || eq==std::wstring::npos) continue;
        Setting s; s.key=settingLower(settingTrim(trimmed.substr(0,eq))); s.defaultValue=settingTrim(trimmed.substr(eq+1));
        s.title=title; s.description=text; s.group=group; s.name=s.key;
        for(const auto& c:kChoices) if(s.key==c.key) {
            s.name=c.name; s.editable=c.editable; s.text=!*c.choices;
            std::wstring all=c.choices; size_t at=0;
            while(!all.empty() && at!=std::wstring::npos) { const auto bar=all.find(L'|',at); s.choices.push_back(all.substr(at,bar-at)); at=bar==std::wstring::npos?bar:bar+1; }
        }
        if(s.choices.empty()) s.choices.push_back(s.defaultValue);
        s.value=s.saved=s.defaultValue; settings.push_back(s);
        inDescription=false;
    }
}

struct IniLine { bool section; std::wstring key,value; size_t eq,comment; };
static IniLine parseLine(const std::wstring& line,bool& inSection) {
    IniLine out{inSection,{},{},std::wstring::npos,std::wstring::npos};
    out.comment=line.find_first_of(L";#");
    const auto content=settingTrim(line.substr(0,out.comment));
    if(content.size()>=2 && content.front()==L'[' && content.back()==L']') {
        inSection=settingLower(settingTrim(content.substr(1,content.size()-2)))==L"vrad3_gpu";
        out.section=false; out.key=L"["; return out;
    }
    out.eq=line.find(L'=');
    if(!inSection || content.empty() || out.eq==std::wstring::npos || out.eq>out.comment) { out.eq=std::wstring::npos; return out; }
    out.key=settingLower(settingTrim(line.substr(0,out.eq)));
    out.value=settingTrim(line.substr(out.eq+1,out.comment==std::wstring::npos?std::wstring::npos:out.comment-out.eq-1));
    return out;
}
static size_t themeLine(const std::vector<std::wstring>& lines,std::wstring* value) {
    bool inApp=false;
    for(size_t i=0;i<lines.size();++i) {
        const auto content=settingTrim(lines[i].substr(0,lines[i].find_first_of(L";#")));
        if(content.size()>=2 && content.front()==L'[' && content.back()==L']') { inApp=settingLower(settingTrim(content.substr(1,content.size()-2)))==L"vrad3_settings"; continue; }
        const auto eq=content.find(L'=');
        if(!inApp || eq==std::wstring::npos || settingLower(settingTrim(content.substr(0,eq)))!=L"theme") continue;
        if(value) *value=settingLower(settingTrim(content.substr(eq+1)));
        return i;
    }
    return std::wstring::npos;
}
static void loadFile() {
    std::error_code error; iniExists=fs::is_regular_file(iniPath,error);
    for(auto& s:settings) s.value=s.saved=s.defaultValue;
    theme=themeSaved=L"system";
    if(!iniExists) return;
    bool inSection=true;
    const auto lines=splitLines(settingFileText(iniPath));
    for(const auto& line:lines) {
        const auto parsed=parseLine(line,inSection);
        if(parsed.eq==std::wstring::npos) continue;
        if(Setting* s=findSetting(parsed.key)) s->value=s->saved=parsed.value;
    }
    std::wstring value;
    if(themeLine(lines,&value)!=std::wstring::npos && (value==L"light" || value==L"dark")) theme=themeSaved=value;
}

static bool allowed(const Setting& s) {
    if(s.value.find_first_of(L";#")!=std::wstring::npos) return false;
    std::wstring env,translated; std::map<std::wstring,std::wstring> controls;
    return settingParse(s.key,s.value,true,env,translated,controls);
}

static std::wstring ramEstimate() {
    if(valueOf(L"enabled")==L"false") return L"GPU Baking = false: HLA's CPU compiler, which needs RAM like the original.";
    const bool cs2=valueOf(L"estimator")!=L"hla";
    const auto grid=valueOf(L"sample_grid");
    const double g=grid==L"auto"?(cs2?1.0:5.0):std::stod(grid);
    // Rough RAM model for one 8K block: a fixed base per estimator plus a share that grows with N x N sample points; each halving of block size quarters it.
    double gb=(cs2?8.1:18.1)+0.875*g*g;
    const int at=static_cast<int>(SendMessageW(ramCombo,CB_GETCURSEL,0,0));
    gb/=std::pow(4.0,std::max(at,0));
    MEMORYSTATUSEX memory{sizeof(memory)}; GlobalMemoryStatusEx(&memory);
    const double total=memory.ullTotalPhys/1073741824.0;
    wchar_t text[400];
    swprintf_s(text,L"At final quality one block needs about %.*f GB of RAM (a rough guide). This PC has %.0f GB.",gb<10?1:0,gb,total);
    std::wstring out=text;
    if(gb>total*0.75) out+=L" Too much: Windows will swap and the compile will crawl. Try estimator = cs2 with sample_grid = auto, or a smaller lightmap.";
    else if(gb*2<=total*0.6) out+=L" Several blocks can bake at once.";
    return out;
}

static void refresh() {
    bool dirty=theme!=themeSaved;
    for(auto& s:settings) { s.invalid=!allowed(s); dirty=dirty || s.value!=s.saved; }
    const bool on=valueOf(L"enabled")!=L"false",cs2=valueOf(L"estimator")!=L"hla";
    const auto emissive=valueOf(L"emissive"); const bool glow=emissive!=L"off";
    for(auto& s:settings) {
        std::wstring why,hint;
        if(!on && s.key!=L"enabled" && s.group!=Preview) why=L"Inactive: GPU Baking = false runs HLA's CPU compiler only.";
        else if(s.group==Cs2 && !cs2) why=L"Only with Estimator Mode = cs2.";
        else if(s.group==Hla && cs2) why=L"Only with Estimator Mode = hla.";
        else if((s.key==L"emissive_mask" || s.key==L"emissive_fallback") && !glow) why=L"Needs Emissive Lighting on.";
        else if((s.key==L"verify_blocks" || s.key==L"verify_probes") && valueOf(L"verify")!=L"on") why=L"Needs Verify = on.";
        else if(s.key==L"log_folder" && valueOf(L"logging")==L"off" && valueOf(L"verify")!=L"on" && valueOf(L"light_limit_report")!=L"on")
            why=L"Used when Logging, Verify or Light Limit Report is on.";
        else if(s.key==L"preview_capture" && valueOf(L"hammer_preview")==L"vrad2") why=L"Needs Hammer Preview = vrad3.";
        else if(s.key.rfind(L"preview_",0)==0 && s.key.rfind(L"preview_capture",0)!=0) {
            const bool cpuToo=s.key==L"preview_probe_volumes" || s.key==L"preview_quality";
            if(valueOf(L"hammer_preview")==L"vrad2") why=L"Needs Hammer Preview = vrad3.";
            else if(!on && !cpuToo) why=L"Inactive: GPU Baking = false lights the preview with HLA's CPU code.";
            else if(s.key!=L"preview_gpu" && !cpuToo && valueOf(L"preview_gpu")==L"off") why=L"Needs Preview GPU = auto or on.";
        }
        else if(s.key==L"preview_capture_folder" && (valueOf(L"preview_capture")!=L"on" || valueOf(L"hammer_preview")==L"vrad2")) why=L"Needs Preview Capture = on.";
        if(s.key==L"sample_grid" && settingLower(s.value)==L"auto") hint=cs2?L"auto = 1, CS2's one point per luxel.":L"auto = what the compiler asks for (5 at final).";
        if(s.key==L"light_paths" && settingLower(s.value)==L"auto") hint=L"auto = on at final quality, off for previews.";
        if(s.key==L"gpu_usage" && settingLower(s.value)==L"max") hint=L"Fastest; the desktop may stutter.";
        if(s.key==L"estimator") hint=cs2?L"Counter-Strike 2's GPU baker.":L"Half-Life: Alyx's CPU baker.";
        s.why=why;
        EnableWindow(s.combo,why.empty()); if(s.browse) EnableWindow(s.browse,why.empty());
        SetWindowTextW(s.note,s.invalid?L"Not allowed: vrad3 would ignore this line.":!why.empty()?why.c_str():hint.c_str());
        SendMessageW(s.label,WM_SETFONT,reinterpret_cast<WPARAM>(s.value!=s.defaultValue?boldFont:font),TRUE);
    }
    static std::wstring shown[GroupCount];
    for(int g=0;g<GroupCount;++g) {
        std::wstring name=kTabNames[g];
        if(!on && g!=Basic && g!=Preview) name+=L" (inactive)";
        else if((g==Cs2 && !cs2) || (g==Hla && cs2)) name+=L" (inactive)";
        if(name==shown[g]) continue;
        shown[g]=name;
        TCITEMW item{}; item.mask=TCIF_TEXT; item.pszText=name.data();
        SendMessageW(tab,TCM_SETITEMW,g,reinterpret_cast<LPARAM>(&item));
    }
    SetWindowTextW(ramText,ramEstimate().c_str());
    SetWindowTextW(window,dirty?L"vrad3 GPU settings *":L"vrad3 GPU settings");
}
static bool isDirty() { for(const auto& s:settings) if(s.value!=s.saved) return true; return theme!=themeSaved; }

static const Setting* described;
static void showDescription(const Setting& s) {
    described=&s;
    std::wstring text=s.name+L"    (ini: "+s.key+L", default: "+(s.defaultValue.empty()?L"blank":s.defaultValue)+L")\r\n\r\n"+s.description;
    if(!s.why.empty()) text+=L"\r\n"+s.why+L"\r\n";
    SetWindowTextW(description,text.c_str());
}
static void showGroup(int group) {
    for(auto& s:settings) {
        const int show=s.group==group?SW_SHOW:SW_HIDE;
        ShowWindow(s.label,show); ShowWindow(s.combo,show); ShowWindow(s.note,show); if(s.browse) ShowWindow(s.browse,show);
    }
    for(const auto& s:settings) if(s.group==group) { showDescription(s); break; }
}
static void setCombo(Setting& s) {
    if(s.text) { SetWindowTextW(s.combo,s.value.c_str()); return; }
    const auto at=SendMessageW(s.combo,CB_FINDSTRINGEXACT,static_cast<WPARAM>(-1),reinterpret_cast<LPARAM>(s.value.c_str()));
    if(at!=CB_ERR && settingLower(s.choices[static_cast<size_t>(at)])==settingLower(s.value)) SendMessageW(s.combo,CB_SETCURSEL,at,0);
    else if(s.editable) { SendMessageW(s.combo,CB_SETCURSEL,static_cast<WPARAM>(-1),0); SetWindowTextW(s.combo,s.value.c_str()); }
    else {
        const auto added=SendMessageW(s.combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(s.value.c_str()));
        s.choices.push_back(s.value); SendMessageW(s.combo,CB_SETCURSEL,added,0);
    }
}
static std::wstring statusText() {
    std::wstring text=iniExists?L"Editing "+iniPath.filename().wstring():L"No "+iniPath.filename().wstring()+L" yet: every setting is at its default.";
    std::error_code error;
    if(!fs::exists(iniPath.parent_path()/L"vrad3.exe",error)) text=L"vrad3.exe isn't in this folder. Put this app next to the GPU vrad3.exe (Half-Life Alyx\\game\\bin\\win64), where it reads its settings.";
    return text;
}

static const wchar_t* const kHeader=L"; GPU vrad3 settings file generated by vrad3_settings.exe.";
static const wchar_t* const kThemeComment=L"; Theme for vrad3_settings.exe only.";
static bool isDefault(const Setting& s) { return settingLower(s.value)==settingLower(s.defaultValue); }
static std::wstring sectionName(const std::wstring& line) {
    const auto content=settingTrim(line.substr(0,line.find_first_of(L";#")));
    if(content.size()<2 || content.front()!=L'[' || content.back()!=L']') return {};
    return settingLower(settingTrim(content.substr(1,content.size()-2)));
}
static bool buildFile(std::wstring& text) {
    std::vector<std::wstring> lines; std::map<std::wstring,bool> present;
    auto valueLine=[](const Setting& s) { return s.value.empty()?s.key+L" =":s.key+L" = "+s.value; };
    const auto source=iniExists?splitLines(settingFileText(iniPath)):std::vector<std::wstring>{kHeader,L"[vrad3_gpu]"};
    bool inSection=true,anySection=false; size_t last=std::wstring::npos;
    for(const auto& line:source) {
        const bool before=inSection;
        const auto parsed=parseLine(line,inSection);
        if(parsed.key==L"[") { anySection=true; lines.push_back(line); if(inSection) last=lines.size()-1; continue; }
        if(parsed.eq!=std::wstring::npos) if(const Setting* s=findSetting(parsed.key)) {
            if(isDefault(*s)) continue;
            present[s->key]=true;
            lines.push_back(parsed.value==s->value?line:
                line.substr(0,parsed.eq+1)+L" "+s->value+(parsed.comment==std::wstring::npos?L"":L" "+line.substr(parsed.comment)));
            last=lines.size()-1; continue;
        }
        lines.push_back(line);
        if(before && !settingTrim(line).empty() && settingTrim(line)[0]!=L';' && settingTrim(line)[0]!=L'#') last=lines.size()-1;
    }
    std::vector<std::wstring> added;
    for(const auto& s:settings) if(!present[s.key] && !isDefault(s)) added.push_back(valueLine(s));
    if(!added.empty()) {
        if(last==std::wstring::npos && anySection) { lines.push_back(L""); lines.push_back(L"[vrad3_gpu]"); last=lines.size()-1; }
        const size_t at=last==std::wstring::npos?lines.size():last+1;
        lines.insert(lines.begin()+static_cast<std::ptrdiff_t>(at),added.begin(),added.end());
    }
    const size_t themeAt=themeLine(lines,nullptr);
    if(theme!=L"system") {
        if(themeAt!=std::wstring::npos) lines[themeAt]=L"theme = "+theme;
        else {
            if(!lines.empty() && !settingTrim(lines.back()).empty()) lines.push_back(L"");
            lines.push_back(L"[vrad3_settings]"); lines.push_back(kThemeComment); lines.push_back(L"theme = "+theme);
        }
    } else if(themeAt!=std::wstring::npos) {
        lines.erase(lines.begin()+static_cast<std::ptrdiff_t>(themeAt));
        for(size_t i=0;i<lines.size();++i) {
            if(sectionName(lines[i])!=L"vrad3_settings") continue;
            size_t end=i+1; bool other=false;
            for(;end<lines.size() && sectionName(lines[end]).empty();++end) other=other || (!settingTrim(lines[end]).empty() && lines[end]!=kThemeComment);
            if(!other) lines.erase(lines.begin()+static_cast<std::ptrdiff_t>(i),lines.begin()+static_cast<std::ptrdiff_t>(end));
            break;
        }
    }
    while(!lines.empty() && settingTrim(lines.back()).empty()) lines.pop_back();
    bool needed=false;
    for(const auto& line:lines) needed=needed || (!settingTrim(line).empty() && line!=kHeader && sectionName(line)!=L"vrad3_gpu");
    text.clear();
    for(const auto& line:lines) text+=line+L"\r\n";
    return needed;
}
static std::string utf8(const std::wstring& text) {
    const int n=WideCharToMultiByte(CP_UTF8,0,text.c_str(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    std::string out(static_cast<size_t>(n),'\0');
    WideCharToMultiByte(CP_UTF8,0,text.c_str(),static_cast<int>(text.size()),out.data(),n,nullptr,nullptr);
    return out;
}
static DWORD writeBytes(const fs::path& path,const std::string& bytes) {
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) return GetLastError();
    DWORD written=0; const BOOL ok=WriteFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr);
    const DWORD error=ok && written==bytes.size()?0:GetLastError();
    CloseHandle(file);
    return ok && written==bytes.size()?0:(error?error:ERROR_WRITE_FAULT);
}
static bool runElevated(const std::wstring& parameters) {
    wchar_t self[32768]{}; GetModuleFileNameW(nullptr,self,32768);
    SHELLEXECUTEINFOW run{sizeof(run)}; run.fMask=SEE_MASK_NOCLOSEPROCESS; run.hwnd=window; run.lpVerb=L"runas";
    run.lpFile=self; run.lpParameters=parameters.c_str(); run.nShow=SW_HIDE;
    if(!ShellExecuteExW(&run) || !run.hProcess) return false;
    WaitForSingleObject(run.hProcess,INFINITE); DWORD code=1; GetExitCodeProcess(run.hProcess,&code); CloseHandle(run.hProcess);
    return code==0;
}
static bool saveElevated(const std::string& bytes) {
    wchar_t folder[MAX_PATH+1]{},temp[MAX_PATH+1]{};
    if(!GetTempPathW(MAX_PATH+1,folder) || !GetTempFileNameW(folder,L"v3s",0,temp)) return false;
    // Program Files may need administrator rights: write a temp copy, then an elevated "--copy" run moves it into place.
    const bool ok=!writeBytes(temp,bytes) && runElevated(L"--copy \""+std::wstring(temp)+L"\" \""+iniPath.wstring()+L"\"");
    DeleteFileW(temp);
    return ok;
}
static bool save() {
    for(auto& s:settings) if(s.invalid) {
        TabCtrl_SetCurSel(tab,s.group); showGroup(s.group); showDescription(s); SetFocus(s.combo);
        MessageBoxW(window,(s.key+L" = "+s.value+L" isn't allowed. Pick one of the listed values, or see the description.").c_str(),L"vrad3 GPU settings",MB_ICONWARNING);
        return false;
    }
    std::wstring text; const bool needed=buildFile(text);
    const auto bytes=utf8(text);
    DWORD error=0;
    if(needed) error=writeBytes(iniPath,bytes);
    else if(iniExists && !DeleteFileW(iniPath.c_str()) && GetLastError()!=ERROR_FILE_NOT_FOUND) error=GetLastError();
    if(error==ERROR_ACCESS_DENIED) {
        if(MessageBoxW(window,L"Windows needs administrator permission to save in this folder. Continue?",L"vrad3 GPU settings",MB_OKCANCEL|MB_ICONINFORMATION)!=IDOK) return false;
        error=(needed?saveElevated(bytes):runElevated(L"--delete \""+iniPath.wstring()+L"\""))?0:ERROR_ACCESS_DENIED;
    }
    if(error) {
        MessageBoxW(window,(L"Couldn't save "+iniPath.wstring()+L" (Windows error "+std::to_wstring(error)+L").").c_str(),L"vrad3 GPU settings",MB_ICONERROR);
        return false;
    }
    const bool removed=iniExists && !needed;
    for(auto& s:settings) s.saved=s.value;
    themeSaved=theme; iniExists=needed; refresh();
    SetWindowTextW(status,needed?L"Saved. The next compile uses these settings; Hammer doesn't need a restart.":
                   removed?L"Every setting is at its default, so vrad3_gpu.ini isn't needed and was deleted.":
                   L"Every setting is at its default, so there's nothing to save.");
    return true;
}

static int tabRows() {
    int rows[GroupCount]{},most=10;
    for(const auto& s:settings) most=std::max(most,++rows[s.group]);
    return most;
}
static void layout() {
    RECT client; GetClientRect(window,&client);
    const int w=static_cast<int>(client.right/scale),h=static_cast<int>(client.bottom/scale);
    HDWP moves=BeginDeferWindowPos(static_cast<int>(settings.size()*3+12));
    auto place=[&](HWND control,int x,int y,int cx,int cy) { if(moves) moves=DeferWindowPos(moves,control,nullptr,S(x),S(y),S(cx),S(cy),SWP_NOZORDER|SWP_NOACTIVATE); };
    place(pathText,12,12,w-210,20); place(themeLabel,w-190,8,60,24); place(themeCombo,w-126,8,114,200);
    const int tabHeight=44+tabRows()*30;
    place(tab,12,36,w-24,tabHeight);
    for(const auto& s:settings) {
        const int y=72+s.row*30; place(s.label,28,y,160,24); place(s.note,412,y,w-436,24);
        if(s.text) { place(s.combo,190,y,178,24); place(s.browse,372,y,28,24); } else place(s.combo,190,y,210,220);
    }
    place(description,12,44+tabHeight,w-24,h-152-tabHeight);
    place(ramLabel,12,h-94,96,24); place(ramCombo,110,h-94,80,200); place(ramText,200,h-100,w-212,52);
    place(defaults,12,h-40,100,28); place(status,120,h-44,w-344,36); place(saveButton,w-216,h-40,100,28); place(closeButton,w-112,h-40,100,28);
    if(moves) EndDeferWindowPos(moves);
    for(auto& s:settings) if(s.editable && !s.text) SendMessageW(s.combo,CB_SETEDITSEL,0,MAKELPARAM(-1,0));
}

// Windows' app mode setting (0 = dark).
static bool systemDark() {
    DWORD light=1,size=sizeof(light);
    return RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",L"AppsUseLightTheme",
                        RRF_RT_REG_DWORD,nullptr,&light,&size)==ERROR_SUCCESS && light==0;
}
using AllowDarkModeForWindowFn=bool(WINAPI*)(HWND,bool);
using SetPreferredAppModeFn=int(WINAPI*)(int);
using FlushMenuThemesFn=void(WINAPI*)();
static AllowDarkModeForWindowFn allowDarkModeForWindow;
static SetPreferredAppModeFn setPreferredAppMode;
static FlushMenuThemesFn flushMenuThemes;
// uxtheme's unnamed exports by ordinal (133 AllowDarkModeForWindow, 135 SetPreferredAppMode, 136 FlushMenuThemes); Windows 10 1903 (build 18362) and later.
static void loadDarkMode() {
    using RtlGetVersionFn=LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW version{sizeof(version)};
    const auto rtlGetVersion=reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"RtlGetVersion")));
    if(!rtlGetVersion || rtlGetVersion(&version)!=0 || version.dwMajorVersion<10 || version.dwBuildNumber<18362) return;
    HMODULE uxtheme=LoadLibraryExW(L"uxtheme.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!uxtheme) return;
    setPreferredAppMode=reinterpret_cast<SetPreferredAppModeFn>(reinterpret_cast<void*>(GetProcAddress(uxtheme,MAKEINTRESOURCEA(135))));
    allowDarkModeForWindow=reinterpret_cast<AllowDarkModeForWindowFn>(reinterpret_cast<void*>(GetProcAddress(uxtheme,MAKEINTRESOURCEA(133))));
    flushMenuThemes=reinterpret_cast<FlushMenuThemesFn>(reinterpret_cast<void*>(GetProcAddress(uxtheme,MAKEINTRESOURCEA(136))));
}
enum { DrawCombo=1,DrawButton=2 };
static LRESULT CALLBACK darkControlProc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam,UINT_PTR,DWORD_PTR kind) {
    if(dark && kind==DrawButton && (message==WM_ENABLE || message==BM_SETSTATE || message==WM_SETFOCUS || message==WM_KILLFOCUS)) {
        const LRESULT result=DefSubclassProc(hwnd,message,wParam,lParam);
        InvalidateRect(hwnd,nullptr,FALSE); return result;
    }
    if(!dark || message!=WM_PAINT) return DefSubclassProc(hwnd,message,wParam,lParam);
    PAINTSTRUCT paint; HDC dc=BeginPaint(hwnd,&paint);
    RECT client; GetClientRect(hwnd,&client);
    const bool enabled=IsWindowEnabled(hwnd)!=FALSE,focused=GetFocus()==hwnd;
    HGDIOBJ oldFont=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(hwnd,WM_GETFONT,0,0)));
    SetBkMode(dc,TRANSPARENT);
    wchar_t text[256]{};
    if(kind==DrawButton) {
        const auto state=SendMessageW(hwnd,BM_GETSTATE,0,0);
        const COLORREF fill=(state&BST_PUSHED)?RGB(70,70,70):(state&BST_HOT)?RGB(62,62,62):RGB(52,52,52);
        HBRUSH brush=CreateSolidBrush(fill); FillRect(dc,&client,brush); DeleteObject(brush);
        HBRUSH edge=CreateSolidBrush(focused?RGB(0,120,215):kDarkBorder); FrameRect(dc,&client,edge); DeleteObject(edge);
        GetWindowTextW(hwnd,text,256);
        SetTextColor(dc,enabled?kDarkText:kDarkGrey);
        DrawTextW(dc,text,-1,&client,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    } else {
        COMBOBOXINFO info{sizeof(info)}; GetComboBoxInfo(hwnd,&info);
        FillRect(dc,&client,enabled?darkFieldBrush:darkWindowBrush);
        HBRUSH edge=CreateSolidBrush(focused || (info.hwndItem && GetFocus()==info.hwndItem)?RGB(0,120,215):kDarkBorder);
        FrameRect(dc,&client,edge); DeleteObject(edge);
        if(!info.hwndItem || info.hwndItem==hwnd) {
            const auto at=SendMessageW(hwnd,CB_GETCURSEL,0,0);
            if(at!=CB_ERR && SendMessageW(hwnd,CB_GETLBTEXTLEN,at,0)<256) SendMessageW(hwnd,CB_GETLBTEXT,at,reinterpret_cast<LPARAM>(text));
            RECT item=info.rcItem; item.left+=S(3);
            SetTextColor(dc,enabled?kDarkText:kDarkGrey);
            DrawTextW(dc,text,-1,&item,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
        }
        const RECT& button=info.rcButton; const int cx=(button.left+button.right)/2,cy=(button.top+button.bottom)/2,r=S(4);
        HPEN pen=CreatePen(PS_SOLID,std::max(1,S(1)),enabled?kDarkText:kDarkGrey); HGDIOBJ oldPen=SelectObject(dc,pen);
        const POINT chevron[3]{{cx-r,cy-r/2},{cx,cy+r/2},{cx+r,cy-r/2}}; Polyline(dc,chevron,3);
        SelectObject(dc,oldPen); DeleteObject(pen);
    }
    SelectObject(dc,oldFont); EndPaint(hwnd,&paint);
    return 0;
}
static void themeControl(HWND control,const wchar_t* darkName) {
    if(allowDarkModeForWindow) allowDarkModeForWindow(control,dark);
    SetWindowTheme(control,dark?darkName:nullptr,nullptr);
    SendMessageW(control,WM_THEMECHANGED,0,0);
}
static void applyTheme() {
    dark=theme==L"dark" || (theme==L"system" && systemDark());
    const BOOL on=dark; DwmSetWindowAttribute(window,20  ,&on,sizeof(on));
    if(setPreferredAppMode) setPreferredAppMode(dark?2:3);
    if(flushMenuThemes) flushMenuThemes();
    if(allowDarkModeForWindow) allowDarkModeForWindow(window,dark);
    for(const auto& s:settings) { themeControl(s.combo,L"DarkMode_CFD"); if(s.browse) themeControl(s.browse,L"DarkMode_Explorer"); }
    for(HWND h:{ramCombo,themeCombo}) themeControl(h,L"DarkMode_CFD");
    for(HWND h:{description,defaults,saveButton,closeButton}) themeControl(h,L"DarkMode_Explorer");
    RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|RDW_ALLCHILDREN);
}
static LRESULT CALLBACK tabProc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam,UINT_PTR,DWORD_PTR) {
    if(message==WM_ERASEBKGND) return 1;
    if(message!=WM_PAINT) return DefSubclassProc(hwnd,message,wParam,lParam);
    static const HBRUSH lightTabOffBrush=CreateSolidBrush(RGB(240,240,240)),lightBorderBrush=CreateSolidBrush(RGB(204,204,204));
    const HBRUSH page=windowBrush(),off=dark?darkTabOffBrush:lightTabOffBrush,border=dark?darkBorderBrush:lightBorderBrush;
    PAINTSTRUCT paint; HDC dc=BeginPaint(hwnd,&paint);
    RECT client; GetClientRect(hwnd,&client); FillRect(dc,&client,page);
    const int count=TabCtrl_GetItemCount(hwnd),selected=TabCtrl_GetCurSel(hwnd);
    RECT header{}; TabCtrl_GetItemRect(hwnd,0,&header);
    RECT body=client; body.top=header.bottom; FrameRect(dc,&body,border);
    HGDIOBJ oldFont=SelectObject(dc,font); SetBkMode(dc,TRANSPARENT);
    for(int i=0;i<count;++i) {
        RECT item{}; TabCtrl_GetItemRect(hwnd,i,&item);
        if(i==selected) { item.top-=S(2); item.left-=S(2); item.right+=S(2); }
        FillRect(dc,&item,i==selected?page:off);
        RECT edge=item; ++edge.bottom; FrameRect(dc,&edge,border);
        if(i==selected) { RECT gap{item.left+1,item.bottom-1,item.right-1,item.bottom+1}; FillRect(dc,&gap,page); }
        wchar_t text[128]{}; TCITEMW tabItem{}; tabItem.mask=TCIF_TEXT; tabItem.pszText=text; tabItem.cchTextMax=128; TabCtrl_GetItem(hwnd,i,&tabItem);
        SetTextColor(dc,dark?(i==selected?kDarkText:kDarkGrey):GetSysColor(i==selected?COLOR_WINDOWTEXT:COLOR_GRAYTEXT));
        DrawTextW(dc,text,-1,&item,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    }
    SelectObject(dc,oldFont); EndPaint(hwnd,&paint);
    return 0;
}

static LRESULT CALLBACK editProc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam,UINT_PTR,DWORD_PTR) {
    if(message==WM_CONTEXTMENU && hwnd!=description) return SendMessageW(window,WM_CONTEXTMENU,reinterpret_cast<WPARAM>(hwnd),lParam);
    if(!dark || !(GetWindowLongW(hwnd,GWL_EXSTYLE)&WS_EX_CLIENTEDGE)) return DefSubclassProc(hwnd,message,wParam,lParam);
    if(message==WM_NCPAINT) {
        HDC dc=GetWindowDC(hwnd); RECT r; GetWindowRect(hwnd,&r); OffsetRect(&r,-r.left,-r.top);
        HBRUSH edge=CreateSolidBrush(GetFocus()==hwnd?RGB(0,120,215):kDarkBorder); FrameRect(dc,&r,edge); DeleteObject(edge);
        InflateRect(&r,-1,-1); FrameRect(dc,&r,IsWindowEnabled(hwnd)?darkFieldBrush:darkWindowBrush);
        ReleaseDC(hwnd,dc); return 0;
    }
    const LRESULT result=DefSubclassProc(hwnd,message,wParam,lParam);
    if(message==WM_SETFOCUS || message==WM_KILLFOCUS || message==WM_ENABLE) RedrawWindow(hwnd,nullptr,nullptr,RDW_FRAME|RDW_INVALIDATE);
    return result;
}
static Setting* settingAt(HWND control,POINT screen) {
    if(control==window) { ScreenToClient(window,&screen); control=ChildWindowFromPointEx(window,screen,CWP_SKIPINVISIBLE); }
    if(!control || control==window) return nullptr;
    const HWND parent=GetParent(control);
    for(auto& s:settings) if(control==s.label || control==s.combo || control==s.note || (s.browse && control==s.browse) || parent==s.combo) return &s;
    return nullptr;
}
static void hover(HWND control) {
    POINT at; GetCursorPos(&at);
    if(const Setting* s=settingAt(control,at); s && s!=described) showDescription(*s);
}
static void resetSetting(Setting& s) {
    s.value=s.defaultValue; setCombo(s); refresh(); showDescription(s); SetWindowTextW(status,statusText().c_str());
}
static void contextMenu(HWND control,LPARAM lParam) {
    POINT at{static_cast<short>(LOWORD(lParam)),static_cast<short>(HIWORD(lParam))};
    if(lParam==-1) { RECT r; GetWindowRect(control,&r); at={r.left,r.bottom}; }
    Setting* s=settingAt(control,at); if(!s) return;
    showDescription(*s);
    COMBOBOXINFO info{sizeof(info)};
    const HWND edit=s->text?s->combo:s->editable && GetComboBoxInfo(s->combo,&info)?info.hwndItem:nullptr;
    const bool enabled=IsWindowEnabled(s->combo)!=FALSE;
    HMENU menu=CreatePopupMenu();
    const std::wstring reset=L"Reset to default ("+(s->defaultValue.empty()?std::wstring(L"blank"):s->defaultValue)+L")";
    AppendMenuW(menu,MF_STRING|(s->value==s->defaultValue?MF_GRAYED:0),1,reset.c_str());
    if(edit) {
        const auto selection=SendMessageW(edit,EM_GETSEL,0,0); const bool selected=LOWORD(selection)!=HIWORD(selection);
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        AppendMenuW(menu,MF_STRING|(enabled && selected?0:MF_GRAYED),2,L"Cut");
        AppendMenuW(menu,MF_STRING|(selected?0:MF_GRAYED),3,L"Copy");
        AppendMenuW(menu,MF_STRING|(enabled && IsClipboardFormatAvailable(CF_UNICODETEXT)?0:MF_GRAYED),4,L"Paste");
        AppendMenuW(menu,MF_STRING|(enabled?0:MF_GRAYED),5,L"Select all");
    }
    const int chosen=static_cast<int>(TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,at.x,at.y,0,window,nullptr));
    DestroyMenu(menu);
    if(chosen==1) resetSetting(*s);
    else if(chosen>=2) { SetFocus(edit); SendMessageW(edit,chosen==2?WM_CUT:chosen==3?WM_COPY:chosen==4?WM_PASTE:EM_SETSEL,0,chosen==5?-1:0); }
}
static void pickFolder(Setting& s) {
    IFileOpenDialog* dialog=nullptr;
    if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog)))) return;
    DWORD options=0; dialog->GetOptions(&options); dialog->SetOptions(options|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM);
    dialog->SetTitle((L"Choose the folder for "+s.name).c_str());
    wchar_t start[32768]{};
    ExpandEnvironmentStringsW(!s.value.empty()?s.value.c_str():s.key==L"log_folder"?L"%LOCALAPPDATA%\\vrad3-gpu\\logs":L"%LOCALAPPDATA%\\vrad3-gpu\\captures",start,32768);
    std::error_code error; IShellItem* item=nullptr;
    if(fs::is_directory(start,error) && SUCCEEDED(SHCreateItemFromParsingName(start,nullptr,IID_PPV_ARGS(&item)))) { dialog->SetFolder(item); item->Release(); item=nullptr; }
    if(SUCCEEDED(dialog->Show(window)) && SUCCEEDED(dialog->GetResult(&item))) {
        PWSTR path=nullptr;
        if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))) {
            s.value=path; CoTaskMemFree(path);
            setCombo(s); refresh(); showDescription(s); SetWindowTextW(status,statusText().c_str());
        }
        item->Release();
    }
    dialog->Release();
}

static void createControls() {
    auto make=[](const wchar_t* type,const wchar_t* text,DWORD style,int id) {
        HWND h=CreateWindowW(type,text,WS_CHILD|style,0,0,0,0,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),nullptr,nullptr);
        SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(font),FALSE);
        return h;
    };
    pathText=make(L"STATIC",(L"Settings file: "+iniPath.wstring()).c_str(),WS_VISIBLE|SS_PATHELLIPSIS,IdPath);
    themeLabel=make(L"STATIC",L"Theme:",WS_VISIBLE|SS_CENTERIMAGE|SS_RIGHT,0);
    themeCombo=make(WC_COMBOBOXW,L"",WS_VISIBLE|WS_TABSTOP|CBS_DROPDOWNLIST,IdTheme);
    for(const wchar_t* name:{L"System",L"Light",L"Dark"}) SendMessageW(themeCombo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name));
    for(int i=0;i<3;++i) if(theme==kThemes[i]) SendMessageW(themeCombo,CB_SETCURSEL,i,0);
    tab=make(WC_TABCONTROLW,L"",WS_VISIBLE|WS_CLIPSIBLINGS,IdTab);
    for(int g=0;g<GroupCount;++g) { TCITEMW item{}; item.mask=TCIF_TEXT; item.pszText=const_cast<wchar_t*>(kTabNames[g]); SendMessageW(tab,TCM_INSERTITEMW,g,reinterpret_cast<LPARAM>(&item)); }
    SetWindowSubclass(tab,tabProc,1,0);
    int row[GroupCount]{};
    for(size_t i=0;i<settings.size();++i) {
        auto& s=settings[i]; s.row=row[s.group]++;
        s.label=make(L"STATIC",s.name.c_str(),SS_NOTIFY|SS_CENTERIMAGE,IdLabel+static_cast<int>(i));
        if(s.text) {
            s.combo=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_TABSTOP|ES_AUTOHSCROLL,0,0,0,0,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdCombo+static_cast<int>(i))),nullptr,nullptr);
            SendMessageW(s.combo,WM_SETFONT,reinterpret_cast<WPARAM>(font),FALSE);
            s.browse=make(L"BUTTON",L"...",WS_TABSTOP,IdBrowse+static_cast<int>(i));
        } else s.combo=make(WC_COMBOBOXW,L"",WS_TABSTOP|WS_VSCROLL|(s.editable?CBS_DROPDOWN|CBS_AUTOHSCROLL:CBS_DROPDOWNLIST),IdCombo+static_cast<int>(i));
        s.note=make(L"STATIC",L"",SS_CENTERIMAGE|SS_ENDELLIPSIS,IdNote+static_cast<int>(i));
        for(const auto& c:s.choices) SendMessageW(s.combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(c.c_str()));
        setCombo(s);
    }
    description=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,0,0,0,0,window,reinterpret_cast<HMENU>(IdDescription),nullptr,nullptr);
    SendMessageW(description,WM_SETFONT,reinterpret_cast<WPARAM>(monoFont),FALSE);
    ramLabel=make(L"STATIC",L"Lightmap size:",WS_VISIBLE|SS_CENTERIMAGE,0);
    ramCombo=make(WC_COMBOBOXW,L"",WS_VISIBLE|WS_TABSTOP|CBS_DROPDOWNLIST,IdRamCombo);
    for(const wchar_t* size:{L"8192",L"4096",L"2048",L"1024"}) SendMessageW(ramCombo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(size));
    SendMessageW(ramCombo,CB_SETCURSEL,0,0);
    ramText=make(L"STATIC",L"",WS_VISIBLE,IdRamText);
    defaults=make(L"BUTTON",L"All defaults",WS_VISIBLE|WS_TABSTOP,IdDefaults);
    status=make(L"STATIC",statusText().c_str(),WS_VISIBLE,IdStatus);
    saveButton=make(L"BUTTON",L"Save",WS_VISIBLE|WS_TABSTOP,IdSave);
    closeButton=make(L"BUTTON",L"Close",WS_VISIBLE|WS_TABSTOP,IdClose);
    for(const auto& s:settings) {
        if(s.text) { SetWindowSubclass(s.combo,editProc,3,0); SetWindowSubclass(s.browse,darkControlProc,2,DrawButton); continue; }
        SetWindowSubclass(s.combo,darkControlProc,2,DrawCombo);
        COMBOBOXINFO info{sizeof(info)}; if(GetComboBoxInfo(s.combo,&info) && info.hwndItem) SetWindowSubclass(info.hwndItem,editProc,3,0);
    }
    for(HWND h:{ramCombo,themeCombo}) SetWindowSubclass(h,darkControlProc,2,DrawCombo);
    for(HWND h:{defaults,saveButton,closeButton}) SetWindowSubclass(h,darkControlProc,2,DrawButton);
    SetWindowSubclass(description,editProc,3,0);
    SetWindowPos(tab,HWND_BOTTOM,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    layout(); showGroup(Basic); refresh();
}
static std::wstring comboValue(HWND combo,bool selection) {
    if(selection) {
        const auto at=SendMessageW(combo,CB_GETCURSEL,0,0);
        if(at!=CB_ERR) {
            std::wstring text(static_cast<size_t>(SendMessageW(combo,CB_GETLBTEXTLEN,at,0)),L'\0');
            SendMessageW(combo,CB_GETLBTEXT,at,reinterpret_cast<LPARAM>(text.data()));
            return settingTrim(text.c_str());
        }
    }
    std::wstring text(static_cast<size_t>(GetWindowTextLengthW(combo))+1,L'\0');
    GetWindowTextW(combo,text.data(),static_cast<int>(text.size()));
    return settingTrim(text.c_str());
}
static LRESULT CALLBACK windowProc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam) {
    switch(message) {
    case WM_COMMAND: {
        const int id=LOWORD(wParam),code=HIWORD(wParam);
        if(id>=IdCombo && id<IdCombo+static_cast<int>(settings.size())) {
            auto& s=settings[static_cast<size_t>(id-IdCombo)];
            if(s.text) {
                if(code==EN_CHANGE) {
                    const auto typed=comboValue(s.combo,false);
                    if(typed!=s.value) { s.value=typed; refresh(); showDescription(s); SetWindowTextW(status,statusText().c_str()); }
                } else if(code==EN_SETFOCUS) showDescription(s);
            }
            else if(code==CBN_SELCHANGE || code==CBN_EDITCHANGE) {
                s.value=comboValue(s.combo,code==CBN_SELCHANGE); refresh(); showDescription(s); SetWindowTextW(status,statusText().c_str());
            }
            else if(code==CBN_SETFOCUS) showDescription(s);
        } else if(id>=IdLabel && id<IdLabel+static_cast<int>(settings.size()) && code==STN_CLICKED) {
            auto& s=settings[static_cast<size_t>(id-IdLabel)]; showDescription(s); if(IsWindowEnabled(s.combo)) SetFocus(s.combo);
        } else if(id>=IdBrowse && id<IdBrowse+static_cast<int>(settings.size())) pickFolder(settings[static_cast<size_t>(id-IdBrowse)]);
        else if(id==IdRamCombo && code==CBN_SELCHANGE) refresh();
        else if(id==IdTheme && code==CBN_SELCHANGE) {
            theme=kThemes[std::clamp(static_cast<int>(SendMessageW(themeCombo,CB_GETCURSEL,0,0)),0,2)]; applyTheme(); refresh();
        }
        else if(id==IdDefaults) {
            for(auto& s:settings) { s.value=s.defaultValue; setCombo(s); }
            refresh(); SetWindowTextW(status,L"Every setting is back at its default. Save to keep that.");
        } else if(id==IdSave) save();
        else if(id==IdClose) SendMessageW(hwnd,WM_CLOSE,0,0);
        return 0;
    }
    case WM_CONTEXTMENU: contextMenu(reinterpret_cast<HWND>(wParam),lParam); return 0;
    case WM_NOTIFY:
        if(reinterpret_cast<NMHDR*>(lParam)->hwndFrom==tab && reinterpret_cast<NMHDR*>(lParam)->code==TCN_SELCHANGE) showGroup(TabCtrl_GetCurSel(tab));
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc=reinterpret_cast<HDC>(wParam); HWND control=reinterpret_cast<HWND>(lParam);
        SetTextColor(dc,dark?kDarkText:GetSysColor(COLOR_WINDOWTEXT));
        if(dark && GetParent(control)!=hwnd) {
            SetTextColor(dc,kDarkGrey); SetBkColor(dc,kDarkWindow); return reinterpret_cast<LRESULT>(darkWindowBrush);
        }
        if(const Setting* s=settingAt(control,POINT{}); s && s->text && control==s->combo) {
            if(!dark) break;
            SetTextColor(dc,kDarkGrey); SetBkColor(dc,kDarkWindow); return reinterpret_cast<LRESULT>(darkWindowBrush);
        }
        if(control==description) {
            SetBkColor(dc,dark?kDarkField:GetSysColor(COLOR_WINDOW));
            return reinterpret_cast<LRESULT>(dark?darkFieldBrush:GetSysColorBrush(COLOR_WINDOW));
        }
        SetBkMode(dc,TRANSPARENT);
        const int id=GetDlgCtrlID(control);
        if(id>=IdNote && id<IdNote+static_cast<int>(settings.size()))
            SetTextColor(dc,settings[static_cast<size_t>(id-IdNote)].invalid?(dark?kDarkRed:RGB(200,0,0)):(dark?kDarkGrey:GetSysColor(COLOR_GRAYTEXT)));
        return reinterpret_cast<LRESULT>(windowBrush());
    }
    case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
        if(!dark) break;
        SetTextColor(reinterpret_cast<HDC>(wParam),kDarkText); SetBkColor(reinterpret_cast<HDC>(wParam),kDarkField);
        return reinterpret_cast<LRESULT>(darkFieldBrush);
    case WM_CTLCOLORBTN: return reinterpret_cast<LRESULT>(windowBrush());
    case WM_ERASEBKGND: { RECT client; GetClientRect(hwnd,&client); FillRect(reinterpret_cast<HDC>(wParam),&client,windowBrush()); return 1; }
    case WM_SIZE: if(tab) layout(); return 0;
    case WM_GETMINMAXINFO: reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize={minimum.cx,minimum.cy}; return 0;
    case WM_SETTINGCHANGE:
        if(lParam && lstrcmpW(reinterpret_cast<const wchar_t*>(lParam),L"ImmersiveColorSet")==0 && theme==L"system" && tab) applyTheme();
        break;
    case WM_CLOSE:
        if(isDirty()) {
            const int answer=MessageBoxW(hwnd,L"Save your changes?",L"vrad3 GPU settings",MB_YESNOCANCEL|MB_ICONQUESTION);
            if(answer==IDCANCEL || (answer==IDYES && !save())) return 0;
        }
        DestroyWindow(hwnd); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd,message,wParam,lParam);
}
static void addAdapters() {
    Setting* gpu=findSetting(L"gpu"); IDXGIFactory1* factory=nullptr;
    if(!gpu || FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),reinterpret_cast<void**>(&factory)))) return;
    IDXGIAdapter1* adapter=nullptr;
    for(UINT i=0;factory->EnumAdapters1(i,&adapter)!=DXGI_ERROR_NOT_FOUND;++i) {
        DXGI_ADAPTER_DESC1 desc{}; adapter->GetDesc1(&desc); adapter->Release();
        const std::wstring name=desc.Description;
        if(!(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) && std::find(gpu->choices.begin(),gpu->choices.end(),name)==gpu->choices.end()) gpu->choices.push_back(name);
    }
    factory->Release();
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show) {
    int argc=0; wchar_t** argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(argv && argc==4 && std::wstring(argv[1])==L"--copy") return CopyFileW(argv[2],argv[3],FALSE)?0:1;
    if(argv && argc==3 && std::wstring(argv[1])==L"--delete") return DeleteFileW(argv[2]) || GetLastError()==ERROR_FILE_NOT_FOUND?0:1;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE);
    const UINT dpi=GetDpiForSystem(); scale=dpi/96.0;
    INITCOMMONCONTROLSEX common{sizeof(common),ICC_TAB_CLASSES|ICC_STANDARD_CLASSES}; InitCommonControlsEx(&common);
    wchar_t module[32768]{}; GetModuleFileNameW(nullptr,module,32768);
    iniPath=fs::path(module).parent_path()/L"vrad3_gpu.ini";
    loadTemplate(); addAdapters(); loadFile();
    NONCLIENTMETRICSW metrics{sizeof(metrics)}; SystemParametersInfoW(SPI_GETNONCLIENTMETRICS,sizeof(metrics),&metrics,0);
    font=CreateFontIndirectW(&metrics.lfMessageFont);
    LOGFONTW bold=metrics.lfMessageFont; bold.lfWeight=FW_BOLD; boldFont=CreateFontIndirectW(&bold);
    monoFont=CreateFontW(-MulDiv(9,static_cast<int>(dpi),72),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,FIXED_PITCH,L"Consolas");
    WNDCLASSW type{}; type.lpfnWndProc=windowProc; type.hInstance=instance; type.lpszClassName=L"vrad3_settings";
    type.hCursor=LoadCursorW(nullptr,IDC_ARROW); type.hbrBackground=GetSysColorBrush(COLOR_WINDOW);
    type.hIcon=LoadIconW(nullptr,IDI_APPLICATION);
    RegisterClassW(&type);
    loadDarkMode();
    darkWindowBrush=CreateSolidBrush(kDarkWindow); darkFieldBrush=CreateSolidBrush(kDarkField);
    darkTabOffBrush=CreateSolidBrush(kDarkTabOff); darkBorderBrush=CreateSolidBrush(kDarkBorder);
    const DWORD style=WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN;
    RECT frame{0,0,S(780),S(356+tabRows()*30)}; AdjustWindowRect(&frame,style,FALSE);
    minimum={frame.right-frame.left,frame.bottom-frame.top};
    window=CreateWindowW(L"vrad3_settings",L"vrad3 GPU settings",style,CW_USEDEFAULT,CW_USEDEFAULT,minimum.cx,minimum.cy,nullptr,nullptr,instance,nullptr);
    createControls(); applyTheme();
    ShowWindow(window,show);
    RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|RDW_ALLCHILDREN);
    MSG message;
    while(GetMessageW(&message,nullptr,0,0)>0) {
        if(message.message==WM_MOUSEMOVE) hover(message.hwnd);
        if(IsDialogMessageW(window,&message)) continue;
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    return 0;
}
