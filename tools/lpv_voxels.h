// Builds the light probe volume voxel (solid cell) file that Valve's compile gets from worldrendererbuilder.
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <deque>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// Probe volume voxel file (made by Valve's CUtlGrid::Voxelize in a full compile): one bit per cell,
// cell = x + X*(y + Y*z), set = solid; padding bits of the last word are set.
struct VoxelVolume {
    uint32_t cells[3]{};
    float box[6]{};
    float matrix[12]{};
    bool flood=false;
};
struct VoxelScene { std::vector<float> corners; std::vector<uint32_t> flags; };
struct VoxelResult { std::vector<uint32_t> words; uint64_t solid=0; std::string note; };

namespace lpvVoxels {
// Valve's constants: rays start this far before the box's min plane; triangle flag bits each pass skips;
// flags 3 (translucent | two-sided) mean no interior; column axes per ray axis as Valve nests them.
constexpr double kRayBack=28377.92;
constexpr uint16_t kLightingMask=0xc0b0,kBarrierMask=0x400;
constexpr uint16_t kOpenFlags=3;
constexpr int kOuter[3]={2,2,1},kInner[3]={1,0,0};

static double radicalInverse(uint32_t base,uint32_t index) {
    double scale=1,value=0;
    while(index) { scale/=base; value+=scale*(index%base); index/=base; }
    return value;
}
struct Triangle { double p[3][3]; uint32_t flags; bool front[3]; bool upward; };
struct Bits {
    uint32_t n[3]; std::vector<uint32_t> words;
    explicit Bits(const uint32_t cells[3]) : n{cells[0],cells[1],cells[2]},words((static_cast<size_t>(cells[0])*cells[1]*cells[2]+31)/32,0) {}
    size_t index(uint32_t x,uint32_t y,uint32_t z) const { return x+static_cast<size_t>(n[0])*(y+static_cast<size_t>(n[1])*z); }
    void set(size_t i) { words[i>>5]|=1u<<(i&31); }
    void clear(size_t i) { words[i>>5]&=~(1u<<(i&31)); }
    bool get(size_t i) const { return (words[i>>5]>>(i&31))&1u; }
};
struct Hit { double depth; bool front,closed; };

static void voxelize(const std::vector<Triangle>& all,uint16_t mask,bool floors,const double span[3],unsigned threads,Bits& out) {
    std::vector<uint32_t> chosen;
    for(uint32_t t=0;t<all.size();++t) if(!(all[t].flags&mask)) chosen.push_back(t);
    std::vector<std::pair<double,double>> jitter={{0,0}};
    // Valve runs 16 jobs: 0..2 unjittered, j >= 3 shifted by radical inverse base 2/3 of (j - 2), minus 0.5 cells.
    for(uint32_t j=3;j<16;++j) jitter.push_back({radicalInverse(2,j-2)-0.5,radicalInverse(3,j-2)-0.5});
    const uint32_t* n=out.n;
    for(int axis=0;axis<3;++axis) {
        const int o=kOuter[axis],i=kInner[axis];
        const uint32_t columnsO=n[o],columnsI=n[i],depth=n[axis];
        auto range=[&](const Triangle& t,int a,uint32_t count,int64_t& first,int64_t& last) {
            const double lo=std::min({t.p[0][a],t.p[1][a],t.p[2][a]}),hi=std::max({t.p[0][a],t.p[1][a],t.p[2][a]});
            first=std::max<int64_t>(0,static_cast<int64_t>(std::floor(lo))-1);
            last=std::min<int64_t>(static_cast<int64_t>(count)-1,static_cast<int64_t>(std::floor(hi)));
        };
        std::vector<uint32_t> start(static_cast<size_t>(columnsO)*columnsI+1,0),binned;
        for(int fill=0;fill<2;++fill) {
            std::vector<uint32_t> cursor;
            if(fill) { for(size_t c=1;c<start.size();++c) start[c]+=start[c-1]; cursor.assign(start.begin(),start.end()-1); binned.resize(start.back()); }
            for(uint32_t t:chosen) {
                const Triangle& tri=all[t];
                if(std::max({tri.p[0][axis],tri.p[1][axis],tri.p[2][axis]})<-span[axis] ||
                   std::min({tri.p[0][axis],tri.p[1][axis],tri.p[2][axis]})>span[axis]) continue;
                int64_t o0,o1,i0,i1; range(tri,o,columnsO,o0,o1); range(tri,i,columnsI,i0,i1);
                for(int64_t co=o0;co<=o1;++co) for(int64_t ci=i0;ci<=i1;++ci) {
                    const size_t column=static_cast<size_t>(co)*columnsI+static_cast<size_t>(ci);
                    if(fill) binned[cursor[column]++]=t; else ++start[column+1];
                }
            }
        }
        std::atomic<size_t> next{0};
        std::vector<Bits> partial(threads,Bits(n));
        auto work=[&](unsigned worker) {
            Bits& bits=partial[worker]; std::vector<Hit> hits;
            for(size_t column;(column=next++)<static_cast<size_t>(columnsO)*columnsI;) {
                const uint32_t co=static_cast<uint32_t>(column/columnsI),ci=static_cast<uint32_t>(column%columnsI);
                for(size_t j=0;j<jitter.size();++j) {
                    const double u=co+0.5+jitter[j].first,v=ci+0.5+jitter[j].second;
                    hits.clear();
                    for(uint32_t k=start[column];k<start[column+1];++k) {
                        const Triangle& t=all[binned[k]];
                        const double ax=t.p[0][o]-u,ay=t.p[0][i]-v,bx=t.p[1][o]-u,by=t.p[1][i]-v,cx=t.p[2][o]-u,cy=t.p[2][i]-v;
                        const double e0=bx*cy-cx*by,e1=cx*ay-ax*cy,e2=ax*by-bx*ay,area=e0+e1+e2;
                        if(area==0 || !((e0>=0 && e1>=0 && e2>=0) || (e0<=0 && e1<=0 && e2<=0))) continue;
                        const double d=t.p[0][axis]+(e1*(t.p[1][axis]-t.p[0][axis])+e2*(t.p[2][axis]-t.p[0][axis]))/area;
                        if(d<-span[axis] || d>span[axis]) continue;
                        hits.push_back({d,t.front[axis],!(t.flags&kOpenFlags)});
                    }
                    std::sort(hits.begin(),hits.end(),[](const Hit& a,const Hit& b) { return a.depth<b.depth; });
                    auto cell=[&](uint32_t k) {
                        uint32_t c[3]; c[axis]=k; c[o]=co; c[i]=ci;
                        bits.set(bits.index(c[0],c[1],c[2]));
                    };
                    for(size_t h=0;h<hits.size();++h) {
                        const double k=std::floor(hits[h].depth);
                        if(k>=0 && k<depth) cell(static_cast<uint32_t>(k));
                        if(h+1<hits.size() && hits[h].front && !hits[h+1].front && hits[h].closed && hits[h+1].closed) {
                            const int64_t from=std::max<int64_t>(0,static_cast<int64_t>(k)+1);
                            const int64_t to=std::min<int64_t>(depth,static_cast<int64_t>(std::floor(hits[h+1].depth)));
                            for(int64_t z=from;z<to;++z) cell(static_cast<uint32_t>(z));
                        }
                    }
                }
            }
        };
        std::vector<std::thread> pool;
        for(unsigned w=1;w<threads;++w) pool.emplace_back(work,w);
        work(0);
        for(auto& thread:pool) thread.join();
        for(const Bits& bits:partial) for(size_t w=0;w<out.words.size();++w) out.words[w]|=bits.words[w];
        if(floors && axis==2) {
            for(uint32_t co=0;co<columnsO;++co) for(uint32_t ci=0;ci<columnsI;++ci) {
                const size_t column=static_cast<size_t>(co)*columnsI+ci;
                const double u=co+0.5,v=ci+0.5;
                double best=depth; const Triangle* first=nullptr;
                for(uint32_t k=start[column];k<start[column+1];++k) {
                    const Triangle& t=all[binned[k]];
                    const double ax=t.p[0][o]-u,ay=t.p[0][i]-v,bx=t.p[1][o]-u,by=t.p[1][i]-v,cx=t.p[2][o]-u,cy=t.p[2][i]-v;
                    const double e0=bx*cy-cx*by,e1=cx*ay-ax*cy,e2=ax*by-bx*ay,area=e0+e1+e2;
                    if(area==0 || !((e0>=0 && e1>=0 && e2>=0) || (e0<=0 && e1<=0 && e2<=0))) continue;
                    const double d=t.p[0][axis]+(e1*(t.p[1][axis]-t.p[0][axis])+e2*(t.p[2][axis]-t.p[0][axis]))/area;
                    if(d>=-span[axis] && d<=best) { best=d; first=&t; }
                }
                if(!first || !first->upward) continue;
                const int64_t top=std::min<int64_t>(depth,static_cast<int64_t>(std::floor(best)));
                for(int64_t z=0;z<top;++z) { uint32_t c[3]; c[axis]=static_cast<uint32_t>(z); c[o]=co; c[i]=ci; out.set(out.index(c[0],c[1],c[2])); }
            }
        }
    }
}
}

static VoxelResult voxelizeVolume(const VoxelScene& scene,const VoxelVolume& volume,unsigned threads) {
    using namespace lpvVoxels;
    const uint32_t* n=volume.cells;
    for(int a=0;a<3;++a) if(!n[a] || n[a]>1024 || !(volume.box[3+a]>volume.box[a])) throw std::runtime_error("Invalid probe volume box");
    const uint64_t total=static_cast<uint64_t>(n[0])*n[1]*n[2];
    if(total>(1ull<<27)) throw std::runtime_error("Probe volume too large to voxelize");
    const float* m=volume.matrix;
    const double a00=m[0],a01=m[1],a02=m[2],a10=m[4],a11=m[5],a12=m[6],a20=m[8],a21=m[9],a22=m[10];
    const double det=a00*(a11*a22-a12*a21)-a01*(a10*a22-a12*a20)+a02*(a10*a21-a11*a20);
    if(!std::isfinite(det) || std::fabs(det)<1e-12) throw std::runtime_error("Probe volume transform not invertible");
    const double inv[9]={(a11*a22-a12*a21)/det,(a02*a21-a01*a22)/det,(a01*a12-a02*a11)/det,
                         (a12*a20-a10*a22)/det,(a00*a22-a02*a20)/det,(a02*a10-a00*a12)/det,
                         (a10*a21-a11*a20)/det,(a01*a20-a00*a21)/det,(a00*a11-a01*a10)/det};
    double size[3],span[3];
    for(int a=0;a<3;++a) {
        size[a]=(static_cast<double>(volume.box[3+a])-volume.box[a])/n[a];
        span[a]=kRayBack/size[a];
    }
    const size_t count=scene.flags.size();
    if(scene.corners.size()!=count*9) throw std::runtime_error("Voxel scene arrays disagree");
    std::vector<Triangle> triangles; triangles.reserve(count);
    for(size_t t=0;t<count;++t) {
        const float* w=scene.corners.data()+t*9;
        Triangle tri{}; tri.flags=scene.flags[t];
        for(int c=0;c<3;++c) {
            const double d[3]={w[c*3]-static_cast<double>(m[3]),w[c*3+1]-static_cast<double>(m[7]),w[c*3+2]-static_cast<double>(m[11])};
            for(int a=0;a<3;++a) tri.p[c][a]=((inv[a*3]*d[0]+inv[a*3+1]*d[1]+inv[a*3+2]*d[2])-volume.box[a])/size[a];
        }
        bool usable=true;
        for(int c=0;c<3;++c) for(int a=0;a<3;++a) usable=usable && std::isfinite(tri.p[c][a]) && std::fabs(tri.p[c][a])<1e9;
        if(!usable) continue;
        const double e1[3]={static_cast<double>(w[3])-w[0],static_cast<double>(w[4])-w[1],static_cast<double>(w[5])-w[2]};
        const double e2[3]={static_cast<double>(w[6])-w[0],static_cast<double>(w[7])-w[1],static_cast<double>(w[8])-w[2]};
        const double normal[3]={e1[1]*e2[2]-e1[2]*e2[1],e1[2]*e2[0]-e1[0]*e2[2],e1[0]*e2[1]-e1[1]*e2[0]};
        if(!std::isfinite(normal[0]+normal[1]+normal[2])) continue;
        for(int a=0;a<3;++a) tri.front[a]=m[a]*normal[0]+m[4+a]*normal[1]+m[8+a]*normal[2]<=0;
        tri.upward=normal[2]>=0;
        triangles.push_back(tri);
    }
    threads=std::max(1u,std::min(threads,64u));
    Bits bits(n);
    voxelize(triangles,kLightingMask,true,span,threads,bits);
    VoxelResult result;
    if(volume.flood) {
        int64_t centre[3];
        for(int a=0;a<3;++a) centre[a]=std::clamp<int64_t>(static_cast<int64_t>(std::floor((0.0-volume.box[a])/size[a])),0,n[a]-1);
        const int step[6][3]={{1,0,0},{0,1,0},{0,0,1},{-1,0,0},{0,-1,0},{0,0,-1}};
        std::vector<uint8_t> seen(total,0); std::deque<std::array<int64_t,3>> queue;
        auto at=[&](const std::array<int64_t,3>& c) { return bits.index(static_cast<uint32_t>(c[0]),static_cast<uint32_t>(c[1]),static_cast<uint32_t>(c[2])); };
        queue.push_back({centre[0],centre[1],centre[2]}); seen[at(queue.front())]=1;
        int64_t seed=-1;
        while(!queue.empty()) {
            const auto c=queue.front(); queue.pop_front();
            if(!bits.get(at(c))) { seed=static_cast<int64_t>(at(c)); break; }
            for(const auto& s:step) {
                std::array<int64_t,3> next{c[0]+s[0],c[1]+s[1],c[2]+s[2]};
                // Seed search: nearest empty cell to the volume origin within |dx|+|dy|+dz <= 8 (dz is not abs'd, as in Valve's code).
                bool inside=true; for(int a=0;a<3;++a) inside=inside && next[a]>=0 && next[a]<n[a];
                if(!inside || std::llabs(next[0]-centre[0])+std::llabs(next[1]-centre[1])+(next[2]-centre[2])>8 || seen[at(next)]) continue;
                seen[at(next)]=1; queue.push_back(next);
            }
        }
        if(seed<0) result.note="flood fill skipped: no empty space near the volume's origin";
        else {
            voxelize(triangles,kBarrierMask,false,span,threads,bits);
            Bits reached(n); std::fill(reached.words.begin(),reached.words.end(),0xffffffffu);
            std::vector<size_t> stack={static_cast<size_t>(seed)}; reached.clear(static_cast<size_t>(seed));
            while(!stack.empty()) {
                const size_t cell=stack.back(); stack.pop_back();
                const int64_t c[3]={static_cast<int64_t>(cell%n[0]),static_cast<int64_t>(cell/n[0]%n[1]),static_cast<int64_t>(cell/(static_cast<size_t>(n[0])*n[1]))};
                for(const auto& s:step) {
                    const int64_t x=c[0]+s[0],y=c[1]+s[1],z=c[2]+s[2];
                    if(x<0 || y<0 || z<0 || x>=n[0] || y>=n[1] || z>=n[2]) continue;
                    const size_t next=bits.index(static_cast<uint32_t>(x),static_cast<uint32_t>(y),static_cast<uint32_t>(z));
                    if(bits.get(next) || !reached.get(next)) continue;
                    reached.clear(next); stack.push_back(next);
                }
            }
            bits.words=std::move(reached.words);
        }
    }
    // Padding bits of the last word are solid.
    if(total%32) bits.words.back()|=~((1u<<(total%32))-1);
    // An all-solid volume keeps its centre cell empty.
    if(std::all_of(bits.words.begin(),bits.words.end(),[](uint32_t w) { return w==0xffffffffu; })) {
        uint32_t c[3]; for(int a=0;a<3;++a) c[a]=n[a]/2;
        bits.clear(bits.index(c[0],c[1],c[2]));
    }
    for(uint64_t cell=0;cell<total;++cell) result.solid+=bits.get(static_cast<size_t>(cell));
    result.words=std::move(bits.words);
    return result;
}
