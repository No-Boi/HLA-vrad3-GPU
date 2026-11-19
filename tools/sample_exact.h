#pragma once
// Valve's sample steps after Reject Invalid (normals, map, sort, lookup, confidence) on all CPU threads,
// byte-identical to Valve's serial code. Valid for one Valve build only.
#ifdef VRAD3_STATIC_GPU
#include <immintrin.h>
static bool exactStageWanted() {
    return environmentValue(L"VRAD3_GPU_ESTIMATOR")!=L"cs2";
}
// Hook RVA: the CMP [sample count],0 after Valve prints "Calculate Normals..."; its Reject and
// compaction are done by then.
constexpr uintptr_t exactStageRva=0xa1bb;
// SHA-256 of the vrad3_real.exe build (2026-10-27) whose addresses and code are reproduced here.
constexpr const char* exactStageSha="6cf516c14515c0898619fa2e3b86265ca596ea4938006a4768cdd2738a1d9eda";
static std::string exactStageSummary;
static const char* exactStageStep="";
static bool exactVerifyWanted() { return environmentValue(L"VRAD3_GPU_EXACT_VERIFY")==L"1"; }
struct ExactExpected { std::vector<float> words,cdf; std::vector<int32_t> offsets,indices; int32_t luxels=0; uint64_t inputHash=0; };
static ExactExpected exactExpected;
namespace exact {
static inline double bitsDouble(uint64_t b) { double d; memcpy(&d,&b,8); return d; }
static inline float bitsFloat(uint32_t b) { float f; memcpy(&f,&b,4); return f; }
struct Exp { double table[64]; bool fma; };
// Valve's CRT expf (0x1400b43b0): table-driven, SSE2 or FMA3 path by Valve's CPU flag; matched bit for bit.
static float expValve(float f,const Exp& e) {
    static const double mul=bitsDouble(0x40571547652b82feull),c30=bitsDouble(0x3f862e42fefa39efull),
        c40=bitsDouble(0x3fc5555555555555ull),c50=bitsDouble(0x3fe0000000000000ull);
    const __m128d x=_mm_cvtss_sd(_mm_setzero_pd(),_mm_set_ss(f));
    const __m128i ki=_mm_cvtpd_epi32(_mm_mul_sd(x,_mm_set_sd(mul))); const int k=_mm_cvtsi128_si32(ki);
    const __m128d kd=_mm_cvtepi32_pd(ki);
    const int j=k&63; const uint64_t scale=(static_cast<uint64_t>(static_cast<uint32_t>((k-j)>>6))+0x3ff)<<52;
    const __m128d t=_mm_set_sd(e.table[j]); __m128d p;
    if(e.fma) {
        const __m128d r=_mm_fnmadd_sd(kd,_mm_set_sd(c30),x);
        const __m128d q=_mm_fmadd_sd(r,_mm_set_sd(c40),_mm_set_sd(c50));
        p=_mm_fmadd_sd(q,_mm_mul_sd(r,r),r);
        p=_mm_fmadd_sd(p,t,t);
    } else {
        const __m128d r=_mm_sub_sd(x,_mm_mul_sd(kd,_mm_set_sd(c30)));
        const __m128d q=_mm_add_sd(_mm_mul_sd(_mm_set_sd(c40),r),_mm_set_sd(c50));
        p=_mm_add_sd(_mm_mul_sd(_mm_mul_sd(r,r),q),r);
        p=_mm_add_sd(_mm_mul_sd(p,t),t);
    }
    p=_mm_mul_sd(p,_mm_set_sd(bitsDouble(scale)));
    return _mm_cvtss_f32(_mm_cvtsd_ss(_mm_setzero_ps(),p));
}
static inline float sqrtValve(float x) { return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x))); }
// Valve's VectorNormalize (0x14009e710): returns the length; tiny and huge vectors are rescaled first
// (the tiny branch multiplies by +inf, as Valve's does).
static float normalizeValve(float* v) {
    const float sq=(v[0]*v[0]+v[1]*v[1])+v[2]*v[2];
    const float length=sqrtValve(sq);
    if(!(length>=bitsFloat(0x233877aa))) {
        const float l2=sqrtValve(sq*bitsFloat(0x7f800000));
        if(l2==0.0f || std::isnan(l2)) return 0.0f;
        const float r=1.0f/l2,up=bitsFloat(0x62800000);
        for(int c=0;c<3;++c) v[c]=(r*v[c])*up;
        return l2*bitsFloat(0x1c800000);
    }
    if(!(length>=bitsFloat(0x5bb1a2bc))) {
        const float r=1.0f/length;
        for(int c=0;c<3;++c) v[c]=v[c]*r;
        return length;
    }
    const float s=bitsFloat(0x1c800000),l=sqrtValve(sq*bitsFloat(0x00020000)),r=1.0f/l;
    for(int c=0;c<3;++c) v[c]=(v[c]*s)*r;
    return l*bitsFloat(0x62800000);
}
static inline int32_t floorValve(float x) {
    int32_t i=_mm_cvttss_si32(_mm_set_ss(x));
    if(i!=INT32_MIN && static_cast<float>(i)!=x) i-=std::signbit(x)?1:0;
    return i;
}
struct Entry { int32_t luxel,sample; float w; };
// Valve's sort order: texel ascending, weight descending.
static inline bool before(const Entry& a,const Entry& b) { return a.luxel!=b.luxel ? a.luxel<b.luxel : b.w<a.w; }
// Mirrors MSVC STL's std::sort (Apache-2.0 WITH LLVM-exception) so ties order like Valve's build.
static void insertionSort(Entry* first,Entry* last) {
    if(first==last) return;
    for(Entry* mid=first;++mid!=last;) {
        Entry* hole=mid; const Entry value=*mid;
        if(before(value,*first)) { std::move_backward(first,mid,++hole); *first=value; }
        else { for(Entry* previous=hole;before(value,*--previous);hole=previous) *hole=*previous; *hole=value; }
    }
}
static void median3(Entry* first,Entry* mid,Entry* last) {
    if(before(*mid,*first)) std::swap(*mid,*first);
    if(before(*last,*mid)) { std::swap(*last,*mid); if(before(*mid,*first)) std::swap(*mid,*first); }
}
static void guessMedian(Entry* first,Entry* mid,Entry* last) {
    const ptrdiff_t count=last-first;
    if(40<count) {
        const ptrdiff_t step=(count+1)>>3,twoStep=step<<1;
        median3(first,first+step,first+twoStep); median3(mid-step,mid,mid+step);
        median3(last-twoStep,last-step,last); median3(first+step,mid,last-step);
    } else median3(first,mid,last);
}
static std::pair<Entry*,Entry*> partition(Entry* first,Entry* last) {
    Entry* mid=first+((last-first)>>1);
    guessMedian(first,mid,last-1);
    Entry* pFirst=mid; Entry* pLast=pFirst+1;
    while(first<pFirst && !before(*(pFirst-1),*pFirst) && !before(*pFirst,*(pFirst-1))) --pFirst;
    while(pLast<last && !before(*pLast,*pFirst) && !before(*pFirst,*pLast)) ++pLast;
    Entry* gFirst=pLast; Entry* gLast=pFirst;
    for(;;) {
        for(;gFirst<last;++gFirst) {
            if(before(*pFirst,*gFirst)) continue;
            else if(before(*gFirst,*pFirst)) break;
            else if(pLast!=gFirst) { std::swap(*pLast,*gFirst); ++pLast; }
            else ++pLast;
        }
        for(;first<gLast;--gLast) {
            Entry* const gPrevious=gLast-1;
            if(before(*gPrevious,*pFirst)) continue;
            else if(before(*pFirst,*gPrevious)) break;
            else if(--pFirst!=gPrevious) std::swap(*pFirst,*gPrevious);
        }
        if(gLast==first && gFirst==last) return {pFirst,pLast};
        if(gLast==first) {
            if(pLast!=gFirst) std::swap(*pFirst,*pLast);
            ++pLast; std::swap(*pFirst,*gFirst); ++pFirst; ++gFirst;
        } else if(gFirst==last) {
            if(--gLast!=--pFirst) std::swap(*gLast,*pFirst);
            std::swap(*pFirst,*--pLast);
        } else { std::swap(*gFirst,*--gLast); ++gFirst; }
    }
}
static void pushHeapByIndex(Entry* first,ptrdiff_t hole,ptrdiff_t top,const Entry value) {
    for(ptrdiff_t index=(hole-1)>>1;top<hole && before(first[index],value);index=(hole-1)>>1) { first[hole]=first[index]; hole=index; }
    first[hole]=value;
}
static void popHeapHoleByIndex(Entry* first,ptrdiff_t hole,ptrdiff_t bottom,const Entry value) {
    const ptrdiff_t top=hole,maxNonLeaf=(bottom-1)>>1; ptrdiff_t index=hole;
    while(index<maxNonLeaf) { index=2*index+2; if(before(first[index],first[index-1])) --index; first[hole]=first[index]; hole=index; }
    if(index==maxNonLeaf && bottom%2==0) { first[hole]=first[bottom-1]; hole=bottom-1; }
    pushHeapByIndex(first,hole,top,value);
}
static void heapSort(Entry* first,Entry* last) {
    const ptrdiff_t bottom=last-first;
    for(ptrdiff_t hole=bottom>>1;hole>0;) { --hole; const Entry value=first[hole]; popHeapHoleByIndex(first,hole,bottom,value); }
    for(;last-first>=2;--last) { Entry* const end=last-1; const Entry value=*end; *end=*first; popHeapHoleByIndex(first,0,end-first,value); }
}
static void introsort(Entry* first,Entry* last,ptrdiff_t ideal) {
    for(;;) {
        if(last-first<=32) { insertionSort(first,last); return; }
        if(ideal<=0) { heapSort(first,last); return; }
        const auto mid=partition(first,last);
        ideal=(ideal>>1)+(ideal>>2);
        if(mid.first-first<last-mid.second) { introsort(first,mid.first,ideal); first=mid.second; }
        else { introsort(mid.second,last,ideal); last=mid.first; }
    }
}
struct Sample { float u,v; uint32_t chart; float w40,w44; float n[3]; float t[4]; int32_t fx,fy; uint8_t flags; };
}

// At the hook. Returns true when the steps were done here. Nothing of Valve's changes until the commit,
// so an error before it leaves Valve's code to run.
static bool exactStageAt(CONTEXT& context,bool& changed,bool verify=false) {
    using namespace exact;
    changed=false;
    const ULONGLONG started=GetTickCount64();
    exactStageStep="setup";
    if(!valveBuild || strcmp(valveBuild->sha256,exactStageSha)!=0) throw std::runtime_error("needs the 2026-10-27 Valve build");
    const int32_t count=readValue<int32_t>(address(0x32ad28));
    const uintptr_t samples=pointerAt(0x32ad30);
    if(count<=0 || !samples) return false;
    // Valve's footprint vectors (offsets, indices, cdf) must still be empty.
    for(const uintptr_t vector:{0x32aaf8,0x32acd0,0x32ab50})
        if(readValue<int32_t>(address(vector))!=0) throw std::runtime_error("footprint arrays not empty");
    // RVAs: 1.0 at d5930, 90.0 at d5960, vec3_origin at 32dc08, expf CPU flag at 32f498, expf table at 182070.
    if(readValue<float>(imageBase+0xd5930)!=1.0f || readValue<float>(imageBase+0xd5960)!=90.0f)
        throw std::runtime_error("unverified constants");
    float origin[3]; readMemory(imageBase+0x32dc08,origin,12);
    Exp e{}; e.fma=readValue<int32_t>(imageBase+0x32f498)!=0; readMemory(imageBase+0x182070,e.table,sizeof(e.table));
    if(e.table[0]!=1.0 || e.table[32]!=bitsDouble(0x3ff6a09e667f3bcdull)) throw std::runtime_error("unverified exp table");
    // Sort thread count: g_pThreadPool (RVA d25c0) vtable[8] must be the getter mov eax,[rcx+48]; ret.
    const uintptr_t pool=readValue<uintptr_t>(readValue<uintptr_t>(imageBase+0xd25c0));
    uint8_t getter[4]{}; readMemory(readValue<uintptr_t>(readValue<uintptr_t>(pool)+0x40),getter,4);
    if(getter[0]!=0x8b || getter[1]!=0x41 || getter[2]!=0x48 || getter[3]!=0xc3) throw std::runtime_error("unverified thread pool");
    const uint32_t poolThreads=readValue<uint32_t>(pool+0x48);
    const int32_t x0=readValue<int32_t>(address(0x32aba0)),y0=readValue<int32_t>(address(0x32aba4));
    const int32_t bw=readValue<int32_t>(address(0x32aba8)),bh=readValue<int32_t>(address(0x32abac));
    const int32_t luxels=readValue<int32_t>(address(0x32ab88));
    if(bw<=0 || bh<=0 || luxels!=bw*bh) throw std::runtime_error("unexpected block size");
    if(readValue<int32_t>(address(0x32ac60))<luxels) throw std::runtime_error("luxel info too short");
    exactStageStep="read";
    std::vector<uint32_t> luxelChart(static_cast<size_t>(luxels));
    { const auto info=copyArray(pointerAt(0x32ac68),static_cast<size_t>(luxels),5,"read_exact_luxels");
      for(int32_t l=0;l<luxels;++l) memcpy(&luxelChart[l],info.data()+l*5ull,4); }
    const int32_t charts=readValue<int32_t>(address(0x32ac30));
    const auto chartRecords=copyArray(pointerAt(0x32ac38),static_cast<size_t>(std::max(charts,0)),25,"read_exact_charts");
    requirePhysicalMemory(static_cast<uint64_t>(count)*(sizeof(Sample)+4)+static_cast<uint64_t>(luxels)*64,"exact sample stage");
    std::vector<Sample> sample(static_cast<size_t>(count));
    uint64_t inputHash=0xcbf29ce484222325ull;
    auto hashBytes=[&](const std::vector<uint8_t>& bytes) { if(verify) for(const uint8_t b:bytes) inputHash=(inputHash^b)*0x100000001b3ull; };
    for(int32_t first=0;first<count;first+=4194304) {
        const int32_t n=std::min(4194304,count-first);
        const auto records=copyArray(samples+first*0x48ull,static_cast<size_t>(n),0x48,"read_exact_samples");
        hashBytes(records);
        for(int32_t i=0;i<n;++i) {
            const uint8_t* r=records.data()+i*0x48ull; Sample& s=sample[first+i];
            memcpy(&s.u,r+0x34,4); memcpy(&s.v,r+0x38,4); memcpy(&s.chart,r+0x3c,4); memcpy(&s.w40,r+0x40,4); memcpy(&s.w44,r+0x44,4);
            memcpy(s.n,r+0x0c,12); memcpy(s.t,r+0x24,16);
            if(static_cast<int32_t>(s.chart)<0 || static_cast<int32_t>(s.chart)>=charts) throw std::runtime_error("sample chart out of range");
            s.flags=chartRecords[s.chart*25ull+0x18]; s.fx=floorValve(s.u); s.fy=floorValve(s.v);
        }
    }
    const uintptr_t outputs=pointerAt(0x32ab90);
    std::vector<float> words(static_cast<size_t>(luxels)*11);
    for(int32_t first=0;first<luxels;first+=1048576) {
        const int32_t n=std::min(1048576,luxels-first);
        const auto records=copyArray(outputs+first*0x8cull,static_cast<size_t>(n),0x8c,"read_exact_outputs");
        hashBytes(records);
        for(int32_t q=0;q<n;++q) memcpy(words.data()+(static_cast<size_t>(first)+q)*11,records.data()+q*0x8cull+0x60,44);
    }
    // Valve's window: floor(u)+-2 and floor(v)+-2 (one column / row if chart flag bit 0 / 1), int32 wraparound,
    // clamped to the block.
    auto window=[&](const Sample& s,int32_t& xa,int32_t& xb,int32_t& ya,int32_t& yb) {
        xa=s.flags&1 ? s.fx : static_cast<int32_t>(static_cast<uint32_t>(s.fx)-2u); xb=s.flags&1 ? s.fx : static_cast<int32_t>(static_cast<uint32_t>(s.fx)+2u);
        ya=s.flags&2 ? s.fy : static_cast<int32_t>(static_cast<uint32_t>(s.fy)-2u); yb=s.flags&2 ? s.fy : static_cast<int32_t>(static_cast<uint32_t>(s.fy)+2u);
        xa=std::max(xa,x0); xb=std::min(xb,x0+bw-1); ya=std::max(ya,y0); yb=std::min(yb,y0+bh-1);
    };
    // Valve's kernel: 0.7978846*expf(-2*d2^2), d2 <= 4.
    auto kernel=[&](const Sample& s,int32_t tx,int32_t ty)->float {
        const float du=s.u-(static_cast<float>(tx)+0.5f),dv=s.v-(static_cast<float>(ty)+0.5f),d2=dv*dv+du*du;
        return d2<=4.0f ? expValve((d2*d2)*-2.0f,e)*0.7978846f : 0.0f;
    };
    const ULONGLONG readDone=GetTickCount64();
    exactStageStep="normals";
    const int64_t gx0=static_cast<int64_t>(x0)-2,gy0=static_cast<int64_t>(y0)-2,gw=bw+4ll,gh=bh+4ll;
    auto bucketOf=[&](const Sample& s)->int64_t {
        const int64_t bx=s.fx-gx0,by=s.fy-gy0;
        return bx<0 || by<0 || bx>=gw || by>=gh ? -1 : by*gw+bx;
    };
    std::vector<uint32_t> bucketStart(static_cast<size_t>(gw*gh)+1,0),bucketed;
    for(const auto& s:sample) { const int64_t b=bucketOf(s); if(b>=0) ++bucketStart[b+1]; }
    for(size_t b=1;b<bucketStart.size();++b) bucketStart[b]+=bucketStart[b-1];
    bucketed.resize(bucketStart.back());
    { std::vector<uint32_t> cursor(bucketStart.begin(),bucketStart.end()-1);
      for(uint32_t i=0;i<static_cast<uint32_t>(count);++i) { const int64_t b=bucketOf(sample[i]); if(b>=0) bucketed[cursor[b]++]=i; } }
    std::vector<int64_t> offsets(static_cast<size_t>(luxels)+1,0);
    stageParallel(static_cast<uint32_t>(luxels),[&](uint32_t a,uint32_t b) {
        std::vector<std::pair<uint32_t,float>> list;
        for(uint32_t l=a;l<b;++l) {
            const int32_t lx=static_cast<int32_t>(l%bw),ly=static_cast<int32_t>(l/bw),tx=x0+lx,ty=y0+ly;
            const uint32_t chart=luxelChart[l];
            list.clear();
            for(int64_t by=ty-gy0-2;by<=ty-gy0+2;++by) for(int64_t bx=tx-gx0-2;bx<=tx-gx0+2;++bx) {
                if(bx<0 || by<0 || bx>=gw || by>=gh) continue;
                const size_t bucket=static_cast<size_t>(by*gw+bx);
                for(uint32_t j=bucketStart[bucket];j<bucketStart[bucket+1];++j) {
                    const uint32_t i=bucketed[j]; const Sample& s=sample[i];
                    if(s.chart!=chart) continue;
                    int32_t xa,xb,ya,yb; window(s,xa,xb,ya,yb);
                    if(tx<xa || tx>xb || ty<ya || ty>yb) continue;
                    const float k=kernel(s,tx,ty);
                    if(k!=0.0f) list.push_back({i,k});
                }
            }
            std::sort(list.begin(),list.end(),[](const auto& p,const auto& r) { return p.first<r.first; });
            offsets[l+1]=static_cast<int64_t>(list.size());
            float* w=words.data()+static_cast<size_t>(l)*11;
            for(const auto& [i,k]:list) {
                const Sample& s=sample[i];
                for(int c=0;c<3;++c) w[c]=w[c]+k*s.n[c];
                for(int c=0;c<4;++c) w[4+c]=w[4+c]+k*s.t[c];
                w[9]=w[9]+k;
            }
            const float sum=w[9];
            if(sum==0.0f || std::isnan(sum)) { memcpy(w,origin,12); for(int c=4;c<8;++c) w[c]=0.0f; continue; }
            const float r=1.0f/sum;
            for(int c=0;c<3;++c) w[c]=r*w[c];
            for(int c=4;c<8;++c) w[c]=r*w[c];
            const float L=normalizeValve(w);
            if(L==0.0f || std::isnan(L)) memcpy(w,origin,12);
            const float Lt=sqrtValve((w[4]*w[4]+w[5]*w[5])+w[6]*w[6]);
            w[3]=sqrtValve((1.0f-L)/L);
            w[8]=sqrtValve((1.0f-Lt)/Lt);
            const float sign=w[7];
            normalizeValve(w+4);
            w[7]=0.0f<=sign ? 1.0f : -1.0f;
            const int32_t degrees=_mm_cvttss_si32(_mm_set_ss((sign*sign)*90.0f));
            w[8]=static_cast<float>(static_cast<int32_t>(90u-static_cast<uint32_t>(degrees)))+w[8];
        }
    });
    for(size_t l=1;l<offsets.size();++l) offsets[l]+=offsets[l-1];
    const int64_t entries=offsets.back();
    if(entries>INT32_MAX) throw std::runtime_error("too many footprint entries");
    const ULONGLONG normalsDone=GetTickCount64();
    exactStageStep="map";
    requirePhysicalMemory(static_cast<uint64_t>(entries)*sizeof(Entry),"exact sample stage entries");
    std::vector<int64_t> sampleFirst(static_cast<size_t>(count)+1,0);
    auto forEntries=[&](uint32_t i,auto&& use) {
        const Sample& s=sample[i]; int32_t xa,xb,ya,yb; window(s,xa,xb,ya,yb);
        for(int32_t ty=ya;ty<=yb;++ty) for(int32_t tx=xa;tx<=xb;++tx) {
            const int32_t l=(ty-y0)*bw-x0+tx;
            const float k=kernel(s,tx,ty);
            if(k!=0.0f && luxelChart[l]==s.chart) use(l,k);
        }
    };
    stageParallel(static_cast<uint32_t>(count),[&](uint32_t a,uint32_t b) {
        for(uint32_t i=a;i<b;++i) { int64_t n=0; forEntries(i,[&](int32_t,float) { ++n; }); sampleFirst[i+1]=n; }
    });
    for(size_t i=1;i<sampleFirst.size();++i) sampleFirst[i]+=sampleFirst[i-1];
    if(sampleFirst.back()!=entries) throw std::runtime_error("map and normals disagree on the entries");
    std::vector<Entry> list(static_cast<size_t>(entries));
    stageParallel(static_cast<uint32_t>(count),[&](uint32_t a,uint32_t b) {
        for(uint32_t i=a;i<b;++i) {
            Entry* out=list.data()+sampleFirst[i]; const Sample& s=sample[i];
            forEntries(i,[&](int32_t l,float k) { *out++={l,static_cast<int32_t>(i),(k*s.w40)*s.w44}; });
        }
    });
    std::vector<int64_t>().swap(sampleFirst);
    for(const Entry& entry:list) if(std::isnan(entry.w)) throw std::runtime_error("NaN footprint weight");
    const ULONGLONG mapDone=GetTickCount64();
    exactStageStep="sort";
    // Valve sorts P chunks of E/P entries (the last takes the rest), P = next power of two above the pool's
    // thread count, each with std::sort; the chunks are then merged stably.
    uint32_t smear=poolThreads; smear|=smear>>1; smear|=smear>>2; smear|=smear>>4; smear|=smear>>8; smear|=smear>>16;
    const int32_t parts=static_cast<int32_t>(smear+1),total=static_cast<int32_t>(entries);
    std::vector<int32_t> chunkStart;
    if(total<=parts) chunkStart={0,total};
    else {
        const int32_t size=total/parts;
        for(int32_t c=0;c<parts;++c) chunkStart.push_back(c*size);
        chunkStart.push_back(total);
    }
    const uint32_t chunks=static_cast<uint32_t>(chunkStart.size()-1);
    stageParallel(chunks,[&](uint32_t a,uint32_t b) {
        for(uint32_t c=a;c<b;++c) introsort(list.data()+chunkStart[c],list.data()+chunkStart[c+1],chunkStart[c+1]-chunkStart[c]);
    });
    const ULONGLONG sortDone=GetTickCount64();
    exactStageStep="lookup";
    StageAllocations allocations;
    uintptr_t offsetsAt=0,indicesAt=0,cdfAt=0;
    std::vector<int32_t> offsets32(offsets.size());
    for(size_t l=0;l<offsets.size();++l) offsets32[l]=static_cast<int32_t>(offsets[l]);
    if(verify) {
        exactExpected=ExactExpected{};
        exactExpected.indices.resize(static_cast<size_t>(entries)); exactExpected.cdf.resize(static_cast<size_t>(entries));
    } else {
        offsetsAt=allocations.add<int32_t>(offsets.size(),"Allocate exact offsets");
        indicesAt=allocations.add<int32_t>(static_cast<size_t>(entries),"Allocate exact indices");
        cdfAt=allocations.add<float>(static_cast<size_t>(entries),"Allocate exact cdf");
        stageWrite(offsetsAt,offsets32.data(),offsets32.size()*4,"Write exact offsets");
    }
    std::vector<int32_t> bandIndices; std::vector<float> bandCdf;
    for(int32_t bandFirst=0;bandFirst<luxels;) {
        int32_t bandEnd=bandFirst+1;
        while(bandEnd<luxels && offsets[bandEnd+1]-offsets[bandFirst]<=(16<<20)) ++bandEnd;
        const int64_t base=offsets[bandFirst];
        bandIndices.assign(static_cast<size_t>(offsets[bandEnd]-base),0); bandCdf.assign(bandIndices.size(),0.0f);
        stageParallel(static_cast<uint32_t>(bandEnd-bandFirst),[&](uint32_t a,uint32_t b) {
            const int32_t la=bandFirst+static_cast<int32_t>(a),lb=bandFirst+static_cast<int32_t>(b);
            std::vector<const Entry*> at(chunks),end(chunks); std::vector<Entry> run;
            for(uint32_t c=0;c<chunks;++c) {
                end[c]=list.data()+chunkStart[c+1];
                at[c]=std::lower_bound(static_cast<const Entry*>(list.data()+chunkStart[c]),end[c],la,[](const Entry& entry,int32_t l) { return entry.luxel<l; });
            }
            for(int32_t l=la;l<lb;++l) {
                run.clear();
                for(uint32_t c=0;c<chunks;++c) while(at[c]<end[c] && at[c]->luxel==l) run.push_back(*at[c]++);
                if(static_cast<int64_t>(run.size())!=offsets[l+1]-offsets[l]) throw std::runtime_error("sorted entries disagree");
                std::stable_sort(run.begin(),run.end(),[](const Entry& p,const Entry& r) { return r.w<p.w; });
                int32_t* indices=bandIndices.data()+(offsets[l]-base); float* cdf=bandCdf.data()+(offsets[l]-base);
                const size_t n=run.size();
                float sum=0.0f;
                for(size_t j=0;j<n;++j) { indices[j]=run[j].sample; cdf[j]=run[j].w; sum=sum+run[j].w; }
                if(0.0f<sum) {
                    float running=0.0f;
                    for(size_t j=0;j+1<n;++j) { running=running+cdf[j]; cdf[j]=running/sum; }
                    cdf[n-1]=1.0f;
                }
                float& confidence=words[static_cast<size_t>(l)*11+10];
                float acc=confidence,previous=0.0f;
                for(size_t j=0;j<n;++j) { acc=(cdf[j]-previous)*sample[indices[j]].w40+acc; previous=cdf[j]; }
                if(0.0f<=acc) { if(1.0f<=acc) acc=1.0f; } else acc=0.0f;
                confidence=acc;
            }
        });
        if(verify) {
            std::copy(bandIndices.begin(),bandIndices.end(),exactExpected.indices.begin()+base);
            std::copy(bandCdf.begin(),bandCdf.end(),exactExpected.cdf.begin()+base);
        } else {
            stageWrite(indicesAt+static_cast<uintptr_t>(base)*4,bandIndices.data(),bandIndices.size()*4,"Write exact indices");
            stageWrite(cdfAt+static_cast<uintptr_t>(base)*4,bandCdf.data(),bandCdf.size()*4,"Write exact cdf");
        }
        bandFirst=bandEnd;
    }
    const ULONGLONG lookupDone=GetTickCount64();
    if(verify) {
        exactExpected.words=std::move(words); exactExpected.offsets=std::move(offsets32);
        exactExpected.luxels=luxels; exactExpected.inputHash=inputHash;
        std::cout<<"GPU wrapper exact verify: computed samples="<<count<<" entries="<<entries<<" input_hash="<<std::hex<<inputHash<<std::dec
            <<" stage_ms="<<lookupDone-started<<'\n'<<std::flush;
        return false;
    }
    exactStageStep="commit";
    for(int32_t first=0;first<luxels;first+=1048576) {
        const int32_t n=std::min(1048576,luxels-first);
        auto records=copyArray(outputs+first*0x8cull,static_cast<size_t>(n),0x8c,"read_exact_outputs");
        for(int32_t q=0;q<n;++q) memcpy(records.data()+q*0x8cull+0x60,words.data()+(static_cast<size_t>(first)+q)*11,44);
        changed=true;
        stageWrite(outputs+first*0x8cull,records.data(),records.size(),"Write exact outputs");
    }
    allocations.owned.clear();
    stageVector(0x32aaf8,offsetsAt,static_cast<int32_t>(offsets.size()));
    stageVector(0x32acd0,indicesAt,static_cast<int32_t>(entries));
    stageVector(0x32ab50,cdfAt,static_cast<int32_t>(entries));
    // Empty Valve's Map list local and restore the callee-saved registers the skipped code would have
    // restored (frame offset and saved registers are per build).
    const uint8_t empty[24]{}; stageWrite(context.Rbp+valveBuild->stageMapLocal,empty,24,"Clear sample stage local");
    for(const SavedRegister& saved:valveBuild->stageRestores) {
        if(saved.gpr) readMemory(context.Rsp+saved.rsp,&(context.*saved.gpr),8);
        else readMemory(context.Rsp+saved.rsp,&(&context.Xmm0)[saved.xmm],16);
    }
    context.Rip=address(sampleStageEndRva);
    const ULONGLONG done=GetTickCount64();
    std::ostringstream line;
    line<<"GPU wrapper exact sample stage: samples="<<count<<" entries="<<entries<<" chunks="<<chunks<<(e.fma?" fma":"")
        <<" read_ms="<<readDone-started<<" normals_ms="<<normalsDone-readDone<<" map_ms="<<mapDone-normalsDone
        <<" sort_ms="<<sortDone-mapDone<<" lookup_ms="<<lookupDone-sortDone<<" stage_ms="<<done-started;
    std::cout<<line.str()<<'\n'<<std::flush;
    exactStageSummary="exact entries="+std::to_string(entries)+" stage_ms="+std::to_string(done-started);
    return true;
}
static void exactVerifyAt() {
    ExactExpected expected=std::move(exactExpected); exactExpected=ExactExpected{};
    if(expected.luxels<=0) { std::cout<<"GPU wrapper exact verify: nothing computed\n"<<std::flush; return; }
    const int32_t luxels=expected.luxels;
    const int32_t offsetCount=readValue<int32_t>(address(0x32aaf8)),indexCount=readValue<int32_t>(address(0x32acd0));
    const int32_t cdfCount=readValue<int32_t>(address(0x32ab50));
    uint64_t wordDiff[11]{},offsetDiff=0,indexDiff=0,cdfDiff=0,luxelDiff=0; int64_t firstLuxel=-1,firstEntry=-1;
    const uintptr_t outputs=pointerAt(0x32ab90);
    for(int32_t first=0;first<luxels;first+=1048576) {
        const int32_t n=std::min(1048576,luxels-first);
        const auto records=copyArray(outputs+first*0x8cull,static_cast<size_t>(n),0x8c,"read_verify_outputs");
        for(int32_t q=0;q<n;++q) {
            const uint8_t* got=records.data()+q*0x8cull+0x60;
            const float* want=expected.words.data()+(static_cast<size_t>(first)+q)*11;
            bool any=false;
            for(int c=0;c<11;++c) if(memcmp(got+c*4,want+c,4)) { ++wordDiff[c]; any=true; }
            if(any) { ++luxelDiff; if(firstLuxel<0) firstLuxel=first+q; }
        }
    }
    const bool shapes=offsetCount==static_cast<int32_t>(expected.offsets.size())
        && indexCount==static_cast<int32_t>(expected.indices.size()) && cdfCount==indexCount;
    if(shapes) {
        const auto offsets=copyArray(pointerAt(0x32aaf8+8),expected.offsets.size(),4,"read_verify_offsets");
        for(size_t l=0;l<expected.offsets.size();++l) if(memcmp(offsets.data()+l*4,&expected.offsets[l],4)) ++offsetDiff;
        for(int64_t first=0;first<indexCount;first+=16777216) {
            const size_t n=static_cast<size_t>(std::min<int64_t>(16777216,indexCount-first));
            const auto indices=copyArray(pointerAt(0x32acd0+8)+first*4,n,4,"read_verify_indices");
            const auto cdf=copyArray(pointerAt(0x32ab50+8)+first*4,n,4,"read_verify_cdf");
            for(size_t j=0;j<n;++j) {
                const bool indexSame=!memcmp(indices.data()+j*4,&expected.indices[first+j],4);
                const bool cdfSame=!memcmp(cdf.data()+j*4,&expected.cdf[first+j],4);
                if(!indexSame) ++indexDiff;
                if(!cdfSame) ++cdfDiff;
                if((!indexSame || !cdfSame) && firstEntry<0) firstEntry=first+static_cast<int64_t>(j);
            }
        }
    }
    std::ostringstream line;
    line<<"GPU wrapper exact verify: luxels="<<luxels<<" input_hash="<<std::hex<<expected.inputHash<<std::dec
        <<" shapes="<<(shapes?"same":"differ")<<" valve_offsets="<<offsetCount<<" valve_entries="<<indexCount
        <<" our_entries="<<expected.indices.size()<<" luxels_differ="<<luxelDiff<<" first_luxel="<<firstLuxel<<" words_differ=";
    for(int c=0;c<11;++c) line<<(c?",":"")<<wordDiff[c];
    line<<" offsets_differ="<<offsetDiff<<" indices_differ="<<indexDiff<<" cdf_differ="<<cdfDiff<<" first_entry="<<firstEntry;
    std::cout<<line.str()<<'\n'<<std::flush;
    const bool same=shapes && luxelDiff+offsetDiff+indexDiff+cdfDiff==0;
    exactStageSummary=std::string("exact verify: ")+(same?"same":"differ");
}
#endif
