// Hardware limits: usable CPU count (affinity, CPU sets, groups) and available-RAM checks.
#pragma once
#include <windows.h>
#include <algorithm>
#include <vector>
#include <string>
#include <stdexcept>
#include "block_profile.h"

// Logical CPUs this process may actually use: CPU sets, affinity mask and processor groups combined.
static unsigned usableCpuCount() {
    BlockProfileScope profile("cpu_topology");
    HANDLE process=GetCurrentProcess();
    DWORD_PTR mask=0,systemMask=0;
    bool hasMask=GetProcessAffinityMask(process,&mask,&systemMask)!=FALSE && mask!=0;
    USHORT groupCount=0;
    GetProcessGroupAffinity(process,&groupCount,nullptr);
    std::vector<USHORT> groups(groupCount);
    bool hasGroups=groupCount && GetProcessGroupAffinity(process,&groupCount,groups.data());
    ULONG idsCount=0;
    GetProcessDefaultCpuSets(process,nullptr,0,&idsCount);
    std::vector<ULONG> ids(idsCount);
    if(idsCount && !GetProcessDefaultCpuSets(process,ids.data(),idsCount,&idsCount)) ids.clear();
    ULONG bytes=0;
    GetSystemCpuSetInformation(nullptr,0,&bytes,process,0);
    std::vector<unsigned char> storage(bytes);
    unsigned count=0;
    if(bytes && GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(storage.data()),bytes,&bytes,process,0)) {
        for(size_t offset=0;offset<bytes;) {
            auto* info=reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(storage.data()+offset);
            if(!info->Size || info->Size>bytes-offset) break;
            if(info->Type==CpuSetInformation) {
                const auto& cpu=info->CpuSet;
                bool allowed=ids.empty() || std::find(ids.begin(),ids.end(),cpu.Id)!=ids.end();
                if(hasMask && (mask!=systemMask || GetActiveProcessorGroupCount()==1))
                    allowed=allowed && (!hasGroups || std::find(groups.begin(),groups.end(),cpu.Group)!=groups.end())
                        && cpu.LogicalProcessorIndex<sizeof(mask)*8 && (mask&(DWORD_PTR(1)<<cpu.LogicalProcessorIndex));
                if(cpu.Allocated && !cpu.AllocatedToTargetProcess) allowed=false;
                if(allowed) ++count;
            }
            offset+=info->Size;
        }
    } else if(hasMask) {
        for(;mask;mask&=mask-1) ++count;
    } else count=GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return std::max(1u,count);
}

// RAM a GPU stage may use: free RAM, or when that is short, commit space up to a quarter of physical RAM.
// Windows pages out Hammer's idle resourcecompiler memory, and a CPU fallback would page the same way.
static uint64_t usableMemory(const MEMORYSTATUSEX& memory) {
    return std::max<uint64_t>(memory.ullAvailPhys,std::min<uint64_t>(memory.ullAvailPageFile,memory.ullTotalPhys/4));
}

static void requirePhysicalMemory(uint64_t bytes,const char* operation) {
    BlockProfileScope profile("ram_check");
    MEMORYSTATUSEX memory{}; memory.dwLength=sizeof(memory);
    if(!GlobalMemoryStatusEx(&memory)) throw std::runtime_error("Cannot query available system RAM");
    const uint64_t reserve=256ull*1024*1024,available=usableMemory(memory);
    if(bytes>available || reserve>available-bytes)
        throw std::runtime_error(std::string("Insufficient available system RAM for ")+operation+
            ": required="+std::to_string(bytes)+" reserve="+std::to_string(reserve)+
            " available="+std::to_string(memory.ullAvailPhys)+" usable="+std::to_string(available));
}
