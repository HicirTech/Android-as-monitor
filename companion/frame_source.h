#pragma once

#include "gpu.h"

#include <cstdint>

namespace aum {

struct CapturedFrame {
    // When the desktop surface became available, in PC nanoseconds. Latency is measured from here.
    std::int64_t capturedNs = 0;
    bool isRepeat = false;
};

class FrameSource {
public:
    virtual ~FrameSource() = default;

    // Blocks until a newer frame exists and copies it into `target` (BGRA, panel sized).
    // Returns false when `stopEvent` is signalled.
    virtual bool Next(ID3D11Texture2D* target, CapturedFrame& frame, HANDLE stopEvent) = 0;
};

}  // namespace aum
