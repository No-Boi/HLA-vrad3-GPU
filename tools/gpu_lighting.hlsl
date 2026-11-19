// GPU lighting compute shader: pathtrace, directlight, light probes, sample reject and adaptive tile tests, using inline ray queries.
// VRAD3_CS2 selects the CS2-style estimator; VRAD3_EXPERIMENTAL 0 compiles out the opt-in sampling features.
#ifndef VRAD3_EXPERIMENTAL
#define VRAD3_EXPERIMENTAL 1
#endif
#ifndef VRAD3_CS2
#define VRAD3_CS2 0
#endif
// Register layout matches the root signature in gpu_lighting.cpp: t0 TLAS, t1-t13 scene buffers, t14 traversal metadata, u0 output records, u1 counters, b0 the 30 root constants.
RaytracingAccelerationStructure scene : register(t0);
StructuredBuffer<float4> vertices : register(t1);
StructuredBuffer<float2> uvs : register(t2);
StructuredBuffer<uint2> primitives : register(t3);
StructuredBuffer<float3> colors : register(t4);
// reflectivity.w is opacity; textures = albedo, opacity, mask, mask channel (-1 when absent). 64 bytes per material.
struct Material { float4 reflectivity; float4 multiplier; float4 emission; int4 textures; };
StructuredBuffer<Material> materials : register(t5);
StructuredBuffer<uint4> textureDescriptors : register(t6);
ByteAddressBuffer textureBytes : register(t7);
StructuredBuffer<uint> offsets : register(t8);
StructuredBuffer<uint> indices : register(t9);
StructuredBuffer<float> cdf : register(t10);
struct Sample { float3 position; float3 normal; float3 geometricNormal; };
StructuredBuffer<Sample> samples : register(t11);
struct Light { uint4 flags; float4 rgbRange; float4 positionOffset; float4 directionSize; float4 attenuation; float4 cone; float4 tangent; };
StructuredBuffer<Light> lights : register(t12);
ByteAddressBuffer environmentSky : register(t13);
ByteAddressBuffer traversalMetadata : register(t14);
RWByteAddressBuffer working : register(u0);
RWByteAddressBuffer researchCounters : register(u1);
// maxBounces also carries mode bits (constants[3]): 0x80000000 directlight, 0x40000000 light probes, 0x20000000 tile test, 0x10000000 sample reject, 0x08000000 luxel list.
cbuffer Settings : register(b0) {
    uint luxelCount; uint sampleCount; uint seed; uint maxBounces;
    float3 skyRGB; float visibilityRadius;
    float visibilityStrength; float visibilityExponent; uint suppressLowerHemisphere; uint lightCount;
    uint geometryMask; uint shadowMask; uint meshFlags; uint dispatchLuxels;
    float3 maskWhite; float transmissionEpsilon;
    float minimumCosine; float rouletteCeiling; float directionalDistance; uint firstLuxel;
    uint globalLuxelBase; uint researchEnabled; uint sampleBegin; uint sampleEnd;
    uint traversalOptions; uint lightBoundsOffset;
}
static uint counters[11];
uint probeFlags() { return (maxBounces&0x40000000u) ? asuint(visibilityStrength) : 0; }
void research(uint slot) { if(researchEnabled&1) ++counters[slot]; }
bool hlaShadows() { return (researchEnabled&2)!=0; }
static const float PI = 3.14159265358979323846;

// environmentSky: 96-byte header (size, rotation rows, limits, angle factors), then three float planes (R, G, B), then optional cookie images.
float skyPlane(uint base,uint width,uint height,float2 pixel) {
    uint2 lo=(uint2)pixel,hi=min(lo+1,uint2(width-1,height-1));
    float2 fraction=pixel-lo;
    float a=asfloat(environmentSky.Load(base+(lo.y*width+lo.x)*4));
    float b=asfloat(environmentSky.Load(base+(lo.y*width+hi.x)*4));
    float c=asfloat(environmentSky.Load(base+(hi.y*width+lo.x)*4));
    float d=asfloat(environmentSky.Load(base+(hi.y*width+hi.x)*4));
    return lerp(lerp(a,b,fraction.x),lerp(c,d,fraction.x),fraction.y);
}
float3 skyRadiance(float3 direction) {
    uint width=environmentSky.Load(0),height=environmentSky.Load(4);
    if(width==0) return !suppressLowerHemisphere || direction.z>0 ? skyRGB : 0;
    float3 transformed=direction.x*asfloat(environmentSky.Load3(16))+
                       direction.y*asfloat(environmentSky.Load3(32))+
                       direction.z*asfloat(environmentSky.Load3(48));
    float3 limits=asfloat(environmentSky.Load3(64));
    transformed=clamp(transformed,-limits,limits);
    float4 angles=asfloat(environmentSky.Load4(76));
    float u=atan2(transformed.y,transformed.x)*angles.y;
    float v=acos(transformed.z)*angles.z;
    if(u<0) u+=angles.x;
    if(v<0) v+=angles.x;
    float2 pixel=clamp(float2((width-1)*(angles.x-u),(height-1)*v),0,float2(width-1,height-1));
    uint planeBytes=width*height*4;
    return float3(skyPlane(96,width,height,pixel),skyPlane(96+planeBytes,width,height,pixel),
                  skyPlane(96+2*planeBytes,width,height,pixel));
}

uint hash(uint x) { x ^= x>>16; x *= 0x7feb352d; x ^= x>>15; x *= 0x846ca68b; return x^(x>>16); }
float random(inout uint state) { state = state*747796405u+2891336453u; uint word=((state>>((state>>28u)+4u))^state)*277803737u; word=(word>>22)^word; return (word>>8)*(1.0/16777216.0); }
float3 hemisphere(float2 u,float3 normal) {
    float3 tangent=normalize(cross(abs(normal.z)<0.999 ? float3(0,0,1) : float3(0,1,0),normal));
    float3 bitangent=cross(normal,tangent);
    float radius=sqrt(u.x),angle=2*PI*u.y;
    return tangent*(radius*cos(angle))+bitangent*(radius*sin(angle))+normal*sqrt(1-u.x);
}
uint nestedScramble(uint x,uint seed) {
    x=reversebits(x); x+=seed; x^=x*0x6c50b47cu; x^=x*0xb82f1e52u; x^=x*0xc7afe638u; x^=x*0x8d22f6e6u; return reversebits(x);
}
// Shuffled, Owen-scrambled Sobol (0,2) sequence (Burley 2020); each cell has its own scramble.
float2 shuffledSobol2(uint index,uint seed) {
    index=nestedScramble(index,seed);
    uint a=reversebits(index),b=0,v=1u<<31;
    for(uint i=index;i;i>>=1,v^=v>>1) if(i&1) b^=v;
    a=nestedScramble(a,hash(seed+1)); b=nestedScramble(b,hash(seed+2));
    return float2(a>>8,b>>8)*(1.0/16777216.0);
}
float scrambledVanDerCorput(uint index,uint seed) {
    index=nestedScramble(index,seed);
    return (nestedScramble(reversebits(index),hash(seed+3))>>8)*(1.0/16777216.0);
}
float2 endpointRandom(inout uint rng) {
    float a=random(rng); float b=random(rng); return float2(a,b);
}
struct Hit { uint primitive; float distance; float2 bary; };
Hit traceLegacy(float3 origin,float3 direction,float distance,uint mask,uint cull) {
    RayDesc desc; desc.Origin=origin; desc.Direction=direction; desc.TMin=0; desc.TMax=distance;
    RayQuery<RAY_FLAG_FORCE_NON_OPAQUE> query;
    query.TraceRayInline(scene,RAY_FLAG_NONE,(traversalOptions&4)?16:255,desc);
    while(query.Proceed()) {
        if(query.CandidateType()!=CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        research(7);
        uint primitive=query.CandidatePrimitiveIndex();
        if(((primitives[primitive].x | meshFlags)&mask)!=0) continue;
        if(cull!=0 && !((primitives[primitive].x|meshFlags)&2)) {
            uint first=primitive*3;
            float facing=dot(cross(vertices[first+1].xyz-vertices[first].xyz,
                                   vertices[first+2].xyz-vertices[first].xyz),direction);
            if((cull==1 && facing<0) || (cull==2 && facing>0)) continue;
        }
        query.CommitNonOpaqueTriangleHit();
    }
    Hit hit; hit.primitive=0xffffffff; hit.distance=distance; hit.bary=0;
    if(query.CommittedStatus()==COMMITTED_TRIANGLE_HIT) {
        hit.primitive=query.CommittedPrimitiveIndex(); hit.distance=query.CommittedRayT(); hit.bary=query.CommittedTriangleBarycentrics();
    }
    return hit;
}
Hit trace(float3 origin,float3 direction,float distance,uint mask,uint cull) {
    research(2);
    if(!(traversalOptions&1)) return traceLegacy(origin,direction,distance,mask,cull);
    RayDesc desc; desc.Origin=origin; desc.Direction=direction; desc.TMin=0; desc.TMax=distance;
    uint flags=cull==1 ? RAY_FLAG_CULL_BACK_FACING_TRIANGLES : cull==2 ? RAY_FLAG_CULL_FRONT_FACING_TRIANGLES : RAY_FLAG_NONE;
    RayQuery<RAY_FLAG_NONE> hardware;
    hardware.TraceRayInline(scene,flags,mask==geometryMask ? 1 : 8,desc);
    while(hardware.Proceed()) {
        if(hardware.CandidateType()!=CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        research(7); hardware.CommitNonOpaqueTriangleHit();
    }
    Hit hit; hit.primitive=0xffffffff; hit.distance=distance; hit.bary=0;
    if(hardware.CommittedStatus()==COMMITTED_TRIANGLE_HIT) {
        hit.primitive=traversalMetadata.Load(4*(hardware.CommittedInstanceID()+hardware.CommittedPrimitiveIndex()));
        hit.distance=hardware.CommittedRayT(); hit.bary=hardware.CommittedTriangleBarycentrics();
    }
    if(traversalOptions&4) {
        Hit legacy=traceLegacy(origin,direction,distance,mask,cull);
        if(hit.primitive!=legacy.primitive || asuint(hit.distance)!=asuint(legacy.distance) || any(asuint(hit.bary)!=asuint(legacy.bary))) {
            uint slot; researchCounters.InterlockedAdd(44,1,slot);
            if(slot<64) {
                uint address=256+slot*80;
                researchCounters.Store4(address,uint4(hit.primitive,legacy.primitive,asuint(hit.distance),asuint(legacy.distance)));
                researchCounters.Store2(address+16,uint2(mask,cull));
                researchCounters.Store3(address+24,asuint(origin)); researchCounters.Store3(address+36,asuint(direction));
                researchCounters.Store4(address+48,asuint(float4(hit.bary,legacy.bary)));
            }
        }
    }
    return hit;
}
bool traceBlocked(float3 origin,float3 direction,float distance,uint mask,uint cull) {
    if(!(traversalOptions&1) || (traversalOptions&4)) return trace(origin,direction,distance,mask,cull).primitive!=0xffffffff;
    RayDesc desc; desc.Origin=origin; desc.Direction=direction; desc.TMin=0; desc.TMax=distance;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(scene,cull==1 ? RAY_FLAG_CULL_BACK_FACING_TRIANGLES : cull==2 ? RAY_FLAG_CULL_FRONT_FACING_TRIANGLES : RAY_FLAG_NONE,
                         mask==geometryMask ? 1 : 8,desc);
    query.Proceed();
    return query.CommittedStatus()==COMMITTED_TRIANGLE_HIT;
}
float4 texel(uint4 descriptor,int x,int y) {
    uint pixel=textureBytes.Load(descriptor.x+4*(y*(int)descriptor.y+x));
    return float4(pixel&255,(pixel>>8)&255,(pixel>>16)&255,pixel>>24)*(1.0/255.0);
}
float2 hitUV(Hit hit) {
    uint first=hit.primitive*3;
    return uvs[first]*(1-hit.bary.x-hit.bary.y)+uvs[first+1]*hit.bary.x+uvs[first+2]*hit.bary.y;
}
float3 hitNormal(uint primitive) {
    uint first=primitive*3;
    return normalize(cross(vertices[first+1].xyz-vertices[first].xyz,vertices[first+2].xyz-vertices[first].xyz));
}
float4 sampleTexture(int slot,float2 uv) {
    uint4 descriptor=textureDescriptors[slot];
    float2 p=uv*float2(descriptor.yz)-0.5;
    int2 origin=(int2)floor(p); float2 f=frac(p);
    int width=(int)descriptor.y,height=(int)descriptor.z;
    int x0=(origin.x%width+width)%width,y0=(origin.y%height+height)%height;
    int x1=x0+1==width ? 0 : x0+1,y1=y0+1==height ? 0 : y0+1;
    return lerp(lerp(texel(descriptor,x0,y0),texel(descriptor,x1,y0),f.x),
                lerp(texel(descriptor,x0,y1),texel(descriptor,x1,y1),f.x),f.y);
}
bool translucent(Material material) { return material.textures.y>=0 || material.reflectivity.w<1; }
float transmission(Hit hit,float3 direction) {
    Material material=materials[primitives[hit.primitive].y];
    if(material.textures.y>=0) return 1-sampleTexture(material.textures.y,hitUV(hit)).a;
    return pow(1-material.reflectivity.w,1/max(abs(dot(direction,hitNormal(hit.primitive))),minimumCosine));
}
float3 reflectance(Hit hit) {
    Material material=materials[primitives[hit.primitive].y];
    float2 uv=hitUV(hit);
    float3 base=material.textures.x>=0 ? sampleTexture(material.textures.x,uv).rgb : material.reflectivity.rgb;
    float3 tint=material.multiplier.rgb*colors[hit.primitive];
    if(material.textures.z>=0)
        tint=(tint-maskWhite)*sampleTexture(material.textures.z,uv)[material.textures.w]+maskWhite;
    return base*tint;
}
#if VRAD3_CS2
float3 srgbTexel(uint4 descriptor,int x,int y) {
    const float3 c=texel(descriptor,x,y).rgb;
    return select(c<=0.04045,c/12.92,pow((c+0.055)/1.055,2.4));
}
float3 sampleSrgbTexture(int slot,float2 uv) {
    uint4 descriptor=textureDescriptors[slot];
    float2 p=uv*float2(descriptor.yz)-0.5;
    int2 origin=(int2)floor(p); float2 f=frac(p);
    int width=(int)descriptor.y,height=(int)descriptor.z;
    int x0=(origin.x%width+width)%width,y0=(origin.y%height+height)%height;
    int x1=x0+1==width ? 0 : x0+1,y1=y0+1==height ? 0 : y0+1;
    return lerp(lerp(srgbTexel(descriptor,x0,y0),srgbTexel(descriptor,x1,y0),f.x),
                lerp(srgbTexel(descriptor,x0,y1),srgbTexel(descriptor,x1,y1),f.x),f.y);
}
#endif
// emission.rgb is the glow before the albedo factor (emission.w); as in CS2's closest-hit shader the factor uses the hit's albedo times material tint and prop Render Color. multiplier.w > 0 is the per-texel mask's texture slot + 1.
float3 emissionAt(Hit hit,Material material) {
    float3 emission=material.emission.rgb;
#if VRAD3_CS2
    [branch] if(material.multiplier.w>0) emission*=sampleSrgbTexture((int)material.multiplier.w-1,hitUV(hit));
#endif
    [branch] if(material.emission.w==0) return emission;
    float3 base=material.textures.x>=0 ? sampleTexture(material.textures.x,hitUV(hit)).rgb : material.reflectivity.rgb;
    float3 c=colors[hit.primitive];
    float3 render=select(c<=0.04045,c/12.92,pow((c+0.055)/1.055,2.4));
    return emission*lerp(1,base*material.multiplier.rgb*render,material.emission.w);
}
// CS2 shadow rays cull nothing, so a one-sided face shadows whichever way it faces; backface_shadows = hla skips faces turned toward the receiver, as vrad3 does. Instance mask 2 holds opaque partitions, 4 translucent ones.
float shadowVisibility(float3 origin,float3 direction,float distance) {
    research(3);
    RayDesc desc; desc.Origin=origin; desc.Direction=direction; desc.TMin=0; desc.TMax=distance;
    if(traversalOptions&1) {
#if VRAD3_CS2
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> blocker;
        blocker.TraceRayInline(scene,hlaShadows() ? RAY_FLAG_CULL_BACK_FACING_TRIANGLES : RAY_FLAG_NONE,2,desc);
#else
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_CULL_BACK_FACING_TRIANGLES | RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> blocker;
        blocker.TraceRayInline(scene,RAY_FLAG_NONE,2,desc);
#endif
        while(blocker.Proceed()) { }
        if(blocker.CommittedStatus()==COMMITTED_TRIANGLE_HIT) return 0;
        if(traversalOptions&4096) return 1;
        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_CULL_OPAQUE> transmitting;
        transmitting.TraceRayInline(scene,RAY_FLAG_NONE,4,desc);
        float visibility=1;
        while(transmitting.Proceed()) {
            if(transmitting.CandidateType()!=CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
            research(8); research(10);
            Hit hit;
            hit.primitive=traversalMetadata.Load(4*(transmitting.CandidateInstanceID()+transmitting.CandidatePrimitiveIndex()));
            hit.distance=transmitting.CandidateTriangleRayT(); hit.bary=transmitting.CandidateTriangleBarycentrics();
            uint flags=probeFlags();
            if(flags&2) continue;
            visibility*=(flags&1) ? 0 : transmission(hit,direction);
            if(visibility==0) transmitting.Abort();
        }
        return visibility;
    }
    RayQuery<RAY_FLAG_FORCE_NON_OPAQUE> query;
    query.TraceRayInline(scene,RAY_FLAG_NONE,255,desc);
    float visibility=1;
    while(query.Proceed()) {
        if(query.CandidateType()!=CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        research(8);
        uint primitive=query.CandidatePrimitiveIndex();
        uint flags=primitives[primitive].x|meshFlags;
        Material material=materials[primitives[primitive].y];
        if((flags&((maxBounces&0x40000000u)&&translucent(material) ? geometryMask : shadowMask))) continue;
        if(translucent(material)) {
            research(10);
            Hit hit; hit.primitive=primitive; hit.distance=query.CandidateTriangleRayT();
            hit.bary=query.CandidateTriangleBarycentrics();
            visibility*=transmission(hit,direction);
        } else {
            if((!VRAD3_CS2 || hlaShadows()) && !(flags&2) && dot(hitNormal(primitive),direction)<0) continue;
            query.CommitNonOpaqueTriangleHit();
        }
    }
    return query.CommittedStatus()==COMMITTED_TRIANGLE_HIT ? 0 : visibility;
}
// Ortho light (type 4): a parallel light confined to a width x height box in front of its origin along D, up to the range; comparisons are Valve's (strict).
bool orthoInside(Light light,float3 position) {
    float3 delta=light.positionOffset.xyz-position;
    float3 side=cross(light.directionSize.xyz,light.tangent.xyz);
    if(light.cone.w>0) {
        float along=-dot(delta,light.directionSize.xyz);
        return abs(dot(delta,side))<=light.cone.x*0.5 && abs(dot(delta,light.tangent.xyz))<=light.cone.y*0.5 &&
               along>=0 && along<=light.rgbRange.w;
    }
    float along=dot(delta,light.directionSize.xyz);
    return along<0 && -light.rgbRange.w<along && abs(dot(delta,side))<light.cone.x*0.5 && abs(dot(delta,light.tangent.xyz))<light.cone.y*0.5;
}
float3 orthoEndpoint(Light light,float3 position,float2 u) {
    float3 forward=light.directionSize.xyz,direction=-forward;
    if(light.flags.y!=0 && light.directionSize.w>0) {
        direction+=light.directionSize.w*sqrt(u.x)*(cos(2*PI*u.y)*cross(forward,light.tangent.xyz)+sin(2*PI*u.y)*light.tangent.xyz);
        direction=normalize(direction);
    }
    float denominator=dot(direction,forward);
    float t=denominator!=0 ? (dot(forward,light.positionOffset.xyz)-dot(forward,position))/denominator : 0;
    return position+direction*t;
}
float3 lightEndpoint(Light light,float3 position,float3 direction,float distance,inout uint rng) {
    float radius=light.directionSize.w;
    if(light.flags.x==4) {
        float2 u=0;
        if(light.flags.y!=0 && radius>0) u=endpointRandom(rng);
        return orthoEndpoint(light,position,u);
    }
    if(light.flags.x==2) {
        if(light.flags.y!=0 && radius>0) {
            float2 r=endpointRandom(rng); float radial=sqrt(r.x),angle=2*PI*r.y;
            direction+=radius*radial*(cos(angle)*cross(light.tangent.xyz,light.directionSize.xyz)+sin(angle)*light.tangent.xyz);
        }
        return position+direction*(directionalDistance>0 ? directionalDistance : 2000000);
    }
    float3 endpoint=light.positionOffset.xyz;
    if(light.flags.y!=0 && radius>0) {
        float3 axis=-direction;
        float3 tangent=normalize(cross(abs(axis.z)<0.999 ? float3(0,0,1) : float3(0,1,0),axis));
        float ratio=min(radius/distance,1);
        float2 r=endpointRandom(rng); float radial=radius*sqrt(max(0,1-ratio*ratio))*sqrt(r.x),angle=2*PI*r.y;
        float3 offset=radial*(cos(angle)*tangent+sin(angle)*cross(axis,tangent));
        endpoint+=offset+axis*sqrt(max(0,radius*radius-dot(offset,offset)));
    }
    return endpoint;
}
float cookiePlane(uint base,uint width,uint height,float2 uv) {
    float2 pixel=uv*float2(width,height)-0.5;
    int2 lo=(int2)floor(pixel); float2 fraction=frac(pixel);
    int2 a=int2((lo.x%(int)width+(int)width)%(int)width,(lo.y%(int)height+(int)height)%(int)height);
    uint2 b=uint2(a.x+1==width ? 0 : a.x+1,a.y+1==height ? 0 : a.y+1);
    float p00=asfloat(environmentSky.Load(base+(a.y*width+a.x)*4));
    float p10=asfloat(environmentSky.Load(base+(a.y*width+b.x)*4));
    float p01=asfloat(environmentSky.Load(base+(b.y*width+a.x)*4));
    float p11=asfloat(environmentSky.Load(base+(b.y*width+b.x)*4));
    return lerp(lerp(p00,p10,fraction.x),lerp(p01,p11,fraction.x),fraction.y);
}
// Cookie image layout in environmentSky: width, height, then three float planes at header+16; sampled bilinearly with wraparound.
float3 spotCookie(Light light,float3 position) {
    uint header=(uint)light.cone.w,width=environmentSky.Load(header),height=environmentSky.Load(header+4);
    float3 delta=position-light.positionOffset.xyz;
    float3 side=normalize(cross(light.tangent.xyz,light.directionSize.xyz));
    float3 up=normalize(cross(light.directionSize.xyz,side));
    float forward=dot(delta,light.directionSize.xyz);
    float2 uv=0.5-float2(dot(delta,side),dot(delta,up))*(0.5*light.tangent.w/forward);
    uint bytes=width*height*4,base=header+16;
    return float3(cookiePlane(base,width,height,uv),cookiePlane(base+bytes,width,height,uv),
                  cookiePlane(base+2*bytes,width,height,uv));
}
float3 orthoCookie(Light light,float3 position) {
    uint header=(uint)light.cone.w,width=environmentSky.Load(header),height=environmentSky.Load(header+4);
    float3 delta=position-light.positionOffset.xyz;
    float3 side=normalize(cross(light.tangent.xyz,light.directionSize.xyz));
    float3 up=normalize(cross(light.directionSize.xyz,side));
    float2 uv=0.5-float2(dot(delta,side),dot(delta,up))*(0.5*light.tangent.w);
    uint bytes=width*height*4,base=header+16;
    return float3(cookiePlane(base,width,height,uv),cookiePlane(base+bytes,width,height,uv),
                  cookiePlane(base+2*bytes,width,height,uv));
}
static bool lpOn=false;
static float3 lpPrevPos=0,lpPrevNormal=0;
static int lpCount=-1;
bool lpEmitter(Light light) { return (light.flags.w&1) && (light.flags.x==1 || light.flags.x==3 || light.flags.x==4); }
uint lpEmitterCount() {
    if(lpCount<0) { uint n=0; for(uint i=0;i<lightCount;++i) if(lpEmitter(lights[i])) ++n; lpCount=(int)n; }
    return (uint)lpCount;
}
float lpEmissionPdf(Light light,float3 position,float3 normal) {
    if(light.flags.x==1 || light.flags.x==3) {
        float3 toLight=light.positionOffset.xyz-position; float squared=dot(toLight,toLight);
        if(squared<=0) return 0;
        float3 l=toLight*rsqrt(squared); float c=max(dot(l,normal),0);
        if(light.flags.x==1) return c/(4*PI*squared);
        if(-dot(l,light.directionSize.xyz)<light.cone.x) return 0;
        return c/((1-light.cone.x)*2*PI*squared);
    }
    if(light.flags.x==4) {
        float3 delta=position-light.positionOffset.xyz,side=normalize(cross(light.directionSize.xyz,light.tangent.xyz));
        if(abs(dot(delta,light.tangent.xyz))>light.cone.y*0.5 || abs(dot(delta,side))>light.cone.x*0.5 || dot(delta,light.directionSize.xyz)<0) return 0;
        return max(-dot(light.directionSize.xyz,normal),0)/(light.cone.x*light.cone.y);
    }
    return 0;
}
static uint lpTreeAt=0xffffffffu;
uint lpTree() { if(lpTreeAt==0xffffffffu) lpTreeAt=environmentSky.Load(92); return lpTreeAt; }
uint lpWord(uint at) { return environmentSky.Load(at); }
float lpFloat(uint at) { return asfloat(environmentSky.Load(at)); }
float lpSafe(float x) { return x==0 ? 1.1920929e-7 : x; }
float4 lpLoad4(uint at) { return asfloat(environmentSky.Load4(at)); }
float4 lpSafe4(float4 x) { return select(x==0,1.1920929e-7,x); }
float4 lpImportance4(uint soa,uint cosAt,uint sinAt,uint phiAt,float3 p,float3 n) {
    const float4 phi=lpLoad4(phiAt);
    if(!any(phi>0)) return 0;
    const float4 lx=lpLoad4(soa),ly=lpLoad4(soa+16),lz=lpLoad4(soa+32),hx=lpLoad4(soa+48),hy=lpLoad4(soa+64),hz=lpLoad4(soa+80);
    const float4 cx=(lx+hx)*0.5,cy=(ly+hy)*0.5,cz=(lz+hz)*0.5,dx=hx-lx,dy=hy-ly,dz=hz-lz;
    const float4 ox=p.x-cx,oy=p.y-cy,oz=p.z-cz;
    const float4 squared=max(ox*ox+oy*oy+oz*oz,(dx*dx+dy*dy+dz*dz)*0.0625);
    float4 factor=1;
    const bool4 inside=and(and(and(p.x>=lx,p.x<=hx),and(p.y>=ly,p.y<=hy)),and(p.z>=lz,p.z<=hz));
    if(any(n!=0) && !all(inside)) {
        float4 wx=-ox,wy=-oy,wz=-oz; const float4 r=rsqrt(lpSafe4(wx*wx+wy*wy+wz*wz)); wx*=r; wy*=r; wz*=r;
        const float4 cosI=clamp(n.x*wx+n.y*wy+n.z*wz,-1,1),sinI=sqrt(1-cosI*cosI);
        const float4 cosW=clamp(-(wx*lpLoad4(soa+96)+wy*lpLoad4(soa+112)+wz*lpLoad4(soa+128)),-1,1),sinW=sqrt(1-cosW*cosW);
        float4 cosB=1;
        if(traversalOptions&1024) cosB=0.5; else {
        const float4 kx0=lx-p.x,kx1=hx-p.x,ky0=ly-p.y,ky1=hy-p.y,kz0=lz-p.z,kz1=hz-p.z;
        const float4 dx0=wx*kx0,dx1=wx*kx1,dy0=wy*ky0,dy1=wy*ky1,dz0=wz*kz0,dz1=wz*kz1;
        const float4 sx0=kx0*kx0,sx1=kx1*kx1,sy0=ky0*ky0,sy1=ky1*ky1,sz0=kz0*kz0,sz1=kz1*kz1;
        [unroll] for(uint c=0;c<8;++c) {
            const float4 d=((c&1)?dx1:dx0)+((c&2)?dy1:dy0)+((c&4)?dz1:dz0);
            const float4 s=((c&1)?sx1:sx0)+((c&2)?sy1:sy0)+((c&4)?sz1:sz0);
            cosB=min(cosB,d*rsqrt(lpSafe4(s)));
        }
        }
        cosB=clamp(cosB,-1,1); const float4 sinB=sqrt(1-cosB*cosB);
        const float4 cosO=lpLoad4(cosAt),sinO=lpLoad4(sinAt);
        const float4 cosX=clamp(select(cosO<cosW,1,cosO*cosW+sinW*sinO),-1,1),sinX=sqrt(1-cosX*cosX);
        const float4 cosP=max(0,select(cosB<cosX,1,cosX*cosB+sinX*sinB));
        const float4 cosIp=abs(select(cosB<cosI,1,cosI*cosB+sinI*sinB));
        factor=select(inside,1,cosIp*cosP);
    }
    return select(phi>0,factor*phi/lpSafe4(squared),0);
}
float4 lpNodeImportance4(uint at,float3 p,float3 n) { return lpImportance4(at,at+160,at+176,at+144,p,n); }
float4 lpPacketImportance4(uint at,float3 p,float3 n) { return lpImportance4(at+32,at+176,at+192,at+208,p,n); }
float4 lpRect(uint word) { return float4(word&255,(word>>8)&255,(word>>16)&255,word>>24)*0.0625-float4(0,0,1.1920929e-7,1.1920929e-7); }
// Valve's emitter tree is appended to environmentSky (header word 23 is its offset): 240-byte nodes and 224-byte leaf packets, each holding four cell clusters in SoA layout.
bool lpTreeSelect(uint tree,float3 p,float3 n,inout float u,out uint gpuLight,out float4 rect,out float density) {
    gpuLight=0; rect=0; density=0;
    const uint nodes=lpWord(tree+4),records=lpWord(tree+8),nodesAt=lpWord(tree+24),recordsAt=lpWord(tree+28),mapAt=lpWord(tree+36);
    float probability=1; uint child=nodes ? 0 : 0xffffffffu,begin=0,end=records;
    uint depth=0,phase=0,r=0; float total=0,prefix=0,previous=0;
    for(;;) {
        if(phase==0 && !(depth<64 && child<nodes)) {
            if(end<=begin || end>records) return false;
            phase=1; r=begin; total=0;
        }
        if(phase==1 && r==end) { if(!(total>0)) return false; phase=2; r=begin; }
        if(phase==2 && r==end) return false;
        const uint at=phase==0 ? nodesAt+child*240 : recordsAt+r*224;
        const uint4 o=phase==0 ? uint4(0,160,176,144) : uint4(32,176,192,208);
        const float4 importance=lpImportance4(at+o.x,at+o.y,at+o.z,at+o.w,p,n);
        if(phase==0) {
            const float nodeTotal=importance.x+importance.y+importance.z+importance.w;
            if(!(nodeTotal>0)) return false;
            float nodePrefix=0,nodePrevious=0,share=0; uint chosen=4;
            [unroll] for(uint lane=0;lane<4;++lane) {
                nodePrefix+=importance[lane]; const float c=nodePrefix>=nodeTotal ? 1 : nodePrefix/nodeTotal;
                if(chosen==4) { if(u<c) { chosen=lane; share=c-nodePrevious; } else nodePrevious=c; }
            }
            if(chosen==4 || !(share>0)) return false;
            u=min((u-nodePrevious)/share,1-1.1920929e-7); probability*=share;
            child=lpWord(at+192+chosen*4); begin=lpWord(at+208+chosen*4); end=lpWord(at+224+chosen*4);
            ++depth; continue;
        }
        ++r;
        if(phase==1) { total+=importance.x+importance.y+importance.z+importance.w; continue; }
        for(uint lane=0;lane<4;++lane) {
        prefix+=importance[lane]; const float c=prefix>=total ? 1 : prefix/total;
        if(u<c) {
            const float share=c-previous;
            if(!(share>0)) return false;
            u=min((u-previous)/share,1-1.1920929e-7);
            gpuLight=lpWord(mapAt+lpWord(at+lane*4)*4); rect=lpRect(lpWord(at+16+lane*4));
            if(gpuLight>=lightCount) return false;
            density=probability*share/((rect.z-rect.x)*(rect.w-rect.y));
            return true;
        }
        previous=c;
        }
    }
    return false;
}
float lpTreeDensity(uint tree,uint gpuLight,float2 uv,float3 p,float3 n) {
    const uint nodes=lpWord(tree+4),records=lpWord(tree+8),nodesAt=lpWord(tree+24),recordsAt=lpWord(tree+28);
    const uint valve=lpWord(lpWord(tree+40)+gpuLight*4);
    const uint cu=(uint)clamp((int)floor(uv.x*16),0,15),cv=(uint)clamp((int)floor(uv.y*16),0,15);
    const uint trail=lpWord(lpWord(tree+32)+((valve*16+cv)*16+cu)*4);
    if(trail==0xffffffffu) return 0;
    float probability=1; uint child=nodes ? 0 : 0xffffffffu,begin=0,end=records,shift=0;
    uint depth=0,r=0; bool packets=false;
    float total=0,mine=0; float4 rect=0;
    for(;;) {
        if(!packets && !(depth<16 && child<nodes)) {
            if(child<nodes || end<=begin || end>records) return 0;
            packets=true; r=begin;
        }
        if(packets && r>=end) break;
        const uint at=packets ? recordsAt+r*224 : nodesAt+child*240;
        const uint4 o=packets ? uint4(32,176,192,208) : uint4(0,160,176,144);
        const float4 importance=lpImportance4(at+o.x,at+o.y,at+o.z,at+o.w,p,n);
        if(!packets) {
            const uint chosen=(trail>>shift)&3; shift+=2;
            const float nodeTotal=importance.x+importance.y+importance.z+importance.w;
            if(!(nodeTotal>0)) return 0;
            probability*=importance[chosen]/nodeTotal;
            child=lpWord(at+192+chosen*4); begin=lpWord(at+208+chosen*4); end=lpWord(at+224+chosen*4);
            ++depth; continue;
        }
        ++r;
        total+=importance.x+importance.y+importance.z+importance.w;
        const uint4 owner=environmentSky.Load4(at),words=environmentSky.Load4(at+16);
        for(uint lane=0;lane<4;++lane) {
            const uint word=words[lane];
            if(owner[lane]==valve && (word&255)<=cu && ((word>>8)&255)<=cv && cu<((word>>16)&255) && cv<(word>>24) && !(mine>0)) {
                mine=importance[lane]; rect=lpRect(word);
            }
        }
    }
    if(!(total>0) || !(mine>0)) return 0;
    return probability*mine/total/((rect.z-rect.x)*(rect.w-rect.y));
}
void lpSpotFrame(float3 d,out float3 b1,out float3 b2) {
    const float s=d.z>=0 ? 1 : -1,a=-1/(s+d.z),b=d.x*d.y*a;
    b1=float3(1+s*d.x*d.x*a,s*b,-s*d.x); b2=float3(b,s+d.y*d.y*a,-d.y);
}
float2 lpEmissionSample(Light light,float3 position) {
    if(light.flags.x==4) {
        const float3 delta=position-light.positionOffset.xyz,side=normalize(cross(light.directionSize.xyz,light.tangent.xyz));
        return float2(dot(delta,side)/light.cone.x+0.5,dot(delta,light.tangent.xyz)/light.cone.y+0.5);
    }
    float3 d=position-light.positionOffset.xyz; d*=rsqrt(lpSafe(dot(d,d)));
    if(light.flags.x==1) {
        float phi=0;
        if(d.x!=0 || d.y!=0) { phi=atan2(d.y,d.x); if(phi<0) phi+=2*PI; phi*=1/(2*PI); }
        return float2(phi,0.5-d.z*0.5);
    }
    float3 b1,b2; lpSpotFrame(light.directionSize.xyz,b1,b2);
    const float x=dot(d,b1),y=dot(d,b2);
    float phi=0;
    if(x!=0 || y!=0) { phi=atan2(y,x); if(phi<0) phi+=2*PI; phi*=1/(2*PI); }
    return float2(clamp((1-dot(d,light.directionSize.xyz))/(1-light.cone.x),0,0.9999999),clamp(phi,0,0.9999999));
}
float lpCameraWeight(uint index,Light light,float3 position,float3 normal) {
    if(!lpEmitter(light)) return 1;
    const uint tree=(traversalOptions&256) ? 0 : lpTree();
    float pL;
    if(tree) {
        const float density=lpTreeDensity(tree,index,lpEmissionSample(light,position),lpPrevPos,lpPrevNormal);
        if(!(density>0)) return 1;
        pL=density*lpEmissionPdf(light,position,normal);
    }
    else pL=lpEmissionPdf(light,position,normal)/lpEmitterCount();
    if(pL<=0) return 1;
    float3 v=position-lpPrevPos; float squared=dot(v,v);
    if(squared<=0) return 1;
    float3 w=v*rsqrt(squared);
    float pC=max(dot(lpPrevNormal,w),0)/PI*max(-dot(normal,w),0)/squared;
    return pC/(pC+pL);
}
float3 lpLightEnergy(Light light,float3 position,float3 normal) {
    float3 direction; float attenuation=1;
    bool cookie=light.flags.x==3 && light.cone.w>0;
    if(light.flags.x==4) {
        if(!orthoInside(light,position)) return 0;
        direction=-light.directionSize.xyz;
    } else {
        float3 delta=light.positionOffset.xyz-position;
        float squared=dot(delta,delta),distance=sqrt(squared);
        if(distance==0 || (light.rgbRange.w>0 && distance>=light.rgbRange.w)) return 0;
        direction=delta/distance;
        float denominator=0;
        if(light.flags.z&1) denominator+=light.attenuation.x;
        if(light.flags.z&2) denominator+=light.attenuation.y*(cookie ? distance : sqrt(max(squared,1.0)));
        if(light.flags.z&4) denominator+=light.attenuation.z*(cookie ? squared : max(squared,1.0));
        attenuation=max(0,1/max(denominator,1e-20)-light.positionOffset.w);
        if(light.flags.x==3) {
            float coneCosine=-dot(direction,light.directionSize.xyz);
            if(cookie ? coneCosine<light.cone.x : coneCosine<=light.cone.x) return 0;
            if(!cookie) {
                float cone=saturate((coneCosine-light.cone.x)*light.cone.y);
                float exponent=light.cone.z==0 || light.cone.z==1 ? 1 : trunc(light.cone.z*4)*0.25;
                attenuation*=pow(cone,exponent);
            }
        }
    }
    float cosine=max(dot(normal,direction),0);
    const bool orthoProjected=light.flags.x==4 && light.cone.w>0;
    if((cosine<=0 && !orthoProjected) || attenuation<=0) return 0;
    if(cookie) return light.rgbRange.rgb*spotCookie(light,position)*attenuation;
    if(orthoProjected) return light.rgbRange.rgb*orthoCookie(light,position)*attenuation;
    return light.rgbRange.rgb*(attenuation*cosine);
}
float3 illumination(float3 position,float3 normal,inout uint rng) {
    research(4);
    float3 total=0;
#if VRAD3_CS2
    float risU=random(rng),risTotal=0,risWeight=0,risDistance=0; float3 risContribution=0,risDirection=0; uint risLight=0;
#endif
    uint node=0,nodeCount=lightCount ? lightCount*2-1 : 0,pending=0xfffffffeu;
    uint gridCursor=0,gridEnd=0,gridBounds=0; bool grid=false;
    if((traversalOptions&2) && nodeCount) {
        const uint header=lightBoundsOffset+nodeCount*32;
        const uint4 h0=traversalMetadata.Load4(header),h1=traversalMetadata.Load4(header+16),h2=traversalMetadata.Load4(header+32);
        grid=h2.x!=0 && !any(isnan(position));
        if(grid) {
            const uint3 dims=uint3(h0.w,h1.w,h2.x);
            const uint3 c=uint3(clamp(floor((position-asfloat(h0.xyz))*asfloat(h1.xyz)),0,float3(dims-1)));
            const uint2 range=traversalMetadata.Load2(h2.y+((c.z*dims.y+c.y)*dims.x+c.x)*4);
            gridCursor=h2.z+range.x*4; gridEnd=h2.z+range.y*4; gridBounds=h2.w;
        }
    }
    for(uint index=0;index<lightCount;) {
        if(traversalOptions&2) {
            if(pending==0xfffffffeu) {
                pending=0xffffffffu;
                if(grid) {
                    while(gridCursor<gridEnd) {
                        const uint candidate=traversalMetadata.Load(gridCursor); gridCursor+=4;
                        const float4 lower=asfloat(traversalMetadata.Load4(gridBounds+candidate*32));
                        const float4 upper=asfloat(traversalMetadata.Load4(gridBounds+candidate*32+16));
                        if(any(position<lower.xyz) || any(position>upper.xyz)) continue;
                        pending=candidate; break;
                    }
                } else while(node<nodeCount) {
                    uint address=lightBoundsOffset+node*32;
                    const uint4 a=traversalMetadata.Load4(address),b=traversalMetadata.Load4(address+16);
                    if(any(position<asfloat(a.xyz)) || any(position>asfloat(b.xyz))) { node=a.w; continue; }
                    ++node;
                    if(b.w!=0xffffffff) { pending=b.w; break; }
                }
            }
            if(pending==0xffffffffu) break;
            index=pending; pending=0xfffffffeu;
        }
        Light light=lights[index++];
        if(!(light.flags.w&1)) continue;
        research(5);
        float3 direction; float distance; float attenuation=1;
        bool cookie=light.flags.x==3 && light.cone.w>0;
        if(light.flags.x==2) {
            direction=light.directionSize.xyz; distance=1e23;
        } else if(light.flags.x==4) {
            if(!orthoInside(light,position)) continue;
            direction=-light.directionSize.xyz; distance=1e23;
        } else {
            float3 delta=light.positionOffset.xyz-position;
            float squared=dot(delta,delta); distance=sqrt(squared);
            if(distance==0 || (light.rgbRange.w>0 && distance>=light.rgbRange.w)) continue;
            direction=delta/distance;
            float denominator=0;
            if(light.flags.z&1) denominator+=light.attenuation.x;
            if(light.flags.z&2) denominator+=light.attenuation.y*(cookie ? distance : sqrt(max(squared,1.0)));
            if(light.flags.z&4) denominator+=light.attenuation.z*(cookie ? squared : max(squared,1.0));
            attenuation=max(0,1/max(denominator,1e-20)-light.positionOffset.w);
            if(light.flags.x==3) {
                float coneCosine=-dot(direction,light.directionSize.xyz);
                if(cookie ? coneCosine<light.cone.x : coneCosine<=light.cone.x) continue;
                if(!cookie) {
                    float cone=saturate((coneCosine-light.cone.x)*light.cone.y);
                    float exponent=light.cone.z==0 || light.cone.z==1 ? 1 : trunc(light.cone.z*4)*0.25;
                    attenuation*=pow(cone,exponent);
                }
            }
        }
        research(6);
        float cosine=max(dot(normal,direction),0);
        const bool orthoProjected=light.flags.x==4 && light.cone.w>0;
        if((cosine<=0 && !orthoProjected) || attenuation<=0) continue;
#if VRAD3_CS2
        {
            const float3 unoccluded=cookie ? light.rgbRange.rgb*spotCookie(light,position)*attenuation
                                  : orthoProjected ? light.rgbRange.rgb*orthoCookie(light,position)*attenuation
                                  : light.rgbRange.rgb*(attenuation*cosine);
            const float weight=dot(abs(unoccluded),float3(0.2125,0.7154,0.0721));
            if(!(weight>0)) continue;
            risTotal+=weight;
            const float share=weight/risTotal;
            if(risU<share) {
                risU=min(risU/share,0.99999994f);
                risLight=index-1; risWeight=weight; risContribution=unoccluded; risDirection=direction; risDistance=distance;
            } else risU=(risU-share)/(1-share);
            continue;
        }
#endif
        float3 endpoint=lightEndpoint(light,position,direction,distance,rng);
        float3 origin=position+transmissionEpsilon*normal,delta=endpoint-origin;
        float shadowDistance=length(delta);
        float visibility=shadowDistance>0 ? shadowVisibility(origin,delta/shadowDistance,shadowDistance) : 0;
        [branch] if(VRAD3_EXPERIMENTAL && lpOn) {
            const float share=visibility>0 ? lpCameraWeight(index-1,light,position,normal) : 0;
            if(cookie) total+=light.rgbRange.rgb*spotCookie(light,position)*(attenuation*visibility*share);
            else if(orthoProjected) total+=light.rgbRange.rgb*orthoCookie(light,position)*(attenuation*visibility*share);
            else total+=light.rgbRange.rgb*(attenuation*cosine*visibility*share);
        }
        else if(cookie) total+=light.rgbRange.rgb*spotCookie(light,position)*(attenuation*visibility);
        else if(orthoProjected) total+=light.rgbRange.rgb*orthoCookie(light,position)*(attenuation*visibility);
        else total+=light.rgbRange.rgb*(attenuation*cosine*visibility);
    }
#if VRAD3_CS2
    if(risTotal>0) {
        const Light light=lights[risLight];
        const float3 endpoint=lightEndpoint(light,position,risDirection,risDistance,rng);
        const float3 origin=position+transmissionEpsilon*normal,delta=endpoint-origin;
        const float shadowDistance=length(delta);
        const float visibility=shadowDistance>0 ? shadowVisibility(origin,delta/shadowDistance,shadowDistance) : 0;
        total=risContribution*(visibility*(risTotal/risWeight));
    }
#endif
    return total;
}
float3 lightPathAt(float3 pV,float3 nV,inout uint rng,out float3 shRgb,out float3 towardHit) {
    shRgb=0; towardHit=0;
    const uint tree=(traversalOptions&512) ? 0 : lpTree();
    uint index=0; float selection=0;
    float a=random(rng),b=random(rng);
    if(tree) {
        float4 rect;
        if(!lpTreeSelect(tree,pV,nV,a,index,rect,selection)) return 0;
        a=rect.x+(rect.z-rect.x)*a; b=rect.y+(rect.w-rect.y)*b;
    } else {
        const uint count=lpEmitterCount();
        if(!count) return 0;
        uint pick=min((uint)(a*count),count-1),k=0;
        for(uint i=0;i<lightCount;++i) { if(!lpEmitter(lights[i])) continue; if(k==pick) { index=i; break; } ++k; }
        a=random(rng); selection=1.0/count;
    }
    Light light=lights[index];
    if(!lpEmitter(light)) return 0;
    float3 origin,dir;
    if(light.flags.x==1) {
        float z=1-2*b,r=2*sqrt(max(0,b-b*b)),phi=2*PI*a;
        dir=float3(cos(phi)*r,sin(phi)*r,z); origin=light.positionOffset.xyz;
    } else if(light.flags.x==3) {
        float3 b1,b2; lpSpotFrame(light.directionSize.xyz,b1,b2);
        float ct=a*light.cone.x+(1-a),st=sqrt(max(0,1-ct*ct)),phi=2*PI*b;
        dir=b1*(cos(phi)*st)+b2*(sin(phi)*st)+light.directionSize.xyz*ct; origin=light.positionOffset.xyz;
    } else {
        float3 axis=light.directionSize.xyz,side=normalize(cross(axis,light.tangent.xyz));
        origin=light.positionOffset.xyz+side*((a-0.5)*light.cone.x)+light.tangent.xyz*((b-0.5)*light.cone.y); dir=axis;
    }
    origin+=dir*0.1;
    float transmit=1;
    for(uint step=0;step<16;++step) {
        Hit hit=trace(origin,dir,1e23,geometryMask,0);
        if(hit.primitive==0xffffffff) return 0;
        Material material=materials[primitives[hit.primitive].y];
        float3 n2=hitNormal(hit.primitive);
        const bool backFace=dot(n2,dir)>0 && !((primitives[hit.primitive].x|meshFlags)&2);
        if(translucent(material) || backFace) {
            if(!backFace) transmit*=transmission(hit,dir);
            if(transmit<=0) return 0;
            origin+=dir*(hit.distance+transmissionEpsilon); continue;
        }
        if(dot(n2,dir)>0) n2=-n2;
        float3 x2=origin+dir*hit.distance;
        if(traversalOptions&2) {
            Light meta=lights[index+lightCount];
            if(any(x2<meta.rgbRange.xyz) || any(x2>meta.positionOffset.xyz)) return 0;
        }
        float3 energy=lpLightEnergy(light,x2,n2);
        if(all(energy<=0)) return 0;
        float3 v=pV-x2; float squared=dot(v,v);
        if(squared<transmissionEpsilon) return 0;
        float3 w=v*rsqrt(squared);
        float c2=max(dot(n2,w),0),c1=max(-dot(nV,w),0);
        if(c1<=0 || c2<=0) return 0;
        float pC=c1/PI*c2/squared,pL=lpEmissionPdf(light,x2,n2)*selection;
        if(pL<=0) return 0;
        float3 from=pV+nV*transmissionEpsilon,span=x2+n2*transmissionEpsilon-from;
        float spanLength=length(span);
        float seen=spanLength>0 ? shadowVisibility(from,span/spanLength,spanLength) : 0;
        if(seen<=0) return 0;
        float3 L=reflectance(hit)*energy*(transmit*seen);
        towardHit=-w; shRgb=L*(c2/squared/(pC+pL));
        return L*(pC/(pC+pL));
    }
    return 0;
}
float3 radiance(float3 origin,float3 direction,inout uint rng,out float visibility,float3 previousSurfacePosition) {
    research(0);
    float3 result=0,throughput=1; visibility=1;
    uint bounce=0;
    float travelled=0;
#if VRAD3_CS2
    const bool cs2Ao=(traversalOptions&32768)!=0;
    float aoTransmit=1;
#endif
    while((maxBounces&0x07ffffffu)==0 || bounce<(maxBounces&0x07ffffffu)) {
        Hit hit=trace(origin,direction,1e23,geometryMask,0);
        if(hit.primitive==0xffffffff) {
#if VRAD3_CS2
            if(cs2Ao && bounce==0) visibility=aoTransmit;
#endif
            result+=throughput*skyRadiance(direction);
            break;
        }
        Material material=materials[primitives[hit.primitive].y];
        if(translucent(material) && !(probeFlags()&4)) {
            research(9);
#if VRAD3_CS2
            if(cs2Ao && bounce==0 && travelled+hit.distance<16) aoTransmit*=transmission(hit,direction);
#endif
            if(!(probeFlags()&8)) throughput*=transmission(hit,direction);
            if(max(throughput.x,max(throughput.y,throughput.z))<=0) break;
            float advance=hit.distance+transmissionEpsilon;
            origin+=direction*advance;
            travelled+=advance;
            continue;
        }
        research(1);
        if(bounce==0) {
            float ratio=visibilityRadius==0 ? 1 : saturate((travelled+hit.distance)/visibilityRadius);
            visibility=0.5-0.5*cos(ratio*PI);
#if VRAD3_CS2
            if(cs2Ao) visibility=travelled+hit.distance<16 ? 0 : aoTransmit;
#endif
        }
        float3 normal=hitNormal(hit.primitive);
        if(dot(normal,direction)>0) {
            if((primitives[hit.primitive].x|meshFlags)&2) normal=-normal;
#if VRAD3_CS2
            else {
                const float near=0.9999*hit.distance;
                Hit front=trace(origin+direction*near,direction,0.0002*hit.distance,geometryMask,2);
                if(front.primitive==0xffffffff) break;
                hit=front; hit.distance+=near; material=materials[primitives[hit.primitive].y]; normal=hitNormal(hit.primitive);
            }
#else
            else break;
#endif
        }
        float3 position=origin+direction*hit.distance;
        float3 albedo=reflectance(hit);
        float3 previousSegment=position-previousSurfacePosition;
        bool evaluateLights=(maxBounces&0x40000000u) || dot(previousSegment,previousSegment)>=transmissionEpsilon;
        result+=throughput*(emissionAt(hit,material)+albedo*(evaluateLights ? illumination(position,normal,rng) : float3(0,0,0)));
        [branch] if(VRAD3_EXPERIMENTAL && lpOn) {
            float3 unusedSh,unusedDirection;
            result+=throughput*albedo*lightPathAt(position,normal,rng,unusedSh,unusedDirection);
            lpPrevPos=position; lpPrevNormal=normal;
        }
        float probability=min(max(albedo.x,max(albedo.y,albedo.z)),rouletteCeiling>0 ? rouletteCeiling : 0.95);
        if(probability<=0 || random(rng)>=probability) break;
        throughput*=albedo/probability;
        previousSurfacePosition=position;
        origin=position+normal*0.1;
        direction=hemisphere(float2(random(rng),random(rng)),normal);
        ++bounce;
        travelled=0;
    }
    return result;
}
void storeAccumulator(uint offset,float4 mean,float scalarMean,float m2,uint count) {
    working.Store4(offset,asuint(mean));
    uint lo,hi; asuint((double)scalarMean,lo,hi); working.Store2(offset+16,uint2(lo,hi));
    asuint((double)m2,lo,hi); working.Store2(offset+24,uint2(lo,hi));
    working.Store(offset+32,count);
}
float directVisibility(float3 origin,float3 direction,float distance) {
    if(traceBlocked(origin,direction,distance,geometryMask|1,VRAD3_CS2 && !hlaShadows() ? 0 : 1)) return 0;
    float visibility=1;
    while(distance>0) {
        Hit hit=trace(origin,direction,distance,geometryMask,0);
        if(hit.primitive==0xffffffff) break;
        Material material=materials[primitives[hit.primitive].y];
        if(material.textures.y>=0 || material.reflectivity.w<1) visibility*=transmission(hit,direction);
        if(visibility==0) break;
        float step=hit.distance+transmissionEpsilon;
        origin+=direction*step; distance-=step;
    }
    return visibility;
}
float3 directEnergy(Light light,float3 position,out float3 direction,out float distance) {
    float attenuation=1; bool cookie=light.flags.x==3 && light.cone.w>0;
    if(light.flags.x==2) { direction=light.directionSize.xyz; distance=directionalDistance; }
    else if(light.flags.x==4) {
        direction=-light.directionSize.xyz; distance=directionalDistance;
        if(!orthoInside(light,position)) return 0;
        if(light.cone.w>0) return light.rgbRange.rgb*orthoCookie(light,position);
    }
    else {
        float3 delta=light.positionOffset.xyz-position; float squared=dot(delta,delta);
        distance=sqrt(squared); direction=distance>0 ? delta/distance : 0;
        if(distance==0 || (light.rgbRange.w>0 && (cookie ? distance>light.rgbRange.w : max(squared,1)>=light.rgbRange.w*light.rgbRange.w))) return 0;
        float denominator=0;
        if(light.flags.z&1) denominator+=light.attenuation.x;
        if(light.flags.z&2) denominator+=light.attenuation.y*(cookie ? distance : sqrt(max(squared,1)));
        if(light.flags.z&4) denominator+=light.attenuation.z*(cookie ? squared : max(squared,1));
        attenuation=max(0,1/max(denominator,1e-20)-light.positionOffset.w);
        if(light.flags.x==3) {
            float coneCosine=-dot(direction,light.directionSize.xyz);
            if(cookie ? coneCosine<light.cone.x : coneCosine<=light.cone.x) return 0;
            if(!cookie) {
                float cone=saturate((coneCosine-light.cone.x)*light.cone.y);
                float exponent=light.cone.z==0 || light.cone.z==1 ? 1 : trunc(light.cone.z*4)*0.25;
                attenuation*=pow(cone,exponent);
            }
        }
    }
    return light.rgbRange.rgb*attenuation*(cookie ? spotCookie(light,position) : float3(1,1,1));
}
float3 directEndpoint(Light light,float3 position,float3 direction,float distance,float2 u) {
    float radius=light.directionSize.w;
    if(light.flags.x==4) return orthoEndpoint(light,position,u);
    if(light.flags.x==2) {
        if(light.flags.y && radius>0) direction+=radius*sqrt(u.x)*(cos(2*PI*u.y)*cross(light.tangent.xyz,light.directionSize.xyz)+sin(2*PI*u.y)*light.tangent.xyz);
        return position+direction*directionalDistance;
    }
    float3 endpoint=light.positionOffset.xyz;
    if(light.flags.y && radius>0) {
        float3 axis=-direction;
        float3 tangent=normalize(cross(abs(axis.z)<0.999 ? float3(0,0,1) : float3(0,1,0),axis));
        float ratio=min(radius/distance,1);
        float radial=radius*sqrt(max(0,1-ratio*ratio))*sqrt(u.x);
        float3 offset=radial*(cos(2*PI*u.y)*tangent+sin(2*PI*u.y)*cross(axis,tangent));
        endpoint+=offset+axis*sqrt(max(0,radius*radius-dot(offset,offset)));
    }
    return endpoint;
}
void directLighting(uint luxel) {
    uint base=luxel*204,start=offsets[luxel],end=offsets[luxel+1];
    if(sampleBegin==0) for(uint offset=0;offset<204;offset+=4) working.Store(base+offset,0);
    if(start==end) return;
    float3 lower=1e30,upper=-1e30;
    for(uint entry=start;entry<end;++entry) { float3 p=samples[indices[entry]].position; lower=min(lower,p); upper=max(upper,p); }
    const bool census=(traversalOptions&2048)!=0;
    if(census) working.Store3(base+8,asuint((lower+upper)*0.5));
    lower-=maskWhite.x; upper+=maskWhite.x;
    float3 baked=0; float4 shR=0,shG=0,shB=0;
    float strengths[4]={0,0,0,0}; int ids[4]={255,255,255,255}; uint selected=0;
    if(sampleBegin>0) {
        selected=working.Load(base); uint packedIds=working.Load(base+80);
        [unroll] for(uint k=0;k<4;++k) { strengths[k]=asfloat(working.Load(base+64+k*4)); ids[k]=int((packedIds>>(8*k))&255); }
        baked=asfloat(working.Load3(base+144)); shR=asfloat(working.Load4(base+156)); shG=asfloat(working.Load4(base+172)); shB=asfloat(working.Load4(base+188));
    }
    for(uint index=sampleBegin;index<min(sampleEnd,lightCount);++index) {
        Light light=lights[index],meta=lights[index+lightCount];
        if(any(lower>meta.positionOffset.xyz) || any(upper<meta.rgbRange.xyz)) continue;
        int runtimeId=asint(meta.flags.x);
        uint spp=end-start==1 && meta.flags.z ? 1 : sampleCount;
        uint rng=hash((globalLuxelBase+luxel)^hash(seed+1)^hash(index+531));
        float3 rgb=0,energy=0; float4 r=0,g=0,b=0;
        for(uint sampleIndex=0;sampleIndex<spp;++sampleIndex) {
            float choice=random(rng),v=random(rng);
            uint lo=start,hi=end-1;
            while(lo<hi) { uint mid=(lo+hi)/2; if(cdf[mid]<=choice) lo=mid+1; else hi=mid; }
            float previous=lo>start ? cdf[lo-1] : 0;
            float u=end-start==1 ? choice : saturate((choice-previous)/(cdf[lo]-previous));
            Sample footprint=samples[indices[lo]];
            float3 direction; float distance;
            float3 contribution=directEnergy(light,footprint.position,direction,distance);
            float cosine=max(dot(direction,footprint.normal),0);
            if(runtimeId<0 && cosine<=0) contribution=0;
            if(meta.flags.y && any(contribution>0)) {
                float3 origin=footprint.position+transmissionEpsilon*footprint.normal;
                float3 delta=directEndpoint(light,footprint.position,direction,distance,float2(u,v))-origin;
                float segment=length(delta);
                contribution*=segment>0 ? directVisibility(origin,delta/segment,segment) : 0;
            }
            energy+=clamp(contribution,-100,100)/spp;
            rgb+=clamp(contribution*cosine,-100,100)/spp;
            if(cosine>0) {
                float4 sh=PI*float4(0.2820948,-0.48860252*direction.y,0.48860252*direction.z,-0.48860252*direction.x);
                r+=clamp(contribution.r*sh,-100,100)/spp; g+=clamp(contribution.g*sh,-100,100)/spp; b+=clamp(contribution.b*sh,-100,100)/spp;
            }
        }
        if(runtimeId<0) { baked+=rgb; shR+=r; shG+=g; shB+=b; }
        else {
            float denominator=dot(light.rgbRange.rgb,float3(1,1,1))*meta.attenuation.w;
            float scalar=saturate(dot(energy,float3(1,1,1))/denominator);
            uint limit=maxBounces&255;
            if(census && scalar>0) {
                const uint seen=working.Load(base+4);
                if(seen<32) { const uint at=base+20+(seen/4)*4; working.Store(at,working.Load(at)|((uint(runtimeId)&255)<<(8*(seen%4)))); }
                working.Store(base+4,seen+1);
            }
            if(scalar>0 && (selected<limit || scalar>strengths[limit-1])) {
                uint slot=min(selected,limit-1); selected=min(selected+1,limit);
                while(slot>0 && scalar>strengths[slot-1]) { strengths[slot]=strengths[slot-1]; ids[slot]=ids[slot-1]; --slot; }
                strengths[slot]=scalar; ids[slot]=runtimeId;
            }
        }
    }
    if(sampleEnd>=lightCount) for(uint i=1;i<selected;++i) { uint j=i; while(j>0 && ids[j]<ids[j-1]) { int id=ids[j]; ids[j]=ids[j-1]; ids[j-1]=id; float s=strengths[j]; strengths[j]=strengths[j-1]; strengths[j-1]=s; --j; } }
    working.Store(base,selected); uint packed=0;
    for(uint i=0;i<4;++i) { working.Store(base+64+i*4,asuint(strengths[i])); packed|=(uint(ids[i])&255)<<(8*i); }
    working.Store(base+80,packed);
    working.Store3(base+144,asuint(baked)); working.Store4(base+156,asuint(shR)); working.Store4(base+172,asuint(shG)); working.Store4(base+188,asuint(shB));
}

#if VRAD3_CS2
#define CS2_RUNTIME_LIGHTS 16
#define CS2_LANES 8
groupshared float4 cs2Partial[64];
float4 cs2LaneSum(float4 value,uint row) {
    cs2Partial[row]=value;
    GroupMemoryBarrierWithGroupSync();
    float4 sum=0; const uint first=row-row%CS2_LANES;
    for(uint lane=0;lane<CS2_LANES;++lane) sum+=cs2Partial[first+lane];
    GroupMemoryBarrierWithGroupSync();
    return sum;
}
float3 cs2HueClamp(float3 value) {
    const float largest=max(abs(value.r),max(abs(value.g),abs(value.b)));
    return largest>0 ? clamp(value/largest,-1,1)*min(largest,100) : 0;
}
void directLightingCs2(uint thread) {
    const uint lane=thread%CS2_LANES,row=thread%64,luxel=firstLuxel+thread/CS2_LANES;
    const bool valid=thread/CS2_LANES<dispatchLuxels && luxel<luxelCount;
    uint base=luxel*204,start=0,end=0;
    if(valid) { start=offsets[luxel]; end=offsets[luxel+1]; }
    const bool census=(traversalOptions&2048)!=0;
    float3 lower=1e30,upper=-1e30;
    for(uint entry=start;entry<end;++entry) { float3 p=samples[indices[entry]].position; lower=min(lower,p); upper=max(upper,p); }
    const float3 centre=(lower+upper)*0.5;
    lower-=maskWhite.x; upper+=maskWhite.x;
    uint runtimeLights[CS2_RUNTIME_LIGHTS]; uint runtimeCount=0,unshadowed=0;
    float runtimeDenominator[CS2_RUNTIME_LIGHTS],runtimeUnoccluded[CS2_RUNTIME_LIGHTS],runtimeVisible[CS2_RUNTIME_LIGHTS],runtimePicked[CS2_RUNTIME_LIGHTS];
    for(uint k=0;k<CS2_RUNTIME_LIGHTS;++k) { runtimeUnoccluded[k]=0; runtimeVisible[k]=0; runtimePicked[k]=0; }
    const uint chunk=max((lightCount+31)/32,1u); uint chunks=0;
    for(uint index=0;index<(start==end?0:lightCount);++index) {
        Light meta=lights[index+lightCount];
        if(any(lower>meta.positionOffset.xyz) || any(upper<meta.rgbRange.xyz)) continue;
        chunks|=1u<<(index/chunk);
        if(asint(meta.flags.x)<0) continue;
        if(runtimeCount<CS2_RUNTIME_LIGHTS) {
            runtimeLights[runtimeCount]=index;
            runtimeDenominator[runtimeCount]=dot(lights[index].rgbRange.rgb,float3(1,1,1))*meta.attenuation.w;
            if(!meta.flags.y) unshadowed|=1u<<runtimeCount;
            ++runtimeCount;
        }
    }
    uint rng=hash((globalLuxelBase+luxel)^hash(seed+1)^hash(0xc52u+lane));
    const float total=start==end?0:cdf[end-1];
    const float inverseCount=1.0/max(sampleCount,1u);
    float3 baked=0; float4 shR=0,shG=0,shB=0;
    for(uint sampleIndex=lane;sampleIndex<(start==end?0:sampleCount);sampleIndex+=CS2_LANES) {
        float choice=random(rng)*total; uint lo=start,hi=end-1;
        while(lo<hi) { uint mid=(lo+hi)/2; if(cdf[mid]<=choice) lo=mid+1; else hi=mid; }
        Sample footprint=samples[indices[lo]];
        float risU=random(rng),risTotal=0,risWeight=0,risDistance=0,risCosine=0; float3 risContribution=0,risDirection=0; uint risLight=0xffffffffu;
        float slotU=random(rng),slotTotal=0,slotDistance=0,slotFactor=1; float3 slotDirection=0; uint slotPick=0xffffffffu;
        uint slot=0;
        for(uint index=0;index<lightCount;++index) {
            if(!((chunks>>(index/chunk))&1)) { index=(index/chunk+1)*chunk-1; continue; }
            Light meta=lights[index+lightCount];
            if(any(lower>meta.positionOffset.xyz) || any(upper<meta.rgbRange.xyz)) continue;
            const int runtimeId=asint(meta.flags.x);
            uint runtimeSlot=0xffffffffu;
            if(runtimeId>=0) { if(slot<runtimeCount && runtimeLights[slot]==index) runtimeSlot=slot++; else continue; }
            Light light=lights[index];
            float3 direction; float distance;
            const float3 contribution=directEnergy(light,footprint.position,direction,distance);
            const float cosine=max(dot(direction,footprint.normal),0);
            if(runtimeSlot!=0xffffffffu) {
                const float energy=dot(clamp(contribution,-100,100),float3(1,1,1));
                runtimeUnoccluded[runtimeSlot]+=energy*inverseCount;
                const float target=energy/runtimeDenominator[runtimeSlot];
                if(!(target>0) || !meta.flags.y) continue;
                if(traversalOptions&8192) {
                    const float3 at=footprint.position+transmissionEpsilon*footprint.normal;
                    const float3 delta=directEndpoint(light,footprint.position,direction,distance,float2(random(rng),random(rng)))-at;
                    const float segment=length(delta);
                    const float visibility=segment>0 ? directVisibility(at,delta/segment,segment) : 0;
                    runtimeVisible[runtimeSlot]+=visibility*energy; runtimePicked[runtimeSlot]+=energy;
                    continue;
                }
                const float weight=max(target,1.0/64);
                slotTotal+=weight;
                const float share=weight/slotTotal;
                if(slotU<share) { slotU=min(slotU/share,0.99999994f); slotPick=runtimeSlot; slotDirection=direction; slotDistance=distance; slotFactor=target/weight; }
                else slotU=(slotU-share)/(1-share);
                continue;
            }
            const float weight=max(abs(contribution.r),max(abs(contribution.g),abs(contribution.b)));
            if(!(weight>0)) continue;
            if(!meta.flags.y) {
                if(cosine<=0) continue;
                float4 sh=PI*float4(0.2820948,-0.48860252*direction.y,0.48860252*direction.z,-0.48860252*direction.x);
                if(traversalOptions&16384) {
                    const float3 clamped=cs2HueClamp(PI*contribution)*(inverseCount/PI);
                    baked+=clamped*cosine; shR+=clamped.r*sh; shG+=clamped.g*sh; shB+=clamped.b*sh;
                    continue;
                }
                baked+=clamp(contribution*cosine,-100,100)*inverseCount;
                shR+=clamp(contribution.r*sh,-100,100)*inverseCount; shG+=clamp(contribution.g*sh,-100,100)*inverseCount; shB+=clamp(contribution.b*sh,-100,100)*inverseCount;
                continue;
            }
            risTotal+=weight;
            const float share=weight/risTotal;
            if(risU<share) {
                risU=min(risU/share,0.99999994f);
                risLight=index; risWeight=weight; risContribution=contribution; risDirection=direction; risDistance=distance; risCosine=cosine;
            } else risU=(risU-share)/(1-share);
        }
        const float3 origin=footprint.position+transmissionEpsilon*footprint.normal;
        if(slotPick!=0xffffffffu) {
            const float2 u=float2(random(rng),random(rng));
            const float3 delta=directEndpoint(lights[runtimeLights[slotPick]],footprint.position,slotDirection,slotDistance,u)-origin;
            const float segment=length(delta);
            const float visibility=segment>0 ? directVisibility(origin,delta/segment,segment) : 0;
            const float pickWeight=slotFactor*slotTotal;
            runtimeVisible[slotPick]+=visibility*pickWeight; runtimePicked[slotPick]+=pickWeight;
        }
        if(risLight==0xffffffffu || risCosine<=0) continue;
        Light light=lights[risLight];
        const float2 u=float2(random(rng),random(rng));
        const float3 delta=directEndpoint(light,footprint.position,risDirection,risDistance,u)-origin;
        const float segment=length(delta);
        const float visibility=segment>0 ? directVisibility(origin,delta/segment,segment) : 0;
        if(visibility<=0) continue;
        const float scale=risTotal/risWeight*inverseCount;
        const float3 visible=risContribution*visibility;
        float4 sh=PI*float4(0.2820948,-0.48860252*risDirection.y,0.48860252*risDirection.z,-0.48860252*risDirection.x);
        if(traversalOptions&16384) {
            const float3 clamped=cs2HueClamp(PI*visible*(risTotal/risWeight))*(inverseCount/PI);
            baked+=clamped*risCosine; shR+=clamped.r*sh; shG+=clamped.g*sh; shB+=clamped.b*sh;
            continue;
        }
        baked+=clamp(visible*risCosine,-100,100)*scale;
        shR+=clamp(visible.r*sh,-100,100)*scale; shG+=clamp(visible.g*sh,-100,100)*scale; shB+=clamp(visible.b*sh,-100,100)*scale;
    }
    baked=cs2LaneSum(float4(baked,0),row).xyz; shR=cs2LaneSum(shR,row); shG=cs2LaneSum(shG,row); shB=cs2LaneSum(shB,row);
    for(uint k=0;k<CS2_RUNTIME_LIGHTS;++k) {
        const float4 sum=cs2LaneSum(float4(runtimeUnoccluded[k],runtimeVisible[k],runtimePicked[k],0),row);
        runtimeUnoccluded[k]=sum.x; runtimeVisible[k]=sum.y; runtimePicked[k]=sum.z;
    }
    if(!valid || lane!=0) return;
    for(uint offset=0;offset<204;offset+=4) working.Store(base+offset,0);
    if(start==end) return;
    if(census) working.Store3(base+8,asuint(centre));
    float strengths[4]={0,0,0,0}; int ids[4]={255,255,255,255}; uint selected=0;
    const uint limit=maxBounces&255;
    for(uint k=0;k<runtimeCount;++k) {
        const int runtimeId=asint(lights[runtimeLights[k]+lightCount].flags.x);
        const float share=(unshadowed>>k)&1 ? 1 : runtimePicked[k]>0 ? runtimeVisible[k]/runtimePicked[k] : 0;
        const float scalar=saturate(share*runtimeUnoccluded[k]/runtimeDenominator[k]);
        if(census && scalar>0) {
            const uint seen=working.Load(base+4);
            if(seen<32) { const uint at=base+20+(seen/4)*4; working.Store(at,working.Load(at)|((uint(runtimeId)&255)<<(8*(seen%4)))); }
            working.Store(base+4,seen+1);
        }
        if(scalar>0 && (selected<limit || scalar>strengths[limit-1])) {
            uint at=min(selected,limit-1); selected=min(selected+1,limit);
            while(at>0 && scalar>strengths[at-1]) { strengths[at]=strengths[at-1]; ids[at]=ids[at-1]; --at; }
            strengths[at]=scalar; ids[at]=runtimeId;
        }
    }
    for(uint i=1;i<selected;++i) { uint j=i; while(j>0 && ids[j]<ids[j-1]) { int id=ids[j]; ids[j]=ids[j-1]; ids[j-1]=id; float s=strengths[j]; strengths[j]=strengths[j-1]; strengths[j-1]=s; --j; } }
    working.Store(base,selected); uint packed=0;
    for(uint i=0;i<4;++i) { working.Store(base+64+i*4,asuint(strengths[i])); packed|=(uint(ids[i])&255)<<(8*i); }
    working.Store(base+80,packed);
    working.Store3(base+144,asuint(baked)); working.Store4(base+156,asuint(shR)); working.Store4(base+172,asuint(shG)); working.Store4(base+188,asuint(shB));
}
#endif

void probeLighting(uint thread) {
    uint base=thread*256, count=0, rng=hash((globalLuxelBase+thread)^hash(seed+1));
    const uint stripes=max(asuint(visibilityRadius),1u),cell=thread/stripes,stripe=thread%stripes;
    float4 mean[6];
    [unroll] for(uint f=0;f<6;++f) mean[f]=sampleBegin ? asfloat(working.Load4(base+f*36)) : 0;
    if(sampleBegin) { count=working.Load(base+32); rng=working.Load(base+216); }
    if(offsets[thread]!=offsets[thread+1]) {
        Sample box=samples[thread];
        float3 edgeZ=float3(cdf[thread*3],cdf[thread*3+1],cdf[thread*3+2]);
        const bool lowDiscrepancy=visibilityExponent>0.5;
        const uint cellSeed=hash((globalLuxelBase+cell)*4+hash(seed+977));
        for(uint j=sampleBegin;j<sampleEnd;++j) {
            const uint i=stripe*sampleCount+j;
            float d[5];
            if(lowDiscrepancy) {
                float2 o=shuffledSobol2(i,cellSeed),w=shuffledSobol2(i,cellSeed+0x9e3779b9u);
                d[0]=o.x; d[1]=o.y; d[2]=scrambledVanDerCorput(i,cellSeed+0x7f4a7c15u); d[3]=w.x; d[4]=w.y;
            } else { [unroll] for(uint k=0;k<5;++k) d[k]=random(rng); }
            float3 origin=box.position+d[0]*box.normal+d[1]*box.geometricNormal+d[2]*edgeZ;
            float u=d[3],v=d[4],z=1-2*v,r=2*sqrt(v*(1-v)),phi=2*PI*u;
            float3 direction=float3(r*cos(phi),r*sin(phi),z);
            float visibility; float3 rgb=radiance(origin,direction,rng,visibility,origin);
            float weights[6]={max(direction.x,0),max(direction.y,0),max(direction.z,0),max(-direction.x,0),max(-direction.y,0),max(-direction.z,0)};
            ++count;
            [unroll] for(uint f=0;f<6;++f) {
                float4 value=float4(clamp(rgb*(4*weights[f]),-100,100),1);
                mean[f]+=(value-mean[f])/count;
            }
        }
    }
    [unroll] for(uint f=0;f<6;++f) storeAccumulator(base+f*36,mean[f],0,0,count);
    working.Store(base+216,rng);
}
uint adaptiveTile(uint globalLuxel) {
    uint width=traversalOptions>>16, tilesPerRow=(width+3)/4;
    return (globalLuxel/width/4)*tilesPerRow+(globalLuxel%width)/4;
}
Hit rejectTrace(float3 origin,float3 direction,uint mask) {
    RayDesc desc; desc.Origin=origin; desc.Direction=direction; desc.TMin=0; desc.TMax=56755.84;
    RayQuery<RAY_FLAG_FORCE_NON_OPAQUE> query;
    query.TraceRayInline(scene,RAY_FLAG_NONE,0xff,desc);
    const bool partitioned=(traversalOptions&1)!=0;
    while(query.Proceed()) {
        if(query.CandidateType()!=CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        uint primitive=partitioned ? traversalMetadata.Load(4*(query.CandidateInstanceID()+query.CandidatePrimitiveIndex()))
                                   : query.CandidatePrimitiveIndex();
        if(((primitives[primitive].x|meshFlags)&mask)!=0) continue;
        query.CommitNonOpaqueTriangleHit();
    }
    Hit hit; hit.primitive=0xffffffff; hit.distance=0; hit.bary=0;
    if(query.CommittedStatus()==COMMITTED_TRIANGLE_HIT) {
        hit.primitive=partitioned ? traversalMetadata.Load(4*(query.CommittedInstanceID()+query.CommittedPrimitiveIndex()))
                                  : query.CommittedPrimitiveIndex();
        hit.distance=query.CommittedRayT(); hit.bary=query.CommittedTriangleBarycentrics();
    }
    return hit;
}
uint rejectTie(float3 origin,float3 direction,float3 evaluated,float t,uint hitPrimitive,bool hitBack,uint mask) {
    RayDesc desc; desc.Origin=origin; desc.Direction=direction; desc.TMin=max(0.0f,t*(1-1e-4f)-1e-6f); desc.TMax=t*(1+1e-4f)+1e-6f;
    RayQuery<RAY_FLAG_FORCE_NON_OPAQUE> query;
    query.TraceRayInline(scene,RAY_FLAG_NONE,0xff,desc);
    const bool partitioned=(traversalOptions&1)!=0;
    uint found=0;
    while(query.Proceed()) {
        if(query.CandidateType()!=CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        uint primitive=partitioned ? traversalMetadata.Load(4*(query.CandidateInstanceID()+query.CandidatePrimitiveIndex()))
                                   : query.CandidatePrimitiveIndex();
        if(primitive==hitPrimitive || ((primitives[primitive].x|meshFlags)&mask)!=0) continue;
        const uint first=primitive*3;
        precise const float3 Ng=cross(vertices[first+1].xyz-vertices[first].xyz,vertices[first+2].xyz-vertices[first].xyz);
        const bool back=((primitives[primitive].x|meshFlags)&0x42)==0 && dot(evaluated,Ng)>0;
        found|=back==hitBack ? 1u : 2u;
        if(found&2) { query.Abort(); break; }
    }
    return found;
}
void sampleReject(uint index) {
    const Sample s=samples[index];
    const uint mask=asuint(visibilityStrength);
    precise const float3 n=s.normal,F=s.geometricNormal,pos=s.position;
    precise const float sign=n.z>=0 ? 1.0f : -1.0f;
    precise const float b=asfloat(indices[3*index]),c1=asfloat(indices[3*index+1]),c2=asfloat(indices[3*index+2]),e=b*sign,g=n.x*sign;
    const bool reflect=any(F!=0);
    uint back=0,shortRays=0,unsureBack=0,unsureShort=0,unsureHit=0;
    precise const float3 origin=float3(F.x*0.1f+pos.x,F.y*0.1f+pos.y,F.z*0.1f+pos.z);
    for(uint i=0;i<32;++i) {
        precise const float x=cdf[3*i],y=cdf[3*i+1],z=cdf[3*i+2];
        precise float3 w=float3(((b*y)+(z*n.x))+(c1*x),((c2*y)+(z*n.y))+(e*x),((z*n.z)-(n.y*y))-(g*x));
        precise float3 traced=w,evaluated=w;
        if(reflect) {
            precise const float p=F.x*w.x,q=F.y*w.y,r=F.z*w.z;
            precise const float facingFace=(p+q)+r;
            if(!(facingFace>=0)) {
                precise const float k=((r+r)+p)+((p+q)+q);
                precise const float k2=((r+r)+q)+((q+p)+p);
                traced=float3(w.x-F.x*k,w.y-F.y*k,w.z-F.z*k);
                evaluated=float3(w.x-F.x*k2,w.y-F.y*k2,w.z-F.z*k2);
            }
        }
        const Hit hit=rejectTrace(origin,traced,mask);
        if(hit.primitive==0xffffffff) continue;
        const uint first=hit.primitive*3;
        precise const float3 v0=vertices[first].xyz,v1=vertices[first+1].xyz,v2=vertices[first+2].xyz;
        precise float3 Ng=cross(v1-v0,v2-v0);
        precise const float len=sqrt((Ng.x*Ng.x+Ng.y*Ng.y)+Ng.z*Ng.z);
        if(len>0) { precise const float inv=1.0f/len; Ng=float3(Ng.x*inv,Ng.y*inv,Ng.z*inv); }
        precise const float facing=(evaluated.y*Ng.y+evaluated.x*Ng.x)+evaluated.z*Ng.z;
        precise const float span=sqrt((traced.y*traced.y+traced.x*traced.x)+traced.z*traced.z);
        precise const float distance=span*hit.distance;
        const uint flags=primitives[hit.primitive].x|meshFlags;
        const bool counts=(flags&0x42)==0;
        if(counts && facing>0) ++back;
        if(6.0f>distance) ++shortRays;
        const float edge=min(min(hit.bary.x,hit.bary.y),1-hit.bary.x-hit.bary.y);
        const float evaluatedLength=sqrt(dot(evaluated,evaluated));
        const uint tie=rejectTie(origin,traced,evaluated,hit.distance,hit.primitive,counts && facing>0,mask);
        if((tie&2) || (edge<2e-5 && !(tie&1))) ++unsureHit;
        else {
            if(counts && abs(facing)<=1e-5*evaluatedLength) ++unsureBack;
            if(abs(distance-6.0f)<=6e-5) ++unsureShort;
        }
    }
    working.Store4(index*16,uint4(back,shortRays,unsureBack,unsureShort|(unsureHit<<16)));
}
void tileTest(uint index) {
    if(index>=dispatchLuxels) return;
    uint tile=firstLuxel+index, flag=65536+tile*4;
    if(researchCounters.Load(flag)) return;
    uint width=traversalOptions>>16, tilesPerRow=(width+3)/4, height=(globalLuxelBase+luxelCount+width-1)/width;
    uint tx=tile%tilesPerRow, ty=tile/tilesPerRow;
#if VRAD3_CS2
    uint n=0; float sum=0, sumsq=0;
#else
    uint n=0; double sum=0, sumsq=0;
#endif
    for(uint y=ty*4;y<min(ty*4+4,height);++y) for(uint x=tx*4;x<min(tx*4+4,width);++x) {
        uint g=y*width+x;
        if(g<globalLuxelBase || g>=globalLuxelBase+luxelCount) continue;
        uint record=(g-globalLuxelBase)*204;
        if(working.Load(record+32)==0) continue;
        float4 mean=asfloat(working.Load4(record));
#if VRAD3_CS2
        const float luminance=dot(mean.rgb,float3(0.2125,0.7154,0.0721));
        ++n; sum+=luminance; sumsq+=luminance*luminance;
#else
        precise float luminance=0.2126f*mean.x+0.7152f*mean.y+0.0722f*mean.z;
        ++n; sum+=(double)luminance; sumsq+=(double)luminance*(double)luminance;
#endif
    }
    bool done;
    if(n==0) done=true;
    else if(n<3) done=false;
    else {
#if VRAD3_CS2
        const float average=sum/n,variance=sumsq/n-average*average;
        done=!(sqrt(abs(variance))>average*asfloat(sampleEnd));
#else
        double average=sum/n, variance=sumsq/n-average*average;
        done=(double)sqrt((float)abs(variance))<(double)asfloat(sampleEnd)*average;
#endif
    }
    if(done) researchCounters.Store(flag,1);
}
[numthreads(64,1,1)]
void main(uint3 id:SV_DispatchThreadID) {
    if(maxBounces&0x20000000u) { tileTest(id.x); return; }
    if(maxBounces&0x10000000u) { if(id.x<dispatchLuxels && firstLuxel+id.x<luxelCount) sampleReject(firstLuxel+id.x); return; }
#if VRAD3_CS2
    if(maxBounces&0x80000000u) { directLightingCs2(id.x); return; }
#endif
    if(id.x>=dispatchLuxels) return;
    uint luxel=firstLuxel+id.x;
    if(maxBounces&0x08000000u) {
        luxel=researchCounters.Load((researchEnabled&~3u)+luxel*4);
    }
    if(luxel>=luxelCount) return;
#if !VRAD3_CS2
    if(maxBounces&0x80000000u) { directLighting(luxel); return; }
#endif
    if(maxBounces&0x40000000u) { probeLighting(luxel); return; }
    // Working record is 204 bytes per luxel; bytes 144-203 hold the accumulator tail, zeroed for luxels in a stopped tile.
    if((traversalOptions&8) && researchCounters.Load(65536+adaptiveTile(globalLuxelBase+luxel)*4)) {
        for(uint tail=144;tail<204;tail+=4) working.Store(luxel*204+tail,0);
        return;
    }
    uint start=offsets[luxel],end=offsets[luxel+1];
    float4 means[4]; float4 scalarMeans=0,m2=0;
    [unroll] for(uint group=0;group<4;++group) means[group]=0;
    uint count=0,rng=hash((globalLuxelBase+luxel) ^ hash(seed+1));
    uint address=luxel*204;
    if(sampleBegin) {
        count=working.Load(address+32); rng=working.Load(address+144);
        [unroll] for(uint group=0;group<4;++group) {
            uint offset=address+group*36;
            means[group]=asfloat(working.Load4(offset));
            uint2 scalar=working.Load2(offset+16),variance=working.Load2(offset+24);
            scalarMeans[group]=(float)asdouble(scalar.x,scalar.y);
            m2[group]=(float)asdouble(variance.x,variance.y);
        }
    }
    if(start!=end) {
        float total=cdf[end-1];
        for(uint sampleIndex=sampleBegin;sampleIndex<sampleEnd;++sampleIndex) {
            float choice=random(rng)*total; uint lo=start,hi=end-1;
            while(lo<hi) { uint mid=(lo+hi)/2; if(cdf[mid]<=choice) lo=mid+1; else hi=mid; }
            Sample sample=samples[indices[lo]];
            float3 direction=hemisphere(float2(random(rng),random(rng)),sample.normal);
            float cosine=max(dot(direction,sample.normal),0),pdf=cosine/PI;
            float visibility=0; float3 light=0;
            const bool lightPaths=VRAD3_EXPERIMENTAL && (traversalOptions&128)!=0;
            float3 extraLight=0,extraSh=0,extraDirection=0;
            [branch] if(lightPaths) {
                lpOn=true; extraLight=lightPathAt(sample.position,sample.normal,rng,extraSh,extraDirection);
                lpPrevPos=sample.position; lpPrevNormal=sample.normal;
            }
            if(dot(direction,sample.geometricNormal)>0 && pdf>0) light=radiance(sample.position+0.1*sample.geometricNormal,direction,rng,visibility,sample.position);
            float base=1-visibilityStrength;
#if VRAD3_CS2
            if(!(traversalOptions&32768))
#endif
            {
            visibility=visibilityStrength==0 ? (visibility>=1 ? 1 : 0) : saturate((visibility-base)/visibilityStrength);
            visibility=pow(visibility,visibilityExponent);
            }
            float3 contribution=pdf>0 ? light/pdf : 0;
#if VRAD3_CS2
            if(traversalOptions&16384) { contribution=cs2HueClamp(contribution); light=contribution*pdf; }
#endif
            float4 sh=float4(0.2820948,-0.48860252*direction.y,0.48860252*direction.z,-0.48860252*direction.x);
            float4 values[4]; values[0]=float4(light,visibility);
            values[1]=contribution.r*sh; values[2]=contribution.g*sh; values[3]=contribution.b*sh;
            [branch] if(lightPaths) {
                float4 shLight=float4(0.2820948,-0.48860252*extraDirection.y,0.48860252*extraDirection.z,-0.48860252*extraDirection.x);
                values[0].rgb+=extraLight; values[1]+=extraSh.r*shLight; values[2]+=extraSh.g*shLight; values[3]+=extraSh.b*shLight;
            }
            ++count;
            [unroll] for(uint group=0;group<4;++group) {
                float4 value=clamp(values[group],-100,100);
                means[group]+=(value-means[group])/count;
                float scalar=dot(value,float4(0.25,0.25,0.25,0.25));
                float delta=scalar-scalarMeans[group]; scalarMeans[group]+=delta/count;
                m2[group]+=delta*(scalar-scalarMeans[group]);
            }
        }
    }
    [unroll] for(uint group=0;group<4;++group) storeAccumulator(address+group*36,means[group],scalarMeans[group],m2[group],count);
    for(uint tail=144;tail<204;tail+=4) working.Store(address+tail,0);
    if(start!=end && sampleEnd<sampleCount) working.Store(address+144,rng);
    if(researchEnabled&1) for(uint slot=0;slot<11;++slot) {
        uint ignored; researchCounters.InterlockedAdd(slot*4,counters[slot],ignored);
    }
}
