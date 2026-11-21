#pragma once

#include <cstdint>

namespace memonitor {

constexpr unsigned kWarningPercent = 30U;

inline std::uint64_t warningThresholdBytes(std::uint64_t totalBytes, unsigned percent = kWarningPercent)
{
    return totalBytes / 100U * percent
        + (totalBytes % 100U) * percent / 100U;
}

inline bool exceedsWarningThreshold(std::uint64_t valueBytes, std::uint64_t totalBytes,
                                    unsigned percent = kWarningPercent)
{
    if (totalBytes == 0) {
        return false;
    }

    return valueBytes > warningThresholdBytes(totalBytes, percent);
}

inline bool belowAvailableThreshold(std::uint64_t valueBytes, std::uint64_t totalBytes, unsigned percent)
{
    if (totalBytes == 0) {
        return false;
    }

    const std::uint64_t threshold = warningThresholdBytes(totalBytes, percent);
    return valueBytes < threshold
        || (valueBytes == threshold && (totalBytes % 100U) * percent % 100U != 0U);
}

} // namespace memonitor
