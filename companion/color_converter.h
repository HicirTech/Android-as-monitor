#pragma once

#include "gpu.h"

#include <cstdint>

namespace aum {

// BGRA to NV12 on the GPU, BT.709 limited range, the colour signalling hardware video decoders assume for HD.
class ColorConverter {
public:
    ColorConverter(GpuDevice& gpu, std::uint32_t width, std::uint32_t height);

    void Convert(ID3D11Texture2D* bgra, ID3D11Texture2D* nv12);

private:
    GpuDevice& gpu_;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> videoDevice_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext1> videoContext_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> processor_;
};

}  // namespace aum
