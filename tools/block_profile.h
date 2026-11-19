// Opt-in timing scopes (VRAD3_BLOCK_PROFILE): print elapsed ms per named step to stderr.
#pragma once
#include <windows.h>
#include <iostream>
inline bool blockProfileEnabled() {
    static bool enabled=GetEnvironmentVariableW(L"VRAD3_BLOCK_PROFILE",nullptr,0)!=0;
    return enabled;
}
inline double blockProfileNow() {
    LARGE_INTEGER value,frequency; QueryPerformanceCounter(&value); QueryPerformanceFrequency(&frequency);
    return value.QuadPart*1000.0/frequency.QuadPart;
}
struct BlockProfileScope {
    const char* name; uint64_t bytes; double started;
    BlockProfileScope(const char* n,uint64_t b=0):name(n),bytes(b),started(blockProfileEnabled()?blockProfileNow():0) {}
    ~BlockProfileScope() { if(blockProfileEnabled()) std::cerr << "BLOCK_PROFILE " << name << " ms=" << blockProfileNow()-started << " bytes=" << bytes << '\n'; }
};
