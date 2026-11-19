// Corrects Hammer's preview scene (skins, Render Color, uvs) to match the scene a full compile bakes.
#pragma once
#include "resource_files.h"
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <sstream>

// meshoptimizer codecs for model buffers: vertex stream header byte 0xa0, index stream 0xe0 (v0) or 0xe1 (v1).
static std::vector<uint8_t> meshoptVertices(const uint8_t* d,size_t size,size_t count,size_t stride) {
    const size_t tail=std::max<size_t>(stride,32);
    if(!stride || stride>256 || stride%4 || size<1+tail || (d[0]&0xf0)!=0xa0 || (d[0]&0x0f)!=0) throw std::runtime_error("vertex codec");
    std::vector<uint8_t> out(count*stride),transposed;
    uint8_t last[256]; memcpy(last,d+size-stride,stride);
    const size_t block=std::min<size_t>((8192/stride)&~size_t(15),256),end=size-tail;
    uint8_t bytes[256]; size_t p=1;
    transposed.resize(block*stride);
    for(size_t offset=0;offset<count;offset+=block) {
        const size_t n=std::min(block,count-offset),aligned=(n+15)&~size_t(15);
        for(size_t k=0;k<stride;++k) {
            const size_t headerSize=(aligned/16+3)/4;
            if(headerSize>end-p) throw std::runtime_error("vertex codec data");
            const uint8_t* header=d+p; p+=headerSize;
            for(size_t i=0;i<aligned;i+=16) {
                const size_t g=i/16; const int mode=(header[g/4]>>((g%4)*2))&3; uint8_t* group=bytes+i;
                if(mode==0) { memset(group,0,16); continue; }
                if(mode==3) { if(16>end-p) throw std::runtime_error("vertex codec data"); memcpy(group,d+p,16); p+=16; continue; }
                const int bits=mode==1?2:4,packed=mode==1?4:8,escape=(1<<bits)-1;
                if(size_t(packed)>end-p) throw std::runtime_error("vertex codec data");
                size_t extra=p+packed;
                for(int j=0,o=0;j<packed;++j) for(int shift=8-bits;shift>=0;shift-=bits,++o) {
                    const int value=(d[p+j]>>shift)&escape;
                    if(value!=escape) { group[o]=uint8_t(value); continue; }
                    if(extra>=end) throw std::runtime_error("vertex codec data");
                    group[o]=d[extra++];
                }
                p=extra;
            }
            uint8_t v=last[k];
            for(size_t i=0;i<n;++i) { const uint8_t x=bytes[i]; v=uint8_t(v+((0u-(x&1u))^(x>>1))); transposed[i*stride+k]=v; }
        }
        memcpy(out.data()+offset*stride,transposed.data(),n*stride); memcpy(last,transposed.data()+(n-1)*stride,stride);
    }
    if(p!=end) throw std::runtime_error("vertex codec size");
    return out;
}
static std::vector<uint32_t> meshoptIndices(const uint8_t* d,size_t size,size_t count) {
    if(count%3 || size<1+count/3+16 || (d[0]&0xf0)!=0xe0 || (d[0]&0x0f)>1) throw std::runtime_error("index codec");
    const int fecmax=(d[0]&0x0f)>=1?13:15;
    uint32_t edges[16][2],vertices[16]; memset(edges,0xff,sizeof(edges)); memset(vertices,0xff,sizeof(vertices));
    size_t edgeAt=0,vertexAt=0,code=1,p=1+count/3; const size_t end=size-16; const uint8_t* aux=d+end;
    uint32_t next=0,last=0;
    std::vector<uint32_t> out; out.reserve(count);
    auto index=[&]() {
        uint32_t v=0;
        for(int shift=0;shift<35;shift+=7) {
            if(p>=end) throw std::runtime_error("index codec data");
            const uint8_t g=d[p++]; v|=uint32_t(g&127)<<shift;
            if(g<128) break;
        }
        return last+=((v>>1)^(0u-(v&1)));
    };
    auto vertex=[&](uint32_t v,bool push) { vertices[vertexAt]=v; vertexAt=(vertexAt+(push?1:0))&15; };
    auto edge=[&](uint32_t a,uint32_t b) { edges[edgeAt][0]=a; edges[edgeAt][1]=b; edgeAt=(edgeAt+1)&15; };
    for(size_t i=0;i<count;i+=3) {
        const uint8_t triangle=d[code++];
        uint32_t a,b,c;
        if(triangle<0xf0) {
            const size_t e=(edgeAt-1-(triangle>>4))&15; a=edges[e][0]; b=edges[e][1];
            const int fec=triangle&15;
            if(fec<fecmax) { c=fec==0?next:vertices[(vertexAt-1-fec)&15]; if(fec==0) ++next; vertex(c,fec==0); }
            else { c=fec!=15?(last+=uint32_t(fec-(fec^3))):index(); vertex(c,true); }
            edge(c,b); edge(a,c);
        } else if(triangle<0xfe) {
            const uint8_t codeaux=aux[triangle&15]; const int feb=codeaux>>4,fec=codeaux&15;
            a=next++;
            b=feb==0?next:vertices[(vertexAt-feb)&15]; if(feb==0) ++next;
            c=fec==0?next:vertices[(vertexAt-fec)&15]; if(fec==0) ++next;
            vertex(a,true); vertex(b,feb==0); vertex(c,fec==0); edge(b,a); edge(c,b); edge(a,c);
        } else {
            if(p>=end) throw std::runtime_error("index codec data");
            const uint8_t codeaux=d[p++]; const int fea=triangle==0xfe?0:15,feb=codeaux>>4,fec=codeaux&15;
            if(!codeaux) next=0;
            a=fea==0?next++:0;
            b=feb==0?next++:vertices[(vertexAt-feb)&15];
            c=fec==0?next++:vertices[(vertexAt-fec)&15];
            if(fea==15) a=index();
            if(feb==15) b=index();
            if(fec==15) c=index();
            vertex(a,true); vertex(b,feb==0 || feb==15); vertex(c,fec==0 || fec==15); edge(b,a); edge(c,b); edge(a,c);
        }
        out.push_back(a); out.push_back(b); out.push_back(c);
    }
    if(p!=end) throw std::runtime_error("index codec size");
    return out;
}

struct PreviewProp { std::string model,skin; std::array<float,3> origin{},color{1,1,1}; };
using PreviewMatrix=std::array<float,12>;
static PreviewMatrix previewMultiply(const PreviewMatrix& a,const PreviewMatrix& b) {
    PreviewMatrix m{};
    for(int r=0;r<3;++r) for(int c=0;c<4;++c)
        m[r*4+c]=a[r*4]*b[c]+a[r*4+1]*b[4+c]+a[r*4+2]*b[8+c]+(c==3?a[r*4+3]:0.0f);
    return m;
}
// The .vmap is DMX binary version 9.
static std::vector<PreviewProp> readVmapProps(const fs::path& path) {
    std::ifstream input(path,std::ios::binary);
    if(!input) throw std::runtime_error("can't read the .vmap");
    const std::vector<uint8_t> d((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>());
    static const char kHeader[]="<!-- dmx encoding binary 9 ";
    if(d.size()<64 || memcmp(d.data(),kHeader,sizeof(kHeader)-1)) throw std::runtime_error(".vmap isn't DMX binary 9");
    size_t at=std::find(d.begin(),d.end(),uint8_t(0))-d.begin()+1;
    auto need=[&](size_t n) { if(n>d.size()-std::min(at,d.size())) throw std::runtime_error(".vmap is cut off"); };
    auto int32=[&]() { need(4); int32_t v; memcpy(&v,d.data()+at,4); at+=4; return v; };
    auto text=[&]() {
        const auto stop=std::find(d.begin()+std::min(at,d.size()),d.end(),uint8_t(0));
        if(stop==d.end()) throw std::runtime_error(".vmap string");
        std::string s(d.begin()+at,stop); at=(stop-d.begin())+1; return s;
    };
    auto count=[&]() { const int32_t n=int32(); if(n<0 || size_t(n)>d.size()) throw std::runtime_error(".vmap count"); return size_t(n); };
    struct Element { std::string type; std::map<std::string,std::string> strings; std::map<std::string,std::array<float,3>> vectors;
                     std::map<std::string,std::vector<int32_t>> elements; };
    std::vector<std::string> table;
    auto value=[&](uint8_t type,bool inlineStrings,Element* element,const std::string& name) {
        const bool array=type>32; const uint8_t base=array?uint8_t(type-32):type;
        const size_t n=array?count():1;
        static const uint8_t sizes[17]={0,4,4,4,1,0,0,4,4,8,12,16,12,16,64,8,1};
        if(base<1 || base>16) throw std::runtime_error(".vmap attribute type "+std::to_string(type));
        for(size_t i=0;i<n;++i) {
            if(base==1) {
                const int32_t index=int32();
                if(index==-2) text();
                else if(element && index>=0) element->elements[name].push_back(index);
            } else if(base==5) {
                std::string s;
                if(inlineStrings || array) s=text();
                else { const int32_t index=int32(); if(index<0 || size_t(index)>=table.size()) throw std::runtime_error(".vmap string index"); s=table[index]; }
                if(element && !array) element->strings[name]=std::move(s);
            } else if(base==6) { const size_t bytes=count(); need(bytes); at+=bytes; }
            else {
                need(sizes[base]);
                if(element && !array && (base==10 || base==12)) { std::array<float,3> v; memcpy(v.data(),d.data()+at,12); element->vectors[name]=v; }
                at+=sizes[base];
            }
        }
    };
    for(size_t e=count();e;--e) for(size_t a=count();a;--a) { text(); need(1); const uint8_t type=d[at++]; value(type,true,nullptr,{}); }
    for(size_t s=count();s;--s) table.push_back(text());
    std::vector<Element> elements(count());
    for(auto& element:elements) {
        const int32_t type=int32(); int32(); need(16); at+=16;
        if(type<0 || size_t(type)>=table.size()) throw std::runtime_error(".vmap element type");
        element.type=table[type];
    }
    static const std::set<std::string> wanted={"origin","angles","scales","children","entity_properties","world","model","skin","rendercolor"};
    for(auto& element:elements) for(size_t a=count();a;--a) {
        const int32_t name=int32(); need(1); const uint8_t type=d[at++];
        if(name<0 || size_t(name)>=table.size()) throw std::runtime_error(".vmap attribute name");
        const bool keep=wanted.count(table[name])!=0;
        value(type,false,keep?&element:nullptr,table[name]);
    }
    std::vector<PreviewProp> props;
    std::function<void(int32_t,const PreviewMatrix&,int)> walk=[&](int32_t index,const PreviewMatrix& parent,int depth) {
        if(index<0 || size_t(index)>=elements.size() || depth>64) return;
        const Element& node=elements[index];
        if(node.type!="CMapWorld" && node.type!="CMapGroup" && node.type!="CMapEntity") return;
        auto vector=[&](const char* name,std::array<float,3> fallback) { auto f=node.vectors.find(name); return f==node.vectors.end()?fallback:f->second; };
        const auto origin=vector("origin",{0,0,0}),angles=vector("angles",{0,0,0}),scales=vector("scales",{1,1,1});
        const float k=3.14159265358979f/180;
        const float sp=std::sin(angles[0]*k),cp=std::cos(angles[0]*k),sy=std::sin(angles[1]*k),cy=std::cos(angles[1]*k),sr=std::sin(angles[2]*k),cr=std::cos(angles[2]*k);
        const PreviewMatrix local={cp*cy*scales[0],(sr*sp*cy-cr*sy)*scales[1],(cr*sp*cy+sr*sy)*scales[2],origin[0],
                                   cp*sy*scales[0],(sr*sp*sy+cr*cy)*scales[1],(cr*sp*sy-sr*cy)*scales[2],origin[1],
                                   -sp*scales[0],sr*cp*scales[1],cr*cp*scales[2],origin[2]};
        const PreviewMatrix world=previewMultiply(parent,local);
        if(node.type=="CMapEntity") {
            auto found=node.elements.find("entity_properties");
            if(found!=node.elements.end() && !found->second.empty() && size_t(found->second[0])<elements.size()) {
                const auto& values=elements[found->second[0]].strings;
                auto get=[&](const char* name) { auto v=values.find(name); return v==values.end()?std::string():v->second; };
                PreviewProp prop; prop.model=get("model"); prop.skin=get("skin");
                for(char& c:prop.model) c=c=='\\'?'/':static_cast<char>(tolower(static_cast<unsigned char>(c)));
                std::istringstream color(get("rendercolor")); float rgb[3];
                if(color>>rgb[0]>>rgb[1]>>rgb[2]) for(int c=0;c<3;++c) prop.color[c]=std::isfinite(rgb[c])?std::clamp(rgb[c],0.0f,255.0f)/255:1;
                prop.origin={world[3],world[7],world[11]};
                if(prop.model.size()>5 && prop.model.compare(prop.model.size()-5,5,".vmdl")==0) props.push_back(std::move(prop));
            }
        }
        if(auto children=node.elements.find("children"); children!=node.elements.end())
            for(int32_t child:children->second) walk(child,world,depth+1);
    };
    const PreviewMatrix identity={1,0,0,0,0,1,0,0,0,0,1,0};
    for(const auto& element:elements)
        if(element.type=="CMapRootElement")
            if(auto world=element.elements.find("world"); world!=element.elements.end() && !world->second.empty()) walk(world->second[0],identity,0);
    return props;
}

struct PreviewModel {
    std::vector<std::string> groupNames;
    std::vector<std::vector<std::string>> groups;
    bool meshesRead=false;
    struct Mesh { std::vector<float> corners,uvs; };
    std::vector<Mesh> meshes;
};
static const EmissiveKv* kvGet(const EmissiveKv* v,const char* name) { return v?v->get(name):nullptr; }
static double kvNumber(const EmissiveKv* v,const char* name,double fallback=0) { const EmissiveKv* n=kvGet(v,name); return n?n->number:fallback; }
static std::string kvText(const EmissiveKv* v,const char* name) { const EmissiveKv* n=kvGet(v,name); return n?n->text:std::string(); }
static std::string previewLower(std::string s) { for(char& c:s) c=c=='\\'?'/':static_cast<char>(tolower(static_cast<unsigned char>(c))); return s; }
static float previewHalf(uint16_t h) {
    const uint32_t sign=uint32_t(h&0x8000)<<16,exponent=(h>>10)&31,mantissa=h&1023; uint32_t bits;
    if(!exponent) { const float v=std::ldexp(float(mantissa),-24); return sign?-v:v; }
    if(exponent==31) bits=sign|0x7f800000u|(mantissa<<13);
    else bits=sign|((exponent+112)<<23)|(mantissa<<13);
    float f; memcpy(&f,&bits,4); return f;
}
static PreviewModel::Mesh readModelMesh(const ResourceBlock& mdat,const ResourceBlock& mbuf) {
    const EmissiveKv mesh=emissiveKv3(mdat.data,mdat.size);
    const uint8_t* b=mbuf.data; const size_t size=mbuf.size;
    auto u32=[&](size_t at) { if(at+4>size) throw std::runtime_error("mesh buffers"); uint32_t v; memcpy(&v,b+at,4); return v; };
    struct Buffer { std::vector<uint8_t> bytes; size_t count=0,stride=0; int position=-1,uv=-1,uvFormat=0; };
    auto buffers=[&](size_t field,bool vertex) {
        const size_t first=field+u32(field),n=u32(field+4); std::vector<Buffer> list;
        if(n>64) throw std::runtime_error("mesh buffers");
        for(size_t i=0;i<n;++i) {
            const size_t at=first+i*24; Buffer buffer;
            buffer.count=u32(at); buffer.stride=u32(at+4);
            const size_t attributes=at+8+u32(at+8),attributeCount=u32(at+12),data=at+16+u32(at+16),bytes=u32(at+20);
            if(data>size || bytes>size-data || !buffer.stride || buffer.stride>256 || buffer.count>(1u<<24)) throw std::runtime_error("mesh buffer bounds");
            if(vertex) for(size_t a=0;a<attributeCount && a<32;++a) {
                // Vertex attribute records are 56 bytes: name at 0, semantic at 32, format at 36, offset at 40 (6 = R32G32B32_FLOAT, 34 = R16G16, 16 = R32G32_FLOAT).
                const size_t attribute=attributes+a*56; u32(attribute+52);
                const std::string name(reinterpret_cast<const char*>(b+attribute),strnlen(reinterpret_cast<const char*>(b+attribute),32));
                const uint32_t semantic=u32(attribute+32),format=u32(attribute+36),offset=u32(attribute+40);
                if(semantic) continue;
                if(name=="POSITION" && format==6 && offset+12<=buffer.stride) buffer.position=int(offset);
                if(name=="TEXCOORD" && (format==34 || format==16) && offset+(format==34?4:8)<=buffer.stride) { buffer.uv=int(offset); buffer.uvFormat=int(format); }
            }
            if(bytes==buffer.count*buffer.stride) buffer.bytes.assign(b+data,b+data+bytes);
            else if(vertex) buffer.bytes=meshoptVertices(b+data,bytes,buffer.count,buffer.stride);
            else {
                const auto indices=meshoptIndices(b+data,bytes,buffer.count);
                buffer.bytes.resize(buffer.count*buffer.stride);
                for(size_t k=0;k<indices.size();++k) {
                    if(buffer.stride==2) { const uint16_t v=uint16_t(indices[k]); memcpy(buffer.bytes.data()+k*2,&v,2); }
                    else if(buffer.stride==4) memcpy(buffer.bytes.data()+k*4,&indices[k],4);
                    else throw std::runtime_error("index size");
                }
            }
            list.push_back(std::move(buffer));
        }
        return list;
    };
    const auto vertexBuffers=buffers(0,true),indexBuffers=buffers(8,false);
    PreviewModel::Mesh out; bool uvs=true;
    const EmissiveKv* objects=mesh.get("m_sceneObjects");
    if(!objects) throw std::runtime_error("mesh without scene objects");
    for(const auto& object:objects->items) {
        const EmissiveKv* calls=object.get("m_drawCalls");
        if(!calls) continue;
        for(const auto& call:calls->items) {
            const std::string primitive=kvText(&call,"m_nPrimitiveType");
            if(!primitive.empty() && primitive!="RENDER_PRIM_TRIANGLES") throw std::runtime_error("draw call primitive "+primitive);
            const EmissiveKv* vbs=call.get("m_vertexBuffers"); const EmissiveKv* ib=call.get("m_indexBuffer");
            if(!vbs || vbs->items.size()!=1 || !ib) throw std::runtime_error("draw call buffers");
            const size_t v=size_t(kvNumber(&vbs->items[0],"m_hBuffer",-1)),i=size_t(kvNumber(ib,"m_hBuffer",-1));
            if(v>=vertexBuffers.size() || i>=indexBuffers.size()) throw std::runtime_error("draw call buffer index");
            const Buffer& vb=vertexBuffers[v]; const Buffer& indexBuffer=indexBuffers[i];
            if(vb.position<0) throw std::runtime_error("vertex buffer without positions");
            uvs=uvs && vb.uv>=0;
            const size_t base=size_t(kvNumber(&call,"m_nBaseVertex"))+size_t(kvNumber(&vbs->items[0],"m_nBindOffsetBytes"))/vb.stride;
            const size_t start=size_t(kvNumber(&call,"m_nStartIndex"))+size_t(kvNumber(ib,"m_nBindOffsetBytes"))/indexBuffer.stride,count=size_t(kvNumber(&call,"m_nIndexCount"));
            if(count%3 || start+count>indexBuffer.count) throw std::runtime_error("draw call indices");
            for(size_t k=start;k<start+count;++k) {
                uint32_t index=0; memcpy(&index,indexBuffer.bytes.data()+k*indexBuffer.stride,indexBuffer.stride);
                const size_t vertex=base+index;
                if(vertex>=vb.count) throw std::runtime_error("draw call vertex");
                const uint8_t* at=vb.bytes.data()+vertex*vb.stride;
                float p[3]; memcpy(p,at+vb.position,12); out.corners.insert(out.corners.end(),p,p+3);
                if(!uvs) continue;
                if(vb.uvFormat==16) { float uv[2]; memcpy(uv,at+vb.uv,8); out.uvs.insert(out.uvs.end(),uv,uv+2); }
                else { uint16_t h[2]; memcpy(h,at+vb.uv,4); out.uvs.push_back(previewHalf(h[0])); out.uvs.push_back(previewHalf(h[1])); }
            }
        }
    }
    if(!uvs) out.uvs.clear();
    return out;
}
static PreviewModel readModel(const EmissiveFiles& files,const std::string& name,bool meshes) {
    std::vector<uint8_t> file;
    if(!files.read(name+"_c",file)) throw std::runtime_error("not found");
    const auto blocks=resourceBlocks(file);
    auto block=[&](const char* type) -> const ResourceBlock* { for(const auto& b:blocks) if(!memcmp(b.type,type,4)) return &b; return nullptr; };
    const ResourceBlock* data=block("DATA"); const ResourceBlock* ctrl=block("CTRL");
    if(!data) throw std::runtime_error("no DATA block");
    const EmissiveKv model=emissiveKv3(data->data,data->size);
    PreviewModel out;
    if(const EmissiveKv* groups=model.get("m_materialGroups"))
        for(const auto& group:groups->items) {
            out.groupNames.push_back(kvText(&group,"m_name")); out.groups.emplace_back();
            if(const EmissiveKv* list=group.get("m_materials")) for(const auto& m:list->items) out.groups.back().push_back(previewLower(m.text));
        }
    if(!meshes) return out;
    out.meshesRead=true;
    if(!ctrl) throw std::runtime_error("no embedded meshes");
    const EmissiveKv control=emissiveKv3(ctrl->data,ctrl->size);
    const EmissiveKv* embedded=control.get("embedded_meshes"); const EmissiveKv* lods=model.get("m_refLODGroupMasks");
    if(!embedded) throw std::runtime_error("no embedded meshes");
    PreviewModel::Mesh all; bool allUvs=true; size_t lod0=0;
    for(const auto& entry:embedded->items) {
        const size_t index=size_t(kvNumber(&entry,"mesh_index",-1)),mdat=size_t(kvNumber(&entry,"data_block",-1)),mbuf=size_t(kvNumber(&entry,"vbib_block",-1));
        if(lods && index<lods->items.size() && !(int64_t(lods->items[index].number)&1)) continue;
        if(mdat>=blocks.size() || mbuf>=blocks.size()) throw std::runtime_error("embedded mesh block");
        PreviewModel::Mesh mesh=readModelMesh(blocks[mdat],blocks[mbuf]);
        all.corners.insert(all.corners.end(),mesh.corners.begin(),mesh.corners.end());
        allUvs=allUvs && !mesh.uvs.empty(); if(allUvs) all.uvs.insert(all.uvs.end(),mesh.uvs.begin(),mesh.uvs.end());
        out.meshes.push_back(std::move(mesh)); ++lod0;
    }
    if(!allUvs) all.uvs.clear();
    if(lod0>1) out.meshes.push_back(std::move(all));
    return out;
}
static bool previewSelfIllum(const EmissiveFiles& files,const std::string& material) {
    try {
        std::vector<uint8_t> file;
        if(!files.read(material+"_c",file)) return false;
        const auto block=emissiveDataBlock(file);
        if(block.second<4 || memcmp(block.first,"\x01" "3VK",4)) return false;
        const EmissiveKv kv=emissiveKv3(block.first,block.second);
        if(_stricmp(kvText(&kv,"m_shaderName").c_str(),"vr_complex.vfx")) return false;
        if(const EmissiveKv* ints=kv.get("m_intParams"))
            for(const auto& item:ints->items) if(kvText(&item,"m_name")=="F_SELF_ILLUM") return kvNumber(&item,"m_nValue")!=0;
    } catch(const std::exception&) {}
    return false;
}

struct PreviewScene {
    enum { kVersion,kUnknown,kFlags,kExtended,kTriangles,kInts,kColors,kUvs,kKeys };
    struct Child { int32_t mesh=0; float xf[12]{}; std::vector<uint64_t> from,to; };
    struct Mesh {
        int32_t header[9]{}; size_t at=0,trianglesAt=0,keysAt=0,colorsAt=0,uvsAt=0;
        std::vector<Child> children;
        std::vector<float> colors,uvs; bool newColors=false,newUvs=false;
    };
    std::vector<uint8_t> bytes; size_t magic=0,tableAt=0; std::vector<Mesh> meshes; int32_t materials=0;
    std::vector<std::vector<uint8_t>> addedRecords;
    void read(const fs::path& path) {
        std::ifstream input(path,std::ios::binary);
        if(!input) throw std::runtime_error("can't read the scene");
        bytes.assign(std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>());
        // Path trace scene file (Valve's CPathTraceSceneInfoEmbree::Unserialize, vrad3 0x140091e50): magic, int32 mesh count,
        // per mesh int32 x9 header, bounds, triangles (3 float3 + uint16 flags), ints, uint64 material keys, per-triangle
        // colours, per-corner uvs, then children (mesh, 3x4 row-major transform, material overrides from/to), then the
        // material table of 136-byte records (uint64 key first).
        static const char kMagic[]="Valve Path Trace Scene Info V0001";
        if(bytes.size()<sizeof(kMagic)+4 || memcmp(bytes.data(),kMagic,sizeof(kMagic))) throw std::runtime_error("not a V0001 scene");
        size_t at=magic=sizeof(kMagic);
        auto need=[&](uint64_t n) { if(n>bytes.size()-at) throw std::runtime_error("scene is cut off"); };
        auto int32=[&]() { need(4); int32_t v; memcpy(&v,bytes.data()+at,4); at+=4; return v; };
        const int32_t count=int32();
        if(count<1 || count>65536) throw std::runtime_error("scene mesh count");
        meshes.resize(count);
        for(auto& mesh:meshes) {
            mesh.at=at;
            for(int32_t& v:mesh.header) { v=int32(); if(v<0) throw std::runtime_error("scene mesh header"); }
            if(mesh.header[kVersion]!=6 || mesh.header[kExtended]!=0) throw std::runtime_error("scene mesh layout");
            const uint64_t triangles=uint64_t(mesh.header[kTriangles]);
            const uint64_t colors=uint64_t(mesh.header[kColors]),uvs=uint64_t(mesh.header[kUvs]),keys=uint64_t(mesh.header[kKeys]);
            need(24); at+=24;
            mesh.trianglesAt=at; need(triangles*38); at+=size_t(triangles*38);
            need(uint64_t(mesh.header[kInts])*4); at+=size_t(mesh.header[kInts])*4;
            mesh.keysAt=at; need(keys*8); at+=size_t(keys*8);
            mesh.colorsAt=at; need(colors*12); at+=size_t(colors*12);
            mesh.uvsAt=at; need(uvs*8); at+=size_t(uvs*8);
            const int32_t children=int32();
            if(children<0 || children>(1<<20)) throw std::runtime_error("scene children");
            mesh.children.resize(children);
            for(auto& child:mesh.children) {
                child.mesh=int32(); need(48); memcpy(child.xf,bytes.data()+at,48); at+=48;
                if(child.mesh<0 || child.mesh>=count) throw std::runtime_error("scene child mesh");
                const int32_t n=int32();
                if(n<0 || n>65536) throw std::runtime_error("scene override count");
                need(uint64_t(n)*16); child.from.resize(n); child.to.resize(n);
                if(n) { memcpy(child.from.data(),bytes.data()+at,n*8ull); memcpy(child.to.data(),bytes.data()+at+n*8ull,n*8ull); }
                at+=n*16ull;
            }
        }
        tableAt=at; materials=int32();
        if(materials<0 || materials>65536) throw std::runtime_error("scene material count");
        need(uint64_t(materials)*136);
    }
    const uint8_t* record(uint64_t key) const {
        for(int32_t i=0;i<materials;++i) { const uint8_t* r=bytes.data()+tableAt+4+i*136ull; if(!memcmp(r,&key,8)) return r; }
        for(const auto& r:addedRecords) if(!memcmp(r.data(),&key,8)) return r.data();
        return nullptr;
    }
    std::vector<uint8_t> write() const {
        std::vector<uint8_t> out; out.reserve(bytes.size()+bytes.size()/4);
        auto raw=[&](const void* p,size_t n) { const uint8_t* b=static_cast<const uint8_t*>(p); out.insert(out.end(),b,b+n); };
        auto int32=[&](int32_t v) { raw(&v,4); };
        raw(bytes.data(),magic); int32(int32_t(meshes.size()));
        for(const auto& mesh:meshes) {
            int32_t header[9]; memcpy(header,mesh.header,sizeof(header));
            if(mesh.newColors) header[kColors]=int32_t(mesh.colors.size()/3);
            if(mesh.newUvs) header[kUvs]=int32_t(mesh.uvs.size()/2);
            raw(header,sizeof(header));
            raw(bytes.data()+mesh.at+36,mesh.colorsAt-(mesh.at+36));
            if(mesh.newColors) raw(mesh.colors.data(),mesh.colors.size()*4); else raw(bytes.data()+mesh.colorsAt,mesh.header[kColors]*12ull);
            if(mesh.newUvs) raw(mesh.uvs.data(),mesh.uvs.size()*4); else raw(bytes.data()+mesh.uvsAt,mesh.header[kUvs]*8ull);
            int32(int32_t(mesh.children.size()));
            for(const auto& child:mesh.children) {
                int32(child.mesh); raw(child.xf,48); int32(int32_t(child.from.size()));
                raw(child.from.data(),child.from.size()*8); raw(child.to.data(),child.to.size()*8);
            }
        }
        int32(materials+int32_t(addedRecords.size()));
        raw(bytes.data()+tableAt+4,materials*136ull);
        for(const auto& r:addedRecords) raw(r.data(),r.size());
        raw(bytes.data()+tableAt+4+materials*136ull,bytes.size()-(tableAt+4+materials*136ull));
        return out;
    }
};

static bool fixPreviewScene(const fs::path& scenePath,const fs::path& vmap,const EmissiveFiles& files,bool uvs,
                            const fs::path& output,std::string& report) {
    const std::vector<PreviewProp> props=readVmapProps(vmap);
    if(props.empty()) { report="no props in the map"; return false; }
    std::map<std::string,PreviewModel> models; std::map<std::string,bool> selfIllum; std::set<std::string> unreadable;
    auto lit=[&](const std::string& material) { auto f=selfIllum.find(material); return f!=selfIllum.end()?f->second:(selfIllum[material]=previewSelfIllum(files,material)); };
    auto skinGroup=[](const PreviewModel& model,const std::string& skin) -> size_t {
        if(skin.empty()) return 0;
        for(size_t g=0;g<model.groupNames.size();++g) if(!_stricmp(model.groupNames[g].c_str(),skin.c_str())) return g;
        if(skin.find_first_not_of("0123456789")==std::string::npos && skin.size()<6) return size_t(std::stoul(skin));
        return 0;
    };
    for(const auto& prop:props) {
        if(models.count(prop.model) || unreadable.count(prop.model)) continue;
        try {
            PreviewModel model=readModel(files,prop.model,false);
            models.emplace(prop.model,std::move(model));
        } catch(const std::exception&) { unreadable.insert(prop.model); }
    }
    for(auto& [name,model]:models) {
        bool needed=false;
        for(const auto& prop:props) {
            if(prop.model!=name) continue;
            const size_t group=skinGroup(model,prop.skin);
            needed=needed || (group>0 && group<model.groups.size()) || prop.color!=std::array<float,3>{1,1,1};
        }
        if(uvs && !needed) for(const auto& group:model.groups) for(const auto& m:group) needed=needed || lit(m);
        if(!needed) continue;
        try { model=readModel(files,name,true); }
        catch(const std::exception&) { unreadable.insert(name); model.meshes.clear(); }
    }
    PreviewScene scene; scene.read(scenePath);
    std::map<int32_t,std::pair<const std::string*,const PreviewModel::Mesh*>> matched;
    std::map<size_t,std::vector<std::pair<const std::string*,const PreviewModel::Mesh*>>> byTriangles;
    for(const auto& [name,model]:models) for(const auto& mesh:model.meshes) byTriangles[mesh.corners.size()/9].push_back({&name,&mesh});
    for(int32_t m=1;m<int32_t(scene.meshes.size());++m) {
        const auto& mesh=scene.meshes[m]; const size_t triangles=size_t(mesh.header[PreviewScene::kTriangles]);
        auto candidates=byTriangles.find(triangles);
        if(!triangles || candidates==byTriangles.end()) continue;
        for(const auto& candidate:candidates->second) {
            bool same=true;
            for(size_t t=0;t<triangles && same;++t) {
                float corners[9]; memcpy(corners,scene.bytes.data()+mesh.trianglesAt+t*38,36);
                for(int k=0;k<9 && same;++k) { const float a=corners[k],b=candidate.second->corners[t*9+k]; same=std::fabs(a-b)<=2e-3f+1e-5f*std::fabs(b); }
            }
            if(same) { matched[m]=candidate; break; }
        }
    }
    if(matched.empty()) { report="no placed mesh matched a prop's model"; return false; }
    std::vector<int> references(scene.meshes.size(),0);
    for(const auto& mesh:scene.meshes) for(const auto& child:mesh.children) ++references[child.mesh];
    struct Placement { std::vector<std::pair<int32_t,size_t>> chain; PreviewMatrix world; };
    std::vector<Placement> placements;
    std::function<void(int32_t,Placement&,int)> walk=[&](int32_t mesh,Placement& at,int depth) {
        if(depth>64) throw std::runtime_error("scene tree too deep");
        const auto& children=scene.meshes[mesh].children;
        for(size_t i=0;i<children.size();++i) {
            Placement next=at; next.chain.push_back({mesh,i});
            PreviewMatrix local; std::copy(children[i].xf,children[i].xf+12,local.begin());
            next.world=previewMultiply(at.world,local);
            if(matched.count(children[i].mesh)) placements.push_back(next);
            walk(children[i].mesh,next,depth+1);
        }
    };
    Placement root; root.world={1,0,0,0,0,1,0,0,0,0,1,0}; walk(0,root,0);
    size_t skins=0,tinted=0,withUvs=0,unmatched=0; uint64_t addedTriangles=0;
    std::map<std::pair<int32_t,std::array<float,3>>,int32_t> copies;
    const size_t originalMeshes=scene.meshes.size();
    for(const auto& placement:placements) {
        PreviewScene::Child& entry=scene.meshes[placement.chain.back().first].children[placement.chain.back().second];
        const int32_t meshIndex=entry.mesh; const std::string& modelName=*matched[meshIndex].first;
        const PreviewProp* prop=nullptr; int found=0;
        for(const auto& p:props) {
            if(p.model!=modelName) continue;
            float d=0; for(int c=0;c<3;++c) d=std::max(d,std::fabs(p.origin[c]-placement.world[c*4+3]));
            if(d<=0.05f+1e-5f*std::fabs(p.origin[0])+1e-5f*std::fabs(p.origin[1])+1e-5f*std::fabs(p.origin[2])) { prop=&p; ++found; }
        }
        if(found!=1) { ++unmatched; continue; }
        bool alone=true;
        for(size_t k=1;k<placement.chain.size();++k) {
            const auto& e=scene.meshes[placement.chain[k].first].children[placement.chain[k].second];
            alone=alone && e.from.empty() && references[placement.chain[k].first]==1;
        }
        if(!alone) { ++unmatched; continue; }
        const PreviewModel& model=models.at(modelName);
        const size_t group=skinGroup(model,prop->skin);
        if(group>0 && group<model.groups.size()) {
            PreviewScene::Child& outer=scene.meshes[placement.chain[0].first].children[placement.chain[0].second];
            const auto& from=model.groups[0]; const auto& to=model.groups[group]; size_t added=0;
            for(size_t i=0;i<std::min(from.size(),to.size());++i) {
                if(from[i]==to[i]) continue;
                const uint64_t a=emissiveResourceId(from[i]),b=emissiveResourceId(to[i]);
                if(std::find(outer.from.begin(),outer.from.end(),a)!=outer.from.end()) continue;
                if(!scene.record(b)) {
                    const uint8_t* source=scene.record(a);
                    if(!source) continue;
                    std::vector<uint8_t> copy(source,source+136); memcpy(copy.data(),&b,8);
                    scene.addedRecords.push_back(std::move(copy));
                }
                outer.from.push_back(a); outer.to.push_back(b); ++added;
            }
            if(added) ++skins;
        }
        if(prop->color!=std::array<float,3>{1,1,1}) {
            const auto& mesh=scene.meshes[meshIndex]; const size_t triangles=size_t(mesh.header[PreviewScene::kTriangles]);
            bool white=mesh.children.empty() && mesh.header[PreviewScene::kColors]==int32_t(triangles);
            for(size_t i=0;white && i<triangles*3;++i) { float v; memcpy(&v,scene.bytes.data()+mesh.colorsAt+i*4,4); white=v==1.0f; }
            auto copy=copies.find({meshIndex,prop->color});
            if(copy!=copies.end()) { entry.mesh=copy->second; ++tinted; }
            else if(white && addedTriangles+triangles<=(4u<<20) && scene.meshes.size()<65536) {
                PreviewScene::Mesh tint=mesh; tint.newColors=true; tint.colors.resize(triangles*3);
                for(size_t t=0;t<triangles;++t) std::copy(prop->color.begin(),prop->color.end(),tint.colors.begin()+t*3);
                const int32_t index=int32_t(scene.meshes.size());
                scene.meshes.push_back(std::move(tint)); copies[{meshIndex,prop->color}]=index;
                matched[index]=matched[meshIndex]; addedTriangles+=triangles;
                scene.meshes[placement.chain.back().first].children[placement.chain.back().second].mesh=index; ++tinted;
            }
        }
    }
    if(uvs) for(auto& [index,source]:matched) {
        auto& mesh=scene.meshes[index];
        if(mesh.header[PreviewScene::kUvs] || source.second->uvs.empty()) continue;
        bool glows=false;
        for(const auto& group:models.at(*source.first).groups) for(const auto& m:group) glows=glows || lit(m);
        if(!glows) continue;
        mesh.newUvs=true; mesh.uvs=source.second->uvs; ++withUvs;
    }
    std::ostringstream line;
    line<<props.size()<<" props, "<<matched.size()<<" meshes matched, "<<skins<<" skin(s), "<<tinted<<" Render Color(s) ("
        <<scene.meshes.size()-originalMeshes<<" mesh copies), "<<withUvs<<" mesh(es) given uvs, "<<scene.addedRecords.size()<<" material(s) added";
    if(unmatched) line<<", "<<unmatched<<" placement(s) left as they are";
    if(!unreadable.empty()) line<<", "<<unreadable.size()<<" model(s) unreadable";
    report=line.str();
    if(!skins && !tinted && !withUvs) return false;
    const auto bytes=scene.write();
    std::ofstream out(output,std::ios::binary|std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size())); out.close();
    if(out.fail()) throw std::runtime_error("can't write the fixed scene");
    PreviewScene check; check.read(output);
    if(check.meshes.size()!=scene.meshes.size() || check.materials!=scene.materials+int32_t(scene.addedRecords.size()) ||
       check.bytes.size()-check.tableAt!=scene.bytes.size()-scene.tableAt+scene.addedRecords.size()*136)
        throw std::runtime_error("the fixed scene doesn't read back");
    return true;
}
