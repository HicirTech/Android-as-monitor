#include "pattern_source.h"

#include "clock.h"
#include "win_error.h"

#include <random>

using Microsoft::WRL::ComPtr;

namespace aum {

static constexpr std::uint32_t kWhite = 0xFFFFFFFF;
static constexpr std::uint32_t kBlack = 0xFF000000;
static constexpr std::uint32_t kBackground = 0xFF202020;
static constexpr std::uint32_t kBarWidth = 32;
static constexpr std::uint32_t kBarStepPixels = 8;
static constexpr std::uint32_t kScrollStepPixels = 7;
static constexpr std::uint32_t kDetailBlockPixels = 3;
// Sleeping the last stretch is too coarse for an 8 ms frame, so the end of the wait spins.
static constexpr std::int64_t kSpinBeforeTickNs = 1'500'000;

static D3D11_TEXTURE2D_DESC ImmutableBgra(std::uint32_t width, std::uint32_t height) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    return desc;
}

PatternSource::PatternSource(GpuDevice& gpu, std::uint32_t width, std::uint32_t height, Rational refresh, PatternKind kind)
    : gpu_(gpu), width_(width), height_(height), kind_(kind) {
    frameIntervalNs_ = static_cast<std::int64_t>(1'000'000'000ull * refresh.denominator / refresh.numerator);
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (kind_ == PatternKind::PixelBands) {
        CreatePixelBandsTexture();
    } else {
        CreateScrollingDetailTexture();
    }
}

PatternSource::~PatternSource() {
    if (timer_) {
        CloseHandle(timer_);
    }
}

void PatternSource::CreatePixelBandsTexture() {
    std::vector<std::uint32_t> pixels(std::size_t{width_} * height_, kBackground);
    for (std::uint32_t y = 0; y < height_; ++y) {
        for (std::uint32_t x = 0; x < width_; ++x) {
            std::uint32_t& pixel = pixels[std::size_t{y} * width_ + x];
            if (y < kPatternBandRows) {
                pixel = (x % 2 == 0) ? kWhite : kBlack;
            } else if (y < 2 * kPatternBandRows) {
                pixel = (y % 2 == 0) ? kWhite : kBlack;
            }
            if (x == 0 || y == 0 || x == width_ - 1 || y == height_ - 1) {
                pixel = kWhite;
            }
        }
    }
    const D3D11_TEXTURE2D_DESC desc = ImmutableBgra(width_, height_);
    const D3D11_SUBRESOURCE_DATA data{pixels.data(), width_ * 4, 0};
    ThrowIfFailed(gpu_.device->CreateTexture2D(&desc, &data, &staticFrame_), "pattern CreateTexture2D");
}

void PatternSource::CreateScrollingDetailTexture() {
    const std::uint32_t detailWidth = width_ * 2;
    std::vector<std::uint32_t> pixels(std::size_t{detailWidth} * height_);
    std::mt19937 random(1234);
    std::uniform_int_distribution<std::uint32_t> anyColor(0, 0xFFFFFF);
    for (std::uint32_t y = 0; y < height_; ++y) {
        for (std::uint32_t x = 0; x < detailWidth; ++x) {
            const bool startsBlock = (x % kDetailBlockPixels == 0) && (y % kDetailBlockPixels == 0);
            std::uint32_t& pixel = pixels[std::size_t{y} * detailWidth + x];
            if (startsBlock) {
                pixel = 0xFF000000 | anyColor(random);
            } else if (x % kDetailBlockPixels != 0) {
                pixel = (&pixel)[-1];
            } else {
                pixel = pixels[std::size_t{y - 1} * detailWidth + x];
            }
        }
    }
    const D3D11_TEXTURE2D_DESC desc = ImmutableBgra(detailWidth, height_);
    const D3D11_SUBRESOURCE_DATA data{pixels.data(), detailWidth * 4, 0};
    ThrowIfFailed(gpu_.device->CreateTexture2D(&desc, &data, &staticFrame_), "pattern CreateTexture2D");
}

void PatternSource::WaitForNextTick(HANDLE stopEvent) {
    if (nextTickNs_ == 0) {
        nextTickNs_ = NowNs();
    }
    nextTickNs_ += frameIntervalNs_;
    // After a stall, carry on from now rather than rushing through frames to catch up.
    if (nextTickNs_ < NowNs()) {
        nextTickNs_ = NowNs();
    }
    const std::int64_t sleepNs = nextTickNs_ - NowNs() - kSpinBeforeTickNs;
    if (sleepNs > 0 && timer_) {
        LARGE_INTEGER due;
        due.QuadPart = -(sleepNs / 100);
        SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE);
        const HANDLE handles[] = {timer_, stopEvent};
        WaitForMultipleObjects(ARRAYSIZE(handles), handles, FALSE, INFINITE);
    }
    while (NowNs() < nextTickNs_ && WaitForSingleObject(stopEvent, 0) != WAIT_OBJECT_0) {
        YieldProcessor();
    }
}

void PatternSource::DrawPixelBands(ID3D11Texture2D* target) {
    gpu_.context->CopyResource(target, staticFrame_.Get());

    ComPtr<ID3D11RenderTargetView> view;
    ThrowIfFailed(gpu_.device->CreateRenderTargetView(target, nullptr, &view), "pattern CreateRenderTargetView");
    ComPtr<ID3D11DeviceContext1> context;
    ThrowIfFailed(gpu_.context.As(&context), "ID3D11DeviceContext1 query");
    const LONG barLeft = static_cast<LONG>((frameIndex_ * kBarStepPixels) % (width_ - kBarWidth));
    // The lower half stays still, so the border there can be checked in any frame.
    const D3D11_RECT bar{barLeft, static_cast<LONG>(2 * kPatternBandRows + 1), barLeft + static_cast<LONG>(kBarWidth), static_cast<LONG>(height_ / 2)};
    static constexpr float kBarColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    context->ClearView(view.Get(), kBarColor, &bar, 1);
}

void PatternSource::DrawScrollingDetail(ID3D11Texture2D* target) {
    const std::uint32_t offset = static_cast<std::uint32_t>((frameIndex_ * kScrollStepPixels) % width_);
    const D3D11_BOX source{offset, 0, 0, offset + width_, height_, 1};
    gpu_.context->CopySubresourceRegion(target, 0, 0, 0, 0, staticFrame_.Get(), 0, &source);
}

bool PatternSource::Next(ID3D11Texture2D* target, CapturedFrame& frame, HANDLE stopEvent) {
    WaitForNextTick(stopEvent);
    if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
        return false;
    }
    if (kind_ == PatternKind::PixelBands) {
        DrawPixelBands(target);
    } else {
        DrawScrollingDetail(target);
    }
    ++frameIndex_;
    frame.capturedNs = NowNs();
    frame.isRepeat = false;
    return true;
}

}  // namespace aum
