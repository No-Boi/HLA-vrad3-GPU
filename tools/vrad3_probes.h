#pragma once
// Light-probe (lpv_indirect) GPU path: reads the volume geometry from the child, traces it with the
// lightmap path estimator and packs the result in Valve's file format.
struct ProbeArguments {
    std::string name;
    uint32_t start[3],end[3],samples;
    uint32_t factor=1;
};
// Stage arguments: lpv_indirect <file> <sx> <sy> <sz> <ex> <ey> <ez> <minspp> <maxspp> <errortol>
// (argc in EDX, argv in R8). Only this full explicit form is supported.
static ProbeArguments probeArguments(const CONTEXT& context) {
    if(static_cast<uint32_t>(context.Rdx)!=11 || !context.R8) throw std::runtime_error("Probe stage needs explicit arguments");
    auto pointers=copyArray(context.R8,11,8);
    auto args=reinterpret_cast<const uintptr_t*>(pointers.data());
    auto token=[&](uint32_t i) {
        std::string value;
        for(size_t j=0;j<1024;++j) { char c=readValue<char>(args[i]+j); if(!c) return value; value+=c; }
        throw std::runtime_error("Probe argument too long");
    };
    auto number=[&](uint32_t i) {
        std::string text=token(i); size_t used=0; long value=-1;
        try { value=std::stol(text,&used); } catch(...) { used=0; }
        if(!used || used!=text.size() || value<0) throw std::runtime_error("Unsupported probe argument");
        return static_cast<uint32_t>(value);
    };
    ProbeArguments result{}; result.name=token(1);
    for(uint32_t i=0;i<3;++i) {
        result.start[i]=number(2+i); result.end[i]=number(5+i);
        if(result.start[i]>=result.end[i] || result.end[i]>countAt(0x1de868+i*4)) throw std::runtime_error("Invalid probe range");
    }
    result.samples=number(9);
    if(!result.samples || result.samples>4096) throw std::runtime_error("Unsupported probe spp");
    if(fs::path(result.name).filename()!=fs::path(result.name)) throw std::runtime_error("Probe output requires a filename");
    return result;
}
constexpr uint32_t probeRecordBytes=256;
static uint32_t probeStripes(uint64_t cells,uint32_t samples) {
    uint32_t stripes=1;
    while(stripes<64 && samples%(stripes*2)==0 && cells*stripes<200000 && cells*stripes*2*probeRecordBytes<=192ull*1024*1024) stripes*=2;
    return stripes;
}
struct ProbePlan { uint32_t stripes=1; };
static NativeLighting prepareProbe(const ProbeArguments& args,ProbePlan& plan,bool sceneResident=false) {
    // Volume globals (RVAs): dims at 1de868, bounds (min xyz, max xyz) at 1de850, 3x4 transform at 32adf0,
    // mask bit array (set = masked) via the pointer at 32add0.
    uint64_t cells=1,full=1; uint32_t dims[3]; float bounds[6],matrix[12],extent[3];
    for(uint32_t i=0;i<3;++i) { dims[i]=countAt(0x1de868+i*4); cells*=args.end[i]-args.start[i]; full*=dims[i]; }
    if(cells>1048576 || full>134217728) throw std::runtime_error("Probe dimensions exceed GPU bounds");
    readMemory(address(0x1de850),bounds,sizeof(bounds)); readMemory(address(0x32adf0),matrix,sizeof(matrix));
    for(float v:bounds) if(!std::isfinite(v)) throw std::runtime_error("Invalid probe bounds");
    for(float v:matrix) if(!std::isfinite(v)) throw std::runtime_error("Invalid probe transform");
    for(uint32_t i=0;i<3;++i) { extent[i]=(bounds[3+i]-bounds[i])/dims[i]; if(extent[i]<=0) throw std::runtime_error("Invalid probe extent"); }
    auto mask=copyArray(pointerAt(0x32add0),(full+31)/32,4,"read_probe_mask");
    uint32_t stripes=probeStripes(cells,args.samples);
    { const auto forced=environmentValue(L"VRAD3_PROBE_STRIPES");
      if(!forced.empty()) { stripes=static_cast<uint32_t>(std::stoul(forced)); if(!stripes || stripes>64 || args.samples%stripes) throw std::runtime_error("VRAD3_PROBE_STRIPES must divide the sample count"); } }
    plan.stripes=stripes;
    const uint32_t perStripe=args.samples/stripes;
    NativeLighting input=prepareNative(perStripe,0,static_cast<uint32_t>(cells*stripes),sceneResident);
    // Probe mode is bit 30 of constants[3]; constants[7] is the stripe count, [9] selects the first-segment sampling.
    input.constants[3]=0x40000000u;
    input.constants[15]=args.factor;
    input.constants[2]=0;
    memcpy(input.constants+7,&stripes,4);
    { const auto sampling=environmentValue(L"VRAD3_PROBE_SAMPLING");
      const float low=sampling==L"random"?0.0f:1.0f; memcpy(input.constants+9,&low,4); }
    { const auto flags=environmentValue(L"VRAD3_PROBE_FLAGS");
      uint32_t bits=flags.empty()?0:static_cast<uint32_t>(std::stoul(flags)); memcpy(input.constants+8,&bits,4); }
    { const auto depth=environmentValue(L"VRAD3_PROBE_DEPTH");
      if(!depth.empty()) input.constants[3]|=static_cast<uint32_t>(std::stoul(depth))&0xffffu; }
    // Ray classes: nearest-hit rays use mask m (global 17ae44), shadow rays m|1, as in Valve's visibility queues.
    input.constants[12]=countAt(0x17ae44); input.constants[13]=input.constants[12]|1;
    if(environmentValue(L"VRAD3_PROBE_OLD_SHADOWMASK")==L"1") input.constants[13]=input.constants[12];
    uint32_t cursor=0,active=0,thread=0;
    for(uint32_t z=args.start[2];z<args.end[2];++z)
    for(uint32_t y=args.start[1];y<args.end[1];++y)
    for(uint32_t x=args.start[0];x<args.end[0];++x,++cursor) {
        uint32_t cell=(z*dims[1]+y)*dims[0]+x,word;
        memcpy(&word,mask.data()+(cell/32)*4ull,4);
        const bool open=!(word&(1u<<(cell%32)));
        float local[3]={bounds[0]+x*extent[0],bounds[1]+y*extent[1],bounds[2]+z*extent[2]},box[9],edgeZ[3];
        for(uint32_t r=0;r<3;++r) {
            box[r]=matrix[r*4]*local[0]+matrix[r*4+1]*local[1]+matrix[r*4+2]*local[2]+matrix[r*4+3];
            box[3+r]=matrix[r*4]*extent[0]; box[6+r]=matrix[r*4+1]*extent[1]; edgeZ[r]=matrix[r*4+2]*extent[2];
        }
        if(open) ++active;
        for(uint32_t s=0;s<stripes;++s,++thread) {
            const uint32_t count=open?(active-1)*stripes+s+1:active*stripes;
            memcpy(input.buffers[7].data()+(thread+1ull)*4,&count,4);
            memcpy(input.buffers[10].data()+thread*36ull,box,36); memcpy(input.buffers[9].data()+thread*12ull,edgeZ,12);
        }
    }
    if(!active) throw std::runtime_error("Probe volume range has no unmasked cells");
    input.totalPaths=static_cast<uint64_t>(active)*args.samples;
    std::cout << "Probe cells=" << cells << " active=" << active << " spp=" << args.samples << " stripes=" << stripes << " mask=" << input.constants[12] << '\n';
    if(environmentValue(L"VRAD3_PROBE_LIGHTS")==L"1") {
        std::map<uint32_t,std::pair<uint32_t,uint32_t>> histogram;
        for(size_t i=0;i+8<=input.buffers[2].size();i+=8) {
            uint32_t words[2]; memcpy(words,input.buffers[2].data()+i,8);
            float opacity; int32_t texture; memcpy(&opacity,input.buffers[4].data()+words[1]*64ull+12,4); memcpy(&texture,input.buffers[4].data()+words[1]*64ull+52,4);
            auto& entry=histogram[words[0]]; ++entry.first; if(texture>=0 || opacity<1) ++entry.second;
        }
        std::cout << "PROBE_MESHFLAGS " << input.constants[14] << '\n';
        for(const auto& entry:histogram) std::cout << "PROBE_PRIM flags=0x" << std::hex << entry.first << std::dec << " count=" << entry.second.first << " translucent=" << entry.second.second << '\n';
        for(uint32_t i=0;i<input.constants[11];++i) {
            const uint8_t* record=input.buffers[11].data()+i*112ull; uint32_t flags[4]; float v[24];
            memcpy(flags,record,16); memcpy(v,record+16,96);
            std::cout << "PROBE_LIGHT " << i << " type=" << flags[0] << " sized=" << flags[1] << " att=" << flags[2] << " elig=" << flags[3]
                      << " rgb=" << v[0] << ',' << v[1] << ',' << v[2] << " range=" << v[3] << " pos=" << v[4] << ',' << v[5] << ',' << v[6]
                      << " offset=" << v[7] << " dir=" << v[8] << ',' << v[9] << ',' << v[10] << " radius=" << v[11]
                      << " att=" << v[12] << ',' << v[13] << ',' << v[14] << " cone=" << v[16] << ',' << v[17] << ',' << v[18]
                      << " cookie=" << v[19] << " proj=" << v[23] << '\n';
        }
    }
    return input;
}
// Scene token for the GPU server: hash of the compile key, facts, constants and light table, so workers
// of one compile share one resident scene.
static std::array<uint8_t,32> sceneToken(const NativeLighting& input,const std::wstring& compileKey,const char* mode) {
    std::vector<uint8_t> bytes;
    bytes.insert(bytes.end(),mode,mode+strlen(mode)+1);
    bytes.insert(bytes.end(),reinterpret_cast<const uint8_t*>(compileKey.data()),reinterpret_cast<const uint8_t*>(compileKey.data()+compileKey.size()));
    bytes.insert(bytes.end(),reinterpret_cast<const uint8_t*>(input.facts),reinterpret_cast<const uint8_t*>(input.facts+8));
    std::vector<uint32_t> slots={4,5,6,10,11,12,13,14,16,17,18,19,20,21,22};
    if(!(input.constants[3]&0x40000000u)) slots.insert(slots.end(),{3,7,8,9});
    for(uint32_t i:slots) appendValue(bytes,input.constants[i]);
    std::vector<uint8_t> lights=input.buffers[11];
    // Light records are 112 bytes; float 19 (byte 92) is the cookie offset, 0 when the scene was not read.
    // Zeroed so a quick read and a full read hash alike.
    for(size_t offset=92;offset+4<=lights.size();offset+=112) memset(lights.data()+offset,0,4);
    appendValue(bytes,static_cast<uint64_t>(lights.size()));
    bytes.insert(bytes.end(),lights.begin(),lights.end());
    std::array<uint8_t,32> digest{};
    if(BCryptHash(nullptr,nullptr,0,bytes.data(),static_cast<ULONG>(bytes.size()),digest.data(),32)<0) {
        BCRYPT_ALG_HANDLE algorithm=nullptr;
        if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) throw std::runtime_error("Scene token hash failed");
        const NTSTATUS status=BCryptHash(algorithm,nullptr,0,bytes.data(),static_cast<ULONG>(bytes.size()),digest.data(),32);
        BCryptCloseAlgorithmProvider(algorithm,0);
        if(status<0) throw std::runtime_error("Scene token hash failed");
    }
    return digest;
}
static std::array<uint8_t,32> probeSceneToken(const NativeLighting& input,const std::wstring& compileKey) {
    return sceneToken(input,compileKey,"probe-scene-v2");
}
// Valve's packing (stage Run 14003f190 -> 14004c400): per face, a zero sentinel (bytes 0x80) if all channels
// are below 1e-16; else frexp(max|c|)=f*2^e, int8 mantissas floor(127*f/max*c+0.5), exponent byte e.
// Layout is face-major, 4 bytes per cell.
static std::vector<uint8_t> packProbe(const std::vector<uint8_t>& records,const ProbeArguments& args,
                                      const std::vector<uint8_t>& offsets,uint32_t stripes) {
    const size_t threads=records.size()/probeRecordBytes;
    if(!stripes || records.size()!=threads*probeRecordBytes || threads%stripes || offsets.size()!=(threads+1)*4 || args.samples%stripes)
        throw std::runtime_error("GPU probe record size mismatch");
    const size_t cells=threads/stripes; const uint32_t perStripe=args.samples/stripes;
    std::vector<uint8_t> packed(cells*24,0x80);
    auto word=[&](size_t i) { uint32_t value; memcpy(&value,offsets.data()+i*4,4); return value; };
    for(size_t cell=0;cell<cells;++cell) {
        const bool active=word(cell*stripes)!=word(cell*stripes+1);
        for(size_t face=0;face<6;++face) {
            double sum[3]={0,0,0};
            for(uint32_t stripe=0;stripe<stripes;++stripe) {
                const uint8_t* record=records.data()+(cell*stripes+stripe)*probeRecordBytes+face*36;
                float rgb[3]; memcpy(rgb,record,12);
                uint32_t count; memcpy(&count,record+32,4);
                if(count!=(active?perStripe:0)) throw std::runtime_error("Invalid probe GPU sample count");
                for(int c=0;c<3;++c) { if(!std::isfinite(rgb[c]) || std::abs(rgb[c])>100.001f) throw std::runtime_error("Invalid probe GPU RGB"); sum[c]+=rgb[c]; }
            }
            float rgb[3]; float maximum=0;
            for(int c=0;c<3;++c) { rgb[c]=static_cast<float>(sum[c]/stripes); maximum=std::max(maximum,std::abs(rgb[c])); }
            if(maximum<1e-16f) continue;
            int exponent; float fraction=std::frexp(maximum,&exponent),scale=127*fraction/maximum;
            for(size_t c=0;c<3;++c) packed[(face*cells+cell)*4+c]=static_cast<uint8_t>(std::clamp(static_cast<int>(std::floor(rgb[c]*scale+0.5f)),-128,127));
            packed[(face*cells+cell)*4+3]=static_cast<uint8_t>(std::clamp(exponent,-128,127));
        }
    }
    return packed;
}
