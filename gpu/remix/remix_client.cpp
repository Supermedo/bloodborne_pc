// SPDX-License-Identifier: GPL-2.0-or-later
#include "remix_client.h"
#include <algorithm>
#include <bcrypt.h>
#include <chrono>
#include <cstdio>

namespace BbRemix {
struct Client::Impl {
    HANDLE pipe{INVALID_HANDLE_VALUE}, process{}, job{};
    Wire::Reply reply{};
    std::string error;
    bool Fail(const char* what) { error=what; std::printf("RTX Remix game: %s; returning to Vulkan\n",what); return false; }
    bool Transfer(void* data, size_t bytes, bool write, DWORD timeout=90000) {
        if (pipe==INVALID_HANDLE_VALUE) return Fail("host disconnected");
        auto* p=static_cast<uint8_t*>(data);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout);
        while (bytes) {
            OVERLAPPED ov{}; ov.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
            if (!ov.hEvent) return Fail("IPC event creation failed");
            DWORD done{};
            const DWORD n=DWORD(std::min<size_t>(bytes,1u<<20));
            BOOL ok=write ? WriteFile(pipe,p,n,&done,&ov) : ReadFile(pipe,p,n,&done,&ov);
            if (!ok && GetLastError()==ERROR_IO_PENDING) {
                const auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now()).count();
                HANDLE handles[]{ov.hEvent,process};
                const DWORD waited=WaitForMultipleObjects(2,handles,FALSE,DWORD(std::max<int64_t>(0,left)));
                if (waited==WAIT_OBJECT_0) ok=GetOverlappedResult(pipe,&ov,&done,FALSE);
                else { CancelIoEx(pipe,&ov); GetOverlappedResult(pipe,&ov,&done,TRUE); ok=FALSE; }
            }
            CloseHandle(ov.hEvent);
            if (!ok || !done) {
                DWORD code=STILL_ACTIVE;
                if(process && GetExitCodeProcess(process,&code) && code!=STILL_ACTIVE)
                    std::printf("RTX Remix host exited: 0x%08lx\n",code);
                return Fail("SDK process stopped, timed out or disconnected");
            }
            p+=done; bytes-=done;
        }
        return true;
    }
    bool Exchange(Wire::Operation op, const Wire::Writer& data) {
        Wire::Header header{}; header.operation=op; header.bytes=uint32_t(data.data.size());
        if (!Transfer(&header,sizeof(header),true) ||
            !Transfer(const_cast<uint8_t*>(data.data.data()),data.data.size(),true) ||
            !Transfer(&reply,sizeof(reply),false)) return false;
        if (reply.magic!=Wire::Magic || reply.version!=Wire::Version || !reply.success) return Fail("official SDK rejected the game scene (see Remix log)");
        return true;
    }
};
Client::Client() : impl(std::make_unique<Impl>()) {}
Client::~Client() { Stop(); }
const std::string& Client::Error() const { return impl->error; }
bool Client::Ready() const { return impl->reply.success && impl->reply.ready; }
Renderer::SharedOutput Client::Output() const {
    const auto& p=*impl;
    return Ready() ? Renderer::SharedOutput{reinterpret_cast<HANDLE>(p.reply.memory),p.reply.width,p.reply.height,p.reply.generation,p.reply.luid} : Renderer::SharedOutput{};
}
bool Client::Start(const std::filesystem::path& host, const std::filesystem::path& runtime,
                   const std::filesystem::path& log, const Wire::Initialize& info) {
    Stop(); auto& p=*impl;
    if (!host.is_absolute() || !runtime.is_absolute() || !std::filesystem::is_regular_file(host) || !std::filesystem::is_regular_file(runtime)) return p.Fail("SDK runtime or game host is missing");
    unsigned char random[16]{};
    if (BCryptGenRandom(nullptr,random,sizeof(random),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0) return p.Fail("IPC random identifier failed");
    wchar_t name[128]{};
    swprintf(name,std::size(name),L"\\\\.\\pipe\\bb-remix-%lu-%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
        GetCurrentProcessId(),random[0],random[1],random[2],random[3],random[4],random[5],random[6],random[7],random[8],random[9],random[10],random[11],random[12],random[13],random[14],random[15]);
    p.pipe=CreateNamedPipeW(name,PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,1u<<20,1u<<20,0,nullptr);
    if (p.pipe==INVALID_HANDLE_VALUE) return p.Fail("IPC pipe creation failed");
    p.job=CreateJobObjectW(nullptr,nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{}; limit.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!p.job || !SetInformationJobObject(p.job,JobObjectExtendedLimitInformation,&limit,sizeof(limit))) return p.Fail("SDK isolation job failed");
    std::error_code ec; std::filesystem::create_directories(log.parent_path(),ec);
    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
    HANDLE output=CreateFileW(log.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&security,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    HANDLE input=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (output==INVALID_HANDLE_VALUE || input==INVALID_HANDLE_VALUE) {
        if (output!=INVALID_HANDLE_VALUE) CloseHandle(output);
        if (input!=INVALID_HANDLE_VALUE) CloseHandle(input);
        return p.Fail("SDK log creation failed");
    }
    SIZE_T attributes_size{};
    InitializeProcThreadAttributeList(nullptr,1,0,&attributes_size);
    std::vector<uint8_t> attributes(attributes_size);
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb=sizeof(startup);
    startup.lpAttributeList=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    HANDLE inherited[]{output,input};
    const bool initialized_attributes=InitializeProcThreadAttributeList(startup.lpAttributeList,1,0,&attributes_size);
    const bool setup=initialized_attributes &&
        UpdateProcThreadAttribute(startup.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),nullptr,nullptr);
    startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES; startup.StartupInfo.hStdInput=input;
    startup.StartupInfo.hStdOutput=startup.StartupInfo.hStdError=output;
    const auto environment=std::filesystem::absolute(log.parent_path()/"remix-environment.dds");
    std::wstring command=L"\""+host.wstring()+L"\" \""+name+L"\" \""+runtime.wstring()+L"\" \""+environment.wstring()+L"\"";
    PROCESS_INFORMATION child{};
    const BOOL created=setup && CreateProcessW(host.c_str(),command.data(),nullptr,nullptr,TRUE,
        CREATE_NO_WINDOW|CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT,nullptr,runtime.parent_path().c_str(),&startup.StartupInfo,&child);
    if (initialized_attributes) DeleteProcThreadAttributeList(startup.lpAttributeList);
    CloseHandle(output); CloseHandle(input);
    if (!created) return p.Fail("SDK process launch failed");
    p.process=child.hProcess;
    if (!AssignProcessToJobObject(p.job,child.hProcess)) {
        TerminateProcess(child.hProcess,1); CloseHandle(child.hThread); return p.Fail("SDK process isolation failed");
    }
    ResumeThread(child.hThread); CloseHandle(child.hThread);
    OVERLAPPED connect{}; connect.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if (!connect.hEvent) return p.Fail("IPC connection event creation failed");
    bool connected=ConnectNamedPipe(p.pipe,&connect)!=FALSE;
    if (!connected) {
        const DWORD code=GetLastError();
        if (code==ERROR_PIPE_CONNECTED) connected=true;
        else if (code==ERROR_IO_PENDING) {
            HANDLE handles[]{connect.hEvent,p.process};
            const DWORD waited=WaitForMultipleObjects(2,handles,FALSE,15000);
            DWORD done{};
            if (waited==WAIT_OBJECT_0) connected=GetOverlappedResult(p.pipe,&connect,&done,FALSE)!=FALSE;
            else { CancelIoEx(p.pipe,&connect); GetOverlappedResult(p.pipe,&connect,&done,TRUE); }
        }
    }
    CloseHandle(connect.hEvent);
    ULONG client_pid{};
    if (!connected || !GetNamedPipeClientProcessId(p.pipe,&client_pid) || client_pid!=child.dwProcessId) return p.Fail("SDK IPC connection failed");
    Wire::Writer request; request.Put(info);
    return p.Exchange(Wire::Operation::Initialize,request);
}
bool Client::Render(const Wire::Writer& scene) { return impl->Exchange(Wire::Operation::Frame,scene); }
void Client::Stop() {
    auto& p=*impl;
    // Closing the pipe ends a healthy worker. Closing the job also stops hung SDK calls.
    if (p.pipe!=INVALID_HANDLE_VALUE) { CancelIoEx(p.pipe,nullptr); CloseHandle(p.pipe); p.pipe=INVALID_HANDLE_VALUE; }
    if (p.process) {
        WaitForSingleObject(p.process,1000);
        if (p.job) { CloseHandle(p.job); p.job=nullptr; }
        WaitForSingleObject(p.process,2000); CloseHandle(p.process); p.process=nullptr;
    }
    if (p.job) { CloseHandle(p.job); p.job=nullptr; }
    p.reply={};
}
} // namespace BbRemix
