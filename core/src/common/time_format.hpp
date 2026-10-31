#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace noting::detail {

// Windows FILETIME(1601-01-01 기준 100ns 단위) → "YYYY-MM-DDTHH:MM:SSZ".
// <ctime> 의 시간대/로케일 영향을 받지 않도록 직접 계산한다.
inline std::string fileTimeToIso8601(std::uint64_t fileTime) {
    constexpr std::int64_t kEpochDiffSeconds = 11644473600LL;
    if (fileTime == 0) return {};
    const std::int64_t seconds = static_cast<std::int64_t>(fileTime / 10000000ULL) - kEpochDiffSeconds;

    std::int64_t days = seconds / 86400;
    std::int64_t rem = seconds % 86400;
    if (rem < 0) {
        rem += 86400;
        --days;
    }

    // civil_from_days (Howard Hinnant)
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const std::int64_t doe = days - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    const std::int64_t day = doy - (153 * mp + 2) / 5 + 1;
    const std::int64_t month = mp < 10 ? mp + 3 : mp - 9;
    const std::int64_t year = yoe + era * 400 + (month <= 2 ? 1 : 0);

    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04lld-%02lld-%02lldT%02lld:%02lld:%02lldZ",
                  static_cast<long long>(year), static_cast<long long>(month),
                  static_cast<long long>(day), static_cast<long long>(rem / 3600),
                  static_cast<long long>((rem % 3600) / 60), static_cast<long long>(rem % 60));
    return buffer;
}

}  // namespace noting::detail
