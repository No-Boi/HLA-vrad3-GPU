// Optional minimized console window per worker that shows Valve's live progress and the GPU stages' progress bars.
#pragma once
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <thread>

struct ProgressWindow {
    std::wstring title;
    bool active=false;
    ~ProgressWindow() { if(ticker.joinable()) ticker.detach(); }

    static bool enabled() { return mode()!=0; }
    static int mode() {
        wchar_t value[8]{};
        DWORD n=GetEnvironmentVariableW(L"VRAD3_GPU_PROGRESS_WINDOW",value,8);
        return n==1 && value[0]==L'1' ? 1 : n==1 && value[0]==L'2' ? 2 : 0;
    }
    void launch(STARTUPINFOW& startup,DWORD& flags) {
        if(title.empty()) return;
        if(!active && !tried) { tried=true; openConsole(); }
        if(!active) return;
        flags&=~static_cast<DWORD>(CREATE_NO_WINDOW|CREATE_NEW_CONSOLE|DETACHED_PROCESS);
        startup.dwFlags|=STARTF_USESTDHANDLES; startup.hStdOutput=startup.hStdError=childOut;
    }
    void status(const std::wstring& phase) {
        if(active) SetConsoleTitleW((phase.empty()?title:title+L" - "+phase).c_str());
    }
    void childExited() { copyOut(false); }
    void begin(const std::wstring& text) {
        end(false);
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if(!active || !GetConsoleScreenBufferInfo(console,&info) || info.dwCursorPosition.X) return;
        open=true; stop=false; lineRow=info.dwCursorPosition.Y; lineStarted=GetTickCount64();
        done=0; source=&done; ticks=0;
        write(text+L"[");
        advance(0);
        ticker=std::thread([this] {
            std::unique_lock<std::mutex> lock(mutex);
            while(!wake.wait_for(lock,std::chrono::milliseconds(200),[this] { return stop; })) advance(*source);
        });
    }
    // Progress is the fraction of the stage done, 0..65536.
    volatile uint32_t* progress() { return open?&done:nullptr; }
    void watch(const volatile uint32_t* server) {
        std::lock_guard<std::mutex> lock(mutex); source=server?server:&done;
    }
    void end(bool finished) {
        if(!open) return;
        { std::lock_guard<std::mutex> lock(mutex); stop=true; }
        wake.notify_all(); ticker.join(); open=false;
        if(finished) {
            advance(65536);
            wchar_t text[64]; swprintf_s(text,L"] Done (%.2f seconds)\r\n",static_cast<double>(GetTickCount64()-lineStarted)/1000);
            write(text);
        } else write(L"] GPU unavailable, continuing on the CPU\r\n");
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if(GetConsoleScreenBufferInfo(console,&info)) own.push_back({lineRow,info.dwCursorPosition.Y});
    }
    void close() noexcept {
        if(!active) return;
        try { end(false); copyOut(true); } catch(...) {}
        active=false;
        CloseHandle(childOut); childOut=INVALID_HANDLE_VALUE;
        CloseHandle(console); console=INVALID_HANDLE_VALUE;
        if(host) { TerminateProcess(host,0); CloseHandle(host); host=nullptr; }
        FreeConsole();
    }
private:
    HANDLE console=INVALID_HANDLE_VALUE,childOut=INVALID_HANDLE_VALUE,host=nullptr;
    bool tried=false;
    SHORT copied=0;
    std::vector<std::pair<SHORT,SHORT>> own;
    bool open=false,stop=false;
    SHORT lineRow=0; ULONGLONG lineStarted=0; unsigned ticks=0;
    volatile uint32_t done=0; const volatile uint32_t* source=&done;
    std::mutex mutex; std::condition_variable wake; std::thread ticker;
    void write(const std::wstring& text) {
        DWORD written=0; WriteConsoleW(console,text.c_str(),static_cast<DWORD>(text.size()),&written,nullptr);
    }
    // Valve's bar: 50 marks, a digit every 10%.
    void advance(uint32_t value) {
        std::wstring marks;
        for(;ticks<50 && ticks*65536ull<=value*50ull;++ticks) marks+=ticks%5?L'.':static_cast<wchar_t>(L'0'+ticks/5);
        if(!marks.empty()) write(marks);
    }
    // Valve's child writes straight to a console (on a pipe its C runtime buffers all output until exit), so a minimized
    // never-activated `cmd /c pause` hosts the console and the wrapper attaches to it before starting any child.
    void openConsole() {
        wchar_t system[MAX_PATH]{};
        if(!GetSystemDirectoryW(system,MAX_PATH)) return;
        std::wstring cmd=std::wstring(system)+L"\\cmd.exe";
        std::wstring line=L"\""+cmd+L"\" /d /q /c pause >nul";
        STARTUPINFOW startup{}; startup.cb=sizeof(startup);
        startup.dwFlags=STARTF_USESHOWWINDOW; startup.wShowWindow=SW_SHOWMINNOACTIVE;
        PROCESS_INFORMATION child{};
        if(!CreateProcessW(cmd.c_str(),line.data(),nullptr,nullptr,FALSE,CREATE_NEW_CONSOLE|BELOW_NORMAL_PRIORITY_CLASS,nullptr,nullptr,&startup,&child)) return;
        CloseHandle(child.hThread); host=child.hProcess;
        bool attached=false;
        for(int attempt=0;attempt<200 && !attached;++attempt) {
            if(WaitForSingleObject(host,0)==WAIT_OBJECT_0) break;
            attached=AttachConsole(child.dwProcessId)!=FALSE;
            if(!attached) Sleep(10);
        }
        if(attached) {
            SECURITY_ATTRIBUTES inherit{sizeof(inherit),nullptr,TRUE};
            console=CreateFileW(L"CONOUT$",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
            childOut=CreateFileW(L"CONOUT$",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&inherit,OPEN_EXISTING,0,nullptr);
        }
        if(console==INVALID_HANDLE_VALUE || childOut==INVALID_HANDLE_VALUE) {
            if(console!=INVALID_HANDLE_VALUE) CloseHandle(console);
            if(childOut!=INVALID_HANDLE_VALUE) CloseHandle(childOut);
            console=childOut=INVALID_HANDLE_VALUE;
            if(attached) FreeConsole();
            TerminateProcess(host,0); CloseHandle(host); host=nullptr;
            return;
        }
        active=true;
        SetConsoleScreenBufferSize(console,COORD{400,9999});
        SetConsoleOutputCP(CP_UTF8);
        status(L"");
        // user32 is resolved at run time so the exe gains no new import; removes the close button and keeps the window minimized.
        if(HWND window=GetConsoleWindow()) if(HMODULE user=LoadLibraryW(L"user32.dll")) {
            using GetMenu=HMENU (WINAPI*)(HWND,BOOL); using Delete=BOOL (WINAPI*)(HMENU,UINT,UINT);
            using Show=BOOL (WINAPI*)(HWND,int); using Iconic=BOOL (WINAPI*)(HWND);
            auto getMenu=reinterpret_cast<GetMenu>(GetProcAddress(user,"GetSystemMenu"));
            auto deleteMenu=reinterpret_cast<Delete>(GetProcAddress(user,"DeleteMenu"));
            auto show=reinterpret_cast<Show>(GetProcAddress(user,"ShowWindow"));
            auto iconic=reinterpret_cast<Iconic>(GetProcAddress(user,"IsIconic"));
            if(getMenu && deleteMenu) if(HMENU menu=getMenu(window,FALSE)) deleteMenu(menu,SC_CLOSE,MF_BYCOMMAND);
            if(show && iconic && !iconic(window)) show(window,SW_SHOWMINNOACTIVE);
            if(show && mode()==2) show(window,SW_SHOWNOACTIVATE);
        }
        HANDLE input=CreateFileW(L"CONIN$",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
        if(input!=INVALID_HANDLE_VALUE) {
            DWORD mode=0;
            // No Ctrl+C or QuickEdit: neither may pause or end the compile.
            if(GetConsoleMode(input,&mode)) SetConsoleMode(input,(mode&~static_cast<DWORD>(ENABLE_PROCESSED_INPUT|ENABLE_QUICK_EDIT_MODE))|ENABLE_EXTENDED_FLAGS);
            CloseHandle(input);
        }
    }
    void copyOut(bool all) {
        if(!active) return;
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if(!GetConsoleScreenBufferInfo(console,&info)) return;
        const SHORT end=static_cast<SHORT>(info.dwCursorPosition.Y+((all && info.dwCursorPosition.X>0)?1:0));
        std::string text;
        std::vector<wchar_t> row(static_cast<size_t>(info.dwSize.X));
        for(SHORT y=copied;y<end;++y) {
            if(std::any_of(own.begin(),own.end(),[y](const auto& rows) { return y>=rows.first && y<rows.second; })) continue;
            DWORD read=0;
            if(!ReadConsoleOutputCharacterW(console,row.data(),static_cast<DWORD>(row.size()),COORD{0,y},&read)) break;
            while(read && (row[read-1]==L' ' || row[read-1]==0)) --read;
            int bytes=WideCharToMultiByte(CP_UTF8,0,row.data(),static_cast<int>(read),nullptr,0,nullptr,nullptr);
            std::string line(static_cast<size_t>(bytes),'\0');
            if(bytes) WideCharToMultiByte(CP_UTF8,0,row.data(),static_cast<int>(read),line.data(),bytes,nullptr,nullptr);
            text+=line; text+="\n";
        }
        copied=end;
        if(!text.empty()) std::cout << text << std::flush;
    }
};
static ProgressWindow progressWindow;
