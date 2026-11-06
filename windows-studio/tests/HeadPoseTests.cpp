#include "cortex/tracking/HeadPose.h"
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
std::array<LandmarkPoint, 66> projectShape(HeadRotation angle, unsigned w, unsigned h,
                                           double tx = .12, double ty = -.08, double depth = 5) {
    const double radians = std::numbers::pi / 180;
    const double x = -angle.pitch * radians, y = -angle.yaw * radians,
                 z = -angle.roll * radians;
    const double a = std::cos(x), b = std::sin(x), c = std::cos(y), d = std::sin(y),
                 e = std::cos(z), f = std::sin(z);
    // Independent pinhole projection, including perspective and translation.
    const double r[3][3]{{e * c, e * d * b - f * a, e * d * a + f * b},
                         {f * c, f * d * b + e * a, f * d * a - e * b},
                         {-d, c * b, c * a}};
    std::array<LandmarkPoint, 66> pixels{};
    for (const auto& q : shape) {
        const double px = r[0][0] * q.x + r[0][1] * q.y + r[0][2] * q.z + tx;
        const double py = r[1][0] * q.x + r[1][1] * q.y + r[1][2] * q.z + ty;
        const double pz = r[2][0] * q.x + r[2][1] * q.y + r[2][2] * q.z + depth;
        pixels[q.index] = {float(w * .5 + std::max(w, h) * px / pz),
                           float(h * .5 + std::max(w, h) * py / pz)};
    }
    return pixels;
}
}  // namespace
int main() {
    try {
        int cases = 0;
        for (auto [w, h] : {std::pair{640u, 480u}, std::pair{288u, 512u},
                            std::pair{1920u, 1080u}, std::pair{1080u, 1920u}})
            for (auto angle : {HeadRotation{}, HeadRotation{30, 0, 0}, HeadRotation{-30, 0, 0},
                               HeadRotation{0, 25, 0}, HeadRotation{0, -25, 0},
                               HeadRotation{0, 0, 20}, HeadRotation{20, -15, -12},
                               HeadRotation{50, 0, 0}, HeadRotation{-50, 0, 0},
                               HeadRotation{0, 55, 0}, HeadRotation{0, -45, 0},
                               HeadRotation{35, 35, 15}}) {
                auto pixels = projectShape(angle, w, h);
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
                const auto scaled = estimateHeadRotation(pixels, w, h, 1.5f);
                check(scaled.has_value(), "Scaled head pose must converge");
                check(std::abs(scaled->yaw - angle.yaw * 1.5f) < 1.5f &&
                          std::abs(scaled->pitch - angle.pitch * 1.5f) < 1.5f &&
                          std::abs(scaled->roll - angle.roll * 1.5f) < 1.5f,
                      "Physical head angles must retain range before neutral calibration");
                pixels[30].x = std::numeric_limits<float>::quiet_NaN();
                check(!estimateHeadRotation(pixels, w, h),
                      "Non-finite head input must be rejected");
                ++cases;
            }
        for (double depth : {3.5, 5., 7.})
            for (auto rotation : {HeadRotation{}, HeadRotation{40, -20, 15},
                                  HeadRotation{-35, 25, -10}}) {
                const auto projected = projectShape(rotation, 768, 432, .35, -.2, depth);
                const auto fitted = estimateHeadPose(projected, 768, 432);
                check(fitted && std::abs(fitted->depth - depth) < .03 &&
                          std::abs(fitted->positionX - .35 / depth) < .003 &&
                          std::abs(fitted->positionY - 768. * -.2 / (432 * depth)) < .003,
                      "Perspective position and relative depth must remain correct during head turns");
            }
        auto corrupted = projectShape({25, 35, -10}, 768, 432, .8, -.3);
        std::array<float, 66> confidence;
        confidence.fill(1);
        for (int index : {0, 16, 34}) {
            corrupted[index].x += 50;
            corrupted[index].y -= 35;
            confidence[index] = .1f;
        }
        const auto robust = estimateHeadRotation(corrupted, 768, 432, 1.f, &confidence);
        check(robust && std::abs(robust->yaw - 25) < 2 &&
                  std::abs(robust->pitch - 35) < 2 && std::abs(robust->roll + 10) < 2,
              "Low-confidence occluded landmarks must not pull the physical head pose");
        confidence.fill(.1f);
        check(!estimateHeadRotation(corrupted, 768, 432, 1.f, &confidence),
              "Insufficient reliable head landmarks must be rejected");
        check(!estimateHeadRotation(corrupted, 768, 432,
                                     std::numeric_limits<float>::quiet_NaN()),
              "A non-finite sensitivity must be rejected");
        std::cout
            << "PASS: " << cases
            << " perspective head poses, physical range, confidence gating, orientations and translation.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
