#include "main_window.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QStringList>
#include <QTextStream>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

void setApplicationMetadata()
{
    QCoreApplication::setApplicationName(QStringLiteral("MemMonitor"));
    QCoreApplication::setApplicationVersion(QStringLiteral(MEMONITOR_VERSION));
}

QString formatBytes(std::uint64_t bytes)
{
    static const QStringList units{QStringLiteral("B"), QStringLiteral("KiB"), QStringLiteral("MiB"), QStringLiteral("GiB"), QStringLiteral("TiB")};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit + 1 < units.size()) {
        value /= 1024.0;
        ++unit;
    }
    const int decimals = unit == 0 ? 0 : (value >= 100.0 ? 0 : (value >= 10.0 ? 1 : 2));
    return QStringLiteral("%1 %2").arg(QString::number(value, 'f', decimals), units.at(unit));
}

bool dumpSnapshot(const memonitor::MemorySnapshot& snapshot)
{
    QTextStream out(stdout);
    out << "Memory: " << (snapshot.memoryInfoComplete ? formatBytes(snapshot.memoryUsedBytes) : QStringLiteral("unavailable"))
        << " / " << (snapshot.memoryInfoComplete ? formatBytes(snapshot.memoryTotalBytes) : QStringLiteral("unavailable"))
        << " (estimated available " << (snapshot.memoryInfoComplete ? formatBytes(snapshot.memoryAvailableBytes) : QStringLiteral("unavailable"))
        << ", free " << (snapshot.memoryFreeReadable ? formatBytes(snapshot.memoryFreeBytes) : QStringLiteral("unavailable")) << ")\n";
    out << "Swap:   " << (snapshot.swapInfoComplete ? formatBytes(snapshot.swapUsedBytes) : QStringLiteral("unavailable"))
        << " / " << (snapshot.swapInfoComplete ? formatBytes(snapshot.swapTotalBytes) : QStringLiteral("unavailable"))
        << " (free " << (snapshot.swapInfoComplete ? formatBytes(snapshot.swapFreeBytes) : QStringLiteral("unavailable")) << ")\n";
    out << "Memory PSI (avg10): some="
        << (snapshot.memoryPressureReadable ? QString::number(snapshot.memoryPressureSomeAvg10, 'f', 2) + QStringLiteral("%") : QStringLiteral("unavailable"))
        << ", full="
        << (snapshot.memoryPressureReadable ? QString::number(snapshot.memoryPressureFullAvg10, 'f', 2) + QStringLiteral("%") : QStringLiteral("unavailable")) << "\n";
    out << "OOM kills since boot: "
        << (snapshot.oomKillCountReadable ? QString::number(static_cast<qulonglong>(snapshot.oomKillCount)) : QStringLiteral("unavailable")) << "\n";
    for (const memonitor::ZramDevice& device : snapshot.zramDevices) {
        out << "zram " << device.name << ": mem_used_total=" << (device.statisticsReadable ? formatBytes(device.memoryUsedBytes) : QStringLiteral("unavailable"))
            << ", orig_data=" << (device.statisticsReadable ? formatBytes(device.originalDataBytes) : QStringLiteral("unavailable"))
            << ", compr_data=" << (device.statisticsReadable ? formatBytes(device.compressedDataBytes) : QStringLiteral("unavailable"))
            << ", failed_writes=" << (device.ioStatisticsReadable ? QString::number(static_cast<qulonglong>(device.failedWrites)) : QStringLiteral("unavailable"))
            << ", algorithm=" << device.compressionAlgorithm << "\n";
    }
    out << "Processes: " << snapshot.processes.size() << ", skipped=" << snapshot.processesSkipped << "\n";
    const int count = static_cast<int>(std::min<qsizetype>(10, snapshot.processes.size()));
    for (int index = 0; index < count; ++index) {
        const memonitor::ProcessMemory& process = snapshot.processes.at(index);
        out << process.pid << "\t" << process.name << "\tPSS=" << formatBytes(process.pssBytes)
            << "\tSwapPss=" << formatBytes(process.swapPssBytes)
            << "\tPSSExact=" << (process.pssExact ? "yes" : "no")
            << "\tSwapPssExact=" << (process.swapPssExact ? "yes" : "no")
            << "\tFootprint=" << formatBytes(process.footprintBytes()) << "\n";
    }
    out.flush();
    return out.status() == QTextStream::Ok && std::fflush(stdout) == 0;
}

} // namespace

int main(int argc, char* argv[])
{
    bool headless = false;
    for (int index = 1; index < argc; ++index) {
        const QString argument = QString::fromLocal8Bit(argv[index]);
        if (argument == QStringLiteral("--")) {
            break;
        }
        if (argument == QStringLiteral("--dump") || argument == QStringLiteral("--help")
            || argument == QStringLiteral("--help-all") || argument == QStringLiteral("-h")
            || argument == QStringLiteral("--version") || argument == QStringLiteral("-v")) {
            headless = true;
        }
    }

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Monitor Linux memory, processes and zram."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({QStringLiteral("dump"), QStringLiteral("Print one memory snapshot without opening a window.")});

    if (headless) {
        QCoreApplication app(argc, argv);
        setApplicationMetadata();
        parser.process(app);
        const memonitor::MemorySnapshot snapshot = memonitor::MemorySampler::collect();
        if (!dumpSnapshot(snapshot)) {
            QTextStream(stderr) << "Could not write memory snapshot to stdout.\n";
            return EXIT_FAILURE;
        }
        if (!snapshot.memoryInfoComplete) {
            QTextStream(stderr) << "Could not read complete memory information from /proc/meminfo.\n";
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }

    QApplication app(argc, argv);
    setApplicationMetadata();
    app.setApplicationDisplayName(QStringLiteral("MemMonitor"));
    parser.process(app);
    app.setQuitOnLastWindowClosed(false);

    memonitor::MainWindow window;
    window.show();
    return app.exec();
}
