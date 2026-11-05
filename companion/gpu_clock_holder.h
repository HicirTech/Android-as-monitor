#pragma once

#include "gpu.h"

#include <windows.h>

#include <thread>

namespace aum {

// How much to hold on this GPU when the user does not say.
int DefaultClockHoldCopies(LUID adapter);

// Keeps the GPU out of its idle clocks. Encoding alone does not count as load to the driver's power management, so a few
// seconds into a stream the video clock falls by more than half and each frame takes twice as long to encode. A little
// memory traffic on every tick holds the clocks up for about 5 W, and the very first frame after an idle desktop is fast too.
class GpuClockHolder {
public:
    // 0 holds nothing. Each unit copies a 2048x2048 texture back and forth once per tick.
    GpuClockHolder(LUID adapter, int copiesPerTick);
    ~GpuClockHolder();

    GpuClockHolder(const GpuClockHolder&) = delete;
    GpuClockHolder& operator=(const GpuClockHolder&) = delete;

private:
    void Run(int copiesPerTick);

    GpuDevice gpu_{};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> first_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> second_;
    Microsoft::WRL::ComPtr<ID3D11Query> batchDone_;
    HANDLE stop_ = nullptr;
    std::thread thread_;
};

}  // namespace aum
