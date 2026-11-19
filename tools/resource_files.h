// Read-only access to compiled resources: resource ids, LZ4, binary KV3, resource blocks and VPK paks.
// Damaged input throws std::runtime_error or returns false.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
namespace fs=std::filesystem;

// Resource id of a path: MurmurHash64B with seed 0xEDABCDEF.
static uint64_t emissiveResourceId(const std::string& path) {
    const uint32_t m=0x5bd1e995; const uint64_t seed=0xEDABCDEF;
    const size_t total=path.size();
    uint32_t h1=static_cast<uint32_t>(seed)^static_cast<uint32_t>(total),h2=static_cast<uint32_t>(seed>>32);
    const auto* data=reinterpret_cast<const uint8_t*>(path.data()); size_t length=total;
    auto mix=[&](uint32_t& h,const uint8_t* p) { uint32_t k; memcpy(&k,p,4); k*=m; k^=k>>24; k*=m; h*=m; h^=k; };
    while(length>=8) { mix(h1,data); mix(h2,data+4); data+=8; length-=8; }
    if(length>=4) { mix(h1,data); data+=4; length-=4; }
    switch(length) {
    case 3: h2^=static_cast<uint32_t>(data[2])<<16; [[fallthrough]];
    case 2: h2^=static_cast<uint32_t>(data[1])<<8; [[fallthrough]];
    case 1: h2^=data[0]; h2*=m;
    }
    h1^=h2>>18; h1*=m; h2^=h1>>22; h2*=m; h1^=h2>>17; h1*=m; h2^=h1>>19; h2*=m;
    return (static_cast<uint64_t>(h1)<<32)|h2;
}

// Independent implementation of the LZ4 block format.
static std::vector<uint8_t> emissiveLz4(const uint8_t* source,size_t size,size_t outputSize) {
    std::vector<uint8_t> output; output.reserve(outputSize);
    size_t i=0;
    auto length=[&](size_t n) {
        if(n==15) for(uint8_t b=255;b==255;) { if(i>=size) throw std::runtime_error("lz4"); b=source[i++]; n+=b; }
        return n;
    };
    while(i<size) {
        const uint8_t token=source[i++];
        size_t literals=length(token>>4);
        if(literals>size-i || output.size()+literals>outputSize) throw std::runtime_error("lz4 literals");
        output.insert(output.end(),source+i,source+i+literals); i+=literals;
        if(i>=size) break;
        if(size-i<2) throw std::runtime_error("lz4 offset");
        const size_t offset=source[i]|(source[i+1]<<8); i+=2;
        const size_t match=length(token&15)+4;
        if(!offset || offset>output.size() || output.size()+match>outputSize) throw std::runtime_error("lz4 match");
        for(size_t k=0,from=output.size()-offset;k<match;++k) output.push_back(output[from+k]);
    }
    if(output.size()!=outputSize) throw std::runtime_error("lz4 size");
    return output;
}

struct EmissiveKv {
    uint8_t type=1; double number=0; std::string text;
    std::vector<std::pair<std::string,EmissiveKv>> members; std::vector<EmissiveKv> items;
    const EmissiveKv* get(const char* name) const {
        for(const auto& member:members) if(member.first==name) return &member.second;
        return nullptr;
    }
};
// Binary KV3, encoding id 0x4B563301 (uncompressed or LZ4). Layout after a 40-byte header: bytes, 4-aligned int32s, 8-aligned 8-byte values, strings, type stream.
static EmissiveKv emissiveKv3(const uint8_t* data,size_t size) {
    auto u32=[&](size_t at) { if(at+4>size) throw std::runtime_error("kv3 header"); uint32_t v; memcpy(&v,data+at,4); return v; };
    if(u32(0)!=0x4B563301u) throw std::runtime_error("kv3 encoding");
    const uint32_t compression=u32(20),byteCount=u32(24),intCount=u32(28),eightCount=u32(32),unpacked=u32(36);
    if(unpacked>(64u<<20)) throw std::runtime_error("kv3 size");
    std::vector<uint8_t> buffer;
    if(compression==0) { if(40+size_t(unpacked)>size) throw std::runtime_error("kv3 data"); buffer.assign(data+40,data+40+unpacked); }
    else if(compression==1) buffer=emissiveLz4(data+40,size-40,unpacked);
    else throw std::runtime_error("kv3 compression");
    size_t at=0;
    auto need=[&](size_t bytes) { if(bytes>buffer.size()-at) throw std::runtime_error("kv3 layout"); };
    need(byteCount); const uint8_t* bytes=buffer.data(); at=(byteCount+3)&~size_t(3);
    if(at>buffer.size()) throw std::runtime_error("kv3 layout");
    need(size_t(intCount)*4); std::vector<int32_t> ints(intCount); if(intCount) memcpy(ints.data(),buffer.data()+at,size_t(intCount)*4);
    at=(at+size_t(intCount)*4+7)&~size_t(7); if(at>buffer.size()) throw std::runtime_error("kv3 layout");
    need(size_t(eightCount)*8); const uint8_t* eights=buffer.data()+at; at+=size_t(eightCount)*8;
    if(ints.empty() || ints[0]<0) throw std::runtime_error("kv3 strings");
    std::vector<std::string> strings;
    for(int32_t s=0;s<ints[0];++s) {
        auto end=std::find(buffer.begin()+at,buffer.end(),uint8_t(0));
        if(end==buffer.end()) throw std::runtime_error("kv3 string");
        strings.emplace_back(buffer.begin()+at,end); at=(end-buffer.begin())+1;
    }
    if(buffer.size()<at+4) throw std::runtime_error("kv3 types");
    const size_t typeEnd=buffer.size()-4;
    size_t nextByte=0,nextInt=1,nextEight=0,nextType=at;
    auto takeInt=[&]() { if(nextInt>=ints.size()) throw std::runtime_error("kv3 ints"); return ints[nextInt++]; };
    auto takeType=[&]() {
        if(nextType>=typeEnd) throw std::runtime_error("kv3 type");
        uint8_t t=buffer[nextType++];
        // A flag byte follows when the high bit is set.
        if(t&0x80) { ++nextType; t&=0x7f; }
        return t;
    };
    auto takeEight=[&](void* out) { if(nextEight>=eightCount) throw std::runtime_error("kv3 eights"); memcpy(out,eights+nextEight++*8,8); };
    auto stringAt=[&](int32_t index) -> std::string {
        if(index<0) return {};
        if(static_cast<size_t>(index)>=strings.size()) throw std::runtime_error("kv3 string index");
        return strings[index];
    };
    std::function<EmissiveKv(uint8_t,int)> read=[&](uint8_t type,int depth) {
        if(depth>32) throw std::runtime_error("kv3 depth");
        EmissiveKv value; value.type=type;
        switch(type) {
        case 1: break;
        case 2: if(nextByte>=byteCount) throw std::runtime_error("kv3 bytes"); value.number=bytes[nextByte++]; break;
        case 3: { int64_t v; takeEight(&v); value.number=double(v); break; }
        case 4: { uint64_t v; takeEight(&v); value.number=double(v); break; }
        case 5: takeEight(&value.number); break;
        case 6: value.text=stringAt(takeInt()); break;
        case 7: { const int32_t n=takeInt(); if(n<0 || size_t(n)>byteCount-nextByte) throw std::runtime_error("kv3 blob"); nextByte+=n; break; }
        case 8: { const int32_t n=takeInt(); if(n<0) throw std::runtime_error("kv3 array");
                  for(int32_t k=0;k<n;++k) value.items.push_back(read(takeType(),depth+1)); break; }
        case 9: { const int32_t n=takeInt(); if(n<0) throw std::runtime_error("kv3 object");
                  for(int32_t k=0;k<n;++k) { std::string name=stringAt(takeInt()); value.members.emplace_back(std::move(name),read(takeType(),depth+1)); } break; }
        case 10: { const int32_t n=takeInt(); const uint8_t sub=takeType(); if(n<0) throw std::runtime_error("kv3 typed array");
                   for(int32_t k=0;k<n;++k) value.items.push_back(read(sub,depth+1)); break; }
        case 11: value.number=takeInt(); break;
        case 12: value.number=static_cast<uint32_t>(takeInt()); break;
        case 13: value.number=1; break;
        case 14: case 15: case 17: value.number=0; break;
        case 16: case 18: value.number=1; break;
        default: throw std::runtime_error("kv3 value type");
        }
        return value;
    };
    return read(takeType(),0);
}

// DATA block of a compiled resource: 16-byte header, block table of {type, offset, size} 12-byte entries; offsets are relative to their own field.
static std::pair<const uint8_t*,size_t> emissiveDataBlock(const std::vector<uint8_t>& file) {
    if(file.size()<16) throw std::runtime_error("resource header");
    uint32_t blockOffset,blockCount; memcpy(&blockOffset,file.data()+8,4); memcpy(&blockCount,file.data()+12,4);
    const size_t base=8+size_t(blockOffset);
    for(uint32_t i=0;i<blockCount && i<64;++i) {
        const size_t at=base+size_t(i)*12;
        if(at+12>file.size()) break;
        if(memcmp(file.data()+at,"DATA",4)) continue;
        uint32_t offset,size; memcpy(&offset,file.data()+at+4,4); memcpy(&size,file.data()+at+8,4);
        const size_t start=at+4+size_t(offset);
        if(start>file.size() || size>file.size()-start) throw std::runtime_error("resource DATA bounds");
        return {file.data()+start,size};
    }
    throw std::runtime_error("resource has no DATA");
}

// VPK v1/v2 directory (signature 0x55aa1234): extension/folder/name tree, 18-byte entry records; archive 0x7fff means data follows the directory.
struct EmissivePak {
    fs::path directory; uint32_t dataStart=0;
    struct Entry { uint16_t archive; uint32_t offset,length; std::vector<uint8_t> preload; };
    std::unordered_map<std::string,Entry> entries;
    bool open(const fs::path& path) {
        std::ifstream input(path,std::ios::binary);
        if(!input) return false;
        std::vector<uint8_t> d((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>());
        uint32_t header[3]{};
        if(d.size()<28) return false;
        memcpy(header,d.data(),12);
        if(header[0]!=0x55aa1234u || (header[1]!=1 && header[1]!=2)) return false;
        const size_t first=header[1]==2?28:12,end=first+size_t(header[2]);
        if(end>d.size()) return false;
        directory=path; dataStart=static_cast<uint32_t>(end);
        size_t at=first;
        auto text=[&]() {
            auto stop=std::find(d.begin()+at,d.begin()+end,uint8_t(0));
            if(stop==d.begin()+end) throw std::runtime_error("vpk tree");
            std::string s(d.begin()+at,stop); at=(stop-d.begin())+1; return s;
        };
        try {
            for(std::string extension;!(extension=text()).empty();)
                for(std::string folder;!(folder=text()).empty();)
                    for(std::string name;!(name=text()).empty();) {
                        if(at+18>end) throw std::runtime_error("vpk entry");
                        uint16_t preload,archive; Entry entry;
                        memcpy(&preload,d.data()+at+4,2); memcpy(&archive,d.data()+at+6,2);
                        memcpy(&entry.offset,d.data()+at+8,4); memcpy(&entry.length,d.data()+at+12,4); at+=18;
                        if(preload>end-at) throw std::runtime_error("vpk preload");
                        entry.archive=archive; entry.preload.assign(d.begin()+at,d.begin()+at+preload); at+=preload;
                        std::string full=(folder==" "?std::string():folder+"/")+name+"."+extension;
                        for(char& c:full) c=static_cast<char>(tolower(static_cast<unsigned char>(c)));
                        entries.emplace(std::move(full),std::move(entry));
                    }
        } catch(const std::exception&) { entries.clear(); return false; }
        return true;
    }
    bool read(const std::string& name,std::vector<uint8_t>& out) const {
        auto found=entries.find(name); if(found==entries.end()) return false;
        const auto& entry=found->second;
        if(entry.length>(256u<<20)) return false;
        out=entry.preload;
        if(!entry.length) return true;
        fs::path file=directory; uint64_t offset=entry.offset;
        if(entry.archive==0x7fff) offset+=dataStart;
        else {
            wchar_t suffix[16]; swprintf_s(suffix,L"_%03u.vpk",entry.archive);
            std::wstring stem=directory.filename().wstring();
            if(stem.size()<8) return false;
            // Archive file name: pak01_dir.vpk maps to pak01_NNN.vpk.
            file=directory.parent_path()/(stem.substr(0,stem.size()-8)+suffix);
        }
        std::ifstream input(file,std::ios::binary);
        if(!input.seekg(static_cast<std::streamoff>(offset))) return false;
        const size_t before=out.size(); out.resize(before+entry.length);
        return static_cast<bool>(input.read(reinterpret_cast<char*>(out.data()+before),entry.length));
    }
};

struct EmissiveFiles {
    fs::path addon; std::vector<EmissivePak> paks;
    bool read(const std::string& name,std::vector<uint8_t>& out) const {
        if(!addon.empty()) {
            std::error_code error; const fs::path loose=addon/fs::u8path(name);
            if(fs::is_regular_file(loose,error) && fs::file_size(loose,error)<(256u<<20)) {
                std::ifstream input(loose,std::ios::binary);
                out.assign(std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>());
                if(input || input.eof()) return true;
            }
        }
        for(const auto& pak:paks) if(pak.read(name,out)) return true;
        return false;
    }
};

struct ResourceBlock { char type[5]{}; const uint8_t* data=nullptr; size_t size=0; };
static std::vector<ResourceBlock> resourceBlocks(const std::vector<uint8_t>& file) {
    if(file.size()<16) throw std::runtime_error("resource header");
    uint32_t blockOffset,blockCount; memcpy(&blockOffset,file.data()+8,4); memcpy(&blockCount,file.data()+12,4);
    const size_t base=8+size_t(blockOffset);
    std::vector<ResourceBlock> blocks;
    for(uint32_t i=0;i<blockCount && i<256;++i) {
        const size_t at=base+size_t(i)*12;
        if(at+12>file.size()) throw std::runtime_error("resource blocks");
        uint32_t offset,size; memcpy(&offset,file.data()+at+4,4); memcpy(&size,file.data()+at+8,4);
        const size_t start=at+4+size_t(offset);
        if(start>file.size() || size>file.size()-start) throw std::runtime_error("resource block bounds");
        ResourceBlock block; memcpy(block.type,file.data()+at,4); block.data=file.data()+start; block.size=size;
        blocks.push_back(block);
    }
    return blocks;
}
