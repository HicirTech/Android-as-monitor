#pragma once

#include "driver_includes.h"
#include "../shared/driver_protocol.h"

#include <functional>

// A pipe rather than an IOCTL because its disconnect doubles as a dead-man switch:
// when the companion exits or crashes the monitor is unplugged instead of staying behind as a blank screen.
class ControlPipe {
public:
    using RequestHandler = std::function<aum::ControlResponse(const aum::ControlRequest&)>;
    using DisconnectHandler = std::function<void()>;

    ControlPipe(RequestHandler onRequest, DisconnectHandler onDisconnect);
    ~ControlPipe();

    ControlPipe(const ControlPipe&) = delete;
    ControlPipe& operator=(const ControlPipe&) = delete;

    bool Start();

private:
    static DWORD CALLBACK ThreadMain(LPVOID argument);
    void Run();
    bool WaitForIo(OVERLAPPED& overlapped, DWORD& bytes);
    void ServeClient(HANDLE pipe);

    RequestHandler onRequest_;
    DisconnectHandler onDisconnect_;
    HANDLE stopEvent_ = nullptr;
    HANDLE thread_ = nullptr;
    HANDLE ioEvent_ = nullptr;
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
};
