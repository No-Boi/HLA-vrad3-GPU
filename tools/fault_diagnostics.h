// Opt-in crash handler: writes a stack trace and minidump for the wrapper into its log folder.
#pragma once
#include <dbghelp.h>
#include <exception>
#include <cstdio>
#pragma comment(lib, "dbghelp.lib")

static void (*faultSummaryCallback)() noexcept=nullptr;
static wchar_t faultLogPath[32768]{}, faultDumpPath[32768]{};
static void recordFault(const char* kind, EXCEPTION_POINTERS* exception=nullptr) noexcept {
    FILE* log=nullptr;
    if(_wfopen_s(&log,faultLogPath,L"a") || !log) return;
    fprintf(log,"Fault: %s pid=%lu\n",kind,GetCurrentProcessId());
    if(exception) fprintf(log,"Exception: 0x%08lx at %p\n",
        exception->ExceptionRecord->ExceptionCode,exception->ExceptionRecord->ExceptionAddress);
    if(auto current=std::current_exception()) {
        try { std::rethrow_exception(current); }
        catch(const std::exception& error) { fprintf(log,"C++ exception: %s\n",error.what()); }
        catch(...) { fprintf(log,"C++ exception: unknown\n"); }
    }
    void* frames[64]{};
    USHORT count=CaptureStackBackTrace(0,64,frames,nullptr);
    HANDLE process=GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES|SYMOPT_UNDNAME|SYMOPT_DEFERRED_LOADS);
    wchar_t executable[32768]{};
    GetModuleFileNameW(nullptr,executable,32768);
    auto symbolPath=std::filesystem::path(executable).parent_path().wstring();
    bool symbols=SymInitializeW(process,symbolPath.c_str(),TRUE)!=FALSE;
    for(USHORT i=0;i<count;++i) {
        MEMORY_BASIC_INFORMATION memory{}; VirtualQuery(frames[i],&memory,sizeof(memory));
        wchar_t module[32768]{};
        GetModuleFileNameW(static_cast<HMODULE>(memory.AllocationBase),module,32768);
        fprintf(log,"%u %p %ls+0x%llx",i,frames[i],module,
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i])-reinterpret_cast<uintptr_t>(memory.AllocationBase)));
        alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO)+MAX_SYM_NAME]{};
        auto* symbol=reinterpret_cast<SYMBOL_INFO*>(storage);
        symbol->SizeOfStruct=sizeof(SYMBOL_INFO); symbol->MaxNameLen=MAX_SYM_NAME;
        DWORD64 displacement=0;
        if(symbols && SymFromAddr(process,reinterpret_cast<DWORD64>(frames[i]),&displacement,symbol))
            fprintf(log," %s+0x%llx",symbol->Name,displacement);
        fprintf(log,"\n");
    }
    fflush(log);
    HANDLE dump=CreateFileW(faultDumpPath,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(dump!=INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(),exception,FALSE};
        BOOL written=MiniDumpWriteDump(process,GetCurrentProcessId(),dump,MiniDumpNormal,
            exception?&info:nullptr,nullptr,nullptr);
        fprintf(log,"Minidump written=%d error=%lu\n",written,GetLastError());
        CloseHandle(dump);
    }
    fclose(log);
    if(faultSummaryCallback) faultSummaryCallback();
}
static LONG WINAPI logUnhandledFault(EXCEPTION_POINTERS* exception) {
    recordFault("unhandled",exception); return EXCEPTION_CONTINUE_SEARCH;
}
static void installFaultDiagnostics(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    wcscpy_s(faultLogPath,(directory/L"wrapper-fault.log").c_str());
    wcscpy_s(faultDumpPath,(directory/L"wrapper-fault.dmp").c_str());
    std::set_terminate([]() { recordFault("std::terminate"); abort(); });
    _set_purecall_handler([]() { recordFault("purecall"); abort(); });
    _set_invalid_parameter_handler([](const wchar_t*,const wchar_t*,const wchar_t*,unsigned,uintptr_t) {
        recordFault("invalid parameter"); abort();
    });
    SetUnhandledExceptionFilter(logUnhandledFault);
}
