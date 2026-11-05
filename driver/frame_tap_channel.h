#pragma once

#include "driver_includes.h"
#include "../shared/driver_protocol.h"

class FrameTapChannel {
public:
    FrameTapChannel();
    ~FrameTapChannel();

    FrameTapChannel(const FrameTapChannel&) = delete;
    FrameTapChannel& operator=(const FrameTapChannel&) = delete;

    bool IsOpen() const { return header_ != nullptr && readyEvent_ != nullptr; }
    aum::FrameTapHeader& Header() { return *header_; }
    PSECURITY_ATTRIBUTES SecurityAttributes() { return &securityAttributes_; }
    LONG NextGeneration() { return ++generationCounter_; }
    void SignalFrameReady() { SetEvent(readyEvent_); }

private:
    SECURITY_ATTRIBUTES securityAttributes_{};
    HANDLE mapping_ = nullptr;
    HANDLE readyEvent_ = nullptr;
    aum::FrameTapHeader* header_ = nullptr;
    LONG generationCounter_ = 0;
};
