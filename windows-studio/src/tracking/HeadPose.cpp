// OpenSeeFace reference contour: Copyright (c) 2019 Emiliana, BSD-2-Clause.
#include "cortex/tracking/HeadPose.h"
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
                            {35, .080626352, .041276068, .134161036},
                            {36, -.315905196, -.298337503, .285107408},
                            {39, -.131229724, -.284447362, .234239149},
                            {42, .131229724, -.284447362, .234239149},
                            {45, .315905196, -.298337503, .285107408}};
using Parameters = std::array<double, 6>;  // pitch, yaw, roll, tx, ty, tz
using Residuals = std::array<double, std::size(reference) * 2>;
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
std::optional<HeadPoseEstimate> estimateHeadPose(const std::array<LandmarkPoint, 66>& landmarks,
                                                 unsigned width, unsigned height,
                                                 float sensitivity,
                                                 const std::array<float, 66>* confidence) {
    if (!width || !height || !std::isfinite(sensitivity) || sensitivity <= 0)
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
    std::array<double, std::size(reference)> reliability{};
    unsigned usable = 0;
    for (size_t i = 0; i < std::size(reference); ++i) {
        const auto& p = landmarks[reference[i].index];
        if (!std::isfinite(p.x) || !std::isfinite(p.y))
            return {};
        observed[2 * i] = p.x;
        observed[2 * i + 1] = p.y;
        const double certainty = confidence ? (*confidence)[reference[i].index] : 1.;
        if (!std::isfinite(certainty))
            return {};
        const double weight = confidence ? std::clamp((certainty - .25) / .5, 0., 1.) : 1.;
        // The outer jaw follows the silhouette when the head turns. Eye corners
        // and the nose provide more stable rigid-head constraints.
        const double rigidity = i < 5 ? .65 : (i >= 14 ? 1.3 : 1.);
        reliability[i] = weight * weight * rigidity;
        usable += reliability[i] > .1;
    }
    if (usable < 10)
        return {};
    const double depth = std::clamp(focal * .447 / span - .26, .5, 100.);
    const Parameters initial{0,
                 0,
                 std::atan2(eyeY, eyeX),
                 (landmarks[30].x - cx) * depth / focal,
                 (landmarks[30].y - cy) * depth / focal,
                 depth};
    const double threshold = std::max(1.5, span * .06);
    const auto error = [&](const Residuals& predicted) {
        double sum = 0;
        for (size_t i = 0; i < std::size(reference); ++i) {
            const double dx = predicted[2 * i] - observed[2 * i];
            const double dy = predicted[2 * i + 1] - observed[2 * i + 1];
            const double distance = std::hypot(dx, dy);
            sum += reliability[i] * (distance <= threshold
                                         ? distance * distance
                                         : threshold * (2 * distance - threshold));
        }
        return sum;
    };
    Parameters best = initial;
    double bestError = std::numeric_limits<double>::infinity();
    // A single frontal initialization can converge to the wrong perspective
    // solution on a turned face. These starts add negligible cost beside ONNX.
    for (const auto start : {std::pair{0., 0.}, std::pair{0., -.55}, std::pair{0., .55},
                             std::pair{-.4, 0.}, std::pair{.4, 0.}}) {
      auto p = initial;
      p[0] = start.first;
      p[1] = start.second;
      double lambda = .001;
      for (int iteration = 0; iteration < 45; ++iteration) {
        const auto predicted = project(p, focal, cx, cy);
        const double before = error(predicted);
        if (before < 1e-10)
            break;
        Residuals weights{};
        for (size_t i = 0; i < std::size(reference); ++i) {
            const double distance = std::hypot(predicted[2 * i] - observed[2 * i],
                                                predicted[2 * i + 1] - observed[2 * i + 1]);
            weights[2 * i] = weights[2 * i + 1] =
                reliability[i] * std::min(1., threshold / std::max(1e-9, distance));
        }
        std::array<Residuals, 6> derivative;
        constexpr double epsilon = .0001;
        for (int k = 0; k < 6; ++k) {
            auto test = p;
            test[k] += epsilon;
            derivative[k] = project(test, focal, cx, cy);
            for (size_t i = 0; i < observed.size(); ++i)
                derivative[k][i] = (derivative[k][i] - predicted[i]) / epsilon;
        }
        double system[6][7]{};
        for (int row = 0; row < 6; ++row) {
            for (int col = 0; col < 6; ++col)
                for (size_t i = 0; i < observed.size(); ++i)
                    system[row][col] += weights[i] * derivative[row][i] * derivative[col][i];
            for (size_t i = 0; i < observed.size(); ++i)
                system[row][6] += weights[i] * derivative[row][i] * (observed[i] - predicted[i]);
            system[row][row] += lambda * std::max(1., system[row][row]);
        }
        Parameters step{};
        if (!solve(system, step))
            break;
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
      const double score = error(project(p, focal, cx, cy));
      if (score < bestError) {
          best = p;
          bestError = score;
      }
      if (bestError < 1e-8)
          break;
    }
    const auto predicted = project(best, focal, cx, cy);
    double inlierError = 0, inlierWeight = 0;
    unsigned inliers = 0;
    for (size_t i = 0; i < std::size(reference); ++i) {
        const double distance = std::hypot(predicted[2 * i] - observed[2 * i],
                                            predicted[2 * i + 1] - observed[2 * i + 1]);
        if (reliability[i] > .1 && distance <= std::max(3., span * .12)) {
            inlierError += reliability[i] * distance * distance;
            inlierWeight += reliability[i];
            ++inliers;
        }
    }
    const double rms = std::sqrt(inlierError / std::max(1e-9, inlierWeight));
    if (inliers < 10 || !std::isfinite(rms) || rms > std::max(3., span * .12))
        return {};
    constexpr double degrees = 180 / std::numbers::pi;
    return HeadPoseEstimate{
        {static_cast<float>(-best[1] * degrees * sensitivity),
         static_cast<float>(-best[0] * degrees * sensitivity),
         static_cast<float>(-best[2] * degrees * sensitivity)},
        static_cast<float>(focal * best[3] / (best[5] * width)),
        static_cast<float>(focal * best[4] / (best[5] * height)),
        static_cast<float>(best[5])};
}
std::optional<HeadRotation> estimateHeadRotation(const std::array<LandmarkPoint, 66>& landmarks,
                                                 unsigned width, unsigned height,
                                                 float sensitivity,
                                                 const std::array<float, 66>* confidence) {
    if (auto pose = estimateHeadPose(landmarks, width, height, sensitivity, confidence))
        return pose->rotation;
    return {};
}
}  // namespace cortex
