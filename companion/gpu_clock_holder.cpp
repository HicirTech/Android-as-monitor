#include "gpu_clock_holder.h"

#include "win_error.h"

namespace aum {

static constexpr UINT kTextureSize = 2048;
static constexpr LONGLONG kTickDuration100ns = 80'000;
static constexpr int kNvidiaCopiesPerTick = 8;

int DefaultClockHoldCopies(LUID adapter) {
    // Measured at 120 fps. NVIDIA: encoding drops from 7.4 to 4.1 ms. AMD: already 4.8 ms, and this traffic makes it 11 ms,
    // because an integrated GPU's encoder shares its memory path with the copies.
    return IsNvidiaAdapter(adapter) ? kNvidiaCopiesPerTick : 0;
}

GpuClockHolder::GpuClockHolder(LUID adapter, int copiesPerTick) {
    if (copiesPerTick <= 0) {
        return;
    }
    // Its own device, so that this work never queues behind the encoder's on one immediate context.
    gpu_ = CreateGpuDevice(adapter);

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kTextureSize;
    desc.Height = kTextureSize;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ThrowIfFailed(gpu_.device->CreateTexture2D(&desc, nullptr, &first_), "clock holder CreateTexture2D");
    ThrowIfFailed(gpu_.device->CreateTexture2D(&desc, nullptr, &second_), "clock holder CreateTexture2D");

    const D3D11_QUERY_DESC queryDesc{D3D11_QUERY_EVENT, 0};
    ThrowIfFailed(gpu_.device->CreateQuery(&queryDesc, &batchDone_), "clock holder CreateQuery");

    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = std::thread([this, copiesPerTick] { Run(copiesPerTick); });
}

GpuClockHolder::~GpuClockHolder() {
    if (thread_.joinable()) {
        SetEvent(stop_);
        thread_.join();
    }
    if (stop_) {
        CloseHandle(stop_);
    }
}

void GpuClockHolder::Run(int copiesPerTick) {
    const HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER dueTime;
    dueTime.QuadPart = -kTickDuration100ns;
    SetWaitableTimer(timer, &dueTime, static_cast<LONG>(kTickDuration100ns / 10'000), nullptr, nullptr, FALSE);

    const HANDLE handles[] = {stop_, timer};
    bool batchPending = false;
    while (WaitForMultipleObjects(ARRAYSIZE(handles), handles, FALSE, INFINITE) != WAIT_OBJECT_0) {
        // A GPU still busy with the last batch needs no more help, and this must never build a queue in front of a game.
        if (batchPending && gpu_.context->GetData(batchDone_.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
            continue;
        }
        for (int copy = 0; copy < copiesPerTick; ++copy) {
            gpu_.context->CopyResource(second_.Get(), first_.Get());
            gpu_.context->CopyResource(first_.Get(), second_.Get());
        }
        gpu_.context->End(batchDone_.Get());
        gpu_.context->Flush();
        batchPending = true;
    }
    CloseHandle(timer);
}

}  // namespace aum
