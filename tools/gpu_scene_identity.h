// Content hash identifying the immutable scene held by the GPU server.
#pragma once
#include <array>
#include <thread>
#include <atomic>
#include <bcrypt.h>
#pragma comment(lib,"bcrypt.lib")

struct GpuSceneBuffer { const void* data; uint64_t bytes; };
// Hashes buffers 0-6, 11, 12, their lengths, constants 11-14, the pathtrace/directlight selector and traversal options. CSR, samples, output and RNG values are request-local.
// Tree hash: 1 MiB chunks hashed in parallel, then the digests and lengths hashed once more.
static std::array<uint8_t,32> gpuSceneIdentity(const GpuSceneBuffer* buffers,const uint32_t* constants,
                                              bool hardware,bool lightCulling,bool compare) {
    constexpr uint64_t chunk=1ull<<20;
    auto check=[](NTSTATUS status) { if(status<0) throw std::runtime_error("GPU scene SHA256 failed"); };
    struct Task { size_t buffer; uint64_t offset,bytes; };
    std::vector<Task> tasks; std::vector<size_t> first;
    for(size_t i=0;i<13;++i) {
        first.push_back(tasks.size());
        if(i>=7 && i<=10) continue;
        if(!buffers[i].data || buffers[i].bytes>512ull*1024*1024) throw std::runtime_error("Invalid GPU scene hash buffer");
        for(uint64_t offset=0;offset<buffers[i].bytes;offset+=chunk)
            tasks.push_back({i,offset,std::min(chunk,buffers[i].bytes-offset)});
    }
    first.push_back(tasks.size());
    std::vector<std::array<uint8_t,32>> digests(tasks.size());
    std::atomic<size_t> next{0}; std::atomic<bool> failed{false};
    auto worker=[&]() {
        BCRYPT_ALG_HANDLE algorithm=nullptr;
        if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) { failed=true; return; }
        for(size_t i;(i=next++)<tasks.size();) {
            const auto& task=tasks[i];
            if(BCryptHash(algorithm,nullptr,0,const_cast<PUCHAR>(static_cast<const uint8_t*>(buffers[task.buffer].data)+task.offset),
                          static_cast<ULONG>(task.bytes),digests[i].data(),32)<0) { failed=true; break; }
        }
        BCryptCloseAlgorithmProvider(algorithm,0);
    };
    size_t threads=std::min<size_t>(std::max(1u,std::thread::hardware_concurrency()),std::max<size_t>(1,tasks.size()));
    std::vector<std::thread> pool;
    try { for(size_t t=1;t<threads;++t) pool.emplace_back(worker); } catch(...) {   }
    worker();
    for(auto& thread:pool) thread.join();
    if(failed) throw std::runtime_error("GPU scene SHA256 failed");
    BCRYPT_ALG_HANDLE algorithm=nullptr; BCRYPT_HASH_HANDLE hash=nullptr; std::array<uint8_t,32> result{};
    try {
        check(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0));
        check(BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0));
        auto add=[&](const void* data,uint64_t bytes) { check(BCryptHashData(hash,const_cast<PUCHAR>(static_cast<const uint8_t*>(data)),static_cast<ULONG>(bytes),0)); };
        const uint32_t layout=3,options=(hardware?1u:0u)|(lightCulling?2u:0u)|(compare?4u:0u),direct=(constants[3]&0x80000000u)?1u:0u;
        add(&layout,sizeof(layout)); add(&options,sizeof(options)); add(&direct,sizeof(direct));
        add(constants+11,4*sizeof(uint32_t));
        for(size_t i=0;i<13;++i) {
            if(i>=7 && i<=10) continue;
            add(&buffers[i].bytes,sizeof(uint64_t));
            const uint64_t count=first[i+1]-first[i]; add(&count,sizeof(count));
            for(size_t j=first[i];j<first[i+1];++j) add(digests[j].data(),32);
        }
        check(BCryptFinishHash(hash,result.data(),static_cast<ULONG>(result.size()),0));
    } catch(...) { if(hash) BCryptDestroyHash(hash); if(algorithm) BCryptCloseAlgorithmProvider(algorithm,0); throw; }
    BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm,0); return result;
}
