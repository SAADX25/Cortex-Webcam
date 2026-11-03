#include "cortex/decoding/HardwareH264Decoder.h"
#include <fstream>
#include <iterator>
#include <iostream>
#include <stdexcept>
#include <filesystem>
int main() {
    try {
        const auto path =
            std::filesystem::path(CORTEX_TEST_FIXTURES_DIR) / "plain-baseline-1080p.h264";
        std::ifstream input(path, std::ios::binary);
        if (!input)
            throw std::runtime_error("Missing synthetic Baseline fixture");
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(input)),
                                  std::istreambuf_iterator<char>());
        bool plain = false;
        for (size_t i = 0; i + 6 < data.size(); ++i)
            if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 && (data[i + 3] & 31) == 7)
                plain = data[i + 4] == 66 && (data[i + 5] & 64) == 0;
        if (!plain)
            throw std::runtime_error(
                "Fixture must reproduce plain Baseline, not Constrained Baseline");
        auto gpu = cortex::GpuDevice::create();
        unsigned frames = 0;
        cortex::HardwareH264Decoder decoder(gpu, [&](cortex::FramePtr frame) {
            if (frame->width != 1920 || frame->height != 1080 || frame->gpu != gpu ||
                !frame->texture)
                throw std::runtime_error("Incorrect GPU frame");
            D3D11_TEXTURE2D_DESC desc{};
            frame->texture->GetDesc(&desc);
            if (desc.Format != DXGI_FORMAT_NV12)
                throw std::runtime_error("Expected native GPU NV12");
            ++frames;
        });
        decoder.decode({std::move(data), 90000, cortex::Clock::now(), true});
        decoder.finish();
        if (frames != 1)
            throw std::runtime_error("Plain Baseline must decode one complete hardware frame");
        std::cout
            << "PASS: plain Baseline 1080p decodes to retained D3D11 NV12 without CPU fallback\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
