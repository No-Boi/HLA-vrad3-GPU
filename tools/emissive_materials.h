// Opt-in emission for self-illuminated vr_complex materials: reads each .vmat_c and fills the emission field of Valve's material table.
// Valve's own vrad3 gives every material zero emission; emission only enters through bounced light.
#pragma once
#include <algorithm>
#include <functional>
#include <unordered_map>
#include "resource_files.h"

static bool emissiveApplied=false;
static std::string hammerEmissive;
static std::string hammerRetry;
static bool hammerAbortCpuStage=false;

// Decodes a vtex_c mip of at most maxSide texels per side to RGBA8 (DXT1, DXT5, RGBA8888; LZ4 mips too); false if unsupported.
static bool emissiveDecodeMask(const std::vector<uint8_t>& file,uint32_t maxSide,GpuEmissionMask& out) {
    const auto block=emissiveDataBlock(file);
    if(block.second<40) return false;
    const uint8_t* header=block.first;
    uint16_t width,height,depth; memcpy(&width,header+20,2); memcpy(&height,header+22,2); memcpy(&depth,header+24,2);
    const uint8_t format=header[26],mips=header[27];
    uint32_t extraOffset,extraCount; memcpy(&extraOffset,header+32,4); memcpy(&extraCount,header+36,4);
    if(!width || !height || depth>1 || !mips || mips>16 || (format!=1 && format!=2 && format!=4)) return false;
    const size_t blockBytes=format==1 ? 8 : 16,dataStart=static_cast<size_t>(block.first-file.data())+block.second;
    auto rawSize=[&](uint32_t level) -> size_t {
        const size_t w=std::max(1u,uint32_t(width)>>level),h=std::max(1u,uint32_t(height)>>level);
        return format==4 ? w*h*4 : ((w+3)/4)*((h+3)/4)*blockBytes;
    };
    std::vector<size_t> stored(mips); bool compressed=false;
    for(uint32_t m=0;m<mips;++m) stored[m]=rawSize(m);
    const size_t extraBase=static_cast<size_t>(header-file.data())+32+extraOffset;
    for(uint32_t i=0;i<extraCount && i<16;++i) {
        const size_t at=extraBase+size_t(i)*12;
        if(at+12>file.size()) return false;
        uint32_t type,offset,size; memcpy(&type,file.data()+at,4); memcpy(&offset,file.data()+at+4,4); memcpy(&size,file.data()+at+8,4);
        // Extra data type 4 holds compressed mip sizes: {1 = LZ4, 8, count}, then the sizes.
        if(type!=4) continue;
        const size_t where=at+4+size_t(offset);
        if(where+12>file.size()) return false;
        uint32_t lz4,count; memcpy(&lz4,file.data()+where,4); memcpy(&count,file.data()+where+8,4);
        if(lz4!=1) continue;
        if(count!=mips || where+12+size_t(count)*4>file.size()) return false;
        for(uint32_t m=0;m<mips;++m) { uint32_t size32; memcpy(&size32,file.data()+where+12+m*4,4); stored[m]=size32; }
        compressed=true;
    }
    size_t total=0; for(size_t size:stored) total+=size;
    if(dataStart>file.size() || total!=file.size()-dataStart) return false;
    uint32_t level=0;
    while(level+1<mips && std::max(uint32_t(width)>>level,uint32_t(height)>>level)>maxSide) ++level;
    // Mips are stored smallest first.
    size_t at=dataStart; for(uint32_t m=level+1;m<mips;++m) at+=stored[m];
    std::vector<uint8_t> pixels;
    if(compressed && stored[level]!=rawSize(level)) pixels=emissiveLz4(file.data()+at,stored[level],rawSize(level));
    else pixels.assign(file.data()+at,file.data()+at+stored[level]);
    if(pixels.size()!=rawSize(level)) return false;
    const uint32_t w=std::max(1u,uint32_t(width)>>level),h=std::max(1u,uint32_t(height)>>level);
    out.width=w; out.height=h; out.rgba.assign(size_t(w)*h*4,255);
    if(format==4) { out.rgba=pixels; return true; }
    for(uint32_t by=0;by<(h+3)/4;++by) for(uint32_t bx=0;bx<(w+3)/4;++bx) {
        const uint8_t* b=pixels.data()+(size_t(by)*((w+3)/4)+bx)*blockBytes+(format==2?8:0);
        uint16_t c0,c1; uint32_t bits; memcpy(&c0,b,2); memcpy(&c1,b+2,2); memcpy(&bits,b+4,4);
        auto expand=[](uint16_t c,int channel) { const uint32_t v=channel==0?c>>11:channel==1?(c>>5)&63:c&31;
            return channel==1 ? (v<<2)|(v>>4) : (v<<3)|(v>>2); };
        uint8_t palette[4][3];
        for(int c=0;c<3;++c) {
            const uint32_t a=expand(c0,c),z=expand(c1,c);
            palette[0][c]=uint8_t(a); palette[1][c]=uint8_t(z);
            if(c0>c1 || format==2) { palette[2][c]=uint8_t((2*a+z)/3); palette[3][c]=uint8_t((a+2*z)/3); }
            else { palette[2][c]=uint8_t((a+z)/2); palette[3][c]=0; }
        }
        for(uint32_t i=0;i<16;++i) {
            const uint32_t x=bx*4+i%4,y=by*4+i/4;
            if(x>=w || y>=h) continue;
            memcpy(out.rgba.data()+(size_t(y)*w+x)*4,palette[(bits>>(2*i))&3],3);
        }
    }
    return true;
}

static float emissiveSetting() {
    const auto value=environmentValue(L"VRAD3_GPU_EMISSIVE");
    if(value.empty() || value==L"0" || value==L"off") return 0;
    if(value==L"1" || value==L"on") return 1;
    try { const float scale=std::stof(value); return std::isfinite(scale) && scale>0 && scale<=1000?scale:0; } catch(...) { return 0; }
}

static bool emissiveStopsCpu() { return emissiveSetting()>0 && environmentValue(L"VRAD3_GPU_EMISSIVE_FALLBACK")==L"stop"; }

static void applyEmissiveMaterials(const fs::path& executable,const fs::path& workdir) {
    if(emissiveApplied) return;
    emissiveApplied=true;
    const float userScale=emissiveSetting();
    if(!userScale) return;
    try {
        // Material table in Valve's process: entries of 140 bytes, key is the resource id at +4, albedo at +12, emission at +36.
        uintptr_t materialObject=pointerAt(0x32aa20)+0x128;
        uintptr_t table=readValue<uintptr_t>(materialObject);
        const uint32_t capacity=readValue<uint32_t>(materialObject+8);
        if(!table || capacity>65536) throw std::runtime_error("material table");
        std::unordered_map<uint64_t,uintptr_t> wanted;
        for(uint32_t i=0;i<capacity;++i) {
            const uintptr_t entry=table+static_cast<uintptr_t>(i)*140;
            if(readValue<uint32_t>(entry)&0x80000000u) continue;
            wanted[readValue<uint64_t>(entry+4)]=entry;
        }
        EmissiveFiles files;
        for(fs::path folder=fs::absolute(workdir);folder.has_parent_path() && folder!=folder.parent_path();folder=folder.parent_path())
            if(_wcsicmp(folder.parent_path().filename().c_str(),L"hlvr_addons")==0) { files.addon=folder; break; }
        if(files.addon.empty()) {
            const fs::path shared=environmentValue(L"VRAD3_GPU_PREVIEW_ADDON"); std::error_code error;
            if(!shared.empty() && fs::is_directory(shared,error)) files.addon=shared;
        }
        const fs::path game=fs::absolute(executable).parent_path().parent_path().parent_path();
        std::vector<fs::path> pakPaths;
        if(!files.addon.empty()) pakPaths.push_back(files.addon/L"pak01_dir.vpk");
        pakPaths.push_back(game/L"hlvr"/L"pak01_dir.vpk"); pakPaths.push_back(game/L"core"/L"pak01_dir.vpk");
        for(const auto& path:pakPaths) { EmissivePak pak; std::error_code error; if(fs::is_regular_file(path,error) && pak.open(path)) files.paks.push_back(std::move(pak)); }
        std::unordered_map<uint64_t,std::string> names;
        auto consider=[&](std::string name) {
            if(name.size()<7 || name.compare(name.size()-7,7,".vmat_c")) return;
            // Table key: resource id of the lower-case .vmat path (without the _c).
            const uint64_t id=emissiveResourceId(name.substr(0,name.size()-2));
            if(wanted.count(id)) names.emplace(id,std::move(name));
        };
        if(!files.addon.empty()) {
            std::error_code error;
            for(fs::recursive_directory_iterator it(files.addon,fs::directory_options::skip_permission_denied,error),end;!error && it!=end;it.increment(error)) {
                if(it->path().extension()!=L".vmat_c" || !it->is_regular_file(error)) continue;
                std::string name=fs::relative(it->path(),files.addon,error).generic_u8string();
                for(char& c:name) c=static_cast<char>(tolower(static_cast<unsigned char>(c)));
                consider(name);
            }
        }
        for(const auto& pak:files.paks) for(const auto& entry:pak.entries) if(names.size()<wanted.size()) consider(entry.first);
        uint32_t glowing=0,unreadable=0,perTexelCount=0; std::string first; size_t maskBytes=0;
        const bool perTexel=environmentValue(L"VRAD3_GPU_ESTIMATOR")==L"cs2" && environmentValue(L"VRAD3_GPU_EMISSIVE_MASK")!=L"average";
        for(const auto& [id,name]:names) {
            try {
                std::vector<uint8_t> file;
                if(!files.read(name,file)) { ++unreadable; continue; }
                const auto block=emissiveDataBlock(file);
                if(block.second<4 || memcmp(block.first,"\x01" "3VK",4)) { if(std::string(reinterpret_cast<const char*>(file.data()),file.size()).find("F_SELF_ILLUM")!=std::string::npos) ++unreadable; continue; }
                const EmissiveKv material=emissiveKv3(block.first,block.second);
                const EmissiveKv* shader=material.get("m_shaderName");
                if(!shader || _stricmp(shader->text.c_str(),"vr_complex.vfx")) continue;
                auto parameter=[&](const char* list,const char* field,const char* name) -> const EmissiveKv* {
                    if(const EmissiveKv* params=material.get(list))
                        for(const auto& item:params->items)
                            if(const EmissiveKv* n=item.get("m_name"); n && n->text==name) return item.get(field);
                    return nullptr;
                };
                // Emission per channel: mask average * tint * lerp(1, albedo, albedoFactor) * scale, with tint = 2^g_flSelfIllumBrightness * g_flSelfIllumScale * sRGB-to-linear(g_vSelfIllumTint), as vr_complex computes it.
                const EmissiveKv* selfIllum=parameter("m_intParams","m_nValue","F_SELF_ILLUM");
                if(!selfIllum || !selfIllum->number) continue;
                auto attribute=[&](const char* name,float fallback) {
                    if(const EmissiveKv* list=material.get("m_floatAttributes"))
                        for(const auto& item:list->items)
                            if(const EmissiveKv* n=item.get("m_name"); n && !_stricmp(n->text.c_str(),name))
                                if(const EmissiveKv* v=item.get("m_flValue")) return float(v->number);
                    return fallback;
                };
                if(!attribute("emissiveLightingEnabled",1)) continue;
                auto number=[&](const char* name,float fallback) { const EmissiveKv* v=parameter("m_floatParams","m_flValue",name); return v?float(v->number):fallback; };
                const float brightness=number("g_flSelfIllumBrightness",0),scale=number("g_flSelfIllumScale",1),albedoFactor=number("g_flSelfIllumAlbedoFactor",1);
                const float boost=attribute("emissiveLightingBoost",1);
                float tint[3]={1,1,1};
                if(const EmissiveKv* v=parameter("m_vectorParams","m_value","g_vSelfIllumTint"); v && v->items.size()>=3)
                    for(int c=0;c<3;++c) { const float g=float(v->items[c].number); tint[c]=g<=0.04045f?g/12.92f:std::pow((g+0.055f)/1.055f,2.4f); }
                float mask[3]={1,1,1}; GpuEmissionMask texels;
                if(const EmissiveKv* v=parameter("m_textureParams","m_pValue","g_tSelfIllumMask"); v && !v->text.empty()) {
                    std::string texture=v->text+"_c";
                    for(char& c:texture) c=static_cast<char>(tolower(static_cast<unsigned char>(c)));
                    std::vector<uint8_t> vtex;
                    if(!files.read(texture,vtex)) { ++unreadable; continue; }
                    const auto data=emissiveDataBlock(vtex);
                    if(data.second<16) { ++unreadable; continue; }
                    // vtex header: version, flags, reflectivity (mean colour of the mask).
                    memcpy(mask,data.first+4,12);
                    auto vector=[&](const char* name,std::array<float,2> fallback) {
                        if(const EmissiveKv* p=parameter("m_vectorParams","m_value",name); p && p->items.size()>=2)
                            return std::array<float,2>{float(p->items[0].number),float(p->items[1].number)};
                        return fallback; };
                    if(perTexel && vector("g_vTexCoordScale",{1,1})==std::array<float,2>{1,1} && vector("g_vTexCoordOffset",{0,0})==std::array<float,2>{0,0}) {
                        try { if(!emissiveDecodeMask(vtex,512,texels)) texels={}; } catch(const std::runtime_error&) { texels={}; }
                    }
                }
                const uintptr_t entry=wanted[id];
                float albedo[3],emission[3]; readMemory(entry+12,albedo,sizeof(albedo)); readMemory(entry+12+24,emission,sizeof(emission));
                // Keep any emission Valve already set.
                if(emission[0] || emission[1] || emission[2]) continue;
                const float strength=std::exp2(brightness)*scale*boost*userScale;
                bool any=false;
                std::array<float,4> glow{0,0,0,albedoFactor};
                for(int c=0;c<3;++c) {
                    glow[c]=std::max(0.0f,mask[c]*strength*tint[c]);
                    emission[c]=std::max(0.0f,glow[c]*(1+(albedo[c]-1)*albedoFactor));
                    if(!std::isfinite(emission[c]) || emission[c]>1e6f || !std::isfinite(glow[c]) || glow[c]>1e6f) emission[c]=glow[c]=0;
                    any=any || emission[c]>0;
                }
                if(!any) continue;
                SIZE_T written=0;
                if(!WriteProcessMemory(processHandle,reinterpret_cast<void*>(entry+12+24),emission,sizeof(emission),&written) || written!=sizeof(emission))
                    throw std::runtime_error("write material emission");
                // GPU gets the glow before the albedo factor, and applies the factor per hit using the hit's albedo; Valve's CPU table gets the averaged-albedo value.
                if(std::isfinite(albedoFactor)) {
                    if(!texels.rgba.empty() && maskBytes+texels.rgba.size()<=128ull*1024*1024) {
                        std::array<float,4> unmasked=glow; bool finite=true;
                        for(int c=0;c<3;++c) { unmasked[c]=std::max(0.0f,strength*tint[c]); finite=finite && std::isfinite(unmasked[c]) && unmasked[c]<=1e6f; }
                        if(finite) {
                            texels.average={glow[0],glow[1],glow[2]};
                            glow=unmasked; maskBytes+=texels.rgba.size(); ++perTexelCount; gpuEmissionMasks[id]=std::move(texels);
                        }
                    }
                    gpuEmission[id]=glow;
                }
                if(!glowing++) first=name.substr(0,name.size()-2);
            } catch(const std::runtime_error& error) {
                if(std::string(error.what())=="write material emission") throw;
                ++unreadable;
            }
        }
        std::ostringstream line;
        line<<glowing<<' '<<unreadable<<' '<<wanted.size()-names.size()<<' '<<(first.empty()?"-":first);
        hammerEmissive=line.str();
        std::cout<<"VRAD3 GPU emissive: "<<glowing<<" glowing material(s)"<<(first.empty()?"":" (first "+first+")")<<", "<<unreadable
                 <<" unreadable, "<<wanted.size()-names.size()<<" of "<<wanted.size()<<" not found"
                 <<(perTexel?", "+std::to_string(perTexelCount)+" with per-texel masks":"")<<"\n"<<std::flush;
    } catch(const std::exception& error) {
        hammerEmissive="0 0 0 -";
        std::cout<<"VRAD3 GPU emissive unavailable: "<<error.what()<<'\n'<<std::flush;
    }
}
