#include "noting/unit.hpp"

#include <cmath>
#include <cstdint>

namespace noting {
namespace {

int decimalPlaces(int places) {
    if (places < 0) return 0;
    if (places > 2) return 2;
    return places;
}

std::int64_t truncTowardZero(std::int64_t num, std::int64_t den) {
    if (den <= 0) return 0;
    if (num >= 0) return num / den;
    return -((-num) / den);
}

// 0.5는 0에서 멀어진다.
std::int64_t roundAway(std::int64_t num, std::int64_t den) {
    if (den <= 0) return 0;
    if (num >= 0) return (num + den / 2) / den;
    return -((-num + den / 2) / den);
}

std::int32_t clampHu(std::int64_t hu) {
    if (hu > INT32_MAX) return INT32_MAX;
    if (hu < INT32_MIN) return INT32_MIN;
    return static_cast<std::int32_t>(hu);
}

}  // namespace

std::int32_t mmToHu(double mm) {
    if (!std::isfinite(mm)) return 0;
    // 부동소수 잡음만 지운 뒤 mm × 283465 / 1000 을 버린다.
    const auto micro = static_cast<std::int64_t>(std::llround(mm * 1000000.0));
    const std::int64_t hu = truncTowardZero(micro * 283465, 1000000LL * 1000LL);
    return clampHu(hu);
}

std::int32_t ptToHu(double pt) {
    if (!std::isfinite(pt)) return 0;
    const auto micro = static_cast<std::int64_t>(std::llround(pt * 1000000.0));
    return clampHu(truncTowardZero(micro, 10000));
}

double huToMm(std::int32_t hu, int places) {
    const int digits = decimalPlaces(places);
    const std::int64_t scale = digits == 0 ? 1 : digits == 1 ? 10 : 100;
    const std::int64_t rounded = roundAway(static_cast<std::int64_t>(hu) * 1000 * scale, 283465);
    return static_cast<double>(rounded) / static_cast<double>(scale);
}

double huToPt(std::int32_t hu, int places) {
    const int digits = decimalPlaces(places);
    const std::int64_t scale = digits == 0 ? 1 : digits == 1 ? 10 : 100;
    const std::int64_t rounded = roundAway(static_cast<std::int64_t>(hu) * scale, 100);
    return static_cast<double>(rounded) / static_cast<double>(scale);
}

}  // namespace noting
