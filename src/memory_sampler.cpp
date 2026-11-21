#include "memory_sampler.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QIODevice>
#include <QList>
#include <QStringList>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <limits>
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>

namespace memonitor {
namespace {

using Bytes = std::uint64_t;

bool parseUnsignedCounter(const QByteArray& text, Bytes& value)
{
    if (text.isEmpty() || !std::all_of(text.cbegin(), text.cend(), [](char character) {
            return character >= '0' && character <= '9';
        })) {
        return false;
    }

    bool ok = false;
    const auto parsed = text.toULongLong(&ok);
    if (!ok) {
        return false;
    }
    value = parsed;
    return true;
}

bool parseBytesFromLine(const QByteArray& line, Bytes& bytes)
{
    const qsizetype colon = line.indexOf(':');
    if (colon < 0) {
        return false;
    }

    const QList<QByteArray> fields = line.mid(colon + 1).simplified().split(' ');
    if (fields.size() != 2 || fields.at(1).compare("kB", Qt::CaseInsensitive) != 0) {
        return false;
    }

    Bytes kibibytes = 0;
    if (!parseUnsignedCounter(fields.at(0), kibibytes)
        || kibibytes > std::numeric_limits<Bytes>::max() / 1024U) {
        return false;
    }
    bytes = kibibytes * 1024U;
    return true;
}

QString stringValueFromLine(const QByteArray& line)
{
    const qsizetype colon = line.indexOf(':');
    return colon < 0 ? QString() : QString::fromLocal8Bit(line.mid(colon + 1).trimmed());
}

bool readProcFile(const QString& path, QByteArray& contents)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    contents = file.readAll();
    return file.error() == QFileDevice::NoError;
}

bool readSingleValueFile(const QString& path, Bytes& value)
{
    QByteArray contents;
    if (!readProcFile(path, contents)) {
        return false;
    }
    return parseUnsignedCounter(contents.trimmed(), value);
}

QString readTextFile(const QString& path)
{
    QByteArray contents;
    if (!readProcFile(path, contents)) {
        return QString();
    }
    return QString::fromLocal8Bit(contents.trimmed());
}

bool parsePressureAverage(const QList<QByteArray>& fields, double& average)
{
    for (const QByteArray& field : fields) {
        if (!field.startsWith("avg10=")) {
            continue;
        }

        bool ok = false;
        const double parsed = field.mid(6).toDouble(&ok);
        if (!ok || !std::isfinite(parsed) || parsed < 0.0 || parsed > 100.0) {
            return false;
        }
        average = parsed;
        return true;
    }
    return false;
}

bool readMemoryPressure(const QString& path, double& someAverage, double& fullAverage)
{
    QByteArray contents;
    if (!readProcFile(path, contents)) {
        return false;
    }

    bool hasSome = false;
    bool hasFull = false;
    double parsedSome = 0.0;
    double parsedFull = 0.0;
    const QList<QByteArray> lines = contents.split('\n');
    for (const QByteArray& line : lines) {
        const QList<QByteArray> fields = line.simplified().split(' ');
        if (fields.isEmpty()) {
            continue;
        }
        if (fields.constFirst() == "some") {
            hasSome = parsePressureAverage(fields, parsedSome);
        } else if (fields.constFirst() == "full") {
            hasFull = parsePressureAverage(fields, parsedFull);
        }
    }

    if (!hasSome || !hasFull) {
        return false;
    }
    someAverage = parsedSome;
    fullAverage = parsedFull;
    return true;
}

bool readOomKillCount(const QString& path, Bytes& count)
{
    QByteArray contents;
    if (!readProcFile(path, contents)) {
        return false;
    }

    const QList<QByteArray> lines = contents.split('\n');
    for (const QByteArray& line : lines) {
        const QList<QByteArray> fields = line.simplified().split(' ');
        if (!fields.isEmpty() && fields.constFirst() == "oom_kill") {
            return fields.size() == 2 && parseUnsignedCounter(fields.at(1), count);
        }
    }
    return false;
}

bool parseFirstUnsignedValue(const QByteArray& line, Bytes& value)
{
    const qsizetype colon = line.indexOf(':');
    if (colon < 0) {
        return false;
    }

    const QList<QByteArray> fields = line.mid(colon + 1).simplified().split(' ');
    if (fields.isEmpty()) {
        return false;
    }

    return parseUnsignedCounter(fields.constFirst(), value);
}

bool readProcessStartTime(const QString& procPath, qint64 pid, Bytes& startTime)
{
    QByteArray contents;
    if (!readProcFile(procPath + QStringLiteral("/stat"), contents)) {
        return false;
    }

    const qsizetype openingName = contents.indexOf('(');
    const qsizetype closingName = contents.lastIndexOf(')');
    Bytes statPid = 0;
    if (openingName <= 0 || closingName <= openingName
        || !parseUnsignedCounter(contents.left(openingName).trimmed(), statPid)
        || statPid != static_cast<Bytes>(pid)) {
        return false;
    }

    const QList<QByteArray> fields = contents.mid(closingName + 1).simplified().split(' ');
    return fields.size() > 19 && parseUnsignedCounter(fields.at(19), startTime);
}

QString userNameForUid(Bytes uid)
{
    if (uid > static_cast<Bytes>(std::numeric_limits<uid_t>::max())) {
        return QString();
    }

    long configuredBufferSize = ::sysconf(_SC_GETPW_R_SIZE_MAX);
    if (configuredBufferSize <= 0) {
        configuredBufferSize = 16 * 1024;
    }
    constexpr long maxBufferSize = 1024 * 1024;
    const long bufferSize = std::min(configuredBufferSize, maxBufferSize);
    QByteArray buffer(static_cast<qsizetype>(bufferSize), '\0');

    passwd passwordEntry{};
    passwd* result = nullptr;
    const int error = ::getpwuid_r(static_cast<uid_t>(uid),
                                   &passwordEntry,
                                   buffer.data(),
                                   static_cast<std::size_t>(buffer.size()),
                                   &result);
    if (error != 0 || result == nullptr || result->pw_name == nullptr) {
        return QString();
    }
    return QString::fromLocal8Bit(result->pw_name);
}

struct ProcessMetrics {
    Bytes rssBytes = 0;
    Bytes pssBytes = 0;
    Bytes swapBytes = 0;
    Bytes swapPssBytes = 0;
    bool hasRss = false;
    bool hasPss = false;
    bool hasSwap = false;
    bool hasSwapPss = false;
};

void readMetricLines(const QByteArray& contents, ProcessMetrics& metrics)
{
    const QList<QByteArray> lines = contents.split('\n');
    for (const QByteArray& line : lines) {
        const qsizetype colon = line.indexOf(':');
        if (colon < 0) {
            continue;
        }

        const QByteArray key = line.left(colon).trimmed();
        Bytes value = 0;
        if (!parseBytesFromLine(line, value)) {
            continue;
        }
        if (key == "Rss") {
            metrics.rssBytes = value;
            metrics.hasRss = true;
        } else if (key == "Pss") {
            metrics.pssBytes = value;
            metrics.hasPss = true;
        } else if (key == "Swap") {
            metrics.swapBytes = value;
            metrics.hasSwap = true;
        } else if (key == "SwapPss") {
            metrics.swapPssBytes = value;
            metrics.hasSwapPss = true;
        }
    }
}

bool readProcess(const QString& procRoot,
                 qint64 pid,
                 ProcessMemory& process,
                 QHash<std::uint64_t, QString>& userNameCache)
{
    const QString procPath = QDir(procRoot).filePath(QString::number(pid));
    if (!readProcessStartTime(procPath, pid, process.startTimeTicks)) {
        return false;
    }

    QByteArray statusContents;
    if (!readProcFile(procPath + QStringLiteral("/status"), statusContents)) {
        return false;
    }

    process.pid = pid;
    const QList<QByteArray> statusLines = statusContents.split('\n');
    Bytes statusRss = 0;
    Bytes statusSwap = 0;
    for (const QByteArray& line : statusLines) {
        const qsizetype colon = line.indexOf(':');
        if (colon < 0) {
            continue;
        }
        const QByteArray key = line.left(colon).trimmed();
        if (key == "Name") {
            process.name = stringValueFromLine(line);
        } else if (key == "PPid") {
            Bytes parentPid = 0;
            if (parseFirstUnsignedValue(line, parentPid)
                && parentPid <= static_cast<Bytes>(std::numeric_limits<qint64>::max())) {
                process.ppid = static_cast<qint64>(parentPid);
            }
        } else if (key == "Uid") {
            Bytes uid = 0;
            if (parseFirstUnsignedValue(line, uid)) {
                process.uid = uid;
                auto cachedUserName = userNameCache.find(uid);
                if (cachedUserName == userNameCache.end()) {
                    cachedUserName = userNameCache.insert(uid, userNameForUid(uid));
                }
                process.userName = cachedUserName.value();
            }
        } else if (key == "VmRSS") {
            parseBytesFromLine(line, statusRss);
        } else if (key == "VmSwap") {
            parseBytesFromLine(line, statusSwap);
        }
    }

    ProcessMetrics metrics;
    QByteArray rollupContents;
    const bool hasRollup = readProcFile(procPath + QStringLiteral("/smaps_rollup"), rollupContents);
    if (hasRollup) {
        readMetricLines(rollupContents, metrics);
    }

    process.pssExact = metrics.hasPss;
    process.swapPssExact = metrics.hasSwapPss;
    if (!metrics.hasRss) {
        metrics.rssBytes = statusRss;
    }
    if (!metrics.hasPss) {
        metrics.pssBytes = metrics.rssBytes;
    }
    if (!metrics.hasSwap) {
        metrics.swapBytes = statusSwap;
    }
    if (!metrics.hasSwapPss) {
        metrics.swapPssBytes = metrics.swapBytes;
    }
    process.approximate = !process.pssExact || !process.swapPssExact;

    process.rssBytes = metrics.rssBytes;
    process.pssBytes = metrics.pssBytes;
    process.swapBytes = metrics.swapBytes;
    process.swapPssBytes = metrics.swapPssBytes;

    const QString commandPath = procPath + QStringLiteral("/cmdline");
    QByteArray commandContents;
    if (readProcFile(commandPath, commandContents)) {
        commandContents.replace('\0', ' ');
        process.command = QString::fromLocal8Bit(commandContents).simplified();
    }
    process.executable = QFileInfo(procPath + QStringLiteral("/exe")).symLinkTarget();
    if (process.command.isEmpty()) {
        process.command = process.executable;
    }
    if (process.command.isEmpty()) {
        process.command = process.name;
    }
    if (process.name.isEmpty()) {
        process.name = QStringLiteral("pid %1").arg(pid);
    }

    Bytes finalStartTime = 0;
    return readProcessStartTime(procPath, pid, finalStartTime)
        && finalStartTime == process.startTimeTicks;
}

ZramDevice readZramDevice(const QString& sysBlockRoot, const QString& deviceName)
{
    const QString basePath = QDir(sysBlockRoot).filePath(deviceName);
    ZramDevice device;
    device.name = deviceName;
    readSingleValueFile(basePath + QStringLiteral("/disksize"), device.diskSizeBytes);

    const QString algorithmText = readTextFile(basePath + QStringLiteral("/comp_algorithm"));
    const qsizetype leftBracket = algorithmText.indexOf('[');
    const qsizetype rightBracket = algorithmText.indexOf(']', leftBracket + 1);
    if (leftBracket >= 0 && rightBracket > leftBracket) {
        device.compressionAlgorithm = algorithmText.mid(leftBracket + 1, rightBracket - leftBracket - 1);
    } else {
        device.compressionAlgorithm = algorithmText.section(' ', 0, 0);
    }

    QByteArray mmStat;
    if (readProcFile(basePath + QStringLiteral("/mm_stat"), mmStat)) {
        const QList<QByteArray> fields = mmStat.simplified().split(' ');
        Bytes originalData = 0;
        Bytes compressedData = 0;
        Bytes memoryUsed = 0;
        if (fields.size() >= 3 && parseUnsignedCounter(fields.at(0), originalData)
            && parseUnsignedCounter(fields.at(1), compressedData)
            && parseUnsignedCounter(fields.at(2), memoryUsed)) {
            device.originalDataBytes = originalData;
            device.compressedDataBytes = compressedData;
            device.memoryUsedBytes = memoryUsed;
            device.statisticsReadable = true;
        }
        if (fields.size() > 3) {
            parseUnsignedCounter(fields.at(3), device.memoryLimitBytes);
        }
    }

    QByteArray ioStat;
    if (readProcFile(basePath + QStringLiteral("/io_stat"), ioStat)) {
        const QList<QByteArray> fields = ioStat.simplified().split(' ');
        device.ioStatisticsReadable = fields.size() > 1 && parseUnsignedCounter(fields.at(1), device.failedWrites);
    }
    return device;
}

} // namespace

MemorySnapshot MemorySampler::collect()
{
    return collect(QStringLiteral("/proc"), QStringLiteral("/sys/block"));
}

MemorySnapshot MemorySampler::collect(const QString& procRoot, const QString& sysBlockRoot)
{
    MemorySnapshot snapshot;
    snapshot.timestamp = QDateTime::currentDateTime();

    QByteArray meminfo;
    if (readProcFile(QDir(procRoot).filePath(QStringLiteral("meminfo")), meminfo)) {
        snapshot.memoryInfoReadable = true;
        bool hasMemoryTotal = false;
        bool hasMemoryAvailable = false;
        bool hasMemoryFree = false;
        bool hasSwapTotal = false;
        bool hasSwapFree = false;
        const QList<QByteArray> lines = meminfo.split('\n');
        for (const QByteArray& line : lines) {
            const qsizetype colon = line.indexOf(':');
            if (colon < 0) {
                continue;
            }

            const QByteArray key = line.left(colon).trimmed();
            Bytes value = 0;
            if (!parseBytesFromLine(line, value)) {
                continue;
            }
            if (key == "MemTotal") {
                snapshot.memoryTotalBytes = value;
                hasMemoryTotal = true;
            } else if (key == "MemAvailable") {
                snapshot.memoryAvailableBytes = value;
                hasMemoryAvailable = true;
            } else if (key == "MemFree") {
                snapshot.memoryFreeBytes = value;
                hasMemoryFree = true;
            } else if (key == "SwapTotal") {
                snapshot.swapTotalBytes = value;
                hasSwapTotal = true;
            } else if (key == "SwapFree") {
                snapshot.swapFreeBytes = value;
                hasSwapFree = true;
            }
        }

        snapshot.memoryInfoComplete = hasMemoryTotal
            && hasMemoryAvailable
            && snapshot.memoryTotalBytes > 0
            && snapshot.memoryAvailableBytes <= snapshot.memoryTotalBytes;
        snapshot.memoryFreeReadable = hasMemoryTotal
            && hasMemoryFree
            && snapshot.memoryTotalBytes > 0
            && snapshot.memoryFreeBytes <= snapshot.memoryTotalBytes;
        snapshot.swapInfoComplete = hasSwapTotal
            && hasSwapFree
            && snapshot.swapFreeBytes <= snapshot.swapTotalBytes;
    }

    if (snapshot.memoryInfoComplete) {
        snapshot.memoryUsedBytes = snapshot.memoryTotalBytes - snapshot.memoryAvailableBytes;
    }
    if (snapshot.swapInfoComplete) {
        snapshot.swapUsedBytes = snapshot.swapTotalBytes - snapshot.swapFreeBytes;
    }

    const QDir procDirectory(procRoot);
    snapshot.memoryPressureReadable = readMemoryPressure(procDirectory.filePath(QStringLiteral("pressure/memory")),
                                                        snapshot.memoryPressureSomeAvg10,
                                                        snapshot.memoryPressureFullAvg10);
    snapshot.oomKillCountReadable = readOomKillCount(procDirectory.filePath(QStringLiteral("vmstat")), snapshot.oomKillCount);
    const QStringList entries = procDirectory.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    snapshot.processes.reserve(entries.size());
    QHash<std::uint64_t, QString> userNameCache;
    for (const QString& entry : entries) {
        bool ok = false;
        const qint64 pid = entry.toLongLong(&ok);
        if (!ok || pid <= 0) {
            continue;
        }

        ProcessMemory process;
        if (readProcess(procRoot, pid, process, userNameCache)) {
            snapshot.processes.push_back(std::move(process));
        } else {
            ++snapshot.processesSkipped;
        }
    }

    std::sort(snapshot.processes.begin(), snapshot.processes.end(), [](const ProcessMemory& left, const ProcessMemory& right) {
        if (left.footprintBytes() != right.footprintBytes()) {
            return left.footprintBytes() > right.footprintBytes();
        }
        return left.pid < right.pid;
    });

    const QDir sysBlock(sysBlockRoot);
    const QStringList zramEntries = sysBlock.entryList(QStringList{QStringLiteral("zram*")}, QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    snapshot.zramDevices.reserve(zramEntries.size());
    for (const QString& entry : zramEntries) {
        snapshot.zramDevices.push_back(readZramDevice(sysBlockRoot, entry));
    }

    return snapshot;
}

} // namespace memonitor
