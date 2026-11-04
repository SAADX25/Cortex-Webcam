#include "cortex/filters/HeadPose.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
using namespace cortex;
namespace {
struct Sample {
    int index;
    double x, y, z;
};
constexpr Sample shape[]{{0, -.455176969, -.300895790, .764429434},
                         {1, -.448998827, -.166995838, .765143004},
                         {8, 0, .621079019, .287294771},
                         {15, .448998827, -.166995838, .765143004},
                         {16, .455176969, -.300895790, .764429434},
                         {27, 0, -.293332603, .137582089},
                         {28, 0, -.194828702, .069158109},
                         {29, 0, -.103844017, .009151820},
                         {30, 0, 0, 0},
                         {31, -.080626352, .041276068, .134161036},
                         {32, -.046439347, .057675224, .102990627},
                         {33, 0, .068753126, .090545348},
                         {34, .046439347, .057675224, .102990627},
                         {35, .080626352, .041276068, .134161036},
                         {36, -.315905196, -.298337503, .285107408},
                         {39, -.131229724, -.284447362, .234239149},
                         {42, .131229724, -.284447362, .234239149},
                         {45, .315905196, -.298337503, .285107408}};
void check(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
}  // namespace
int main() {
    try {
        int cases = 0;
        for (auto [w, h] : {std::pair{640u, 480u}, std::pair{288u, 512u}})
            for (auto angle : {HeadRotation{}, HeadRotation{30, 0, 0}, HeadRotation{-30, 0, 0},
                               HeadRotation{0, 25, 0}, HeadRotation{0, -25, 0},
                               HeadRotation{0, 0, 20}, HeadRotation{20, -15, -12}}) {
                const double radians = std::numbers::pi / 180;
                const double x = -angle.pitch * radians, y = -angle.yaw * radians,
                             z = -angle.roll * radians;
                const double a = std::cos(x), b = std::sin(x), c = std::cos(y), d = std::sin(y),
                             e = std::cos(z), f = std::sin(z);
                // Independent pinhole projection of known rotations, depth and off-center
                // translation.
                const double r[3][3]{{e * c, e * d * b - f * a, e * d * a + f * b},
                                     {f * c, f * d * b + e * a, f * d * a - e * b},
                                     {-d, c * b, c * a}};
                std::array<LandmarkPoint, 66> pixels{};
                for (const auto& q : shape) {
                    const double px = r[0][0] * q.x + r[0][1] * q.y + r[0][2] * q.z + .12;
                    const double py = r[1][0] * q.x + r[1][1] * q.y + r[1][2] * q.z - .08;
                    const double pz = r[2][0] * q.x + r[2][1] * q.y + r[2][2] * q.z + 5;
                    pixels[q.index] = {float(w * .5 + std::max(w, h) * px / pz),
                                       float(h * .5 + std::max(w, h) * py / pz)};
                }
                const auto estimated = estimateHeadRotation(pixels, w, h);
                check(estimated.has_value(),
                      "Head pose fitting must converge for known camera rotations");
                if (std::abs(estimated->yaw - angle.yaw) > 1 ||
                    std::abs(estimated->pitch - angle.pitch) > 1 ||
                    std::abs(estimated->roll - angle.roll) > 1) {
                    std::cerr << "Expected " << angle.yaw << "," << angle.pitch << "," << angle.roll
                              << " got " << estimated->yaw << "," << estimated->pitch << ","
                              << estimated->roll << '\n';
                    throw std::runtime_error("Head pose recovered the wrong direction or scale");
                }
                pixels[30].x = std::numeric_limits<float>::quiet_NaN();
                check(!estimateHeadRotation(pixels, w, h),
                      "Non-finite head input must be rejected");
                ++cases;
            }
        std::cout
            << "PASS: " << cases
            << " perspective head poses, both phone orientations, translation and invalid input.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
