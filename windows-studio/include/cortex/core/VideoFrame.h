#pragma once
#include "cortex/gpu/GpuDevice.h"
#include <chrono>
#include <cstdint>

namespace cortex {
// Common source format: GPU-resident NV12 on the manager's D3D11 device.
// Decoder textures may be arrays: consumers MUST use arraySlice, not slice zero.
struct VideoFrame {
    ComPtr<ID3D11Texture2D> texture;
    UINT arraySlice{};
    uint32_t width{},height{}; // Visible size, not padded texture dimensions.
    int64_t sourceTimestamp90k{};
    int rotationDegrees{};
    std::chrono::steady_clock::time_point receivedAt;
    std::chrono::steady_clock::time_point readyAt;
    enum class Matrix { Bt601, Bt709, Bt2020, Unspecified } matrix{Matrix::Unspecified};
    enum class Range { Limited, Full, Unspecified } range{Range::Unspecified};
    // Keeps the decoder pool slot alive, not just the COM texture resource.
    std::shared_ptr<void> backingOwner;
    std::shared_ptr<GpuDevice> gpu;
};
using FramePtr=std::shared_ptr<const VideoFrame>;
}
