#include "control_pipe.h"

#include "event_log.h"

#include <sddl.h>

ControlPipe::ControlPipe(RequestHandler onRequest, DisconnectHandler onDisconnect)
    : onRequest_(std::move(onRequest)), onDisconnect_(std::move(onDisconnect)) {
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ioEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

ControlPipe::~ControlPipe() {
    SetEvent(stopEvent_);
    if (thread_) {
        WaitForSingleObject(thread_, INFINITE);
        CloseHandle(thread_);
    }
    if (pipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_);
    }
    CloseHandle(ioEvent_);
    CloseHandle(stopEvent_);
}

bool ControlPipe::Start() {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(aum::kSharedObjectSddl, SDDL_REVISION_1,
                                                              &attributes.lpSecurityDescriptor, nullptr)) {
        LogEvent(L"control pipe: security descriptor failed, error %lu", GetLastError());
        return false;
    }
    pipe_ = CreateNamedPipeW(aum::kControlPipeName, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                             PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                             sizeof(aum::ControlResponse), sizeof(aum::ControlRequest), 0, &attributes);
    const DWORD createError = GetLastError();
    LocalFree(attributes.lpSecurityDescriptor);
    if (pipe_ == INVALID_HANDLE_VALUE) {
        LogEvent(L"control pipe: CreateNamedPipe failed, error %lu", createError);
        return false;
    }
    thread_ = CreateThread(nullptr, 0, ThreadMain, this, 0, nullptr);
    return thread_ != nullptr;
}

DWORD CALLBACK ControlPipe::ThreadMain(LPVOID argument) {
    static_cast<ControlPipe*>(argument)->Run();
    return 0;
}

bool ControlPipe::WaitForIo(OVERLAPPED& overlapped, DWORD& bytes) {
    const HANDLE handles[] = {overlapped.hEvent, stopEvent_};
    if (WaitForMultipleObjects(ARRAYSIZE(handles), handles, FALSE, INFINITE) != WAIT_OBJECT_0) {
        CancelIoEx(pipe_, &overlapped);
        GetOverlappedResult(pipe_, &overlapped, &bytes, TRUE);
        return false;
    }
    return GetOverlappedResult(pipe_, &overlapped, &bytes, FALSE) != FALSE;
}

void ControlPipe::Run() {
    LogEvent(L"control pipe: listening");
    while (WaitForSingleObject(stopEvent_, 0) == WAIT_TIMEOUT) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = ioEvent_;
        ResetEvent(ioEvent_);

        DWORD bytes = 0;
        if (!ConnectNamedPipe(pipe_, &overlapped)) {
            const DWORD error = GetLastError();
            if (error == ERROR_IO_PENDING) {
                if (!WaitForIo(overlapped, bytes)) {
                    continue;
                }
            } else if (error != ERROR_PIPE_CONNECTED) {
                LogEvent(L"control pipe: ConnectNamedPipe failed, error %lu", error);
                return;
            }
        }
        ServeClient(pipe_);
        DisconnectNamedPipe(pipe_);
        // During device removal the framework has already destroyed the monitor, so it must not be touched again.
        if (WaitForSingleObject(stopEvent_, 0) == WAIT_TIMEOUT) {
            onDisconnect_();
        }
    }
}

void ControlPipe::ServeClient(HANDLE pipe) {
    for (;;) {
        aum::ControlRequest request{};
        OVERLAPPED overlapped{};
        overlapped.hEvent = ioEvent_;
        ResetEvent(ioEvent_);

        DWORD bytes = 0;
        if (!ReadFile(pipe, &request, sizeof(request), nullptr, &overlapped) && GetLastError() != ERROR_IO_PENDING) {
            return;
        }
        if (!WaitForIo(overlapped, bytes) || bytes != sizeof(request)) {
            return;
        }

        aum::ControlResponse response = onRequest_(request);
        response.sequence = request.sequence;

        ResetEvent(ioEvent_);
        if (!WriteFile(pipe, &response, sizeof(response), nullptr, &overlapped) && GetLastError() != ERROR_IO_PENDING) {
            return;
        }
        if (!WaitForIo(overlapped, bytes)) {
            return;
        }
    }
}
