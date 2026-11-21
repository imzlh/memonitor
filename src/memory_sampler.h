#pragma once

#include <QDateTime>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include <cstdint>
#include <limits>

namespace memonitor {

struct ProcessMemory {
    qint64 pid = 0;
    qint64 ppid = -1;
    std::uint64_t uid = std::numeric_limits<std::uint64_t>::max();
    QString userName;
    QString name;
    QString command;
    QString executable;
    std::uint64_t rssBytes = 0;
    std::uint64_t pssBytes = 0;
    std::uint64_t swapBytes = 0;
    std::uint64_t swapPssBytes = 0;
    std::uint64_t startTimeTicks = 0;
    bool pssExact = false;
    bool swapPssExact = false;
    bool approximate = false;

    [[nodiscard]] std::uint64_t residentBytes() const
    {
        return pssBytes;
    }

    [[nodiscard]] std::uint64_t footprintBytes() const
    {
        if (swapPssBytes > std::numeric_limits<std::uint64_t>::max() - pssBytes) {
            return std::numeric_limits<std::uint64_t>::max();
        }
        return pssBytes + swapPssBytes;
    }
};

struct ZramDevice {
    QString name;
    QString compressionAlgorithm;
    std::uint64_t diskSizeBytes = 0;
    std::uint64_t originalDataBytes = 0;
    std::uint64_t compressedDataBytes = 0;
    std::uint64_t memoryUsedBytes = 0;
    std::uint64_t memoryLimitBytes = 0;
    std::uint64_t failedWrites = 0;
    bool statisticsReadable = false;
    bool ioStatisticsReadable = false;
};

struct MemorySnapshot {
    QDateTime timestamp;
    std::uint64_t memoryTotalBytes = 0;
    std::uint64_t memoryAvailableBytes = 0;
    std::uint64_t memoryFreeBytes = 0;
    std::uint64_t memoryUsedBytes = 0;
    std::uint64_t swapTotalBytes = 0;
    std::uint64_t swapFreeBytes = 0;
    std::uint64_t swapUsedBytes = 0;
    bool memoryInfoReadable = false;
    bool memoryInfoComplete = false;
    bool memoryFreeReadable = false;
    bool swapInfoComplete = false;
    double memoryPressureSomeAvg10 = 0.0;
    double memoryPressureFullAvg10 = 0.0;
    bool memoryPressureReadable = false;
    std::uint64_t oomKillCount = 0;
    bool oomKillCountReadable = false;
    QVector<ProcessMemory> processes;
    QVector<ZramDevice> zramDevices;
    int processesSkipped = 0;
};

class MemorySampler final {
public:
    [[nodiscard]] static MemorySnapshot collect();
    [[nodiscard]] static MemorySnapshot collect(const QString& procRoot, const QString& sysBlockRoot);
};

} // namespace memonitor
