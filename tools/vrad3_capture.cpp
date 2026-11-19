// Wrapper main: runs Valve's vrad3_real.exe under a debugger, stops it at fixed code addresses and replaces
// its lightmap, directlight and sample stages with GPU results.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <string>
#include <stdexcept>
#include <cstdint>
#include <map>
#include <iomanip>
#include <limits>
#include <cmath>
#include <sstream>
#include <set>
#include <mutex>
#ifdef VRAD3_STATIC_GPU
#include "lighting_embedded.h"
#include "lighting_embedded_full.h"
#include "lighting_embedded_cs2.h"
static bool lightingShaderFullWanted() {
    wchar_t value[8]{};
    return GetEnvironmentVariableW(L"VRAD3_GPU_LIGHT_PATHS",value,8)==1 && value[0]==L'1';
}
static bool lightingShaderCs2Wanted() {
    wchar_t value[8]{};
    return GetEnvironmentVariableW(L"VRAD3_GPU_ESTIMATOR",value,8)==3 && _wcsicmp(value,L"cs2")==0;
}
static const uint8_t* lightingShaderBytes() {
    return lightingShaderCs2Wanted()?g_lightingShaderCs2:lightingShaderFullWanted()?g_lightingShaderFull:g_lightingShader;
}
static uint64_t lightingShaderSize() {
    return lightingShaderCs2Wanted()?sizeof(g_lightingShaderCs2):lightingShaderFullWanted()?sizeof(g_lightingShaderFull):sizeof(g_lightingShader);
}
#endif
#pragma comment(lib, "bcrypt.lib")

#include "gpu_lighting_api.h"
#include "block_profile.h"
#include "hardware_limits.h"
#include "fault_diagnostics.h"
namespace fs = std::filesystem;
#include "wrapper_settings.h"
#include "responsiveness.h"
#include "progress_window.h"
static HANDLE processHandle;
static uintptr_t imageBase;
static fs::path outputPath;
static bool hammerMode=false;
static bool hammerCapture=false;
static bool fileDiagnostics() {
    return !hammerMode || hammerCapture || wrapperLogging==2 || GetEnvironmentVariableW(L"VRAD3_BLOCK_PROFILE",nullptr,0)!=0;
}
static std::wstring hammerCommand;
struct HammerTiming {
    uint32_t gpuBlocks=0,cpuBlocks=0;
    double dispatchMs=0,hostMs=0,stageMs=0;
    ULONGLONG processMs=0;
    uint32_t directGpuBlocks=0,directCpuBlocks=0;
    double directMs=0;
};
static HammerTiming hammerTiming;
static GpuLightingDiagnostics hammerHardware;
static std::string hammerFallbackReason;
static int hammerChildExit=-1;
static uint64_t hammerPeakResources=0;
static bool prefetchBake=false;
#ifdef VRAD3_STATIC_GPU
struct NativeLighting;
namespace gpuServer {
static int bakeForWorker(const GpuLightingRequest&,GpuLightingTiming&,std::string& reason);
static int bakeResident(uint32_t spp,uint32_t lights,NativeLighting& inputs,std::vector<uint8_t>& records,
                        GpuLightingTiming& timing,std::string& reason);
static std::array<uint8_t,32> lightmapToken(const NativeLighting& inputs,uint32_t lights);
static bool active();
}
#endif

static void cleanupGpu(void (*shutdown)()) noexcept {
    if(!shutdown) return;
    try { shutdown(); }
    catch(const std::exception& error) { std::cerr << "GPU cleanup failed: " << error.what() << '\n'; }
    catch(...) { std::cerr << "GPU cleanup failed: unknown exception\n"; }
}

static std::string fileSha256(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot open file for SHA256");
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    uint8_t digest[32];
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("SHA256 initialization failed");
    NTSTATUS status = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
    std::vector<uint8_t> data(1024*1024);
    while(status>=0 && file) {
        file.read(reinterpret_cast<char*>(data.data()),data.size());
        if(file.gcount()) status=BCryptHashData(hash,data.data(),static_cast<ULONG>(file.gcount()),0);
    }
    if(file.bad()) status=static_cast<NTSTATUS>(0xC0000001L);
    if (status >= 0) status = BCryptFinishHash(hash, digest, sizeof(digest), 0);
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) throw std::runtime_error("SHA256 failed");
    static const char hex[] = "0123456789abcdef";
    std::string text;
    for (uint8_t byte : digest) { text += hex[byte >> 4]; text += hex[byte & 15]; }
    return text;
}
struct BlockDigest { fs::path path; uintmax_t size=0; fs::file_time_type time{}; std::string digest; };
static std::mutex blockDigestMutex; static BlockDigest lastBlockDigest;
static std::string blockSha256(const fs::path& path,std::vector<uint8_t>* bytes=nullptr) {
    BlockDigest key; std::error_code error; bool keyed=false;
    key.path=fs::absolute(path,error).lexically_normal();
    if(!error) key.size=fs::file_size(path,error);
    if(!error) { key.time=fs::last_write_time(path,error); keyed=!error; }
    std::string cached;
    if(keyed) { std::lock_guard<std::mutex> lock(blockDigestMutex);
        if(lastBlockDigest.path==key.path && lastBlockDigest.size==key.size && lastBlockDigest.time==key.time) cached=lastBlockDigest.digest; }
    if(!bytes) {
        if(!cached.empty()) return cached;
        key.digest=fileSha256(path);
    } else {
        std::ifstream file(path,std::ios::binary|std::ios::ate);
        if(!file) throw std::runtime_error("Cannot open file for SHA256");
        const auto end=file.tellg(); if(end<0) throw std::runtime_error("SHA256 failed");
        bytes->resize(static_cast<size_t>(end)); file.seekg(0);
        if(!bytes->empty() && !file.read(reinterpret_cast<char*>(bytes->data()),bytes->size())) throw std::runtime_error("SHA256 failed");
        if(!cached.empty() && bytes->size()==key.size) return cached;
        BCRYPT_ALG_HANDLE algorithm=nullptr; BCRYPT_HASH_HANDLE hash=nullptr; uint8_t digest[32];
        if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) throw std::runtime_error("SHA256 initialization failed");
        NTSTATUS status=BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0);
        for(size_t p=0;status>=0 && p<bytes->size();) {
            const ULONG n=static_cast<ULONG>(std::min<size_t>(bytes->size()-p,size_t(1)<<30));
            status=BCryptHashData(hash,bytes->data()+p,n,0); p+=n;
        }
        if(status>=0) status=BCryptFinishHash(hash,digest,sizeof(digest),0);
        if(hash) BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm,0);
        if(status<0) throw std::runtime_error("SHA256 failed");
        static const char hex[]="0123456789abcdef";
        for(uint8_t byte:digest) { key.digest+=hex[byte>>4]; key.digest+=hex[byte&15]; }
    }
    if(keyed) { std::lock_guard<std::mutex> lock(blockDigestMutex); lastBlockDigest=key; }
    return key.digest;
}
// Supported Valve builds, keyed by the SHA-256 of vrad3_real.exe. RVAs in this file are the 2026-10-05 build's;
// the 2026-10-27 build has the same code addresses and moves some data. The 2020-06-13 previous_release build
// is older-compiler output where every RVA moves, so its table maps each one used and unmapped RVAs are refused.
struct DataShift { uintptr_t first,end; intptr_t shift; };
static constexpr DataShift at(uintptr_t from,uintptr_t to) { return {from,from+1,static_cast<intptr_t>(to)-static_cast<intptr_t>(from)}; }
// A callee-saved register the sample stage restores from its save slot (RSP+rsp in the stage body) between
// the Reject call and the stage's end; a jump past that code must restore it.
struct SavedRegister { uint32_t rsp; DWORD64 CONTEXT::* gpr; int xmm; };
static constexpr SavedRegister savedGpr(uint32_t rsp,DWORD64 CONTEXT::* gpr) { return {rsp,gpr,-1}; }
static constexpr SavedRegister savedXmm(uint32_t rsp,int xmm) { return {rsp,nullptr,xmm}; }
struct ValveBuild {
    const char* sha256; const char* name; std::vector<DataShift> shifts;
    // complete: every RVA must be in shifts. stageMapLocal: RBP offset of the sample stage's Map list local.
    bool complete;
    uint32_t stageMapLocal;
    std::vector<SavedRegister> stageRestores;
};
static const ValveBuild valveBuilds[]={
    {"6cf516c14515c0898619fa2e3b86265ca596ea4938006a4768cdd2738a1d9eda","2026-10-27",
     {{0x17ae40,0x17ae48,0x10},{0x31fcc0,0x31fccc,0x20},{0x32aa20,0x32ae30,0x20}},false,0x80,
     // Restores skipped by the jump: 0x14000a455 xmm8, 0x14000ab19-0x14000ab29 xmm7, r13, r12.
     {savedXmm(0xea0,8),savedXmm(0xeb0,7),savedGpr(0xee0,&CONTEXT::R13),savedGpr(0xee8,&CONTEXT::R12)}},
    {"bc178f594768fcf792b7c5c7dce07a5b2877a90da2f6ea9944db7dac6c860d6c","2020-06-13 previous_release",{
        // Code: command Runs, hook sites, the reject dispatcher, profile boundaries.
        at(0xca90,0x56b0),at(0x52d0,0x5a20),at(0x75e0,0x7be0),at(0x3f190,0x3c4f0),at(0x22bb0,0x212e0),at(0x91524,0xa26e2),
        at(0xa0b0,0xa12b),at(0xa0b5,0xa130),at(0xace1,0xab80),at(0x2b8c0,0x26ff0),
        at(0x48da,0x3ea9),at(0x7877,0x7e72),at(0x916b,0x928d),at(0x9ec7,0x9f30),at(0x9fe0,0xa058),at(0xa197,0xa215),
        at(0xa42c,0xa4b9),at(0xa585,0xa5c2),at(0xa62f,0xa66c),at(0xaaec,0xa919),at(0x536d,0x5ac3),at(0x5491,0x5be7),
        at(0x591d,0x63ca),
        // Masks and float constants (the pool is reordered, so one entry each).
        at(0x17ae40,0x1ad0d0),at(0x17ae44,0x1ad0d4),at(0x31fcc0,0x3604b0),at(0xd58e4,0x103c48),at(0xd58f0,0x103c58),
        at(0xd58f8,0x103c60),at(0xd5910,0x103c78),at(0xd591c,0x103c84),at(0xd5928,0x103c90),at(0xd5930,0x103c98),
        at(0xd5954,0x103cc4),at(0xd5968,0x103cd8),
        // Globals: probe volume, lightmap block state, probe block.
        {0x1de850,0x1de874,0x40908},{0x32aa20,0x32ad90,0x40750},{0x32adc8,0x32ae68,0x412c0}},true,0xb0,
        // Restores skipped by the jump: 0x14000a8b0-0x14000a8b8 r14, r12; 0x14000a943-0x14000a953 xmm7, r15, r13.
        {savedGpr(0xf28,&CONTEXT::R14),savedGpr(0xf38,&CONTEXT::R12),savedXmm(0xf00,7),savedGpr(0xf20,&CONTEXT::R15),
         savedGpr(0xf30,&CONTEXT::R13)}},
};
static const ValveBuild* valveBuild=nullptr;
static void verifyExecutable(const fs::path& path) {
    BlockProfileScope profile("verify_executable");
    const std::string sha=fileSha256(path);
    for(const auto& build:valveBuilds) if(sha==build.sha256) { valveBuild=&build; return; }
    throw std::runtime_error("vrad3_real.exe is not a Valve build this GPU wrapper supports (updated by Steam? run Setup to check)");
}

static void fail(const std::string& message) {
    throw std::runtime_error(message + " (Win32 " + std::to_string(GetLastError()) + ")");
}
static void readMemory(uintptr_t address, void* buffer, size_t bytes) {
    SIZE_T read = 0;
    if (bytes && (!ReadProcessMemory(processHandle, reinterpret_cast<void*>(address), buffer, bytes, &read) || read != bytes))
        fail("ReadProcessMemory");
}
template<class T> static T readValue(uintptr_t address) {
    T value{}; readMemory(address, &value, sizeof(value)); return value;
}
// Child address of an RVA, applying the running build's shifts.
static uintptr_t address(uintptr_t rva) {
    if(valveBuild) {
        for(const auto& move:valveBuild->shifts) if(rva>=move.first && rva<move.end) return imageBase+rva+move.shift;
        if(valveBuild->complete) {
            char text[96]; snprintf(text,sizeof(text),"RVA 0x%llx has no mapping for Valve build %s",static_cast<unsigned long long>(rva),valveBuild->name);
            throw std::runtime_error(text);
        }
    }
    return imageBase+rva;
}
static void writeFile(const fs::path& path, const void* data, size_t bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(static_cast<const char*>(data), bytes);
    if (!file) throw std::runtime_error("Failed to write capture file");
}
static std::vector<uint8_t> dump(uintptr_t pointer, size_t count, size_t stride, const fs::path& path) {
    if (count > 134217728 || stride > 4096 || count * stride > 1024ull * 1024 * 1024)
        throw std::runtime_error("Capture count exceeds limit");
    if (count && !pointer) throw std::runtime_error("Null array pointer");
    std::vector<uint8_t> bytes(count * stride);
    readMemory(pointer, bytes.data(), bytes.size());
    writeFile(path, bytes.data(), bytes.size());
    return bytes;
}
static std::wstring threadCount() { return std::to_wstring(usableCpuCount()); }
static uint32_t countAt(uintptr_t rva) { return readValue<uint32_t>(address(rva)); }
static uintptr_t pointerAt(uintptr_t rva) { return readValue<uintptr_t>(address(rva)); }
#include "vrad3_native_lighting.h"

static std::wstring environmentValue(const wchar_t* name);
#include "vrad3_probes.h"
#ifdef VRAD3_STATIC_GPU
#include "sample_reject.h"
#include "sample_stage.h"
#include "sample_exact.h"
static std::string rejectSummaryText() {
    return sampleStageSummary.empty()?rejectSummary+(exactStageSummary.empty()?"":" "+exactStageSummary):"stage "+sampleStageSummary;
}
static std::string rejectLogContents() { return rejectLogText; }
#else
static std::string rejectSummaryText() { return {}; }
static std::string rejectLogContents() { return {}; }
#endif
static bool seedChanged=false;
static uint32_t originalSeed=0;
static void changePathtraceSeed(bool cpuOnly) {
    std::wstring text=environmentValue(L"VRAD3_SEED_XOR");
    if(text.empty()) return;
    if(!cpuOnly || hammerMode) throw std::runtime_error("VRAD3_SEED_XOR requires CPU capture mode");
    if(text.find_first_not_of(L"0123456789")!=std::wstring::npos)
        throw std::runtime_error("VRAD3_SEED_XOR must be a decimal uint32");
    unsigned long long mask=std::stoull(text);
    if(mask>UINT32_MAX) throw std::runtime_error("VRAD3_SEED_XOR exceeds uint32");
    // The pathtrace RNG seed is the uint32 at RVA 32aae8.
    originalSeed=countAt(0x32aae8);
    uint32_t altered=originalSeed^static_cast<uint32_t>(mask);
    SIZE_T written=0;
    if(!WriteProcessMemory(processHandle,reinterpret_cast<void*>(address(0x32aae8)),&altered,sizeof(altered),&written) || written!=sizeof(altered))
        fail("WriteProcessMemory seed");
    seedChanged=true;
    std::ofstream report(outputPath/L"seed.json");
    report << "{\"original_seed\":" << originalSeed << ",\"xor\":" << mask << ",\"pathtrace_seed\":" << altered << "}\n";
    if(!report) throw std::runtime_error("Seed report write failed");
}
static void restorePathtraceSeed() {
    if(!seedChanged) return;
    SIZE_T written=0;
    if(!WriteProcessMemory(processHandle,reinterpret_cast<void*>(address(0x32aae8)),&originalSeed,sizeof(originalSeed),&written) || written!=sizeof(originalSeed))
        fail("WriteProcessMemory restore seed");
    if(countAt(0x32aae8)!=originalSeed) throw std::runtime_error("Seed restoration verification failed");
    std::cout << "Restored pathtrace seed before directlight: " << originalSeed << '\n' << std::flush;
    seedChanged=false;
}
static void isolatePathtrace(bool probeResearch=false) {
    std::wstring selection=environmentValue(L"VRAD3_ISOLATE");
    std::wstring opaque=environmentValue(L"VRAD3_FORCE_OPAQUE");
    if(selection.empty() && opaque.empty()) return;
    if(hammerMode && !probeResearch) throw std::runtime_error("Pathtrace research options are capture-only");
    if(!opaque.empty() && opaque!=L"1") throw std::runtime_error("VRAD3_FORCE_OPAQUE must be 1");
    uintptr_t scene=pointerAt(0x32aa20);
    uint32_t count=readValue<uint32_t>(scene);
    uintptr_t table=readValue<uintptr_t>(scene+8);
    if(count>65536 || (count && !table)) throw std::runtime_error("Invalid isolation light table");
    bool sky=selection==L"sky";
    std::set<uint32_t> chosen;
    if(selection.rfind(L"type:",0)==0) {
        std::wstring type=selection.substr(5);
        if(type!=L"1" && type!=L"2" && type!=L"3")
            throw std::runtime_error("VRAD3_ISOLATE type must be 1, 2 or 3");
        for(uint32_t i=0;i<count;++i)
            if(readValue<uint32_t>(table+static_cast<uintptr_t>(i)*0x108)==static_cast<uint32_t>(type[0]-L'0')) chosen.insert(i);
    } else if(!selection.empty() && !sky) {
        if(selection.back()==L',') throw std::runtime_error("Empty isolation light index");
        std::wistringstream stream(selection); std::wstring token;
        while(std::getline(stream,token,L',')) {
            if(token.empty() || token.find_first_not_of(L"0123456789")!=std::wstring::npos)
                throw std::runtime_error("VRAD3_ISOLATE must be sky, type:1..3, or comma-separated light indices");
            unsigned long long index=std::stoull(token);
            if(index>=count) throw std::runtime_error("VRAD3_ISOLATE light index out of bounds");
            chosen.insert(static_cast<uint32_t>(index));
        }
    }
    uintptr_t materials=readValue<uintptr_t>(scene+0x128);
    uint32_t capacity=readValue<uint32_t>(scene+0x130);
    if(!opaque.empty() && (capacity>65536 || (capacity && !materials)))
        throw std::runtime_error("Invalid force-opaque material table");
    auto write=[&](uintptr_t location,const void* bytes,size_t size) {
        SIZE_T written=0;
        if(!WriteProcessMemory(processHandle,reinterpret_cast<void*>(location),bytes,size,&written) || written!=size)
            fail("WriteProcessMemory isolation");
    };
    uint8_t inactive=0;
    if(!selection.empty()) {
        for(uint32_t i=0;i<count;++i) if(!chosen.count(i))
            write(table+static_cast<uintptr_t>(i)*0x108+0x84,&inactive,sizeof(inactive));
        if(!sky) {
            float black[3]{}; write(scene+0x54,black,sizeof(black));
            uintptr_t noImage=0; write(scene+0x70,&noImage,sizeof(noImage));
        }
    }
    uint32_t changedMaterials=0;
    if(!opaque.empty()) for(uint32_t i=0;i<capacity;++i) {
        uintptr_t entry=materials+static_cast<uintptr_t>(i)*140;
        if(readValue<uint32_t>(entry)&0x80000000u) continue;
        float one=1; uintptr_t noTexture=0;
        write(entry+12+36,&one,sizeof(one));
        write(entry+12+56,&noTexture,sizeof(noTexture));
        ++changedMaterials;
    }
    std::ofstream report(outputPath/L"isolation.json");
    std::string label; for(wchar_t character:selection) label+=static_cast<char>(character);
    report << "{\"selection\":\"" << label
           << "\",\"light_count\":" << count << ",\"selected_indices\":[";
    bool first=true; for(uint32_t index:chosen) { if(!first) report << ','; report << index; first=false; }
    report << "],\"constant_sky_zeroed\":" << (!selection.empty() && !sky?"true":"false")
           << ",\"sky_image_cleared\":" << (!selection.empty() && !sky?"true":"false")
           << ",\"force_opaque_materials\":" << changedMaterials << "}\n";
    if(!report) throw std::runtime_error("Isolation report write failed");
    std::wcout << L"Research pathtrace isolation: " << selection << L'\n' << std::flush;
    if(!opaque.empty()) std::cout << "Research force-opaque materials: " << changedMaterials << '\n' << std::flush;
}

static void capture(bool after) {
    uint32_t count = countAt(0x32ab88), width = countAt(0x32aba8), height = countAt(0x32abac);
    if (!validGpuBlock(count,width,height))
        throw std::runtime_error("Invalid or oversized block dimensions");
    dump(pointerAt(0x32ab20), count, 0xcc, outputPath / (after ? L"working-after.bin" : L"working-before.bin"));
    dump(pointerAt(0x32ab90), count, 0x8c, outputPath / (after ? L"output-after.bin" : L"output-before.bin"));
    if (after) return;
    auto offsetsRaw = dump(pointerAt(0x32ab00), count + 1, 4, outputPath / L"footprint-offsets.bin");
    std::vector<uint32_t> offsets(count + 1);
    memcpy(offsets.data(), offsetsRaw.data(), offsetsRaw.size());
    if (offsets[0] != 0) throw std::runtime_error("CSR must start at zero");
    for (size_t i = 1; i < offsets.size(); ++i)
        if (offsets[i] < offsets[i-1]) throw std::runtime_error("Nonmonotonic CSR");
    uint32_t entries = offsets.back(), samples = countAt(0x32ad28);
    if (samples > 14000000) throw std::runtime_error("Too many sample records");
    auto indicesRaw = dump(pointerAt(0x32acd8), entries, 4, outputPath / L"footprint-indices.bin");
    for (size_t i = 0; i < indicesRaw.size(); i += 4) {
        uint32_t index; memcpy(&index, indicesRaw.data() + i, 4);
        if (index >= samples) throw std::runtime_error("Footprint index out of bounds");
    }
    dump(pointerAt(0x32ab58), entries, 4, outputPath / L"footprint-cdf.bin");
    std::vector<uint8_t> vectors(static_cast<size_t>(samples) * 36);
    uintptr_t samplePointer = pointerAt(0x32ad30);
    for (uint32_t i = 0; i < samples; ++i)
        readMemory(samplePointer + static_cast<uintptr_t>(i) * 0x48, vectors.data() + static_cast<size_t>(i) * 36, 36);
    writeFile(outputPath / L"sample-vectors.bin", vectors.data(), vectors.size());
    uintptr_t scene = pointerAt(0x32aa20);
    uint32_t lightCount = readValue<uint32_t>(scene);
    if (lightCount > 65536) throw std::runtime_error("Too many lights");
    uintptr_t lightTable = readValue<uintptr_t>(scene + 8);
    std::ofstream lightJson(outputPath / L"lights.json");
    lightJson << std::setprecision(std::numeric_limits<float>::max_digits10) << "{\"lights\":[";
    auto lightFloats = [&](uintptr_t light, const char* name, uintptr_t offset, int size) {
        std::vector<float> values(size); readMemory(light + offset, values.data(), values.size()*4);
        lightJson << ",\"" << name << "\":[";
        for (int j=0;j<size;++j) { if(j) lightJson << ','; lightJson << values[j]; }
        lightJson << ']';
    };
    for (uint32_t i=0;i<lightCount;++i) {
        uintptr_t light=lightTable+static_cast<uintptr_t>(i)*0x108;
        if (i) lightJson << ',';
        lightJson << "{\"type\":" << readValue<uint32_t>(light)
                  << ",\"active\":" << (readValue<uint8_t>(light+0x84)?"true":"false")
                  << ",\"casts_shadows\":" << (readValue<uint8_t>(light+0x44)?"true":"false")
                  << ",\"source_sampling_disabled\":" << readValue<uint32_t>(light+0x8c)
                  << ",\"flags\":" << readValue<uint32_t>(light+0x104);
        lightFloats(light,"direct_rgb",4,3);
        lightFloats(light,"indirect_rgb",0x10,3);
        lightFloats(light,"range",0x1c,1);
        lightFloats(light,"attenuation",0x24,3);
        lightFloats(light,"source_size",0x90,2);
        lightFloats(light,"area_size",0x98,2);
        lightFloats(light,"position",0xac,3);
        lightFloats(light,"direction",0xb8,3);
        lightFloats(light,"tangent",0xc4,3);
        lightFloats(light,"cone_cosines",0xd4,2);
        lightFloats(light,"cone_scale",0xdc,1);
        lightFloats(light,"cone_exponent",0x20,1);
        lightFloats(light,"attenuation_offset",0xe0,1);
        lightJson << ",\"attenuation_flags\":" << readValue<uint32_t>(light+0xe4);
        lightFloats(light,"bounds",0xec,6);
        lightJson << '}';
    }
    lightJson << "]}\n";
    if (!lightJson) throw std::runtime_error("Failed to write light metadata");
    uintptr_t materials = scene + 0x128;
    uintptr_t materialTable = readValue<uintptr_t>(materials);
    uint32_t capacity = readValue<uint32_t>(materials + 8);
    uint32_t used = readValue<uint32_t>(materials + 0x10);
    if (capacity > 65536) throw std::runtime_error("Too many material slots");
    std::map<uint64_t, uint32_t> materialSlots;
    std::ofstream materialJson(outputPath / L"materials.json");
    materialJson << std::setprecision(std::numeric_limits<float>::max_digits10);
    materialJson << "{\"materials\":[";
    bool firstMaterial = true;
    for (uint32_t i = 0; i < capacity; ++i) {
        uintptr_t entry = materialTable + static_cast<uintptr_t>(i) * 0x8c;
        uint32_t hash = readValue<uint32_t>(entry);
        if (hash & 0x80000000u) continue;
        materialSlots[readValue<uint64_t>(entry + 4)] = i;
        float numeric[10]; readMemory(entry + 12, numeric, sizeof(numeric));
        if (!firstMaterial) materialJson << ',';
        firstMaterial = false;
        materialJson << "{\"slot\":" << i << ",\"numeric_first_10\":[";
        for (int j = 0; j < 10; ++j) { if (j) materialJson << ','; materialJson << numeric[j]; }
        materialJson << "],\"texture_slots\":[";
        for (int j = 0; j < 3; ++j) {
            if (j) materialJson << ',';
            uintptr_t ref = entry + 12 + 0x28 + static_cast<uintptr_t>(j) * 16;
            materialJson << (readValue<uint64_t>(ref) ? readValue<int32_t>(ref + 12) : -1);
        }
        materialJson << "]}";
    }
    materialJson << "]}\n";
    if (materialSlots.size() != used || !materialJson) throw std::runtime_error("Material table occupancy did not validate");
    uint32_t textureCount = readValue<uint32_t>(materials + 0x40);
    uintptr_t textureTable = readValue<uintptr_t>(materials + 0x48);
    if (textureCount > 65536) throw std::runtime_error("Too many textures");
    std::ofstream textureJson(outputPath / L"textures.json");
    textureJson << "{\"textures\":[";
    for (uint32_t i = 0; i < textureCount; ++i) {
        uintptr_t texture = textureTable + static_cast<uintptr_t>(i) * 0x20;
        uint32_t tw = readValue<uint32_t>(texture + 0x10), th = readValue<uint32_t>(texture + 0x14), bpp = readValue<uint32_t>(texture + 0x18);
        if (i) textureJson << ',';
        textureJson << "{\"slot\":" << i << ",\"width\":" << tw << ",\"height\":" << th << ",\"bytes_per_pixel\":" << bpp << "}";
        if (tw > 8192 || th > 8192 || bpp < 1 || bpp > 16) throw std::runtime_error("Unexpected texture dimensions");
        dump(readValue<uintptr_t>(texture + 8), static_cast<size_t>(tw) * th, bpp, outputPath / (L"texture-" + std::to_wstring(i) + L".bin"));
    }
    textureJson << "]}\n";
    if (!textureJson) throw std::runtime_error("Failed to write texture metadata");
    uint32_t meshCount = readValue<uint32_t>(scene + 0x180);
    uintptr_t meshTable = readValue<uintptr_t>(scene + 0x188);
    if (meshCount > 65536) throw std::runtime_error("Too many meshes");
    for (uint32_t i = 0; i < meshCount; ++i) {
        uintptr_t mesh = readValue<uintptr_t>(meshTable + static_cast<uintptr_t>(i) * 8);
        fs::path dir = outputPath / (L"mesh-" + std::to_wstring(i));
        fs::create_directories(dir);
        uint32_t vertices = readValue<uint32_t>(mesh + 0x10);
        uint32_t triangles = readValue<uint32_t>(mesh + 0x28);
        if (static_cast<uint64_t>(triangles) * 3 != vertices)
            throw std::runtime_error("Unexpected mesh vertex/triangle relationship");
        dump(readValue<uintptr_t>(mesh + 0x18), vertices, 16, dir / L"vertices-float4.bin");
        dump(readValue<uintptr_t>(mesh + 0x30), triangles, 2, dir / L"triangle-flags-u16.bin");
        uint32_t materialCount = readValue<uint32_t>(mesh + 0x58);
        if (materialCount != triangles) throw std::runtime_error("Unexpected per-triangle material count");
        uintptr_t primitiveKeys = readValue<uintptr_t>(mesh + 0x60);
        std::vector<uint32_t> slots(triangles);
        for (uint32_t j = 0; j < triangles; ++j) {
            uint64_t key = readValue<uint64_t>(primitiveKeys + static_cast<uintptr_t>(j) * 8);
            auto found = materialSlots.find(key);
            if (found == materialSlots.end()) throw std::runtime_error("Triangle material not in table");
            slots[j] = found->second;
        }
        writeFile(dir / L"triangle-material-slots.bin", slots.data(), slots.size() * 4);
        uint32_t colors = readValue<uint32_t>(mesh + 0x70), uvs = readValue<uint32_t>(mesh + 0x88);
        dump(readValue<uintptr_t>(mesh + 0x78), colors, 12, dir / L"triangle-colors.bin");
        dump(readValue<uintptr_t>(mesh + 0x90), uvs, 8, dir / L"vertex-uvs.bin");
        std::ofstream meshJson(dir / L"mesh.json");
        meshJson << "{\"vertices\":" << vertices << ",\"triangles\":" << triangles << ",\"colors\":" << colors << ",\"uvs\":" << uvs << ",\"flags\":" << readValue<uint32_t>(mesh + 0xa0) << ",\"children\":" << readValue<uint32_t>(mesh + 0xd8) << "}\n";
        if (!meshJson) throw std::runtime_error("Failed to write mesh metadata");
    }
    float skyColor[3], visibility[3], skyMatrix[12];
    readMemory(scene + 0x54, skyColor, sizeof(skyColor)); readMemory(scene + 0x44, visibility, sizeof(visibility)); readMemory(scene + 0x78, skyMatrix, sizeof(skyMatrix));
    std::ofstream skyJson(outputPath / L"sky.json");
    skyJson << std::setprecision(std::numeric_limits<float>::max_digits10);
    uintptr_t skyImage = readValue<uintptr_t>(scene + 0x70);
    uint32_t skyWidth = 0, skyHeight = 0;
    if (skyImage) {
        skyWidth = readValue<uint32_t>(skyImage); skyHeight = readValue<uint32_t>(skyImage + 4);
        if (skyWidth > 8192 || skyHeight > 8192) throw std::runtime_error("Unexpected sky dimensions");
        for (uint32_t c = 0; c < 3; ++c) {
            uintptr_t plane = readValue<uintptr_t>(skyImage + 0x20 + c * 8);
            uintptr_t stride = readValue<uintptr_t>(skyImage + 0x2a0 + c * 8);
            if (stride < skyWidth * 4ull || stride > 8192 * 16ull) throw std::runtime_error("Unexpected sky stride");
            std::vector<uint8_t> pixels(static_cast<size_t>(skyWidth) * skyHeight * 4);
            for (uint32_t y = 0; y < skyHeight; ++y) readMemory(plane + y * stride, pixels.data() + static_cast<size_t>(y) * skyWidth * 4, skyWidth * 4ull);
            writeFile(outputPath / (L"sky-plane-" + std::to_wstring(c) + L".bin"), pixels.data(), pixels.size());
        }
    }
    skyJson << "{\"constant_rgb\":[" << skyColor[0] << ',' << skyColor[1] << ',' << skyColor[2] << "],\"suppress_lower_hemisphere\":" << (readValue<uint8_t>(scene + 0x6c) ? "true" : "false") << ",\"width\":" << skyWidth << ",\"height\":" << skyHeight << ",\"transform\":[";
    for (int i = 0; i < 12; ++i) { if (i) skyJson << ','; skyJson << skyMatrix[i]; }
    skyJson << "],\"visibility_parameters\":[" << visibility[0] << ',' << visibility[1] << ',' << visibility[2] << "]}\n";
    if (!skyJson) throw std::runtime_error("Failed to write sky metadata");
    std::ofstream manifest(outputPath / L"capture.json");
    manifest << "{\n  \"schema\": 2,\n  \"executable_sha256\": \"" << (valveBuild?valveBuild->sha256:"") << "\",\n  \"width\": " << width << ",\n  \"height\": " << height
             << ",\n  \"luxels\": " << count << ",\n  \"footprint_entries\": " << entries
             << ",\n  \"sample_records\": " << samples << ",\n  \"meshes\": " << meshCount
             << ",\n  \"seed\": " << countAt(0x32aae8) << ",\n  \"light_count\": " << readValue<uint32_t>(scene) << ",\n  \"scene_export_complete\": false\n}\n";
    if (!manifest) throw std::runtime_error("Failed to write manifest");
}

struct Breakpoint {
    uintptr_t location = 0;
    uint8_t original = 0;
    bool armed = false;
    void write(uint8_t byte) {
        DWORD protection;
        if (!VirtualProtectEx(processHandle, reinterpret_cast<void*>(location), 1, PAGE_EXECUTE_READWRITE, &protection)) fail("VirtualProtectEx");
        SIZE_T written = 0;
        BOOL ok = WriteProcessMemory(processHandle, reinterpret_cast<void*>(location), &byte, 1, &written);
        DWORD ignored;
        VirtualProtectEx(processHandle, reinterpret_cast<void*>(location), 1, protection, &ignored);
        FlushInstructionCache(processHandle, reinterpret_cast<void*>(location), 1);
        if (!ok || written != 1) fail("WriteProcessMemory breakpoint");
    }
    void arm(uintptr_t where) {
        location = where; original = readValue<uint8_t>(where);
        if (original == 0xcc) throw std::runtime_error("Existing breakpoint at capture location");
        write(0xcc); armed = true;
    }
    void restore() { write(original); armed = false; }
};

struct ProfileBoundary { uintptr_t rva; const char* name; Breakpoint breakpoint; };
static void profileChildBoundary(const char* name,double launched) {
    if(!blockProfileEnabled()) return;
    FILETIME creation,exit,kernel,user;
    if(!GetProcessTimes(processHandle,&creation,&exit,&kernel,&user)) fail("GetProcessTimes");
    auto ticks=[](FILETIME value) { return (static_cast<uint64_t>(value.dwHighDateTime)<<32)|value.dwLowDateTime; };
    std::cerr << "BLOCK_BOUNDARY " << name << " wall_ms=" << blockProfileNow()-launched
              << " cpu_ms=" << (ticks(kernel)+ticks(user))/10000.0 << '\n';
}

static std::vector<uint8_t> loadFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot read replay file: " + path.string());
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

static void validateWorking(const std::vector<uint8_t>& bytes,const std::vector<uint8_t>& offsets) {
    uint32_t count = countAt(0x32ab88);
    if (!validGpuBlock(count,countAt(0x32aba8),countAt(0x32abac)))
        throw std::runtime_error("Invalid or oversized block dimensions");
    if (bytes.size() != static_cast<size_t>(count) * 204)
        throw std::runtime_error("GPU working record size mismatch");
    if(offsets.size()!=(count+1ull)*4) throw std::runtime_error("GPU CSR size mismatch");
    auto getUint = [](const uint8_t* p) { uint32_t value; memcpy(&value, p, 4); return value; };
    { BlockProfileScope profile("working_validation",bytes.size());
    parallelRange(count,1u<<14,[&](size_t from,size_t to) {
    for (size_t i = from; i < to; ++i) {
        bool valid = getUint(offsets.data() + i*4) != getUint(offsets.data() + (i+1)*4);
        uint32_t samples = getUint(bytes.data() + i*204 + 32);
        if ((valid && (samples == 0 || samples > 4096)) || (!valid && samples != 0))
            throw std::runtime_error("GPU sample count/validity mismatch");
        for (uint32_t group = 0; group < 4; ++group) {
            const uint8_t* acc = bytes.data() + i*204 + group*36;
            if (getUint(acc+32) != samples) throw std::runtime_error("GPU channel counts differ");
            float mean[4]; double scalar, m2;
            memcpy(mean, acc, 16); memcpy(&scalar, acc+16, 8); memcpy(&m2, acc+24, 8);
            for (float value : mean) if (!std::isfinite(value)) throw std::runtime_error("Nonfinite GPU mean");
            if (!std::isfinite(scalar) || !std::isfinite(m2) || m2 < 0)
                throw std::runtime_error("Invalid GPU variance");
        }
        for (uint32_t offset = 144; offset < 204; offset += 4)
            if (getUint(bytes.data() + i*204 + offset) != 0)
                throw std::runtime_error("GPU additive direct-light tail must be zero");
    }
    });
    }
}
static void validateAndWriteWorking(const std::vector<uint8_t>& bytes,const std::vector<uint8_t>& offsets,bool native,bool writeResult=true,bool validated=false) {
    if(!validated) validateWorking(bytes,offsets);
    if(!writeResult) return;
    BlockProfileScope profile("write_working",bytes.size());
    SIZE_T written = 0;
    if (!WriteProcessMemory(processHandle, reinterpret_cast<void*>(pointerAt(0x32ab20)), bytes.data(), bytes.size(), &written) || written != bytes.size())
        fail("Write GPU working records");
    if(!fileDiagnostics()) return;
    std::ofstream manifest(outputPath / L"gpu-injection.json");
    manifest << "{\"cpu_pathtrace_skipped\":true,\"inputs_byte_identical\":" << (native?"false":"true")
             << ",\"generated_from_paused_child\":" << (native?"true":"false") << ",\"quality_validated\":false}\n";
    if (!manifest) throw std::runtime_error("Failed to write injection metadata");
}

static void injectWorking(const fs::path& workingPath, const fs::path& sourceCapture) {
    for (const auto& entry : fs::recursive_directory_iterator(outputPath)) {
        if (!entry.is_regular_file() || entry.path().filename() == L"vrad3.log") continue;
        auto relative = fs::relative(entry.path(), outputPath);
        if (loadFile(entry.path()) != loadFile(sourceCapture / relative))
            throw std::runtime_error("Runtime input differs from GPU source: " + relative.string());
    }
    validateAndWriteWorking(loadFile(workingPath),loadFile(outputPath / L"footprint-offsets.bin"),false);
}

// lightmap_pathtrace Run (RVA ca90; argc in EDX, argv in R8): lightmap_pathtrace <min> <max> <tol>,
// Valve's defaults 128, 512, 0.02.
static void scriptPathtraceArguments(const CONTEXT& context) {
    pathtraceMinSamples=128; pathtraceTolerance=0.02f;
    uint32_t argc=static_cast<uint32_t>(context.Rdx);
    if(argc>64 || !context.R8) return;
    auto token=[&](uint32_t index) {
        std::string text; uintptr_t pointer=readValue<uintptr_t>(context.R8+8ull*index);
        for(size_t i=0;i<32 && pointer;++i) { char ch=readValue<char>(pointer+i); if(!ch) return text; text+=ch; }
        return std::string();
    };
    try {
        if(argc>=2) { size_t used=0; long value=std::stol(token(1),&used); if(value>=1 && value<=4096) pathtraceMinSamples=static_cast<uint32_t>(value); }
        if(argc>=4) { size_t used=0; float value=std::stof(token(3),&used); if(std::isfinite(value) && value>0 && value<1) pathtraceTolerance=value; }
    } catch(...) { pathtraceMinSamples=0; }
}
static uint32_t scriptMaxSamples(const CONTEXT& context) {
    uint32_t argc=static_cast<uint32_t>(context.Rdx);
    if(argc<3) return 512;
    if(argc>64 || !context.R8) throw std::runtime_error("Invalid pathtrace script arguments");
    uintptr_t text=readValue<uintptr_t>(context.R8+16);
    std::string token;
    for(size_t i=0;i<32;++i) {
        char ch=readValue<char>(text+i);
        if(!ch) {
            size_t used=0;
            long value=std::stol(token,&used);
            if(used!=token.size() || value<1 || value>4096) throw std::runtime_error("Unsupported pathtrace sample maximum");
            return static_cast<uint32_t>(value);
        }
        token+=ch;
    }
    throw std::runtime_error("Pathtrace sample token is too long");
}

struct DirectLightArguments { uint32_t lights,samples; };
// lightmap_directlight Run (RVA 52d0; argc in EDX, argv in R8): directlight <lights> <spp> <mode>. Only explicit
// arguments with mode 0 are supported; the visibility contract was measured for that mode only.
static DirectLightArguments directlightArguments(const CONTEXT& context) {
    uint32_t argc=static_cast<uint32_t>(context.Rdx);
    if(argc<4) throw std::runtime_error("directlight needs explicit lights/spp/mode arguments");
    if(argc>64 || !context.R8) throw std::runtime_error("Invalid directlight script arguments");
    long values[3];
    for(uint32_t index=0;index<3;++index) {
        uintptr_t text=readValue<uintptr_t>(context.R8+8+index*8);
        std::string token;
        for(size_t i=0;;++i) {
            if(i>=16) throw std::runtime_error("Directlight argument token is too long");
            char ch=readValue<char>(text+i);
            if(!ch) break;
            token+=ch;
        }
        size_t used=0;
        try { values[index]=std::stol(token,&used); } catch(...) { used=0; }
        if(!used || used!=token.size()) throw std::runtime_error("Unsupported directlight argument");
    }
    if(values[0]<1 || values[0]>4) throw std::runtime_error("Unsupported directlight light count (stored slots are 1..4)");
    if(values[1]<1 || values[1]>4096) throw std::runtime_error("Unsupported directlight sample count");
    if(values[2]!=0) throw std::runtime_error("Unsupported directlight mode argument (only 0 was measured)");
    return {static_cast<uint32_t>(values[0]),static_cast<uint32_t>(values[1])};
}
static std::string lightLimitText;
static bool lightLimitReportEnabled() {
    wchar_t value[8]{};
    return GetEnvironmentVariableW(L"VRAD3_GPU_LIGHT_LIMIT_REPORT",value,8)==1 && value[0]==L'1';
}
static std::string commandBlockNumber() {
    const std::wstring line=GetCommandLineW(); const auto at=line.find(L"-blocknum");
    if(at==std::wstring::npos) return "?";
    size_t i=at+9; while(i<line.size() && (line[i]==L' ' || line[i]==L'=' || line[i]==L'"')) ++i;
    std::string block; while(i<line.size() && line[i]>=L'0' && line[i]<=L'9') block+=static_cast<char>(line[i++]);
    return block.empty()?"?":block;
}
static void lightLimitReport(const std::vector<uint8_t>& records,uint32_t lightLimit) {
    struct Group { uint64_t texels=0; float lo[3]={1e30f,1e30f,1e30f},hi[3]={-1e30f,-1e30f,-1e30f}; uint32_t most=0; };
    std::map<std::vector<uint32_t>,Group> groups; uint64_t over=0; uint32_t most=0,peak=0;
    const size_t count=records.size()/204;
    for(size_t i=0;i<count;++i) {
        const uint8_t* record=records.data()+i*204;
        uint32_t seen; memcpy(&seen,record+4,4);
        peak=std::max(peak,seen);
        if(seen<=lightLimit) continue;
        ++over; most=std::max(most,seen);
        std::vector<uint32_t> ids;
        for(uint32_t k=0;k<std::min<uint32_t>(seen,32);++k) ids.push_back(record[20+k]);
        std::sort(ids.begin(),ids.end()); ids.erase(std::unique(ids.begin(),ids.end()),ids.end());
        auto& group=groups[ids]; ++group.texels; group.most=std::max(group.most,seen);
        float centre[3]; memcpy(centre,record+8,12);
        for(int a=0;a<3;++a) { group.lo[a]=std::min(group.lo[a],centre[a]); group.hi[a]=std::max(group.hi[a],centre[a]); }
    }
    std::ostringstream text; text.setf(std::ios::fixed); text.precision(0);
    text << "lightmap block " << commandBlockNumber() << ": " << over << " of " << count << " texels are reached by more than "
         << lightLimit << " indexed lights";
    if(!over) { lightLimitText+=text.str()+" (most on one texel: "+std::to_string(peak)+")\n"; return; }
    text << " (up to " << most << "); the weakest are dropped there\n";
    std::map<uint32_t,std::string> described;
    try {
        // Scene light table: count at scene+0, array at scene+8; 264-byte records: +0 type (1 omni, 2 sun, 3 spot,
        // 4 ortho), +4 colour, +0x78 runtime index, +0xac position.
        uintptr_t scene=pointerAt(0x32aa20); uint32_t lights=readValue<uint32_t>(scene); uintptr_t table=readValue<uintptr_t>(scene+8);
        for(uint32_t i=0;i<lights && i<65536;++i) {
            uintptr_t light=table+static_cast<uintptr_t>(i)*264;
            const int32_t id=readValue<int32_t>(light+0x78); if(id<0 || id>255) continue;
            const uint32_t type=readValue<uint32_t>(light);
            float position[3],colour[3]; readMemory(light+0xac,position,12); readMemory(light+0x4,colour,12);
            std::ostringstream line; line.setf(std::ios::fixed); line.precision(0);
            line << (type==1?"omni":type==2?"sun":type==3?"spot":type==4?"ortho":"light") << " at (" << std::lround(position[0]) << ", " << std::lround(position[1]) << ", " << std::lround(position[2]) << ")";
            line.precision(2); line << " colour (" << colour[0] << ", " << colour[1] << ", " << colour[2] << ")";
            described.emplace(static_cast<uint32_t>(id),line.str());
        }
    } catch(...) {}
    std::vector<std::pair<std::vector<uint32_t>,Group>> sorted(groups.begin(),groups.end());
    std::sort(sorted.begin(),sorted.end(),[](const auto& a,const auto& b) { return a.second.texels>b.second.texels; });
    size_t shown=0;
    for(const auto& [ids,group]:sorted) {
        if(++shown>20) { text << "  ... " << sorted.size()-20 << " more groups\n"; break; }
        text << "  " << group.texels << " texels around (" << std::lround((group.lo[0]+group.hi[0])/2) << ", " << std::lround((group.lo[1]+group.hi[1])/2) << ", "
             << std::lround((group.lo[2]+group.hi[2])/2) << "), area " << std::lround(group.hi[0]-group.lo[0]) << " x " << std::lround(group.hi[1]-group.lo[1]) << " x "
             << std::lround(group.hi[2]-group.lo[2]) << ", up to " << group.most << " lights:\n";
        for(uint32_t id:ids) { auto found=described.find(id); text << "    light " << id << (found!=described.end()?": "+found->second:"") << "\n"; }
    }
    lightLimitText+=text.str();
    std::cout << "Light limit report: block " << commandBlockNumber() << ", " << over << " texels over the limit of " << lightLimit
              << " (" << groups.size() << " light groups)\n" << std::flush;
}
// Working records are 204 bytes (RVA 32ab20); their tail +0x90..+0xc8 holds the baked RGB and SH sums. Output
// records are 0x8c bytes (RVA 32ab90): +0x40 light scalars, +0x50 id bytes. Directlight adds to the tail and
// overwrites the used light slots; unused slots keep their contents.
static void applyDirectResults(const std::vector<uint8_t>& records,const std::vector<uint8_t>& offsets,uint32_t lightLimit) {
    uint32_t count=countAt(0x32ab88);
    if(!validGpuBlock(count,countAt(0x32aba8),countAt(0x32abac))) throw std::runtime_error("Invalid or oversized block dimensions");
    if(records.size()!=static_cast<size_t>(count)*204 || offsets.size()!=(count+1ull)*4)
        throw std::runtime_error("GPU directlight record size mismatch");
    auto word=[](const uint8_t* p) { uint32_t value; memcpy(&value,p,4); return value; };
    auto real=[](const uint8_t* p) { float value; memcpy(&value,p,4); return value; };
    { BlockProfileScope profile("direct_validation",records.size());
    for(uint32_t i=0;i<count;++i) {
        const uint8_t* record=records.data()+static_cast<size_t>(i)*204;
        bool valid=word(offsets.data()+i*4)!=word(offsets.data()+(i+1)*4);
        uint32_t selected=word(record);
        if(selected>lightLimit || (!valid && selected)) throw std::runtime_error("GPU directlight light count mismatch");
        for(uint32_t slot=0;slot<selected;++slot) {
            float scalar=real(record+64+slot*4);
            if(!std::isfinite(scalar) || scalar<=0 || scalar>1) throw std::runtime_error("GPU directlight scalar out of range");
        }
        for(uint32_t offset=144;offset<204;offset+=4) {
            float value=real(record+offset);
            if(!std::isfinite(value) || (!valid && value!=0)) throw std::runtime_error("GPU directlight tail invalid");
        }
    } }
    auto working=copyArray(pointerAt(0x32ab20),count,204,"read_direct_working");
    auto output=copyArray(pointerAt(0x32ab90),count,0x8c,"read_direct_output");
    { BlockProfileScope profile("direct_merge",records.size());
    for(uint32_t i=0;i<count;++i) {
        const uint8_t* record=records.data()+static_cast<size_t>(i)*204;
        uint8_t* tail=working.data()+static_cast<size_t>(i)*204;
        for(uint32_t offset=144;offset<204;offset+=4) {
            float sum=real(tail+offset)+real(record+offset);
            if(!std::isfinite(sum)) throw std::runtime_error("Nonfinite directlight tail sum");
            memcpy(tail+offset,&sum,4);
        }
        uint8_t* out=output.data()+static_cast<size_t>(i)*0x8c;
        uint32_t selected=word(record);
        for(uint32_t slot=0;slot<selected;++slot) {
            memcpy(out+0x40+slot*4,record+64+slot*4,4);
            out[0x50+slot]=record[80+slot];
        }
    } }
    BlockProfileScope profile("write_direct",working.size()+output.size());
    SIZE_T written=0;
    if(!WriteProcessMemory(processHandle,reinterpret_cast<void*>(pointerAt(0x32ab20)),working.data(),working.size(),&written) || written!=working.size())
        fail("Write GPU directlight working records");
    written=0;
    if(!WriteProcessMemory(processHandle,reinterpret_cast<void*>(pointerAt(0x32ab90)),output.data(),output.size(),&written) || written!=output.size())
        fail("Write GPU directlight output records");
}
enum class DirectMode { Off, Auto, Force };
static DirectMode directlightMode() {
    std::wstring value=environmentValue(L"VRAD3_GPU_DIRECTLIGHT");
    if(value.empty()) return DirectMode::Auto;
    if(value==L"1") return DirectMode::Force;
    if(value==L"0") return DirectMode::Off;
    throw std::runtime_error("VRAD3_GPU_DIRECTLIGHT must be 0 or 1");
}
static uint64_t directlightMinimumWork() {
    static const uint64_t work=(1ull<<19)*std::max(1u,usableCpuCount());
    return work;
}
#include "verify_mode.h"
#include "emissive_materials.h"

static int runCapture(int argc, wchar_t** argv,bool keepGpuSession=false) {
    hammerTiming={};
    hammerHardware={}; hammerFallbackReason.clear(); hammerChildExit=-1;
    emissiveApplied=false; hammerEmissive.clear(); gpuEmission.clear(); gpuEmissionMasks.clear();
    seedChanged=false;
    bool cpuBlock=argc==6 && std::wstring(argv[4])==L"--cpu-block";
    bool nativeMode=(argc==9 || argc==10) && std::wstring(argv[4])==L"--native";
    if (argc != 4 && argc != 6 && !nativeMode) {
        std::wcerr << L"Usage: vrad3_capture.exe <vrad3_real.exe> <isolated-bake-folder> <capture-folder> [<gpu-working.bin> <source-capture-folder>]\n"
                     L"  or ... --cpu-block <block-number> (VRAD3_SEED_XOR is CPU capture only)\n"
                     L"  or ... --native <gpu-library.dll> <shader.dxil> <samples> <lm_compute|lm_compute_all> [block-number]\n";
        return 2;
    }
    fs::path executable = fs::absolute(argv[1]), workdir = fs::absolute(argv[2]);
    outputPath = fs::absolute(argv[3]);
    fs::path runPath=outputPath;
    bool gpuReplay = argc == 6 && !cpuBlock;
    if (!hammerMode && fs::exists(outputPath) && !fs::is_empty(outputPath)) {
        std::cerr << "Capture output folder must be new or empty\n";
        return 2;
    }
    if(fileDiagnostics()) fs::create_directories(outputPath);
    PROCESS_INFORMATION info{};
    HANDLE log = INVALID_HANDLE_VALUE;
    HMODULE gpuLibrary=nullptr;
    GpuLightingBake bakeGpu=nullptr;
    void (*shutdownGpu)()=nullptr;
    ULONGLONG started=GetTickCount64();
    uint32_t nativeSamples=0,gpuBlocks=0,fallbackBlocks=0,stages=0;
    uint32_t blockNumber=0;
    bool childExited=false;
    DWORD childExitCode=1;
    try {
        verifyExecutable(executable);
        if(cpuBlock) {
            std::wstring text=argv[5];
            if(text.empty() || text.find_first_not_of(L"0123456789")!=std::wstring::npos || std::stoull(text)>UINT32_MAX)
                throw std::runtime_error("Invalid CPU block number");
            blockNumber=static_cast<uint32_t>(std::stoull(text));
        }
        if(nativeMode) {
            nativeSamples=static_cast<uint32_t>(std::stoul(argv[7]));
            if(argc==10) blockNumber=static_cast<uint32_t>(std::stoul(argv[9]));
            if((!nativeSamples && !hammerMode) || nativeSamples>4096 || (std::wstring(argv[8])!=L"lm_compute" && std::wstring(argv[8])!=L"lm_compute_all"))
                throw std::runtime_error("Unsupported native samples/entrypoint");
#ifdef VRAD3_STATIC_GPU
            if(hammerMode) { bakeGpu=&BakeGpuLighting; shutdownGpu=&ShutdownGpuLighting; }
            else
#endif
            {
            gpuLibrary=LoadLibraryW(fs::absolute(argv[5]).c_str());
            if(!gpuLibrary) fail("Load parent-side GPU library");
            bakeGpu=reinterpret_cast<GpuLightingBake>(GetProcAddress(gpuLibrary,"BakeGpuLighting"));
            if(!bakeGpu) fail("Resolve GPU lighting API");
            shutdownGpu=reinterpret_cast<void (*)()>(GetProcAddress(gpuLibrary,"ShutdownGpuLighting"));
            if(!shutdownGpu) fail("Resolve GPU shutdown API");
            }
        }
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        if(fileDiagnostics()) log = CreateFileW((outputPath / L"vrad3.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (fileDiagnostics() && log == INVALID_HANDLE_VALUE) fail("Create log");
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES; startup.hStdOutput = log; startup.hStdError = log;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        if(hammerMode) { startup.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE); startup.hStdError=GetStdHandle(STD_ERROR_HANDLE); }
        std::wstring entrypoint=nativeMode?argv[8]:L"lm_compute";
        wchar_t blockFile[64]=L"captured-block.dat";
        if(cpuBlock || (nativeMode && argc==10)) swprintf_s(blockFile,L"lm_block_%08u.dat",blockNumber);
        std::wstring command = L"\"" + executable.wstring() + L"\" -map maps/test.vmap -script script.vrad3 -entrypoint " + entrypoint + L" -blockfile=" + blockFile + L" -blocknum=" + std::to_wstring(blockNumber) + L" -threads " + threadCount() + L" -outdir results/outputs";
        if(hammerMode) command=hammerCommand;
        double childLaunched=blockProfileNow();
        DWORD childFlags = DEBUG_ONLY_THIS_PROCESS | CREATE_NO_WINDOW | childPriorityFlag();
        progressWindow.launch(startup, childFlags);
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE, childFlags, nullptr, workdir.c_str(), &startup, &info)) fail("CreateProcessW");
        processHandle = info.hProcess;
        DebugSetProcessKillOnExit(TRUE);
        Breakpoint entry, leave, rayResult, directQueues, directRun, directLeave, rejectCall, rejectAfter, sampleGridRun, exactStage, exactEnd;
        std::wstring sampleGrid=nativeMode?environmentValue(L"VRAD3_GPU_SAMPLE_GRID"):std::wstring();
        if(nativeMode && sampleGrid.empty() && environmentValue(L"VRAD3_GPU_ESTIMATOR")==L"cs2") sampleGrid=L"1";
#ifdef VRAD3_STATIC_GPU
        const RejectSetting rejectMode=hammerMode && nativeMode ? rejectSetting() : RejectSetting::Off;
        const bool stageMode=hammerMode && nativeMode && sampleStageWanted();
        const bool exactMode=hammerMode && nativeMode && !stageMode && exactStageWanted();
#endif
        const DirectMode directPolicy=nativeMode?directlightMode():DirectMode::Off;
        const bool directGpu=directPolicy!=DirectMode::Off;
        VerifyBlock verification;
        bool pathtraceOnGpu=false;
        uint32_t directGpuBlocks=0,directCpuBlocks=0; double directGpuMs=0;
        const bool inspectDirectQueues=!hammerMode && environmentValue(L"VRAD3_DIRECTLIGHT_QUEUES")==L"1";
        std::vector<ProfileBoundary> boundaries={
            {0x48da,"first_log_call",{}}, {0x7877,"rasterize_small",{}},
            {0x916b,"rasterize_large",{}}, {0x9ec7,"reproject",{}},
            {0x9fe0,"reject_invalid",{}}, {0xa197,"calculate_normals",{}},
            {0xa42c,"map_samples",{}}, {0xa585,"sort_mapping",{}},
            {0xa62f,"build_lookup",{}}, {0xaaec,"confidence",{}},
            {0x536d,"directlight",{}}, {0x5491,"directlight_done",{}},
            {0x591d,"block_written",{}}
        };
        bool before = false, after = false, raysCaptured = false;
        DWORD exitCode = 1;
        DEBUG_EVENT event{};
        for (;;) {
            if (!WaitForDebugEvent(&event, hammerMode?1000:60000)) {
                if(hammerMode && GetLastError()==ERROR_SEM_TIMEOUT) continue;
                fail("Child debug event wait failed or timed out");
            }
            DWORD disposition = DBG_CONTINUE;
            if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
                imageBase = reinterpret_cast<uintptr_t>(event.u.CreateProcessInfo.lpBaseOfImage);
                if (event.u.CreateProcessInfo.hFile) CloseHandle(event.u.CreateProcessInfo.hFile);
                // Each hook site is checked for the expected prologue (mov rax,rsp = 48 8b c4) before a breakpoint is armed:
                // RVA ca90 pathtrace Run, 52d0 directlight Run, 75e0 sample-grid stage Run.
                uint8_t prefix[3]; readMemory(address(0xca90), prefix, 3);
                if (prefix[0] != 0x48 || prefix[1] != 0x8b || prefix[2] != 0xc4)
                    throw std::runtime_error("Unexpected pathtrace entry; refusing to patch");
                entry.arm(address(0xca90));
                if(directGpu) {
                    uint8_t directPrefix[3]; readMemory(address(0x52d0), directPrefix, 3);
                    if (directPrefix[0] != 0x48 || directPrefix[1] != 0x8b || directPrefix[2] != 0xc4)
                        throw std::runtime_error("Unexpected directlight entry; refusing to patch");
                    directRun.arm(address(0x52d0));
                }
                if(inspectDirectQueues) directQueues.arm(address(0x22bb0));
                if(sampleGrid.size()==1 && sampleGrid[0]>=L'1' && sampleGrid[0]<=L'9') {
                    uint8_t stagePrefix[3]; readMemory(address(0x75e0),stagePrefix,3);
                    if(stagePrefix[0]!=0x48 || stagePrefix[1]!=0x8b || stagePrefix[2]!=0xc4) std::cerr << "sample_grid unavailable: unexpected stage entry\n";
                    else sampleGridRun.arm(address(0x75e0));
                }
#ifdef VRAD3_STATIC_GPU
                if(rejectMode!=RejectSetting::Off || stageMode) {
                    // Reject hook: the stage's CALL rel32 (E8) must target the expected dispatcher.
                    uint8_t call[5]; readMemory(address(rejectCallRva),call,5); int32_t rel=0; memcpy(&rel,call+1,4);
                    if(call[0]!=0xe8 || address(rejectCallRva)+5+rel!=address(rejectDispatchRva)) std::cerr << "GPU sample reject unavailable: unexpected call site\n";
                    else rejectCall.arm(address(rejectCallRva));
                }
                if(exactMode) {
                    // Exact-stage hook: CMP dword [rip+rel32],0 (83 3D rel32 00) on the sample count global (RVA 32ad28),
                    // right after Valve prints "Calculate Normals...".
                    uint8_t compare[7]{}; int32_t rel=0;
                    if(valveBuild && strcmp(valveBuild->sha256,exactStageSha)==0) { readMemory(address(exactStageRva),compare,7); memcpy(&rel,compare+2,4); }
                    if(compare[0]!=0x83 || compare[1]!=0x3d || compare[6]!=0 || address(exactStageRva)+7+rel!=address(0x32ad28))
                        exactStageSummary="exact: unavailable for this Valve build";
                    else exactStage.arm(address(exactStageRva));
                }
#endif
                if(blockProfileEnabled()) for(auto& boundary:boundaries) boundary.breakpoint.arm(address(boundary.rva));
            } else if (event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT) {
                if (event.u.LoadDll.hFile) CloseHandle(event.u.LoadDll.hFile);
            } else if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
                auto exception = event.u.Exception.ExceptionRecord;
                uintptr_t hit = reinterpret_cast<uintptr_t>(exception.ExceptionAddress);
                ProfileBoundary* boundaryHit=nullptr;
                if(blockProfileEnabled()) for(auto& boundary:boundaries)
                    if(boundary.breakpoint.armed && hit==boundary.breakpoint.location) boundaryHit=&boundary;
#ifdef VRAD3_STATIC_GPU
                if(exception.ExceptionCode==EXCEPTION_BREAKPOINT && ((rejectCall.armed && hit==rejectCall.location) || (rejectAfter.armed && hit==rejectAfter.location))) {
                    HANDLE thread=OpenThread(THREAD_GET_CONTEXT|THREAD_SET_CONTEXT,FALSE,event.dwThreadId);
                    if(!thread) fail("Open sample reject thread");
                    CONTEXT context{}; context.ContextFlags=CONTEXT_FULL;
                    if(!GetThreadContext(thread,&context)) fail("Get sample reject context");
                    applyEmissiveMaterials(executable,workdir);
                    if(rejectAfter.armed && hit==rejectAfter.location) {
                        rejectAfter.restore(); context.Rip=hit;
                        if(rejectAfterCall(context)) rejectAfter.arm(address(rejectAfterRva));
                    } else if(stageMode && [&] {
                        rejectCall.restore(); context.Rip=hit;
                        progressWindow.status(L"GPU sample stage");
                        bool changed=false;
                        try { return sampleStageAtCall(context,executable,changed); }
                        catch(const std::exception& error) {
                            if(changed) throw;
                            std::cerr << "GPU sample stage unavailable; continuing with Valve's: " << sampleStageStep << ": " << error.what() << '\n' << std::flush;
                            sampleStageSummary="cpu: "+std::string(error.what());
                            return false;
                        }
                    }()) {
                        progressWindow.status(L"Valve CPU stages");
                    } else if(rejectMode!=RejectSetting::Off) {
                        rejectCall.restore(); context.Rip=hit;
                        progressWindow.status(L"GPU sample reject");
                        bool skip=false;
                        try { skip=rejectAtCall(context,executable,rejectMode); }
                        catch(const std::exception& error) {
                            rejectRun=RejectRun{}; rejectSummary=std::string("CPU: ")+error.what();
                            std::cerr << "GPU sample reject unavailable; continuing with CPU: " << error.what() << '\n' << std::flush;
                        }
                        if(skip) {
                            // Skip Valve's call: continue at the instruction after it.
                            context.Rip=address(rejectAfterRva);
                            std::cout << "GPU sample reject: samples=" << rejectRun.count << " gpu=" << rejectRun.certain << " cpu=0 gpu_ms="
                                      << rejectRun.gpuMs << " stage_ms=" << rejectRun.stageMs << '\n' << std::flush;
                            rejectSummary="gpu="+std::to_string(rejectRun.certain)+" cpu=0";
                        } else if(rejectRun.active) rejectAfter.arm(address(rejectAfterRva));
                        progressWindow.status(L"Valve CPU stages");
                    }
                    if(!SetThreadContext(thread,&context)) fail("Set sample reject context");
                    CloseHandle(thread);
                } else if(exception.ExceptionCode==EXCEPTION_BREAKPOINT && exactStage.armed && hit==exactStage.location) {
                    HANDLE thread=OpenThread(THREAD_GET_CONTEXT|THREAD_SET_CONTEXT,FALSE,event.dwThreadId);
                    if(!thread) fail("Open exact sample stage thread");
                    CONTEXT context{}; context.ContextFlags=CONTEXT_FULL;
                    if(!GetThreadContext(thread,&context)) fail("Get exact sample stage context");
                    exactStage.restore(); context.Rip=hit;
                    progressWindow.status(L"Sample texel mapping");
                    bool changed=false;
                    const bool verify=exactVerifyWanted();
                    try { exactStageAt(context,changed,verify); if(verify && exactExpected.luxels>0) exactEnd.arm(address(sampleStageEndRva)); }
                    catch(const std::exception& error) {
                        if(changed) throw;
                        std::cerr << "Exact sample stage unavailable; continuing with Valve's: " << exactStageStep << ": " << error.what() << '\n' << std::flush;
                        exactStageSummary="exact: cpu: "+std::string(error.what());
                    }
                    progressWindow.status(L"Valve CPU stages");
                    if(!SetThreadContext(thread,&context)) fail("Set exact sample stage context");
                    CloseHandle(thread);
                } else if(exception.ExceptionCode==EXCEPTION_BREAKPOINT && exactEnd.armed && hit==exactEnd.location) {
                    HANDLE thread=OpenThread(THREAD_GET_CONTEXT|THREAD_SET_CONTEXT,FALSE,event.dwThreadId);
                    if(!thread) fail("Open exact verify thread");
                    CONTEXT context{}; context.ContextFlags=CONTEXT_FULL;
                    if(!GetThreadContext(thread,&context)) fail("Get exact verify context");
                    exactEnd.restore(); context.Rip=hit;
                    try { exactVerifyAt(); }
                    catch(const std::exception& error) { std::cerr << "Exact verify failed: " << error.what() << '\n' << std::flush; }
                    if(!SetThreadContext(thread,&context)) fail("Set exact verify context");
                    CloseHandle(thread);
                } else
#endif
                if(exception.ExceptionCode==EXCEPTION_BREAKPOINT && sampleGridRun.armed && hit==sampleGridRun.location) {
                    HANDLE thread=OpenThread(THREAD_GET_CONTEXT|THREAD_SET_CONTEXT,FALSE,event.dwThreadId);
                    if(!thread) fail("Open sample grid thread");
                    CONTEXT context{}; context.ContextFlags=CONTEXT_FULL;
                    if(!GetThreadContext(thread,&context)) fail("Get sample grid context");
                    sampleGridRun.restore(); context.Rip=hit;
                    // Stage Run (argc in EDX, argv in R8): argv[1] is the one-digit samples-per-side argument of
                    // lightmap_generate_samples_from_packing_geometry; it is replaced in place (same length).
                    char argument[2]{};
                    const uintptr_t text=static_cast<uint32_t>(context.Rdx)>=2?readValue<uintptr_t>(context.R8+8):0;
                    if(text) readMemory(text,argument,2);
                    if(text && argument[0]>='1' && argument[0]<='9' && argument[1]==0 && static_cast<char>(sampleGrid[0])>=argument[0]) {}
                    else if(text && argument[0]>='1' && argument[0]<='9' && argument[1]==0) {
                        const char wanted=static_cast<char>(sampleGrid[0]);
                        SIZE_T written=0;
                        if(WriteProcessMemory(processHandle,reinterpret_cast<void*>(text),&wanted,1,&written) && written==1)
                            std::cout << "GPU wrapper: sample grid " << argument[0] << "x" << argument[0] << " -> " << wanted << "x" << wanted << '\n' << std::flush;
                        else std::cerr << "sample_grid unavailable: argument not writable\n";
                    } else std::cerr << "sample_grid unavailable: unexpected stage argument\n";
                    if(!SetThreadContext(thread,&context)) fail("Set sample grid context");
                    CloseHandle(thread);
                } else if(exception.ExceptionCode==EXCEPTION_BREAKPOINT && boundaryHit) {
                    HANDLE thread=OpenThread(THREAD_GET_CONTEXT|THREAD_SET_CONTEXT,FALSE,event.dwThreadId);
                    if(!thread) fail("Open profile thread");
                    CONTEXT context{}; context.ContextFlags=CONTEXT_CONTROL;
                    if(!GetThreadContext(thread,&context)) fail("Get profile context");
                    profileChildBoundary(boundaryHit->name,childLaunched);
                    boundaryHit->breakpoint.restore(); context.Rip=hit;
                    if(!SetThreadContext(thread,&context)) fail("Set profile context");
                    CloseHandle(thread);
                } else if (exception.ExceptionCode == EXCEPTION_BREAKPOINT && directQueues.armed && hit==directQueues.location) {
                    HANDLE thread=OpenThread(THREAD_GET_CONTEXT|THREAD_SET_CONTEXT,FALSE,event.dwThreadId);
                    if(!thread) fail("Open directlight queue thread");
                    CONTEXT context{}; context.ContextFlags=CONTEXT_FULL;
                    if(!GetThreadContext(thread,&context)) fail("Get directlight queue context");
                    directQueues.restore(); context.Rip=hit;
                    std::ofstream json(outputPath/L"directlight-queues.json");
                    // Directlight queue context: RCX+0x58+i*8 points at the three visibility queues; per queue +0x5818 root mesh,
                    // +0x5820 ray mask, +0x5824 cull.
                    json << "{\"mode\":" << readValue<uint32_t>(context.Rsp+0x30) << ",\"queues\":[";
                    for(uint32_t i=0;i<3;++i) {
                        uintptr_t queue=readValue<uintptr_t>(context.Rcx+0x58+i*8);
                        if(!queue) throw std::runtime_error("Null directlight queue");
                        uintptr_t root=readValue<uintptr_t>(queue+0x5818);
                        if(!root) throw std::runtime_error("Null directlight queue root");
                        if(i) json << ',';
                        json << "{\"context_offset\":" << 0x58+i*8 << ",\"queue\":" << queue
                             << ",\"root\":" << root << ",\"embree_scene\":" << readValue<uintptr_t>(root+8)
                             << ",\"mask\":" << readValue<uint32_t>(queue+0x5820)
                             << ",\"cull\":" << readValue<uint32_t>(queue+0x5824)
                             << ",\"mesh_flags\":" << readValue<uint32_t>(root+0xa0) << '}';
                    }
                    json << "]}\n";
                    if(!json) throw std::runtime_error("Directlight queue capture write failed");
                    if(!SetThreadContext(thread,&context)) fail("Set directlight queue context");
                    CloseHandle(thread);
                    std::cout << "Captured directlight queue contract\n" << std::flush;
                } else if (exception.ExceptionCode == EXCEPTION_BREAKPOINT && ((directRun.armed && hit==directRun.location) || (directLeave.armed && hit==directLeave.location))) {
                    HANDLE thread=OpenThread(THREAD_GET_CONTEXT|THREAD_SET_CONTEXT,FALSE,event.dwThreadId);
                    if(!thread) fail("Open directlight thread");
                    CONTEXT context{}; context.ContextFlags=CONTEXT_FULL;
                    if(!GetThreadContext(thread,&context)) fail("Get directlight context");
                    if(directLeave.armed && hit==directLeave.location) {
                        directLeave.restore(); context.Rip=hit;
                        if(verifyItem && verification.pathCompared) { try { verification.finish(); } catch(const std::exception& error) { verifyUnavailable(error.what()); } }
                        directRun.arm(address(0x52d0));
                    } else {
                        directRun.restore(); context.Rip=hit;
                        applyEmissiveMaterials(executable,workdir);
                        profileChildBoundary("wrapper_direct",childLaunched);
                        progressWindow.status(L"GPU direct lighting");
                        ULONGLONG directStarted=GetTickCount64();
                        bool directComputed=false;
 bool directServed=false;
                        std::string directNote;
                        try {
                            if(!pathtraceOnGpu) throw std::runtime_error("pathtrace did not run on the GPU for this block");
                            DirectLightArguments arguments=directlightArguments(context);
                            if(directPolicy==DirectMode::Auto && !lightLimitReportEnabled() && static_cast<uint64_t>(countAt(0x32ab88))*arguments.samples<directlightMinimumWork())
                                throw std::runtime_error("small block: CPU directlight is cheaper than GPU setup (VRAD3_GPU_DIRECTLIGHT=1 forces GPU)");
                            progressWindow.begin(L"Direct Light on GPU ("+std::to_wstring(arguments.lights)+L" lights, "+std::to_wstring(arguments.samples)+L" spp)... ");
                            NativeLighting inputs; GpuLightingTiming timing{};
                            std::vector<uint8_t> records;
                            int resident=0; (void)resident;
#ifdef VRAD3_STATIC_GPU
                            if(hammerMode) {
                                std::string reason; resident=gpuServer::bakeResident(arguments.samples,arguments.lights,inputs,records,timing,reason);
                                if(resident==2) throw std::runtime_error(reason);
                                directServed=resident==1;
                                if(resident==0 && !reason.empty())
                                    std::cout << "GPU server unavailable; using in-worker GPU: " << reason << '\n' << std::flush;
                            }
#endif
                            if(!directServed) {
                                { BlockProfileScope profile("prepare_direct"); inputs=prepareNative(arguments.samples,arguments.lights); }
                                requirePhysicalMemory(static_cast<uint64_t>(inputs.constants[0])*204,"directlight records");
                                records.assign(static_cast<size_t>(inputs.constants[0])*204,0);
                                std::wstring shader=fs::absolute(argv[6]).wstring();
                                GpuLightingRequest request{}; request.version=5;
                                memcpy(request.constants,inputs.constants,sizeof(inputs.constants));
                                for(size_t i=0;i<inputs.buffers.size();++i)
                                    request.buffers[i]={inputs.buffers[i].data(),inputs.buffers[i].size()};
                                request.shaderPath=shader.c_str(); request.output=records.data(); request.outputBytes=records.size();
                                request.readSlice=readNativeSlice; request.sliceContext=&inputs; request.totalPaths=inputs.totalPaths;
                                request.progress=progressWindow.progress();
#ifdef VRAD3_STATIC_GPU
                                if(hammerMode) {
                                    request.shaderPath=nullptr;
                                    request.shaderBytes=lightingShaderBytes();
                                    request.shaderSize=lightingShaderSize();
                                }
#endif
                                char error[512]{};
#ifdef VRAD3_STATIC_GPU
                                if(hammerMode && resident==3) {
                                    const auto token=gpuServer::lightmapToken(inputs,arguments.lights);
                                    request.quickToken=token.data();
                                    std::string reason; int outcome=gpuServer::bakeForWorker(request,timing,reason);
                                    request.quickToken=nullptr;
                                    if(outcome==2) throw std::runtime_error(reason);
                                    directServed=outcome==1;
                                    if(!directServed && !reason.empty())
                                        std::cout << "GPU server unavailable; using in-worker GPU: " << reason << '\n' << std::flush;
                                }
#endif
                                if(!directServed && bakeGpu(&request,&timing,error,sizeof(error))) throw std::runtime_error(error);
                            }
                            if(hammerCapture) writeFile(outputPath/L"gpu-direct-working.bin",records.data(),records.size());
                            if(verifyItem) {
                                verification.baseline=copyArray(pointerAt(0x32ab20),countAt(0x32ab88),204,"verify_direct_baseline");
                                verification.outputBaseline=copyArray(pointerAt(0x32ab90),countAt(0x32ab88),140,"verify_output_baseline");
                                verification.direct=std::move(records);
                            } else {
                                if(lightLimitReportEnabled()) lightLimitReport(records,arguments.lights);
                                applyDirectResults(records,inputs.buffers[7],arguments.lights);
                            }
                            hammerPeakResources=std::max(hammerPeakResources,std::max(timing.peakResourceBytes,timing.peakLocalUsageBytes));
                            directComputed=true;
                            directGpuMs+=timing.lightingMs; hammerTiming.directMs+=timing.lightingMs;
                            directNote="slices="+std::to_string(timing.slices);
                            if(!hammerMode || hammerCapture) {
                                std::ofstream report(outputPath/L"native-direct-timing.json");
                                report << "{\"gpu_lighting_ms\":" << timing.lightingMs << ",\"gpu_host_ms\":" << timing.hostMs
                                       << ",\"slices\":" << timing.slices << ",\"live_stage_ms\":" << GetTickCount64()-directStarted
                                       << ",\"samples\":" << arguments.samples << ",\"lights\":" << arguments.lights << "}\n";
                            }
                            std::cout << "Live GPU directlight: " << timing.lightingMs << " ms GPU, "
                                      << GetTickCount64()-directStarted << " ms stage\n" << std::flush;
                            progressWindow.status(L"Valve CPU stages");
                        } catch(const std::exception& error) {
                            directNote=std::string("CPU: ")+error.what();
                            if(fileDiagnostics()) fs::create_directories(outputPath);
                            if(fileDiagnostics()) {
                                std::ofstream reason(outputPath/L"direct-fallback-reason.txt");
                                reason << error.what() << '\n';
                            }
                            std::cerr << "GPU directlight unavailable; continuing with CPU: " << error.what() << "\n" << std::flush;
                        }
                        progressWindow.end(directComputed);
                        hammerFallbackReason += (hammerFallbackReason.empty()?"":"; ")+std::string("directlight ")+(directComputed?"GPU ":"")+directNote;
                        pathtraceOnGpu=false;
#ifdef VRAD3_STATIC_GPU
                        if(hammerMode && !directServed) { GpuLightingDiagnostics local; GetGpuLightingDiagnostics(&local); if(local.adapter[0]) hammerHardware=local; }
#endif
                        if(directComputed && !verifyItem) {
                            ++directGpuBlocks; ++hammerTiming.directGpuBlocks;
                            hammerTiming.stageMs+=static_cast<double>(GetTickCount64()-directStarted);
                            context.Rip=readValue<uintptr_t>(context.Rsp); context.Rsp+=sizeof(uintptr_t);
                            directRun.arm(hit);
                            profileChildBoundary("wrapper_direct_done",childLaunched);
                        } else {
                            ++directCpuBlocks; ++hammerTiming.directCpuBlocks;
                            directLeave.arm(readValue<uintptr_t>(context.Rsp));
                        }
                    }
                    if(!SetThreadContext(thread,&context)) fail("Set directlight context");
                    CloseHandle(thread);
                } else if (exception.ExceptionCode == EXCEPTION_BREAKPOINT && ((entry.armed && hit == entry.location) || (leave.armed && hit == leave.location) || (rayResult.armed && hit == rayResult.location))) {
                    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, event.dwThreadId);
                    if (!thread) fail("OpenThread");
                    CONTEXT context{}; context.ContextFlags = CONTEXT_FULL;
                    if (!GetThreadContext(thread, &context)) fail("GetThreadContext");
                    if (entry.armed && hit == entry.location) {
                        entry.restore(); context.Rip = hit;
                        before = true;
                        if(nativeMode) {
                            outputPath=hammerMode?runPath:runPath/(L"block-"+std::to_wstring(stages));
                            if(fileDiagnostics()) fs::create_directories(outputPath);
                        }
                        changePathtraceSeed(!nativeMode && !gpuReplay);
                        isolatePathtrace();
                        applyEmissiveMaterials(executable,workdir);
                        if(nativeMode) {
                            profileChildBoundary("wrapper_gpu",childLaunched);
                            ULONGLONG stageStarted=GetTickCount64();
                            outputPath=hammerMode?runPath:runPath/(L"block-"+std::to_wstring(stages));
                            ++stages;
                            if(fileDiagnostics()) fs::create_directories(outputPath);
                            bool computed=false;
                            bool serverComputed=false;
                            std::vector<uint8_t> records;
                            NativeLighting inputs;
                            GpuLightingTiming timing{};
                            try {
                                progressWindow.status(L"GPU path tracing");
                                uint32_t spp=nativeSamples?nativeSamples:scriptMaxSamples(context);
                                if(nativeSamples) pathtraceMinSamples=0; else scriptPathtraceArguments(context);
                                if(cs2SamplesEnabled() && !nativeSamples) {
                                    spp=std::min<uint32_t>(4096,spp*4);
                                    pathtraceMinSamples=std::min<uint32_t>(spp,pathtraceMinSamples*2);
                                }
                                {
                                    wchar_t text[96];
                                    if(pathtraceMinSamples && pathtraceMinSamples<spp)
                                        swprintf_s(text,L"Path Trace on GPU (%uspp - %uspp - %.2f%% errtol)... ",pathtraceMinSamples,spp,pathtraceTolerance*100);
                                    else swprintf_s(text,L"Path Trace on GPU (%uspp)... ",spp);
                                    progressWindow.begin(text);
                                }
                                int resident=0; (void)resident;
#ifdef VRAD3_STATIC_GPU
                                if(hammerMode) {
                                    std::string reason; resident=gpuServer::bakeResident(spp,0,inputs,records,timing,reason);
                                    if(resident==2) throw std::runtime_error(reason);
                                    serverComputed=resident==1;
                                    if(resident==0 && !reason.empty())
                                        std::cout << "GPU server unavailable; using in-worker GPU: " << reason << '\n' << std::flush;
                                }
#endif
                                if(!serverComputed) {
                                    try { BlockProfileScope profile("prepare_native"); inputs=prepareNative(spp); }
                                    catch(...) { if(!hammerMode) surveyNative(spp); throw; }
                                    requirePhysicalMemory(static_cast<uint64_t>(inputs.constants[0])*204,"working records");
                                    records.resize(static_cast<size_t>(inputs.constants[0])*204);
                                    std::wstring shader=fs::absolute(argv[6]).wstring();
                                    GpuLightingRequest request{}; request.version=5;
                                    memcpy(request.constants,inputs.constants,sizeof(inputs.constants));
                                    for(size_t i=0;i<inputs.buffers.size();++i)
                                        request.buffers[i]={inputs.buffers[i].data(),inputs.buffers[i].size()};
                                    request.shaderPath=shader.c_str(); request.output=records.data(); request.outputBytes=records.size();
                                    request.readSlice=readNativeSlice; request.sliceContext=&inputs; request.totalPaths=inputs.totalPaths;
                                    request.adaptiveMin=inputs.adaptiveMin; request.blockWidth=inputs.width; request.adaptiveTol=inputs.adaptiveTol;
                                    request.progress=progressWindow.progress();
#ifdef VRAD3_STATIC_GPU
                                    if(hammerMode) {
                                        request.shaderPath=nullptr;
                                        request.shaderBytes=lightingShaderBytes();
                                        request.shaderSize=lightingShaderSize();
                                    }
#endif
                                    char error[512]{};
#ifdef VRAD3_STATIC_GPU
                                    if(hammerMode && resident==3) {
                                        const auto token=gpuServer::lightmapToken(inputs,0);
                                        request.quickToken=token.data();
                                        std::string reason; int outcome=gpuServer::bakeForWorker(request,timing,reason);
                                        request.quickToken=nullptr;
                                        if(outcome==2) throw std::runtime_error(reason);
                                        serverComputed=outcome==1;
                                        if(!serverComputed && !reason.empty())
                                            std::cout << "GPU server unavailable; using in-worker GPU: " << reason << '\n' << std::flush;
                                    }
#endif
                                    if(!serverComputed && bakeGpu(&request,&timing,error,sizeof(error))) throw std::runtime_error(error);
                                }
                                validateWorking(records,inputs.buffers[7]);
                                computed=true;
                                hammerFallbackReason="slices="+std::to_string(timing.slices)+(serverComputed?" server":"");
                            } catch(const std::exception& error) {
                                hammerFallbackReason=error.what();
                                if(fileDiagnostics()) {
                                    std::ofstream reason(outputPath/L"fallback-reason.txt");
                                    reason << error.what() << '\n';
                                }
                                records.clear(); inputs=NativeLighting{};
                                std::cerr << "GPU stage unavailable; continuing with CPU: " << error.what()
                                          << " threads=" << usableCpuCount() << "\n" << std::flush;
                            }
                            progressWindow.end(computed);
#ifdef VRAD3_STATIC_GPU
                            if(hammerMode && !serverComputed) { GpuLightingDiagnostics local; GetGpuLightingDiagnostics(&local); if(local.adapter[0]) hammerHardware=local; }
#else
                            if(hammerMode && gpuLibrary) {
                                auto diagnostics=reinterpret_cast<void (*)(GpuLightingDiagnostics*)>(GetProcAddress(gpuLibrary,"GetGpuLightingDiagnostics"));
                                if(diagnostics) diagnostics(&hammerHardware);
                            }
#endif
                            if(computed) {
                                hammerPeakResources=std::max(hammerPeakResources,std::max(timing.peakResourceBytes,timing.peakLocalUsageBytes));
                                if(verifyItem) verifyCorrupt(records,204,{0,4,8});
                                validateAndWriteWorking(records,inputs.buffers[7],true,!verifyItem,true);
                                if(!hammerMode || hammerCapture) writeFile(outputPath/L"gpu-working.bin",records.data(),records.size());
                                if(fileDiagnostics()) {
                                std::ofstream report(outputPath/L"native-timing.json");
                                report << "{\"gpu_lighting_ms\":" << timing.lightingMs << ",\"gpu_host_ms\":" << timing.hostMs
                                       << ",\"slices\":" << timing.slices << ",\"peak_resource_bytes\":" << timing.peakResourceBytes << ",\"peak_local_usage_bytes\":" << timing.peakLocalUsageBytes
                                       << ",\"live_stage_ms\":" << GetTickCount64()-stageStarted << ",\"samples\":" << inputs.constants[1] << "}\n";
                                if(!report) throw std::runtime_error("Native timing write failed");
                                }
                                if(verifyItem) {
                                    verification.path=std::move(records);
                                    // To let Valve's CPU stage run and still regain control after it, a breakpoint is armed on its return
                                    // address (top of stack).
                                    leave.arm(readValue<uintptr_t>(context.Rsp)); ++fallbackBlocks;
                                } else {
                                    context.Rip=readValue<uintptr_t>(context.Rsp); context.Rsp+=sizeof(uintptr_t);
                                    after=true; ++gpuBlocks; entry.arm(hit);
                                }
                                pathtraceOnGpu=true;
                                hammerTiming.dispatchMs+=timing.lightingMs;
                                hammerTiming.hostMs+=timing.hostMs;
                                hammerTiming.stageMs+=static_cast<double>(GetTickCount64()-stageStarted);
                                profileChildBoundary("wrapper_gpu_done",childLaunched);
                                std::cout << "Live GPU stage " << stages << ": " << timing.lightingMs << " ms GPU\n" << std::flush;
                                progressWindow.status(L"Valve CPU stages");
                            } else {
                                if(hammerMode && hammerAbortCpuStage && !emptyBlock(hammerFallbackReason)) throw std::runtime_error("GPU stage unavailable: "+hammerFallbackReason);
                                ++fallbackBlocks; pathtraceOnGpu=false;
                                if(verifyItem) verifyUnavailable(hammerFallbackReason);
                                leave.arm(readValue<uintptr_t>(context.Rsp));
                            }
                        } else if (gpuReplay) {
                            capture(false);
                            injectWorking(fs::absolute(argv[4]), fs::absolute(argv[5]));
                            context.Rip = readValue<uintptr_t>(context.Rsp);
                            context.Rsp += sizeof(uintptr_t);
                            capture(true); after = true;
                            std::cout << "Injected GPU working records; skipped CPU pathtrace\n" << std::flush;
                        } else {
                        capture(false);
                        // RVA 91524 is just after the Embree ray call; RSI is the completed ray queue (count at +0, root at +0x5818).
                        rayResult.arm(address(0x91524));
                        leave.arm(readValue<uintptr_t>(context.Rsp));
                        std::cout << "Captured prepared inputs\n" << std::flush;
                        }
                    } else if (rayResult.armed && hit == rayResult.location) {
                        rayResult.restore(); context.Rip = hit;
                        uint32_t rayCount = readValue<uint32_t>(context.Rsi);
                        if (!rayCount || rayCount > 256) throw std::runtime_error("Unexpected completed ray queue count");
                        uintptr_t root = readValue<uintptr_t>(context.Rsi + 0x5818);
                        uintptr_t scene = pointerAt(0x32aa20);
                        uintptr_t firstMesh = readValue<uintptr_t>(readValue<uintptr_t>(scene + 0x188));
                        if (root != firstMesh) throw std::runtime_error("Ray oracle requires the exported root mesh");
                        dump(context.Rsi + 0x10, rayCount, 0x50, outputPath / L"ray-oracle.bin");
                        std::ofstream json(outputPath / L"ray-oracle.json");
                        json << "{\"rays\":" << rayCount << ",\"mask\":" << readValue<uint32_t>(context.Rsi + 0x5820)
                             << ",\"cull\":" << readValue<uint32_t>(context.Rsi + 0x5824)
                             << ",\"mesh_flags\":" << readValue<uint32_t>(root + 0xa0)
                             << ",\"upper_bound_before_trace_saved\":false}\n";
                        if (!json) throw std::runtime_error("Failed to write ray oracle metadata");
                        raysCaptured = true;
                        std::cout << "Captured " << rayCount << " actual Embree ray results\n" << std::flush;
                    } else {
                        leave.restore(); context.Rip = hit;
                        if(nativeMode) {
                            if(verifyItem && !verification.path.empty()) {
                                try { verification.comparePath(); if(!directGpu) verification.finish(); }
                                catch(const std::exception& error) { verifyUnavailable(error.what()); }
                            }
                            if(!hammerMode || hammerCapture) dump(pointerAt(0x32ab20),countAt(0x32ab88),204,outputPath/L"cpu-fallback-working.bin");
                            if(fileDiagnostics()) {
                                std::ofstream fallback(outputPath/L"cpu-fallback.json"); fallback << "{\"cpu_pathtrace_skipped\":false}\n";
                            }
                            entry.arm(address(0xca90));
                        } else capture(true);
                        restorePathtraceSeed();
                        after = true;
                        std::cout << "Captured pathtrace result\n" << std::flush;
                    }
                    if (!SetThreadContext(thread, &context)) fail("SetThreadContext");
                    CloseHandle(thread);
                } else if (exception.ExceptionCode != EXCEPTION_BREAKPOINT) {
                    disposition = DBG_EXCEPTION_NOT_HANDLED;
                    if (!event.u.Exception.dwFirstChance)
                        std::cerr << "Unhandled exception 0x" << std::hex << exception.ExceptionCode << std::dec << "\n";
                }
            } else if (event.dwDebugEventCode == EXIT_THREAD_DEBUG_EVENT) {
            } else if (event.dwDebugEventCode == CREATE_THREAD_DEBUG_EVENT) {
            } else if (event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
                profileChildBoundary("child_exit",childLaunched);
                exitCode = event.u.ExitProcess.dwExitCode;
                progressWindow.childExited();
                childExited=true; childExitCode=exitCode; hammerChildExit=static_cast<int>(exitCode);
                if(!ContinueDebugEvent(event.dwProcessId, event.dwThreadId, disposition)) fail("Continue child exit");
                break;
            }
            if (!ContinueDebugEvent(event.dwProcessId, event.dwThreadId, disposition)) fail("ContinueDebugEvent");
        }
        if(exitCode!=0) std::cerr << "Child failed with exit code " << exitCode << "; see vrad3.log\n";
        else if (!before || !after || (!gpuReplay && !nativeMode && !raysCaptured)) throw std::runtime_error("Capture breakpoints were not reached; see vrad3.log");
        if(nativeMode && fileDiagnostics()) {
            std::ofstream report(runPath/L"native-run.json");
            report << "{\"process_total_ms\":" << GetTickCount64()-started << ",\"gpu_blocks\":" << gpuBlocks
                   << ",\"cpu_fallback_blocks\":" << fallbackBlocks << ",\"direct_gpu_blocks\":" << directGpuBlocks
                   << ",\"direct_cpu_blocks\":" << directCpuBlocks << ",\"direct_gpu_ms\":" << directGpuMs
                   << ",\"cpu_capture_prerequisite\":false,\"quality_validated\":false}\n";
            if(!report) throw std::runtime_error("Native run report failed");
        }
        CloseHandle(info.hThread); CloseHandle(info.hProcess); if(log!=INVALID_HANDLE_VALUE) CloseHandle(log);
        info={}; log=INVALID_HANDLE_VALUE;
        hammerTiming.gpuBlocks=gpuBlocks; hammerTiming.cpuBlocks=fallbackBlocks;
        hammerTiming.processMs=GetTickCount64()-started;
        if(!keepGpuSession) cleanupGpu(shutdownGpu);
        return static_cast<int>(exitCode);
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        hammerFallbackReason=error.what();
        if (info.hProcess) {
            if(!childExited) { TerminateProcess(info.hProcess, 1); DebugActiveProcessStop(info.dwProcessId); WaitForSingleObject(info.hProcess,30000); }
            CloseHandle(info.hProcess); CloseHandle(info.hThread);
        }
        if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
        cleanupGpu(shutdownGpu);
        return childExited && childExitCode ? static_cast<int>(childExitCode) : 1;
    }
}

static std::wstring quoteArgument(const std::wstring& value) {
    std::wstring result=L"\"";
    size_t slashes=0;
    for(wchar_t ch:value) {
        if(ch==L'\\') { ++slashes; continue; }
        result.append(ch==L'"'?slashes*2+1:slashes,L'\\'); slashes=0;
        result+=ch;
    }
    result.append(slashes*2,L'\\'); result+=L'"'; return result;
}
static std::wstring environmentValue(const wchar_t* name) {
    DWORD size=GetEnvironmentVariableW(name,nullptr,0);
    if(!size) return {};
    std::wstring value(size,L'\0');
    DWORD length=GetEnvironmentVariableW(name,value.data(),size);
    if(!length || length>=size) throw std::runtime_error("Environment changed during read");
    value.resize(length); return value;
}
static int launchCpu(const fs::path& executable,std::wstring command) {
    STARTUPINFOW startup{}; startup.cb=sizeof(startup); startup.dwFlags=STARTF_USESTDHANDLES;
    startup.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE); startup.hStdError=GetStdHandle(STD_ERROR_HANDLE); startup.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION child{};
    DWORD flags=CREATE_NO_WINDOW|childPriorityFlag();
    progressWindow.launch(startup,flags);
    if(!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,flags,nullptr,nullptr,&startup,&child)) fail("Create CPU fallback");
    CloseHandle(child.hThread);
    WaitForSingleObject(child.hProcess,INFINITE);
    progressWindow.childExited();
    DWORD code=1; GetExitCodeProcess(child.hProcess,&code); CloseHandle(child.hProcess);
    return static_cast<int>(code);
}
static std::wstring argumentValue(const std::vector<std::wstring>& args,const std::wstring& name) {
    std::wstring value;
    for(size_t i=0;i<args.size();++i) {
        if(args[i]==name && i+1<args.size()) value=args[i+1];
        else if(args[i].rfind(name+L"=",0)==0) value=args[i].substr(name.size()+1);
    }
    return value;
}
static fs::path blockStatusPath(const fs::path& block) {
    auto absolute=fs::absolute(block).lexically_normal();
    auto path=absolute.wstring();
    if(path.rfind(L"\\\\?\\",0)!=0) path=path.rfind(L"\\\\",0)==0?L"\\\\?\\UNC\\"+path.substr(2):L"\\\\?\\"+path;
    absolute=path;
    if(absolute.wstring().rfind(compileTempRoot().wstring()+L"\\",0)==0) {
        absolute+=L".gpu-status"; return absolute;
    }
    uint64_t identity=14695981039346656037ull;
    for(wchar_t c:absolute.parent_path().wstring()) { identity^=towlower(c); identity*=1099511628211ull; }
    std::wostringstream suffix; suffix<<std::hex<<identity;
    return compileTempRoot()/L"status"/(suffix.str()+L"-"+block.filename().wstring()+L".gpu-status");
}
static void writeHammerStatus(const std::vector<std::wstring>& args,const HammerTiming& timing,std::string reason) {
    try {
        std::wstring filename=argumentValue(args,L"-blockfile");
        if(filename.empty()) return;
        fs::path block=fs::path(argumentValue(args,L"-outdir"))/filename;
        if(!fs::is_regular_file(block)) return;
        fs::path status=blockStatusPath(block),temporary=status;
        fs::create_directories(status.parent_path());
        temporary+=L".tmp-"+std::to_wstring(GetCurrentProcessId());
        std::ofstream report(temporary);
        report << "VRAD3_GPU_STATUS_1\n" << blockSha256(block) << "\n"
               << timing.gpuBlocks << ' ' << timing.cpuBlocks << ' ' << std::setprecision(12)
               << timing.dispatchMs << ' ' << timing.hostMs << ' ' << timing.stageMs << ' ' << timing.processMs
               << ' ' << timing.directGpuBlocks << ' ' << timing.directCpuBlocks << ' ' << timing.directMs << '\n';
        if(!verifyLine.empty()) report<<verifyLine<<'\n';
        for(char& c:reason) if(c=='\r' || c=='\n') c=' ';
        if(!reason.empty()) report<<"reason="<<reason<<'\n';
        char adapter[512]{};
        if(hammerHardware.adapter[0] && WideCharToMultiByte(CP_UTF8,0,hammerHardware.adapter,-1,adapter,sizeof(adapter),nullptr,nullptr))
            report<<"adapter="<<adapter<<'\n';
        if(!hammerEmissive.empty()) report<<"emissive="<<hammerEmissive<<'\n';
        if(!hammerRetry.empty()) {
            std::string retry=hammerRetry; for(char& c:retry) if(c=='\r' || c=='\n') c=' ';
            report<<"gpu_retry="<<retry<<'\n';
        }
        report.close();
        if(!report || !MoveFileExW(temporary.c_str(),status.c_str(),MOVEFILE_REPLACE_EXISTING))
            throw std::runtime_error("Could not publish block GPU status");
    } catch(const std::exception& error) {
        std::cerr << "GPU status unavailable: " << error.what() << '\n';
    }
}
#include "user_logs.h"
#include "prefetch.h"
#include "gpu_server.h"
#include "probe_worker.h"
#include "lpv_voxels.h"
#include "preview_worker.h"
#include "cs2_fireflies.h"

static void printHammerSummary(const std::vector<std::wstring>& args) {
    try {
        fs::path script=argumentValue(args,L"-script");
        std::ifstream input(script);
        if(!input) throw std::runtime_error("Cannot read combine script");
        bool combine=false; std::string line;
        std::set<std::string> names;
        while(std::getline(input,line)) {
            std::istringstream tokens(line); std::string command,name;
            if(!(tokens>>command)) continue;
            if(command.back()==':') { combine=command=="lm_combine:"; continue; }
            if(combine && command=="lightmap_read_block" && (tokens>>name)) names.insert(name);
        }
        if(names.empty()) throw std::runtime_error("No block list in combine script");
        fs::path folder=argumentValue(args,L"-indir");
        uint32_t gpu=0,cpu=0,mixed=0,unknown=0,directBlocks=0,directCpu=0,prefetched=0; double dispatch=0,directMs=0;
        std::map<std::string,std::pair<uint32_t,std::string>> cpuReasons;
        std::map<std::string,uint32_t> adapters;
        uint32_t emissiveBlocks=0,emissiveGlowing=0,emissiveUnreadable=0,emissiveMissing=0; std::string emissiveFirst;
        uint32_t cpuDark=0,cpuDim=0,retried=0; std::string cpuFirst,retryFirst,retryReason;
        for(const auto& name:names) {
            fs::path block=folder/fs::path(name);
            std::ifstream report(blockStatusPath(block));
            std::string version,hash; HammerTiming timing;
            if(!(report>>version>>hash>>timing.gpuBlocks>>timing.cpuBlocks>>timing.dispatchMs
                       >>timing.hostMs>>timing.stageMs>>timing.processMs)) { ++unknown; continue; }
            if(!(report>>timing.directGpuBlocks>>timing.directCpuBlocks>>timing.directMs)) {
                timing.directGpuBlocks=timing.directCpuBlocks=0; timing.directMs=0;
            }
            if(
               version!="VRAD3_GPU_STATUS_1" || (timing.gpuBlocks==0 && timing.cpuBlocks==0) ||
               !std::isfinite(timing.dispatchMs) || timing.dispatchMs<0 || hash!=blockSha256(block)) {
                ++unknown; continue;
            }
            if(timing.gpuBlocks && timing.cpuBlocks) ++mixed;
            else if(timing.gpuBlocks) ++gpu;
            else ++cpu;
            std::string extra; bool glowLine=false,verified=false,empty=false;
            while(std::getline(report,extra)) {
                if(extra=="prefetched=1") ++prefetched;
                if(extra.rfind("reason=",0)==0 && emptyBlock(extra.substr(7))) empty=true;
                if(extra.rfind("gpu_retry=",0)==0 && !retried++) { retryFirst=name; retryReason=extra.substr(10); }
                if(extra.rfind("adapter=",0)==0) ++adapters[extra.substr(8)];
                if(extra.rfind("emissive=",0)==0) {
                    std::istringstream fields(extra.substr(9)); uint32_t glowing=0,unreadable=0,missing=0; std::string first;
                    if(fields>>glowing>>unreadable>>missing>>first) {
                        ++emissiveBlocks; glowLine=true; emissiveUnreadable=std::max(emissiveUnreadable,unreadable); emissiveMissing=std::max(emissiveMissing,missing);
                        if(glowing>emissiveGlowing) { emissiveGlowing=glowing; emissiveFirst=first; }
                    }
                }
                if(extra.rfind("reason=",0)==0 && (timing.cpuBlocks || (timing.directCpuBlocks && extra.find("cheaper than GPU")==std::string::npos))) {
                    auto& reason=cpuReasons[extra.substr(7)];
                    if(!reason.first++) reason.second=name;
                }
                if(extra.rfind("VRAD3 GPU verify:",0)==0) {
                    verified=true; std::cout<<extra<<'\n';
                    if(!userLogRoot.empty()) { UserLogLock lock(userLogRoot); std::ofstream(userLogRoot/L"vrad3-gpu.log",std::ios::app)<<extra<<'\n'; }
                }
            }
            if(timing.cpuBlocks && !verified && !empty) { if(!cpuDark && !cpuDim) cpuFirst=name; ++(glowLine?cpuDim:cpuDark); }
            dispatch+=timing.dispatchMs; directBlocks+=timing.directGpuBlocks; directCpu+=timing.directCpuBlocks; directMs+=timing.directMs;
        }
        std::cout << "VRAD3 GPU summary: " << gpu << '/' << names.size() << " blocks GPU, " << cpu
                  << " CPU, " << mixed << " mixed, " << unknown << " unverified; GPU tracing "
                  << dispatch << " ms total; directlight GPU " << directBlocks << ", CPU " << directCpu << " (" << directMs
                  << " ms GPU); prefetched " << prefetched << ". CPU preparation and denoising remain.";
        if(!userLogRoot.empty()) std::cout<<" Logs: "<<logUtf8((userLogRoot/L"vrad3-gpu.log").wstring());
        std::cout<<"\n";
        size_t shown=0;
        for(const auto& [reason,blocks]:cpuReasons) {
            if(++shown>5) { std::cout<<"VRAD3 GPU: "<<cpuReasons.size()-5<<" more CPU reasons in the logs\n"; break; }
            std::cout<<"VRAD3 GPU: CPU used for "<<blocks.first<<" block(s) (first "<<blocks.second<<"): "<<reason<<'\n';
        }
        for(const auto& [adapter,blocks]:adapters) std::cout<<"VRAD3 GPU: graphics card: "<<adapter<<" ("<<blocks<<" block(s))\n";
        if(emissiveBlocks) {
            std::cout<<"VRAD3 GPU: emissive = on: "<<emissiveGlowing<<" glowing material(s)";
            if(emissiveFirst!="-") std::cout<<" (one is "<<emissiveFirst<<")";
            if(emissiveUnreadable) std::cout<<", "<<emissiveUnreadable<<" self-illum material(s) unreadable (old format or missing texture), no glow";
            if(emissiveMissing) std::cout<<", "<<emissiveMissing<<" material file(s) not found";
            std::cout<<"\n";
        }
        if(emissiveSetting()>0) {
            if(retried) std::cout<<"VRAD3 GPU: emissive = on: "<<retried<<" block(s) left the GPU and were tried on it again (first "<<retryFirst<<"): "<<retryReason<<'\n';
            if(cpuDark || cpuDim) {
                std::cout<<"VRAD3 GPU: WARNING: emissive = on, but "<<cpuDark+cpuDim<<" block(s) were baked by Valve's CPU (first "<<cpuFirst<<"):";
                if(cpuDark) std::cout<<' '<<cpuDark<<" without the glow"<<(cpuDim?",":"");
                if(cpuDim) std::cout<<' '<<cpuDim<<" with dimmer, untinted glow";
                std::cout<<". emissive_fallback = stop fails the compile instead.\n";
            }
        }
        for(const auto& warning:settingWarnings) std::cout<<"VRAD3 GPU: vrad3_gpu.ini: "<<logUtf8(warning)<<'\n';
        std::cout<<std::flush;
    } catch(const std::exception& error) {
        std::cout << "VRAD3 GPU summary unavailable: " << error.what() << '\n' << std::flush;
    }
}
static int runHammer(int argc,wchar_t** argv) {
    UserWorkerLog worker;
    faultSummaryCallback=&summarizeWorkerFault;
    try {
        std::wstring module(32768,L'\0');
        DWORD length=GetModuleFileNameW(nullptr,module.data(),static_cast<DWORD>(module.size()));
        if(!length || length>=module.size()) fail("Read wrapper module path");
        module.resize(length); fs::path directory=fs::path(module).parent_path();
        std::wstring realOverride=environmentValue(L"VRAD3_REAL_EXE");
        fs::path real=realOverride.empty()?directory/L"vrad3_real.exe":fs::path(realOverride);
        if(!fs::is_regular_file(real))
            throw std::runtime_error("vrad3_real.exe is missing. Rename Valve's original vrad3.exe to vrad3_real.exe beside this wrapper (or set VRAD3_REAL_EXE to the original executable).");
        if(fs::equivalent(real,fs::path(module))) throw std::runtime_error("Wrapper cannot launch itself");
        bool enabled=environmentValue(L"VRAD3_GPU_ENABLED")!=L"0",forcedCpu=false;
        uint32_t samples=0; std::wstring entrypoint;
        std::vector<std::wstring> original(argv+1,argv+argc);
        std::vector<std::wstring> forwarded;
        for(int i=1;i<argc;++i) {
            std::wstring arg=argv[i];
            if(arg==L"-gpu") { enabled=true; continue; }
            if(arg==L"-gpu-cpu") { forcedCpu=true; continue; }
            if(arg==L"-gpu-samples") {
                if(++i==argc) throw std::runtime_error("-gpu-samples requires a value");
                samples=static_cast<uint32_t>(std::stoul(argv[i]));
                if(!samples || samples>4096) throw std::runtime_error("-gpu-samples must be 1..4096");
                continue;
            }
            if(arg==L"-threads") {
                if(++i==argc) throw std::runtime_error("-threads requires a value");
                continue;
            }
            if(arg.rfind(L"-threads=",0)==0) continue;
            if(arg==L"-entrypoint" && i+1<argc) entrypoint=argv[i+1];
            if(arg.rfind(L"-entrypoint=",0)==0) entrypoint=arg.substr(12);
            forwarded.push_back(arg);
        }
        forwarded.push_back(L"-threads"); forwarded.push_back(threadCount());
        const bool emissiveOn=emissiveSetting()>0;
        hammerCommand=quoteArgument(fs::absolute(real).wstring());
        for(const auto& arg:forwarded) hammerCommand+=L" "+quoteArgument(arg);
        if(ProgressWindow::enabled()) {
            if(entrypoint==L"lm_compute") progressWindow.title=L"vrad3 GPU: block "+argumentValue(forwarded,L"-blocknum");
            else if(entrypoint==L"lpv_compute") progressWindow.title=L"vrad3 GPU: probe volume "+argumentValue(forwarded,L"-init");
        }
        bool probeCoordinator=false;
#ifdef VRAD3_STATIC_GPU
        const bool probeWorker=entrypoint==L"lpv_compute";
        ProbeSetting probes=ProbeSetting::Auto;
        if(probeWorker || entrypoint==L"lpv_finish_all") probes=probeSetting();
        bool probeGpu=false;
        probeCoordinator=probeWorker && !prefetchBake && enabled && !forcedCpu && probes!=ProbeSetting::Off &&
                         environmentValue(L"VRAD3_GPU_PREFETCH")!=L"0" && !verifyEnabled();
        if(probeWorker && enabled && !forcedCpu && probes!=ProbeSetting::Off) {
            auto cellsOf=[&](const wchar_t* start,const wchar_t* end) {
                try { return std::stol(argumentValue(forwarded,end))-std::stol(argumentValue(forwarded,start)); } catch(...) { return 0l; }
            };
            uint64_t cells=1ull*std::max(0l,cellsOf(L"-startx",L"-endx"))*std::max(0l,cellsOf(L"-starty",L"-endy"))*std::max(0l,cellsOf(L"-startz",L"-endz"));
            uint64_t maxSamples=0; try { maxSamples=std::stoull(argumentValue(forwarded,L"-maxspp")); } catch(...) {}
            probeGpu=probes==ProbeSetting::On || emissiveOn || cells*maxSamples>=probeMinimumPaths();
        }
        if(probeWorker) { if(!prefetchBake && (probeGpu || probeCoordinator)) prefetch::prepareCompile(real,true); }
        else
#endif
        if(!prefetchBake && (entrypoint==L"lm_compute" || entrypoint==L"lm_combine")) prefetch::prepareCompile(real);
        try { worker.start(forwarded); }
        catch(const std::exception& error) {
            std::cerr << "User log unavailable: " << error.what() << '\n';
            worker.directory.clear();
        }
        if(verifyEnabled() && !forcedCpu && enabled) {
            if(entrypoint==L"lm_compute") {
                try {
                    auto plan=prefetch::discover(original,false);
                    auto id=prefetch::number(argumentValue(forwarded,L"-blocknum"));
                    size_t n=std::min<size_t>(verifyLimit(false),plan.blocks.size());
                    for(size_t i=0;i<n;++i) if(id==i*(plan.blocks.size()-1)/std::max<size_t>(1,n-1)) verifyItem=true;
                    verifyLabel="block "+std::to_string(id);
                } catch(const std::exception& error) { std::cerr<<"GPU verify selection unavailable: "<<error.what()<<'\n'; }
            }
#ifdef VRAD3_STATIC_GPU
            if(probeWorker && probeGpu) {
                std::ifstream commands(L"lightprobevolumes.cmd"); std::string line; std::set<std::wstring> names;
                while(std::getline(commands,line)) {
                    auto tokens=launchTokens(prefetch::wide(line));
                    auto name=argumentValue(tokens,L"-init"); if(!name.empty()) names.insert(name);
                }
                auto target=argumentValue(forwarded,L"-init");
                if(names.empty()) names.insert(target);
                size_t n=std::min<size_t>(verifyLimit(true),names.size()),i=0;
                for(const auto& name:names) { if(name==target && i<n) verifyItem=true; ++i; }
                verifyLabel="volume "+logUtf8(argumentValue(forwarded,L"-indirectfile"));
            }
#endif
        }
        prefetch::Foreground foreground;
        if(prefetchBake) worker.prefetchDecision="on";
        else if(entrypoint==L"lm_compute") foreground.start(original,real,!forcedCpu && enabled,worker);
        else if(probeCoordinator) foreground.start(original,real,true,worker,true);
        if(foreground.served) { worker.result="prefetched"; worker.exitCode=0; return 0; }
        auto probeFallback=[&]() {
            return !(worker.result=="GPU probes (server)" || worker.result=="CPU verified" ||
                     (worker.result=="CPU stage" && worker.reason.rfind("small volume",0)==0));
        };
        auto finish=[&](int result) {
            foreground.finish(forwarded,result,worker,probeFallback());
            if(const auto file=environmentValue(L"VRAD3_GPU_PREVIEW_REPORT"); !file.empty() && entrypoint!=L"lm_compute") {
                std::ostringstream line;
                line<<"result="<<(worker.result.empty()?"Valve CPU":worker.result)<<" exit="<<result;
#ifdef VRAD3_STATIC_GPU
                line<<" triangles: gpu_calls="<<previewReport.gpuCalls<<" cpu_calls="<<previewReport.cpuCalls<<" gpu_triangles="<<previewReport.gpuTriangles
                    <<" split="<<previewReport.splitTriangles<<" into "<<previewReport.subTriangles<<" (edge "<<previewReport.splitEdge<<")"
                    <<" probes: gpu="<<previewReport.probeGpu<<" cpu="<<previewReport.probeCpu<<" voxels="<<previewReport.voxelVolumes
                    <<" gpu_ms="<<static_cast<uint64_t>(previewReport.gpuMs)<<" triangle_stage_ms="<<static_cast<uint64_t>(previewReport.stageMs);
#endif
                if(!hammerEmissive.empty()) line<<" emissive(glowing unreadable missing first)="<<hammerEmissive;
                std::string reason=worker.reason; for(char& c:reason) if(c=='\r' || c=='\n') c=' ';
                if(!reason.empty()) line<<" reason="<<reason;
                std::ofstream(fs::path(file),std::ios::trunc)<<line.str()<<'\n';
            }
            if(prefetchBake && result==0) {
                try {
                    std::ostringstream metrics; metrics<<worker.hardware.budget<<' '<<hammerPeakResources<<' '
                        <<(entrypoint==L"lpv_compute"?probeFallback():!(worker.result=="GPU" || worker.result=="CPU verified"))<<' '<<GetTickCount64()-worker.started<<' '
                        <<worker.hardware.adapterLuidLow<<' '<<worker.hardware.adapterLuidHigh<<'\n';
                    prefetch::atomicText(fs::path(argumentValue(forwarded,L"-outdir"))/L"metrics.txt",metrics.str());
                } catch(const std::exception& error) { std::cerr<<"Prefetch metrics unavailable: "<<error.what()<<'\n'; }
            }
            return result;
        };
        auto cpuRun=[&]() {
            ULONGLONG started=GetTickCount64();
            std::wstring command=hammerCommand;
            if(entrypoint==L"lm_combine" && enabled) {
                if(const auto script=cs2Fireflies::prepare(forwarded); !script.empty()) {
                    command=quoteArgument(fs::absolute(real).wstring());
                    for(size_t i=0;i<forwarded.size();++i) {
                        const bool value=i>0 && forwarded[i-1]==L"-script";
                        command+=L" "+quoteArgument(value?script:forwarded[i]);
                    }
                }
            }
            int result=launchCpu(real,command);
            worker.exitCode=result;
            if(result==0 && entrypoint==L"lm_compute") {
                HammerTiming timing; timing.cpuBlocks=1; timing.processMs=GetTickCount64()-started;
                writeHammerStatus(forwarded,timing,worker.reason);
            }
#ifdef VRAD3_STATIC_GPU
            gpuServer::awaitShutdown();
#endif
            if(result==0 && entrypoint==L"lm_combine") prefetch::removeCompile(real);
#ifdef VRAD3_STATIC_GPU
            if(result==0 && entrypoint==L"lpv_finish_all") prefetch::removeProbeCompile(real);
#endif
            return finish(result);
        };
#ifdef VRAD3_STATIC_GPU
        if(entrypoint==L"lm_combine") gpuServer::requestShutdown();
        if(entrypoint==L"lpv_finish_all") {
            try {
                auto key=gpuServer::readCompileKey();
                if(!key.empty() && key==prefetch::wide(prefetch::key(real,true))) gpuServer::requestShutdown();
            } catch(...) {}
        }
#endif
        if(entrypoint==L"lm_combine") printHammerSummary(forwarded);
        if(entrypoint==L"lpv_finish_all" && fs::exists(compileTempRoot()/L"verify-probes")) {
            for(const auto& item:fs::directory_iterator(compileTempRoot()/L"verify-probes")) {
                std::ifstream status(item.path()); std::string file,hash,line;
                if(std::getline(status,file) && std::getline(status,hash) && std::getline(status,line) &&
                   fs::is_regular_file(fs::u8path(file)) && fileSha256(fs::u8path(file))==hash) {
                    std::cout<<line<<'\n';
                    if(!userLogRoot.empty()) { UserLogLock lock(userLogRoot); std::ofstream(userLogRoot/L"vrad3-gpu.log",std::ios::app)<<line<<'\n'; }
                }
            }
        }
#ifdef VRAD3_STATIC_GPU
        if(probeWorker && !forcedCpu && enabled && !probeGpu) {
            worker.result="CPU stage"; worker.reason=probes==ProbeSetting::Off?"light_probes=off":"small volume: CPU probes are cheaper than GPU setup (light_probes=on forces GPU)";
            return cpuRun();
        }
#else
        const bool probeWorker=false;
#endif
        auto cpuOrStop=[&]() {
            if(!emissiveStopsCpu() || (entrypoint!=L"lm_compute" && !probeWorker)) return cpuRun();
            std::string message="VRAD3 GPU: emissive = on and emissive_fallback = stop: this "+std::string(probeWorker?"probe volume":"block")+
                " would be baked by Valve's CPU ("+worker.reason+"), without the GPU glow; stopping the compile";
            std::cerr<<message<<'\n'<<std::flush; std::cout<<message<<'\n'<<std::flush;
            worker.result="stopped"; worker.reason="emissive_fallback = stop: "+worker.reason;
            return finish(1);
        };
#ifdef VRAD3_STATIC_GPU
        if(entrypoint!=L"lm_compute" && !probeWorker && entrypoint!=L"lm_combine" && entrypoint!=L"lpv_finish_all") {
            PreviewScript found; PreviewPlan plan;
            try { found=scanPreviewScript(forwarded,entrypoint); } catch(const std::exception&) {}
            if(enabled && !forcedCpu && (found.corners || found.probes)) {
                try {
                    const PreviewSetting preview=previewSetting();
                    if(preview!=PreviewSetting::Off) {
                        plan.samples=previewSamples(); plan.corners=previewCorners(); plan.subdivide=previewSubdivide();
                        plan.triangles=found.corners && (preview==PreviewSetting::On || found.corners/3*256>=previewMinimumPaths());
                        plan.smallTriangles=found.corners && !plan.triangles && (emissiveSetting()>0 || plan.subdivide>0);
                        if(found.probes) plan.probes=preview==PreviewSetting::On?ProbeSetting::On:ProbeSetting::Auto;
                    }
                } catch(const std::exception& error) {
                    plan=PreviewPlan{};
                    std::cerr << "GPU preview settings invalid; using CPU: " << error.what() << '\n';
                }
            }
            plan.voxels=found.voxels>0;
            plan.emissive=emissiveSetting()>0 && (found.corners || found.probes);
            if(plan.triangles || plan.smallTriangles || plan.probes!=ProbeSetting::Off || plan.voxels || plan.emissive) {
                try { verifyExecutable(real); }
                catch(const std::exception& error) {
                    worker.reason=error.what();
                    std::cerr << "GPU unsupported for this executable; using CPU: " << error.what() << "\n";
                    if(plan.voxels) { worker.result="stopped"; return finish(1); }
                    return cpuRun();
                }
                hammerMode=true;
                int result=runPreviewWorker(real,forwarded,plan);
                worker.hardware=hammerHardware; worker.traceMs=previewReport.gpuMs; worker.reason=previewReport.reason;
                worker.result=previewReport.gpuCalls || previewReport.probeGpu?"GPU preview":"CPU stage";
                if(previewReport.childExit<0) { if(plan.voxels) { worker.result="stopped"; return finish(1); } return cpuRun(); }
                worker.exitCode=previewReport.childExit;
                return finish(result);
            }
        }
#endif
        if(forcedCpu || !enabled || (entrypoint!=L"lm_compute" && !probeWorker)) {
            if(entrypoint==L"lm_compute") { worker.result="CPU forced"; worker.reason=forcedCpu?"-gpu-cpu":"GPU disabled"; }
            return cpuRun();
        }
        worker.result="CPU fallback";
#ifdef VRAD3_STATIC_GPU
        if(probeWorker) {
            try { verifyExecutable(real); }
            catch(const std::exception& error) {
                worker.reason=error.what();
                std::cerr << "GPU unsupported for this executable; using CPU: " << error.what() << "\n";
                return cpuOrStop();
            }
            hammerMode=true;
            try { gpuServer::configure(); }
            catch(const std::exception& error) { gpuServer::enabled=false; gpuServer::lastNote=std::string("off:")+error.what(); }
            worker.serverDecision=gpuServer::lastNote;
            int result=runProbeWorker(real,forwarded,probes);
            worker.exitCode=probeReport.childExit; worker.hardware=hammerHardware;
            worker.serverDecision=gpuServer::summary();
            worker.traceMs=probeReport.gpuMs; worker.reason=probeReport.reason;
            worker.result=verifyItem && !verifyLine.empty()?"CPU verified":probeReport.gpu?(probeReport.server?"GPU probes (server)":"GPU probes"):"CPU stage";
            if(result && worker.reason.empty()) worker.reason="GPU worker failed; see detailed logs";
            if(result==0 && emissiveOn && !probeReport.gpu && !verifyItem && emissiveStopsCpu()) return cpuOrStop();
            return finish(result);
        }
#endif
        fs::path library=directory/L"gpu_lighting.dll",shader=directory/L"lighting.dxil";
#ifndef VRAD3_STATIC_GPU
        if(!fs::exists(library) || !fs::exists(shader)) {
            worker.reason="GPU assets unavailable";
            std::cerr << "GPU assets unavailable; using CPU\n";
            return cpuOrStop();
        }
#endif
        try { verifyExecutable(real); }
        catch(const std::exception& error) {
            worker.reason=error.what();
            std::cerr << "GPU unsupported for this executable; using CPU: " << error.what() << "\n";
            return cpuOrStop();
        }
#ifndef VRAD3_STATIC_GPU
        HMODULE probe=LoadLibraryW(library.c_str());
        if(!probe || !GetProcAddress(probe,"BakeGpuLighting") || !GetProcAddress(probe,"ShutdownGpuLighting")) {
            worker.reason="GPU library unavailable";
            std::cerr << "GPU library unavailable; using CPU\n";
            return cpuOrStop();
        }
        FreeLibrary(probe);
#endif
        fs::path cache=worker.directory;
        if(cache.empty()) cache=compileTempRoot()/(L"run-"+std::to_wstring(GetCurrentProcessId()));
        std::vector<std::wstring> args={module,fs::absolute(real).wstring(),fs::current_path().wstring(),cache.wstring(),
                                     L"--native",library.wstring(),shader.wstring(),std::to_wstring(samples),L"lm_compute"};
        std::vector<wchar_t*> pointers; for(auto& arg:args) pointers.push_back(arg.data());
        hammerMode=true;
#ifdef VRAD3_STATIC_GPU
        try { gpuServer::configure(); }
        catch(const std::exception& error) { gpuServer::enabled=false; gpuServer::lastNote=std::string("off:")+error.what(); }
        worker.serverDecision=gpuServer::lastNote;
#endif
        hammerCapture=environmentValue(L"VRAD3_GPU_CAPTURE")==L"1";
        std::wcerr << L"Experimental GPU path tracing enabled. Diagnostics: " << cache.wstring() << L"\n";
        auto gpuRun=[&]() {
            int result=runCapture(static_cast<int>(pointers.size()),pointers.data());
            worker.exitCode=hammerChildExit; worker.hardware=hammerHardware;
#ifdef VRAD3_STATIC_GPU
            worker.serverDecision=gpuServer::summary();
#endif
            worker.traceMs=hammerTiming.dispatchMs; worker.directMs=hammerTiming.directMs; worker.reason=hammerFallbackReason;
            worker.result=verifyItem && verifyLine.find("(CPU result written)")!=std::string::npos?"CPU verified":hammerTiming.gpuBlocks?"GPU":"CPU fallback";
            if(result && worker.reason.empty()) worker.reason="GPU worker failed; see detailed logs";
            return result;
        };
        hammerAbortCpuStage=emissiveOn && !verifyItem;
        int result=gpuRun();
        auto leftGpu=[&]() { return !verifyItem && (result?!prefetchBake:!hammerTiming.gpuBlocks && !emptyBlock(worker.reason)); };
        if(emissiveOn && leftGpu()) {
            hammerRetry=worker.reason; hammerAbortCpuStage=emissiveStopsCpu();
            std::cerr << "VRAD3 GPU: emissive = on: block left the GPU (" << worker.reason << "); trying the GPU once more\n" << std::flush;
            result=gpuRun();
            if(emissiveStopsCpu() && leftGpu()) return cpuOrStop();
        }
        if(result && !prefetchBake) {
            std::cerr << "GPU worker failed; rebaking this block with Valve's CPU: " << worker.reason << '\n' << std::flush;
            worker.result="CPU retry"; worker.reason="GPU worker failed ("+worker.reason+"); block rebaked by Valve's CPU";
            return cpuRun();
        }
        if(result==0) writeHammerStatus(forwarded,hammerTiming,worker.reason);
        return finish(result);
    } catch(const std::exception& error) { worker.reason=error.what(); std::cerr << error.what() << "\n"; return 1; }
}

int wmain(int argc,wchar_t** argv) {
    loadWrapperSettings();
    resolveLightPathsAuto();
    lowerPriorities();
#ifdef VRAD3_STATIC_GPU
    if(argc==4 && std::wstring(argv[1])==L"--gpu-server")
        return gpuServer::serve(argv[2],prefetch::number(argv[3]));
#endif
    if(argc==3 && std::wstring(argv[1])==L"--launch-probe") return runLaunchProbe(fs::absolute(argv[2]));
    if(argc==5 && std::wstring(argv[1])==L"--prefetch-coordinator")
        return prefetch::coordinator(fs::absolute(argv[2]),prefetch::number(argv[3]),fs::absolute(argv[4]));
    if(argc==5 && std::wstring(argv[1])==L"--probe-coordinator")
        return prefetch::coordinator(fs::absolute(argv[2]),prefetch::number(argv[3]),fs::absolute(argv[4]),true);
    if(argc>2 && std::wstring(argv[1])==L"--prefetch-bake") { prefetchBake=true; const int code=runHammer(argc-1,argv+1); progressWindow.close(); return code; }
#ifdef VRAD3_STATIC_GPU
    const int code=runHammer(argc,argv); progressWindow.close(); return code;
#else
    if(argc==1 || (argc>1 && argv[1][0]==L'-')) { const int code=runHammer(argc,argv); progressWindow.close(); return code; }
    if(argc!=9 || std::wstring(argv[4])!=L"--native-batch") return runCapture(argc,argv);
    ULONGLONG started=GetTickCount64();
    try {
        std::vector<uint32_t> blockIds;
        std::wstring selection=argv[8];
        if(selection.find(L',')==std::wstring::npos) {
            uint32_t blocks=static_cast<uint32_t>(std::stoul(selection));
            for(uint32_t block=0;block<blocks;++block) blockIds.push_back(block);
        } else {
            std::wistringstream stream(selection); std::wstring token;
            while(std::getline(stream,token,L',')) if(!token.empty()) blockIds.push_back(static_cast<uint32_t>(std::stoul(token)));
        }
        if(blockIds.empty()) throw std::runtime_error("Native batch requires at least one block");
        fs::path root=fs::absolute(argv[3]);
        if(fs::exists(root) && !fs::is_empty(root)) throw std::runtime_error("Batch output folder must be new or empty");
        fs::create_directories(root);
        for(uint32_t block:blockIds) {
            std::vector<std::wstring> args={argv[0],argv[1],argv[2],(root/(L"block-"+std::to_wstring(block))).wstring(),
                                         L"--native",argv[5],argv[6],argv[7],L"lm_compute",std::to_wstring(block)};
            std::vector<wchar_t*> pointers;
            for(auto& arg:args) pointers.push_back(arg.data());
            if(runCapture(static_cast<int>(pointers.size()),pointers.data(),true)) throw std::runtime_error("Native batch child failed");
        }
        HMODULE library=GetModuleHandleW(fs::absolute(argv[5]).c_str());
        auto shutdown=reinterpret_cast<void (*)()>(GetProcAddress(library,"ShutdownGpuLighting"));
        cleanupGpu(shutdown);
        std::ofstream report(root/L"native-batch.json");
        report << "{\"process_total_ms\":" << GetTickCount64()-started << ",\"blocks\":" << blockIds.size()
               << ",\"fresh_child_per_block\":true,\"gpu_session_reused\":true,\"cpu_capture_prerequisite\":false,\"quality_validated\":false}\n";
        if(!report) throw std::runtime_error("Native batch report failed");
        return 0;
    } catch(const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
#endif
}
