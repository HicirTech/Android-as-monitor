#pragma once

#include "driver_includes.h"
#include "frame_tap_channel.h"

#include <array>

// Copies each desktop surface into a small ring of textures the companion process can open.
// The swap chain thread must never wait on the companion, so a busy slot means the frame is skipped.
class FrameTap {
public:
    FrameTap(FrameTapChannel& channel, ID3D11Device* device, ID3D11DeviceContext* context, LUID renderAdapter);
    ~FrameTap();

    void Publish(ID3D11Texture2D* surface, const IDDCX_METADATA& metadata, UINT64 acquireQpc);

private:
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<IDXGIKeyedMutex> mutex;
        HANDLE sharedHandle = nullptr;
    };

    bool EnsureTextures(const D3D11_TEXTURE2D_DESC& surfaceDesc);
    void ReleaseTextures();

    FrameTapChannel& channel_;
    ID3D11Device* device_;
    ID3D11DeviceContext* context_;
    LUID renderAdapter_;
    std::array<Slot, aum::kFrameSlotCount> slots_;
    UINT width_ = 0;
    UINT height_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    UINT nextSlot_ = 0;
    UINT64 sequence_ = 0;
    bool loggedFailure_ = false;
};
