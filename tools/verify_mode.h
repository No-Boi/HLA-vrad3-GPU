#pragma once
// Verify mode: the GPU result is compared with Valve's CPU result for the same block or volume, and the CPU
// result is the one kept.
static bool verifyItem=false;
static std::string verifyLabel,verifyLine;
static bool verifyEnabled() { return environmentValue(L"VRAD3_GPU_VERIFY")==L"1"; }
static uint32_t verifyLimit(bool probes) {
    auto value=environmentValue(probes?L"VRAD3_GPU_VERIFY_PROBES":L"VRAD3_GPU_VERIFY_BLOCKS");
    if(value.empty()) return 1;
    try { auto n=std::stoull(value); if(n && n<=UINT32_MAX) return static_cast<uint32_t>(n); } catch(...) {}
    return 1;
}
static uint32_t verifyWord(const uint8_t* p) { uint32_t v; memcpy(&v,p,4); return v; }
static float verifyFloat(const uint8_t* p) { float v; memcpy(&v,p,4); return v; }
struct VerifyMetric {
    double cpu=0,gpu=0,absolute=0,error=0; size_t count=0;
    void add(double c,double g) { cpu+=c; gpu+=g; absolute+=std::abs(c); error+=std::abs(c-g); ++count; }
    double ratio() const { return cpu?gpu/cpu:(gpu==0?1:std::numeric_limits<double>::infinity()); }
    bool ok() const { return count && std::isfinite(ratio()) && std::abs(ratio()-1)<=0.03 && std::isfinite(error); }
    void print(std::ostream& out,const char* name) const {
        out<<' '<<name<<' '<<ratio()<<" relMAE="<<(absolute?error/absolute:(error?std::numeric_limits<double>::infinity():0));
    }
};
static void verifyCorrupt(std::vector<uint8_t>& records,size_t stride,const std::vector<size_t>& offsets) {
    if(environmentValue(L"VRAD3_GPU_VERIFY_SCALE_RGB")!=L"0.9") return;
    for(size_t i=0;i<records.size();i+=stride) for(auto offset:offsets) {
        float v=verifyFloat(records.data()+i+offset)*0.9f; memcpy(records.data()+i+offset,&v,4);
    }
}
static void verifyUnavailable(const std::string& reason) {
    verifyLine="VRAD3 GPU verify: "+verifyLabel+" -> CHECK (comparison unavailable: "+reason+"; CPU result retained)";
    std::cout<<verifyLine<<'\n'<<std::flush;
}
struct VerifyBlock {
    std::vector<uint8_t> path,direct,baseline,outputBaseline;
    VerifyMetric rgb,sh;
    bool pathCompared=false;
    void comparePath() {
        // Per-luxel records (204 bytes, RVA 32ab20, count at 32ab88): +00 RGB, +32 sample count (0 = unused),
        // +36/+72/+108 SH DC terms. Index records (140 bytes) at 32ab90.
        auto cpu=copyArray(pointerAt(0x32ab20),countAt(0x32ab88),204,"verify_cpu_path");
        if(cpu.size()!=path.size()) throw std::runtime_error("Verify path record size mismatch");
        for(size_t i=0;i<cpu.size();i+=204) if(verifyWord(cpu.data()+i+32) && verifyWord(path.data()+i+32)) {
            for(size_t c=0;c<3;++c) rgb.add(verifyFloat(cpu.data()+i+c*4),verifyFloat(path.data()+i+c*4));
            for(size_t c=1;c<4;++c) sh.add(verifyFloat(cpu.data()+i+c*36),verifyFloat(path.data()+i+c*36));
        }
        path.clear(); path.shrink_to_fit(); pathCompared=true;
    }
    void finish() {
        bool ok=pathCompared && rgb.ok() && sh.ok();
        std::ostringstream line; line<<std::setprecision(6)<<"VRAD3 GPU verify: "<<verifyLabel;
        rgb.print(line,"RGB"); sh.print(line,"SH-DC");
        if(!direct.empty()) {
            auto cpu=copyArray(pointerAt(0x32ab20),countAt(0x32ab88),204,"verify_cpu_direct");
            auto out=copyArray(pointerAt(0x32ab90),countAt(0x32ab88),140,"verify_cpu_indices");
            if(cpu.size()!=direct.size() || baseline.size()!=cpu.size() || outputBaseline.size()!=out.size()) throw std::runtime_error("Verify direct record size mismatch");
            VerifyMetric bakedRgb,bakedSh,scalar; size_t valid=0,same=0;
            for(size_t i=0;i<cpu.size()/204;++i) if(verifyWord(cpu.data()+i*204+32)) {
                auto g=direct.data()+i*204,c=cpu.data()+i*204,b=baseline.data()+i*204,o=out.data()+i*140;
                ++valid; uint32_t selected=verifyWord(g); bool equal=true;
                for(size_t slot=0;slot<4;++slot) {
                    auto previous=outputBaseline.data()+i*140;
                    uint8_t id=slot<selected?g[80+slot]:previous[80+slot];
                    equal=equal && id==o[80+slot];
                    scalar.add(verifyFloat(o+64+slot*4),slot<selected?verifyFloat(g+64+slot*4):verifyFloat(previous+64+slot*4));
                }
                same+=equal;
                for(size_t channel=0;channel<3;++channel) {
                    bakedRgb.add(verifyFloat(c+144+channel*4)-verifyFloat(b+144+channel*4),verifyFloat(g+144+channel*4));
                    bakedSh.add(verifyFloat(c+156+channel*16)-verifyFloat(b+156+channel*16),verifyFloat(g+156+channel*16));
                }
            }
            double agreement=valid?double(same)/valid:0;
            line<<" indices "<<agreement*100<<"% scalarMAE="<<(scalar.count?scalar.error/scalar.count:0);
            bakedRgb.print(line,"bakedRGB"); bakedSh.print(line,"bakedSH-DC");
            ok=ok && agreement>=0.97 && bakedRgb.ok() && bakedSh.ok();
        } else line<<" direct=CPU";
        line<<" -> "<<(ok?"OK":"CHECK")<<" (CPU result written)";
        verifyLine=line.str(); std::cout<<verifyLine<<'\n'<<std::flush;
    }
};
