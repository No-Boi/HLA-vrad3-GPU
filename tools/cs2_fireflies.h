// CS2-style firefly removal on the lightmap block files, run before Valve's lm_combine.
// Valve's own pass is left out of a rewritten copy of the combine script.
#pragma once
#include <thread>
#include <functional>
#include <atomic>
#include <mutex>
#include <cstring>

namespace cs2Fireflies {

static bool enabled() {
    wchar_t value[8]{};
    if(GetEnvironmentVariableW(L"VRAD3_GPU_ESTIMATOR",value,8)!=3 || _wcsicmp(value,L"cs2")!=0) return false;
    DWORD n=GetEnvironmentVariableW(L"VRAD3_GPU_CS2_FIREFLIES",value,8);
    return !(n==1 && value[0]==L'0');
}

// Independent implementation of the LZ4 block format (decoder).
static void lz4Decode(const uint8_t* source,size_t size,uint8_t* output,size_t outputSize) {
    size_t i=0,o=0;
    auto length=[&](size_t n) {
        if(n==15) for(uint8_t b=255;b==255;) { if(i>=size) throw std::runtime_error("block lz4 length"); b=source[i++]; n+=b; }
        return n;
    };
    while(i<size) {
        const uint8_t token=source[i++];
        const size_t literals=length(token>>4);
        if(literals>size-i || literals>outputSize-o) throw std::runtime_error("block lz4 literals");
        memcpy(output+o,source+i,literals); i+=literals; o+=literals;
        if(i>=size) break;
        if(size-i<2) throw std::runtime_error("block lz4 offset");
        const size_t offset=source[i]|(source[i+1]<<8); i+=2;
        const size_t match=length(token&15)+4;
        if(!offset || offset>o || match>outputSize-o) throw std::runtime_error("block lz4 match");
        const uint8_t* from=output+o-offset;
        if(offset>=match) memcpy(output+o,from,match);
        else for(size_t k=0;k<match;++k) output[o+k]=from[k];
        o+=match;
    }
    if(o!=outputSize) throw std::runtime_error("block lz4 size");
}

// Greedy encoder for the LZ4 block format; Valve's reader decodes it with LZ4_decompress_safe. The last match must start 12+ bytes before the end and the last 5 bytes are literals.
static std::vector<uint8_t> lz4Encode(const uint8_t* source,size_t size) {
    std::vector<uint8_t> out; out.reserve(size+size/255+16);
    auto extend=[&](size_t n) { for(;n>=255;n-=255) out.push_back(255); out.push_back(static_cast<uint8_t>(n)); };
    auto sequence=[&](size_t anchor,size_t literals,size_t offset,size_t match) {
        const size_t code=match?match-4:0;
        out.push_back(static_cast<uint8_t>((std::min<size_t>(literals,15)<<4)|(match?std::min<size_t>(code,15):0)));
        if(literals>=15) extend(literals-15);
        out.insert(out.end(),source+anchor,source+anchor+literals);
        if(!match) return;
        out.push_back(static_cast<uint8_t>(offset)); out.push_back(static_cast<uint8_t>(offset>>8));
        if(code>=15) extend(code-15);
    };
    std::vector<uint32_t> table(1<<16,UINT32_MAX);
    size_t anchor=0,i=0,misses=0;
    while(size>=13 && i+12<=size) {
        uint32_t word; memcpy(&word,source+i,4);
        const uint32_t hash=(word*2654435761u)>>16;
        const size_t candidate=table[hash]; table[hash]=static_cast<uint32_t>(i);
        if(candidate!=UINT32_MAX && i-candidate<=65535 && !memcmp(source+candidate,source+i,4)) {
            size_t match=4; const size_t limit=size-5;
            for(;;) {
                if(i+match+8>limit) { while(i+match<limit && source[candidate+match]==source[i+match]) ++match; break; }
                uint64_t a,b; memcpy(&a,source+candidate+match,8); memcpy(&b,source+i+match,8);
                if(a!=b) { unsigned long bit; _BitScanForward64(&bit,a^b); match+=bit>>3; break; }
                match+=8;
            }
            sequence(anchor,i-anchor,i-candidate,match);
            i+=match; anchor=i; misses=0;
        } else i+=1+(misses++>>6);
    }
    sequence(anchor,size-anchor,0,0);
    return out;
}

// Lightmap block file: one raw LZ4 block holding 0x8C-byte luxel records, row-major. Record: irradiance +0x00, SH +0x0C (3x16 B), chart +0x54, flags +0x5C, normal +0x60.
constexpr size_t recordSize=0x8C;
struct Texel { float irradiance[3]; float sh[3]; int32_t chart; uint8_t flags; uint8_t pad[3]; };
struct Block { std::string line,name; uint32_t number=0; fs::path file; };

// HLA's L1 SH evaluation at a normal (cosine convolved, over pi), as its own firefly pass does.
static float shAt(const float* c,const float* n) { return 0.282095f*c[0]-0.325735f*(c[1]*n[1]-c[2]*n[2]+c[3]*n[0]); }

static fs::path findInput(const std::string& name,const fs::path& indir) {
    fs::path path=fs::u8path(name);
    if(path.is_absolute()) return path;
    if(!indir.empty() && fs::exists(indir/path)) return indir/path;
    return path;
}

static std::vector<uint8_t> readFile(const fs::path& path) {
    std::ifstream input(path,std::ios::binary|std::ios::ate);
    if(!input) throw std::runtime_error("cannot read "+path.u8string());
    std::vector<uint8_t> data(static_cast<size_t>(input.tellg()));
    input.seekg(0);
    if(!data.empty() && !input.read(reinterpret_cast<char*>(data.data()),data.size())) throw std::runtime_error("cannot read "+path.u8string());
    return data;
}

static void parallelRows(uint32_t rows,const std::function<void(uint32_t)>& body) {
    std::atomic<uint32_t> next{0};
    const uint32_t count=std::max(1u,std::min(std::thread::hardware_concurrency(),64u));
    std::vector<std::thread> threads;
    for(uint32_t t=0;t<count;++t) threads.emplace_back([&]() { for(uint32_t y;(y=next++)<rows;) body(y); });
    for(auto& thread:threads) thread.join();
}
static void parallelItems(size_t count,size_t workers,const std::function<void(size_t)>& body) {
    std::atomic<size_t> next{0}; std::exception_ptr error; std::mutex lock;
    std::vector<std::thread> threads;
    for(size_t t=0;t<std::max<size_t>(1,std::min(workers,count));++t) threads.emplace_back([&]() {
        for(size_t k;(k=next++)<count;) {
            try { body(k); }
            catch(...) { std::lock_guard<std::mutex> guard(lock); if(!error) error=std::current_exception(); next=count; }
        }
    });
    for(auto& thread:threads) thread.join();
    if(error) std::rethrow_exception(error);
}

static std::wstring prepare(const std::vector<std::wstring>& args) {
    if(!enabled()) return {};
    try {
        const ULONGLONG started=GetTickCount64();
        std::wstring scriptArg; std::wstring indirArg;
        for(size_t i=0;i+1<args.size();++i) {
            if(args[i]==L"-script") scriptArg=args[i+1];
            if(args[i]==L"-indir") indirArg=args[i+1];
        }
        if(scriptArg.empty()) throw std::runtime_error("no -script");
        const fs::path indir=indirArg.empty()?fs::path():fs::absolute(indirArg);
        std::ifstream scriptInput(scriptArg);
        if(!scriptInput) throw std::runtime_error("cannot read the combine script");
        std::vector<std::string> lines; for(std::string line;std::getline(scriptInput,line);) lines.push_back(line);

        uint32_t width=0,height=0,blockSize=0; bool combine=false,valveFireflies=false;
        std::vector<Block> blocks; std::vector<size_t> blockLines;
        for(size_t l=0;l<lines.size();++l) {
            std::istringstream tokens(lines[l]); std::string command;
            if(!(tokens>>command)) continue;
            if(command.back()==':') { combine=command=="lm_combine:"; continue; }
            if(!combine) continue;
            if(command=="lightmap_size") tokens>>width>>height>>blockSize;
            else if(command=="lightmap_read_block") {
                Block block; block.line=lines[l];
                if(!(tokens>>block.name>>block.number)) throw std::runtime_error("unreadable lightmap_read_block line");
                blocks.push_back(block); blockLines.push_back(l);
            } else if(command=="lightmap_image_remove_fireflies") valveFireflies=true;
        }
        if(!width || !height || !blockSize || blocks.empty()) throw std::runtime_error("no lightmap_size or blocks in lm_combine");
        if(!valveFireflies) throw std::runtime_error("the script has no lightmap_image_remove_fireflies");
        const uint32_t across=(width+blockSize-1)/blockSize,down=(height+blockSize-1)/blockSize;
        for(auto& block:blocks) {
            if(block.number>=across*down) throw std::runtime_error("block number outside the atlas");
            block.file=findInput(block.name,indir);
        }

        std::vector<uint8_t> superseded(blocks.size(),0);
        for(size_t b=0;b<blocks.size();++b) for(size_t later=b+1;later<blocks.size();++later)
            if(blocks[later].number==blocks[b].number) superseded[b]=1;

        const uint64_t texels=1ull*width*height;
        MEMORYSTATUSEX memory{}; memory.dwLength=sizeof(memory); GlobalMemoryStatusEx(&memory);
        const uint64_t fixed=texels*(sizeof(Texel)+2+12),perBlock=3ull*blockSize*blockSize*recordSize;
        const uint64_t usable=usableMemory(memory);
        if(usable<(fixed+perBlock)*3/2) throw std::runtime_error("not enough free memory ("+std::to_string((fixed+perBlock)>>20)+" MB needed)");
        const uint64_t spare=usable*2/3>fixed?usable*2/3-fixed:0;
        const size_t workers=static_cast<size_t>(std::clamp<uint64_t>(spare/perBlock,1,
                                                                      std::max(1u,std::min(std::thread::hardware_concurrency(),64u))));

        std::vector<Texel> atlas(texels);
        for(auto& texel:atlas) { texel={}; texel.chart=-1; }
        auto rect=[&](uint32_t number,uint32_t& x0,uint32_t& y0,uint32_t& w,uint32_t& h) {
            x0=number%across*blockSize; y0=number/across*blockSize;
            w=std::min(blockSize,width-x0); h=std::min(blockSize,height-y0);
        };
        auto decode=[&](const Block& block,uint32_t w,uint32_t h) {
            const auto compressed=readFile(block.file);
            std::vector<uint8_t> records(size_t(w)*h*recordSize);
            lz4Decode(compressed.data(),compressed.size(),records.data(),records.size());
            return records;
        };
        parallelItems(blocks.size(),workers,[&](size_t b) {
            if(superseded[b]) return;
            uint32_t x0,y0,w,h; rect(blocks[b].number,x0,y0,w,h);
            const auto records=decode(blocks[b],w,h);
            for(uint32_t y=0;y<h;++y) for(uint32_t x=0;x<w;++x) {
                const uint8_t* record=records.data()+(size_t(y)*w+x)*recordSize;
                Texel& texel=atlas[size_t(y0+y)*width+x0+x];
                float normal[3]; memcpy(normal,record+0x60,12);
                memcpy(texel.irradiance,record,12);
                for(int c=0;c<3;++c) texel.sh[c]=shAt(reinterpret_cast<const float*>(record+0x0C+16*c),normal);
                memcpy(&texel.chart,record+0x54,4);
                texel.flags=record[0x5C];
            }
        });

        // CS2's test: a texel is flagged when its irradiance or SH exceeds 4x the mean of its same-chart 3x3 neighbours (valid, luminance >= 0.0005); flag bits 1 and 2 (chart small in y / in x) both set means a 1x1 chart, which is skipped.
        const float ratio=4.0f;
        auto luminance=[](const float* c) { return 0.2126f*c[0]+0.7152f*c[1]+0.0722f*c[2]; };
        auto candidate=[&](const Texel& texel) { return (texel.flags&1) && texel.chart!=-1 && (texel.flags&6)!=6; };
        std::vector<uint8_t> flagged(texels,0);
        std::atomic<uint64_t> found{0};
        parallelRows(height,[&](uint32_t y) {
            uint64_t local=0;
            for(uint32_t x=0;x<width;++x) {
                const Texel& centre=atlas[size_t(y)*width+x];
                if(!candidate(centre)) continue;
                float irradiance[3]{},sh[3]{}; uint32_t n=0;
                for(int dy=-1;dy<=1;++dy) for(int dx=-1;dx<=1;++dx) {
                    if(!dx && !dy) continue;
                    const int64_t nx=int64_t(x)+dx,ny=int64_t(y)+dy;
                    if(nx<0 || ny<0 || nx>=width || ny>=height) continue;
                    const Texel& other=atlas[size_t(ny)*width+nx];
                    if(other.chart!=centre.chart || !(other.flags&1) || !(luminance(other.irradiance)>=0.0005f)) continue;
                    for(int c=0;c<3;++c) { irradiance[c]+=other.irradiance[c]; sh[c]+=std::max(0.0f,other.sh[c]); }
                    ++n;
                }
                if(n) for(int c=0;c<3;++c) { irradiance[c]/=n; sh[c]/=n; }
                bool keep=true;
                for(int c=0;c<3;++c) {
                    const float value=std::fabs(centre.sh[c]);
                    keep=keep && centre.irradiance[c]<=ratio*irradiance[c] && value<=ratio*sh[c] && value<=ratio*centre.irradiance[c];
                }
                if(!keep) { flagged[size_t(y)*width+x]=1; ++local; }
            }
            found+=local;
        });

        std::vector<float> replacement(found?texels*3:0);
        std::vector<uint8_t> replacedAt(texels,0);
        std::atomic<uint64_t> replaced{0};
        if(found) parallelRows(height,[&](uint32_t y) {
            uint64_t local=0;
            for(uint32_t x=0;x<width;++x) {
                const size_t at=size_t(y)*width+x;
                if(!flagged[at]) continue;
                const Texel& centre=atlas[at];
                float irradiance[3]{}; uint32_t n=0;
                for(int dy=-1;dy<=1;++dy) for(int dx=-1;dx<=1;++dx) {
                    if(!dx && !dy) continue;
                    const int64_t nx=int64_t(x)+dx,ny=int64_t(y)+dy;
                    if(nx<0 || ny<0 || nx>=width || ny>=height) continue;
                    const size_t index=size_t(ny)*width+nx;
                    const Texel& other=atlas[index];
                    if(flagged[index] || other.chart!=centre.chart || !(other.flags&1) || !(luminance(other.irradiance)>=0.0005f)) continue;
                    for(int c=0;c<3;++c) irradiance[c]+=other.irradiance[c];
                    ++n;
                }
                if(!n) continue;
                for(int c=0;c<3;++c) replacement[at*3+c]=irradiance[c]/n;
                replacedAt[at]=1; ++local;
            }
            replaced+=local;
        });

        const fs::path folder=compileTempRoot()/L"fireflies";
        // Relative paths: Valve resolves them from the working directory, and its script reader splits on spaces.
        const std::string relative="vrad3_gpu_tmp\\fireflies\\";
        fs::remove_all(folder); fs::create_directories(folder);
        std::atomic<uint32_t> changedBlocks{0};
        parallelItems(blocks.size(),workers,[&](size_t b) {
            const auto& block=blocks[b];
            if(superseded[b]) return;
            uint32_t x0,y0,w,h; rect(block.number,x0,y0,w,h);
            bool changed=false;
            for(uint32_t y=0;y<h && !changed;++y) for(uint32_t x=0;x<w;++x) if(replacedAt[size_t(y0+y)*width+x0+x]) { changed=true; break; }
            if(!changed) return;
            auto records=decode(block,w,h);
            for(uint32_t y=0;y<h;++y) for(uint32_t x=0;x<w;++x) {
                const size_t at=size_t(y0+y)*width+x0+x;
                if(replacedAt[at]) memcpy(records.data()+(size_t(y)*w+x)*recordSize,&replacement[at*3],12);
            }
            const auto encoded=lz4Encode(records.data(),records.size());
            const std::string name="lm_block_cs2_"+std::to_string(b)+".dat";
            std::ofstream output(folder/fs::u8path(name),std::ios::binary);
            output.write(reinterpret_cast<const char*>(encoded.data()),encoded.size());
            if(!output) throw std::runtime_error("cannot write "+name);
            lines[blockLines[b]]="    lightmap_read_block "+relative+name+" "+std::to_string(block.number);
            ++changedBlocks;
        });
        combine=false;
        std::ofstream script(folder/L"script.vrad3",std::ios::binary);
        for(const auto& line:lines) {
            std::istringstream tokens(line); std::string command;
            if(tokens>>command) {
                if(command.back()==':') combine=command=="lm_combine:";
                else if(combine && command=="lightmap_image_remove_fireflies") continue;
            }
            script<<line<<'\n';
        }
        if(!script) throw std::runtime_error("cannot write the combine script");
        std::cout<<"CS2 fireflies (cs2_fireflies = on): "<<found<<" found, "<<replaced<<" replaced in "<<changedBlocks
                 <<" blocks ("<<std::fixed<<std::setprecision(1)<<(GetTickCount64()-started)/1000.0<<" s); Valve's remove_fireflies left out\n"<<std::flush;
        return std::wstring(L"vrad3_gpu_tmp\\fireflies\\script.vrad3");
    } catch(const std::exception& error) {
        std::cout<<"CS2 fireflies skipped, Valve's remove_fireflies runs: "<<error.what()<<'\n'<<std::flush;
        return {};
    }
}

}
