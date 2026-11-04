// OpenSeeFace reference contour: Copyright (c) 2019 Emiliana, BSD-2-Clause.
#include "cortex/filters/HeadPose.h"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <limits>
namespace cortex {
namespace {
struct Point {
    int index;
    double x, y, z;
};
// OpenSeeFace's contour expressed in image-right/image-down/camera-forward axes.
constexpr Point reference[]{{0, -.455176969, -.300895790, .764429434},
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
                            {35, .080626352, .041276068, .134161036}};
using Parameters = std::array<double, 6>;  // pitch, yaw, roll, tx, ty, tz
using Residuals = std::array<double, 28>;
Residuals project(const Parameters& p, double focal, double cx, double cy) {
    const double sx = std::sin(p[0]), cxr = std::cos(p[0]), sy = std::sin(p[1]),
                 cyr = std::cos(p[1]), sz = std::sin(p[2]), cz = std::cos(p[2]);
    Residuals result{};
    for (size_t i = 0; i < std::size(reference); ++i) {
        const auto& q = reference[i];
        const double y = cxr * q.y - sx * q.z, z = sx * q.y + cxr * q.z;
        const double x2 = cyr * q.x + sy * z, z2 = -sy * q.x + cyr * z;
        const double x3 = cz * x2 - sz * y + p[3], y3 = sz * x2 + cz * y + p[4], depth = z2 + p[5];
        if (depth <= .05) {
            result.fill(1e8);
            return result;
        }
        result[2 * i] = cx + focal * x3 / depth;
        result[2 * i + 1] = cy + focal * y3 / depth;
    }
    return result;
}
bool solve(double a[6][7], Parameters& answer) {
    for (int col = 0; col < 6; ++col) {
        int pivot = col;
        for (int row = col + 1; row < 6; ++row)
            if (std::abs(a[row][col]) > std::abs(a[pivot][col]))
                pivot = row;
        if (std::abs(a[pivot][col]) < 1e-12)
            return false;
        for (int k = col; k < 7; ++k)
            std::swap(a[col][k], a[pivot][k]);
        const double v = a[col][col];
        for (int k = col; k < 7; ++k)
            a[col][k] /= v;
        for (int row = 0; row < 6; ++row)
            if (row != col) {
                const double factor = a[row][col];
                for (int k = col; k < 7; ++k)
                    a[row][k] -= factor * a[col][k];
            }
    }
    for (int i = 0; i < 6; ++i)
        answer[i] = a[i][6];
    return true;
}
}  // namespace
std::optional<HeadRotation> estimateHeadRotation(const std::array<LandmarkPoint, 66>& landmarks,
                                                 unsigned width, unsigned height) {
    if (!width || !height)
        return {};
    const double focal = std::max(width, height), cx = width * .5, cy = height * .5;
    const double eyeX =
        (landmarks[42].x + landmarks[45].x - landmarks[36].x - landmarks[39].x) * .5;
    const double eyeY =
        (landmarks[42].y + landmarks[45].y - landmarks[36].y - landmarks[39].y) * .5;
    const double span = std::hypot(eyeX, eyeY);
    if (!std::isfinite(span) || span < 4)
        return {};
    Residuals observed{};
    for (size_t i = 0; i < std::size(reference); ++i) {
        const auto& p = landmarks[reference[i].index];
        if (!std::isfinite(p.x) || !std::isfinite(p.y))
            return {};
        observed[2 * i] = p.x;
        observed[2 * i + 1] = p.y;
    }
    const double depth = std::clamp(focal * .447 / span - .26, .5, 100.);
    Parameters p{0,
                 0,
                 std::atan2(eyeY, eyeX),
                 (landmarks[30].x - cx) * depth / focal,
                 (landmarks[30].y - cy) * depth / focal,
                 depth};
    const auto error = [&](const Residuals& predicted) {
        double sum = 0;
        for (size_t i = 0; i < observed.size(); ++i) {
            const double v = predicted[i] - observed[i];
            sum += v * v;
        }
        return sum;
    };
    double lambda = .001;
    for (int iteration = 0; iteration < 40; ++iteration) {
        const auto predicted = project(p, focal, cx, cy);
        const double before = error(predicted);
        std::array<Residuals, 6> derivative;
        constexpr double epsilon = .0001;
        for (int k = 0; k < 6; ++k) {
            auto test = p;
            test[k] += epsilon;
            derivative[k] = project(test, focal, cx, cy);
            for (size_t i = 0; i < 28; ++i)
                derivative[k][i] = (derivative[k][i] - predicted[i]) / epsilon;
        }
        double system[6][7]{};
        for (int row = 0; row < 6; ++row) {
            for (int col = 0; col < 6; ++col)
                for (size_t i = 0; i < 28; ++i)
                    system[row][col] += derivative[row][i] * derivative[col][i];
            for (size_t i = 0; i < 28; ++i)
                system[row][6] += derivative[row][i] * (observed[i] - predicted[i]);
            system[row][row] += lambda * std::max(1., system[row][row]);
        }
        Parameters step{};
        if (!solve(system, step))
            return {};
        auto candidate = p;
        for (int k = 0; k < 6; ++k)
            candidate[k] += step[k];
        for (int k = 0; k < 3; ++k)
            candidate[k] = std::clamp(candidate[k], -1.4, 1.4);
        candidate[5] = std::clamp(candidate[5], .5, 100.);
        if (error(project(candidate, focal, cx, cy)) < before) {
            p = candidate;
            lambda = std::max(1e-8, lambda * .3);
        } else
            lambda = std::min(1e8, lambda * 10);
    }
    const double rms = std::sqrt(error(project(p, focal, cx, cy)) / 28);
    if (!std::isfinite(rms) || rms > std::max(4., span * .25))
        return {};
    constexpr double degrees = 180 / std::numbers::pi;
    // Cubism Y is up; screen coordinates are down. Mirror yaw/roll in UI if desired.
    return HeadRotation{float(-p[1] * degrees), float(-p[0] * degrees), float(-p[2] * degrees)};
}
}  // namespace cortex
