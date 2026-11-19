// Parent-side API of the GPU lighting library; it is never loaded into vrad3's process.
#pragma once
#include <cstdint>
#include <cstddef>

struct GpuLightingBuffer { const void* data; uint64_t bytes; };
struct GpuLightingSlice {
    uint32_t luxels;
    // Slice buffers: local CSR offsets, indices, CDF, samples.
    GpuLightingBuffer buffers[4];
};
using GpuLightingReadSlice = int (*)(void*, uint32_t, uint32_t, uint64_t,
                                     GpuLightingSlice*, char*, uint32_t);
using GpuLightingPrefetchSlice = int (*)(void*, uint32_t, uint32_t, uint64_t);
struct GpuLightingRequest {
    // Request layout version; 6 adds the optional persistent scene hash.
    uint32_t version;
    uint32_t constants[24];
    // Buffers: vertices, UVs, primitive flags/materials, colors, materials, texture descriptors, texture bytes, CSR offsets/indices/CDF, samples, lights, environment sky (96-byte header + three float planes).
    GpuLightingBuffer buffers[13];
    const wchar_t* shaderPath;
    void* output;
    uint64_t outputBytes;
    const uint8_t* shaderBytes;
    uint64_t shaderSize;
    GpuLightingReadSlice readSlice;
    void* sliceContext;
    uint64_t totalPaths;
    // GPU server only: non-null means buffers 0-6, 11 and 12 hash to this value; a session already holding the scene ignores them.
    const uint8_t* persistentSceneHash;
    // GPU server only (light probes): cheap token of the scene facts. With sceneSkipped, buffers 0-6, 11 and 12 are null and the server must already hold the scene under this token.
    const uint8_t* quickToken;
    bool sceneSkipped;
    GpuLightingPrefetchSlice prefetchSlice;
    // adaptiveMin 0 = adaptive stopping off; blockWidth is the block's luxel grid width, used for 4x4 tiles.
    uint32_t adaptiveMin, blockWidth;
    float adaptiveTol;
    volatile uint32_t* progress;
};
struct GpuLightingTiming {
    double buildMs, lightingMs, hostMs;
    uint32_t slices;
    uint64_t peakResourceBytes;
    uint64_t peakLocalUsageBytes;
};
using GpuLightingBake = int (*)(const GpuLightingRequest*, GpuLightingTiming*, char*, uint32_t);
struct GpuServerStats {
    uint64_t vramCurrent=0, vramPeak=0, residentBytes=0;
    uint32_t scenes=0;
};
#ifdef VRAD3_STATIC_GPU
extern "C" int BakeGpuLighting(const GpuLightingRequest*, GpuLightingTiming*, char*, uint32_t);
extern "C" void ShutdownGpuLighting();
extern "C" int PrepareGpuSession(const uint8_t* shader, uint64_t shaderSize, char*, uint32_t);
extern "C" int GpuLightingHasScene(const uint8_t* hash);
extern "C" int GpuLightingLastDecision();
extern "C" void GetGpuLightingStats(GpuServerStats*);
#endif

// Bounds output addressing (204 bytes per luxel) and host allocation.
constexpr uint32_t kMaxBlockLuxels=1u<<22;
inline bool validGpuBlock(uint32_t count,uint32_t width,uint32_t height) {
    return width && height && count && count<=kMaxBlockLuxels &&
           static_cast<uint64_t>(width)*height==count;
}

struct GpuLightingDiagnostics {
    wchar_t adapter[128]{};
    uint64_t driver=0, budget=0;
    uint32_t adapterLuidLow=0;
    int32_t adapterLuidHigh=0;
};
#ifdef VRAD3_STATIC_GPU
extern "C" void GetGpuLightingDiagnostics(GpuLightingDiagnostics*);
#endif
