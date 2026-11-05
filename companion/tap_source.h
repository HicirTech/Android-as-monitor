#pragma once

#include "frame_source.h"

#include <windows.h>

#include "../shared/driver_protocol.h"

#include <array>
#include <optional>

namespace aum {

// Reads the frames the driver publishes straight from its swap chain; the lowest latency path because no OS
// duplication sits between the desktop compositor and the encoder.
class TapSource final : public FrameSource {
public:
    TapSource(GpuDevice& gpu, std::uint32_t width, std::uint32_t height);
    ~TapSource() override;

    // The render adapter is only known once the OS assigned a swap chain, and the encoder must use the same GPU.
    static std::optional<LUID> WaitForRenderAdapter(int timeoutMs);

    bool Next(ID3D11Texture2D* target, CapturedFrame& frame, HANDLE stopEvent) override;

private:
    void OpenTexturesIfNeeded();

    struct Slot {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<IDXGIKeyedMutex> mutex;
    };

    GpuDevice& gpu_;
    std::uint32_t width_;
    std::uint32_t height_;
    HANDLE mapping_ = nullptr;
    HANDLE readyEvent_ = nullptr;
    FrameTapHeader* header_ = nullptr;
    LONG openedGeneration_ = 0;
    std::array<Slot, kFrameSlotCount> slots_;
    std::uint64_t lastSequence_ = 0;
    std::uint32_t lastPresentationFrame_ = 0;
};

}  // namespace aum
