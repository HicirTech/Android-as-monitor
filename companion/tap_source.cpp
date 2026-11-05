#include "tap_source.h"

#include "clock.h"
#include "win_error.h"

#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;

namespace aum {

static constexpr DWORD kFrameWaitMs = 100;
static constexpr DWORD kKeyedMutexWaitMs = 50;

std::optional<LUID> TapSource::WaitForRenderAdapter(int timeoutMs) {
    const std::int64_t deadline = NowNs() + std::int64_t{timeoutMs} * 1'000'000;
    while (NowNs() < deadline) {
        const HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, kFrameHeaderName);
        if (mapping) {
            const auto* header = static_cast<const FrameTapHeader*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(FrameTapHeader)));
            std::optional<LUID> adapter;
            if (header && header->magic == kFrameTapMagic && header->abiVersion == kFrameTapAbiVersion && header->generation != 0) {
                adapter = header->renderAdapter;
            }
            if (header) {
                UnmapViewOfFile(header);
            }
            CloseHandle(mapping);
            if (adapter) {
                return adapter;
            }
        }
        Sleep(50);
    }
    return std::nullopt;
}

TapSource::TapSource(GpuDevice& gpu, std::uint32_t width, std::uint32_t height) : gpu_(gpu), width_(width), height_(height) {
    mapping_ = OpenFileMappingW(FILE_MAP_READ, FALSE, kFrameHeaderName);
    if (!mapping_) {
        throw std::runtime_error("cannot open the frame tap header, error " + std::to_string(GetLastError()));
    }
    header_ = static_cast<FrameTapHeader*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, sizeof(FrameTapHeader)));
    if (!header_) {
        throw std::runtime_error("cannot map the frame tap header, error " + std::to_string(GetLastError()));
    }
    readyEvent_ = OpenEventW(SYNCHRONIZE, FALSE, kFrameReadyEventName);
    if (!readyEvent_) {
        throw std::runtime_error("cannot open the frame tap event, error " + std::to_string(GetLastError()));
    }
    OpenTexturesIfNeeded();
}

TapSource::~TapSource() {
    if (readyEvent_) {
        CloseHandle(readyEvent_);
    }
    if (header_) {
        UnmapViewOfFile(header_);
    }
    if (mapping_) {
        CloseHandle(mapping_);
    }
}

void TapSource::OpenTexturesIfNeeded() {
    const LONG generation = header_->generation;
    if (generation == openedGeneration_ || generation == 0) {
        return;
    }
    if (header_->width != width_ || header_->height != height_) {
        throw std::runtime_error("driver delivers " + std::to_string(header_->width) + "x" + std::to_string(header_->height) +
                                 " but the panel is " + std::to_string(width_) + "x" + std::to_string(height_));
    }
    ComPtr<ID3D11Device1> device;
    ThrowIfFailed(gpu_.device.As(&device), "ID3D11Device1 query");
    for (std::uint32_t index = 0; index < kFrameSlotCount; ++index) {
        wchar_t name[80];
        FormatFrameTextureName(name, generation, index);
        Slot& slot = slots_[index];
        slot.texture.Reset();
        slot.mutex.Reset();
        ThrowIfFailed(device->OpenSharedResourceByName(name, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, IID_PPV_ARGS(&slot.texture)),
                      "OpenSharedResourceByName");
        ThrowIfFailed(slot.texture.As(&slot.mutex), "IDXGIKeyedMutex query");
    }
    openedGeneration_ = generation;
}

bool TapSource::Next(ID3D11Texture2D* target, CapturedFrame& frame, HANDLE stopEvent) {
    const HANDLE handles[] = {readyEvent_, stopEvent};
    for (;;) {
        const DWORD wait = WaitForMultipleObjects(ARRAYSIZE(handles), handles, FALSE, kFrameWaitMs);
        if (wait == WAIT_OBJECT_0 + 1) {
            return false;
        }
        if (wait != WAIT_OBJECT_0) {
            continue;
        }
        OpenTexturesIfNeeded();
        const LONG latest = header_->latestSlot;
        if (latest < 0 || openedGeneration_ == 0 || openedGeneration_ != header_->generation) {
            continue;
        }

        Slot& slot = slots_[static_cast<std::size_t>(latest)];
        const HRESULT acquired = slot.mutex->AcquireSync(0, kKeyedMutexWaitMs);
        if (acquired != S_OK) {
            continue;
        }
        const FrameSlotInfo info = header_->slots[latest];
        const bool isNew = info.sequence > lastSequence_;
        if (isNew) {
            gpu_.context->CopyResource(target, slot.texture.Get());
        }
        slot.mutex->ReleaseSync(0);
        if (!isNew) {
            continue;
        }

        frame.capturedNs = QpcToNs(static_cast<std::int64_t>(info.acquireQpc));
        frame.isRepeat = info.presentationFrame == lastPresentationFrame_;
        lastSequence_ = info.sequence;
        lastPresentationFrame_ = info.presentationFrame;
        return true;
    }
}

}  // namespace aum
