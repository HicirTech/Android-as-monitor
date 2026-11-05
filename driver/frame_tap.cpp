#include "frame_tap.h"

#include "event_log.h"

using Microsoft::WRL::ComPtr;

FrameTap::FrameTap(FrameTapChannel& channel, ID3D11Device* device, ID3D11DeviceContext* context, LUID renderAdapter)
    : channel_(channel), device_(device), context_(context), renderAdapter_(renderAdapter) {}

FrameTap::~FrameTap() {
    ReleaseTextures();
}

void FrameTap::ReleaseTextures() {
    if (channel_.IsOpen()) {
        InterlockedExchange(&channel_.Header().generation, 0);
    }
    for (Slot& slot : slots_) {
        if (slot.sharedHandle) {
            CloseHandle(slot.sharedHandle);
            slot.sharedHandle = nullptr;
        }
        slot.mutex.Reset();
        slot.texture.Reset();
    }
    width_ = 0;
    height_ = 0;
}

bool FrameTap::EnsureTextures(const D3D11_TEXTURE2D_DESC& surfaceDesc) {
    if (surfaceDesc.Width == width_ && surfaceDesc.Height == height_ && surfaceDesc.Format == format_) {
        return true;
    }
    ReleaseTextures();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = surfaceDesc.Width;
    desc.Height = surfaceDesc.Height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = surfaceDesc.Format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    const LONG generation = channel_.NextGeneration();
    for (UINT index = 0; index < aum::kFrameSlotCount; ++index) {
        Slot& slot = slots_[index];
        HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &slot.texture);
        if (FAILED(hr)) {
            LogEvent(L"frame tap: CreateTexture2D failed 0x%08lX", hr);
            ReleaseTextures();
            return false;
        }
        hr = slot.texture.As(&slot.mutex);
        if (FAILED(hr)) {
            LogEvent(L"frame tap: keyed mutex query failed 0x%08lX", hr);
            ReleaseTextures();
            return false;
        }
        ComPtr<IDXGIResource1> resource;
        hr = slot.texture.As(&resource);
        if (FAILED(hr)) {
            LogEvent(L"frame tap: IDXGIResource1 query failed 0x%08lX", hr);
            ReleaseTextures();
            return false;
        }
        wchar_t name[80];
        aum::FormatFrameTextureName(name, generation, index);
        hr = resource->CreateSharedHandle(channel_.SecurityAttributes(),
                                          DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, name,
                                          &slot.sharedHandle);
        if (FAILED(hr)) {
            LogEvent(L"frame tap: CreateSharedHandle failed 0x%08lX", hr);
            ReleaseTextures();
            return false;
        }
    }

    aum::FrameTapHeader& header = channel_.Header();
    header.renderAdapter = renderAdapter_;
    header.width = desc.Width;
    header.height = desc.Height;
    header.dxgiFormat = desc.Format;
    InterlockedExchange(&header.latestSlot, -1);
    InterlockedExchange(&header.generation, generation);

    width_ = desc.Width;
    height_ = desc.Height;
    format_ = desc.Format;
    nextSlot_ = 0;
    LogEvent(L"frame tap: generation %ld, %ux%u format %u", generation, width_, height_, static_cast<UINT>(format_));
    return true;
}

void FrameTap::Publish(ID3D11Texture2D* surface, const IDDCX_METADATA& metadata, UINT64 acquireQpc) {
    if (!channel_.IsOpen()) {
        return;
    }
    D3D11_TEXTURE2D_DESC surfaceDesc;
    surface->GetDesc(&surfaceDesc);
    if (!EnsureTextures(surfaceDesc)) {
        return;
    }

    aum::FrameTapHeader& header = channel_.Header();
    for (UINT attempt = 0; attempt < aum::kFrameSlotCount; ++attempt) {
        const UINT index = (nextSlot_ + attempt) % aum::kFrameSlotCount;
        Slot& slot = slots_[index];
        const HRESULT hr = slot.mutex->AcquireSync(0, 0);
        if (hr != S_OK) {
            if (FAILED(hr) && !loggedFailure_) {
                loggedFailure_ = true;
                LogEvent(L"frame tap: AcquireSync failed 0x%08lX", hr);
            }
            continue;
        }
        context_->CopyResource(slot.texture.Get(), surface);

        aum::FrameSlotInfo& info = header.slots[index];
        info.sequence = ++sequence_;
        info.acquireQpc = acquireQpc;
        info.presentQpc = metadata.PresentDisplayQPCTime;
        info.presentationFrame = metadata.PresentationFrameNumber;

        slot.mutex->ReleaseSync(0);
        context_->Flush();

        InterlockedExchange(&header.latestSlot, static_cast<LONG>(index));
        InterlockedIncrement64(&header.published);
        channel_.SignalFrameReady();
        nextSlot_ = (index + 1) % aum::kFrameSlotCount;
        return;
    }
}
