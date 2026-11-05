#pragma once

#include "control_pipe.h"
#include "driver_includes.h"
#include "frame_tap_channel.h"

#include <array>
#include <memory>

class DeviceContext {
public:
    explicit DeviceContext(WDFDEVICE device) : device_(device) {}

    void InitAdapter();
    void OnAdapterReady();

private:
    aum::ControlResponse HandleRequest(const aum::ControlRequest& request);
    NTSTATUS Plug(const aum::ControlRequest& request, aum::ControlResponse& response);
    void Unplug();

    WDFDEVICE device_;
    IDDCX_ADAPTER adapter_ = nullptr;
    IDDCX_MONITOR monitor_ = nullptr;
    std::array<std::uint8_t, aum::kMaxEdidBytes> edid_{};
    FrameTapChannel frameTapChannel_;
    std::unique_ptr<ControlPipe> controlPipe_;
};
