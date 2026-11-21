#include "memory_sampler.h"
#include "monitor_policy.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {

void writeFile(const QString& path, const QByteArray& contents)
{
    const QFileInfo info(path);
    QDir().mkpath(info.path());

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qFatal("Could not write test fixture: %s", qPrintable(path));
    }
    file.write(contents);
}

void createProcRoot(const QTemporaryDir& temporaryDirectory, const QByteArray& meminfo)
{
    writeFile(temporaryDirectory.filePath(QStringLiteral("proc/meminfo")), meminfo);
}

QByteArray processStat(qint64 pid, const QByteArray& name, std::uint64_t startTime)
{
    return QByteArray::number(pid) + " (" + name
        + ") S 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 "
        + QByteArray::number(static_cast<qulonglong>(startTime)) + " 20\n";
}

} // namespace

class MemorySamplerTest final : public QObject {
    Q_OBJECT

private slots:
    void parsesProcAndZram();
    void fallsBackToStatusMetrics();
    void fallsBackForMissingSwapPss();
    void preservesSwapPssWithoutPss();
    void parsesMemoryByteCounts_data();
    void parsesMemoryByteCounts();
    void checksProcessIdentity_data();
    void checksProcessIdentity();
    void rejectsChangedProcessIdentity_data();
    void rejectsChangedProcessIdentity();
    void reportsIncompleteMemoryInfo();
    void reportsMissingDiagnostics();
    void parsesMemoryFree_data();
    void parsesMemoryFree();
    void parsesMemoryPressure_data();
    void parsesMemoryPressure();
    void parsesOomKillCount_data();
    void parsesOomKillCount();
    void parsesZramIoStatistics_data();
    void parsesZramIoStatistics();
    void parsesZramMemoryStatistics_data();
    void parsesZramMemoryStatistics();
    void readsDiagnosticsWithoutMeminfo();
    void doesNotDoubleCountZramMemory();
    void appliesStrictWarningBoundary();
    void appliesCustomWarningBoundaries_data();
    void appliesCustomWarningBoundaries();
    void commandLineInformationWorksWithoutDisplay();
    void dumpFailsWhenOutputCannotBeWritten();
};

void MemorySamplerTest::parsesProcAndZram()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    createProcRoot(temporaryDirectory,
                   "MemTotal:        1000 kB\n"
                   "MemAvailable:     400 kB\n"
                   "MemFree:          100 kB\n"
                   "SwapTotal:       2048 kB\n"
                   "SwapFree:        1024 kB\n");
    const QString procRoot = temporaryDirectory.filePath(QStringLiteral("proc"));
    const QString sysRoot = temporaryDirectory.filePath(QStringLiteral("sys/block"));

    writeFile(procRoot + QStringLiteral("/pressure/memory"),
              "some avg10=12.50 avg60=4.00 avg300=1.00 total=123456\n"
              "full avg10=2.25 avg60=1.00 avg300=0.50 total=12345\n");
    writeFile(procRoot + QStringLiteral("/vmstat"), "pgpgin 100\noom_kill 17\npgpgout 200\n");

    writeFile(procRoot + QStringLiteral("/42/status"),
              "Name:           alpha\n"
              "PPid:           7\n"
              "Uid:            0        0        0        0\n"
              "VmRSS:          300 kB\n"
              "VmSwap:         120 kB\n");
    writeFile(procRoot + QStringLiteral("/42/smaps_rollup"),
              "Rss:            300 kB\n"
              "Pss:            200 kB\n"
              "Swap:           120 kB\n"
              "SwapPss:         80 kB\n");
    writeFile(procRoot + QStringLiteral("/42/cmdline"), QByteArray("alpha\0--test\0", 13));
    writeFile(procRoot + QStringLiteral("/42/stat"), "42 (alpha worker) S 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 4242 20\n");

    writeFile(procRoot + QStringLiteral("/7/status"),
              "Name:           beta\n"
              "PPid:           1\n"
              "Uid:            0        0        0        0\n"
              "VmRSS:          400 kB\n"
              "VmSwap:         100 kB\n");
    writeFile(procRoot + QStringLiteral("/7/smaps_rollup"),
              "Rss:            400 kB\n"
              "Pss:            300 kB\n"
              "Swap:           100 kB\n"
              "SwapPss:         60 kB\n");
    writeFile(procRoot + QStringLiteral("/7/stat"), "7 (beta) S 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 7777 20\n");

    writeFile(procRoot + QStringLiteral("/50/status"),
              "Name:           unknown-user\n"
              "Uid:            18446744073709551615\n"
              "VmRSS:          100 kB\n");
    writeFile(procRoot + QStringLiteral("/50/stat"), processStat(50, "unknown-user", 5000));

    writeFile(sysRoot + QStringLiteral("/zram0/disksize"), "1048576\n");
    writeFile(sysRoot + QStringLiteral("/zram0/comp_algorithm"), "lzo [zstd] lz4\n");
    writeFile(sysRoot + QStringLiteral("/zram0/mm_stat"), "8192 4096 5120 0 0 0 0 0 0\n");
    writeFile(sysRoot + QStringLiteral("/zram0/io_stat"), "23 7 0 0\n");

    const memonitor::MemorySnapshot snapshot = memonitor::MemorySampler::collect(procRoot, sysRoot);
    QVERIFY(snapshot.memoryInfoReadable);
    QVERIFY(snapshot.memoryInfoComplete);
    QCOMPARE(snapshot.memoryTotalBytes, std::uint64_t(1000U * 1024U));
    QCOMPARE(snapshot.memoryAvailableBytes, std::uint64_t(400U * 1024U));
    QVERIFY(snapshot.memoryFreeReadable);
    QCOMPARE(snapshot.memoryFreeBytes, std::uint64_t(100U * 1024U));
    QCOMPARE(snapshot.memoryUsedBytes, std::uint64_t(600U * 1024U));
    QCOMPARE(snapshot.swapUsedBytes, std::uint64_t(1024U * 1024U));
    QVERIFY(snapshot.memoryPressureReadable);
    QCOMPARE(snapshot.memoryPressureSomeAvg10, 12.50);
    QCOMPARE(snapshot.memoryPressureFullAvg10, 2.25);
    QVERIFY(snapshot.oomKillCountReadable);
    QCOMPARE(snapshot.oomKillCount, std::uint64_t(17));

    QCOMPARE(snapshot.processes.size(), 3);
    QCOMPARE(snapshot.processes.at(0).pid, qint64(7));
    QCOMPARE(snapshot.processes.at(0).ppid, qint64(1));
    QCOMPARE(snapshot.processes.at(0).uid, std::uint64_t(0));
    QCOMPARE(snapshot.processes.at(0).userName, QStringLiteral("root"));
    QCOMPARE(snapshot.processes.at(0).pssBytes, std::uint64_t(300U * 1024U));
    QCOMPARE(snapshot.processes.at(0).swapPssBytes, std::uint64_t(60U * 1024U));
    QCOMPARE(snapshot.processes.at(0).footprintBytes(), std::uint64_t(360U * 1024U));
    QCOMPARE(snapshot.processes.at(0).startTimeTicks, std::uint64_t(7777U));
    QVERIFY(snapshot.processes.at(0).pssExact);
    QVERIFY(snapshot.processes.at(0).swapPssExact);
    QCOMPARE(snapshot.processes.at(1).pid, qint64(42));
    QCOMPARE(snapshot.processes.at(1).ppid, qint64(7));
    QCOMPARE(snapshot.processes.at(1).uid, std::uint64_t(0));
    QCOMPARE(snapshot.processes.at(1).userName, QStringLiteral("root"));
    QCOMPARE(snapshot.processes.at(1).userName, snapshot.processes.at(0).userName);
    QCOMPARE(snapshot.processes.at(1).startTimeTicks, std::uint64_t(4242U));
    QVERIFY(!snapshot.processes.at(0).approximate);
    QCOMPARE(snapshot.processes.at(2).pid, qint64(50));
    QCOMPARE(snapshot.processes.at(2).uid, std::numeric_limits<std::uint64_t>::max());
    QVERIFY(snapshot.processes.at(2).userName.isEmpty());

    QCOMPARE(snapshot.zramDevices.size(), 1);
    const memonitor::ZramDevice& zram = snapshot.zramDevices.constFirst();
    QCOMPARE(zram.name, QStringLiteral("zram0"));
    QCOMPARE(zram.compressionAlgorithm, QStringLiteral("zstd"));
    QCOMPARE(zram.diskSizeBytes, std::uint64_t(1024U * 1024U));
    QCOMPARE(zram.originalDataBytes, std::uint64_t(8192U));
    QCOMPARE(zram.compressedDataBytes, std::uint64_t(4096U));
    QCOMPARE(zram.memoryUsedBytes, std::uint64_t(5120U));
    QVERIFY(zram.statisticsReadable);
    QCOMPARE(zram.failedWrites, std::uint64_t(7));
    QVERIFY(zram.ioStatisticsReadable);
}

void MemorySamplerTest::fallsBackToStatusMetrics()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    createProcRoot(temporaryDirectory,
                   "MemTotal:        4096 kB\n"
                   "MemAvailable:    1024 kB\n"
                   "SwapTotal:       2048 kB\n"
                   "SwapFree:        1536 kB\n");
    const QString procRoot = temporaryDirectory.filePath(QStringLiteral("proc"));
    const QString sysRoot = temporaryDirectory.filePath(QStringLiteral("sys/block"));
    writeFile(procRoot + QStringLiteral("/99/status"),
              "Name:           legacy\n"
              "VmRSS:          2048 kB\n"
              "VmSwap:          256 kB\n");
    writeFile(procRoot + QStringLiteral("/99/stat"), processStat(99, "legacy", 9900));

    const memonitor::MemorySnapshot snapshot = memonitor::MemorySampler::collect(procRoot, sysRoot);
    QCOMPARE(snapshot.processes.size(), 1);
    const memonitor::ProcessMemory& process = snapshot.processes.constFirst();
    QVERIFY(process.approximate);
    QCOMPARE(process.rssBytes, std::uint64_t(2048U * 1024U));
    QCOMPARE(process.pssBytes, std::uint64_t(2048U * 1024U));
    QCOMPARE(process.swapPssBytes, std::uint64_t(256U * 1024U));
    QVERIFY(!process.pssExact);
    QVERIFY(!process.swapPssExact);
    QCOMPARE(process.ppid, qint64(-1));
    QCOMPARE(process.uid, std::numeric_limits<std::uint64_t>::max());
    QVERIFY(process.userName.isEmpty());
}

void MemorySamplerTest::fallsBackForMissingSwapPss()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    createProcRoot(temporaryDirectory,
                   "MemTotal:        4096 kB\n"
                   "MemAvailable:    1024 kB\n"
                   "SwapTotal:       2048 kB\n"
                   "SwapFree:        1536 kB\n");
    const QString procRoot = temporaryDirectory.filePath(QStringLiteral("proc"));
    const QString sysRoot = temporaryDirectory.filePath(QStringLiteral("sys/block"));
    writeFile(procRoot + QStringLiteral("/55/status"),
              "Name:           no-swap-pss\n"
              "VmRSS:          600 kB\n"
              "VmSwap:          25 kB\n");
    writeFile(procRoot + QStringLiteral("/55/stat"), processStat(55, "no-swap-pss", 5500));
    writeFile(procRoot + QStringLiteral("/55/smaps_rollup"),
              "Rss:            600 kB\n"
              "Pss:            500 kB\n"
              "Swap:            25 kB\n");

    const memonitor::MemorySnapshot snapshot = memonitor::MemorySampler::collect(procRoot, sysRoot);
    QCOMPARE(snapshot.processes.size(), 1);
    const memonitor::ProcessMemory& process = snapshot.processes.constFirst();
    QVERIFY(process.pssExact);
    QVERIFY(!process.swapPssExact);
    QVERIFY(process.approximate);
    QCOMPARE(process.pssBytes, std::uint64_t(500U * 1024U));
    QCOMPARE(process.swapPssBytes, std::uint64_t(25U * 1024U));
}

void MemorySamplerTest::preservesSwapPssWithoutPss()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    createProcRoot(temporaryDirectory, "MemTotal: 4096 kB\nMemAvailable: 1024 kB\n");
    const QString procRoot = temporaryDirectory.filePath(QStringLiteral("proc"));
    writeFile(procRoot + QStringLiteral("/55/stat"), processStat(55, "partial-rollup", 5500));
    writeFile(procRoot + QStringLiteral("/55/status"),
              "Name: partial-rollup\nVmRSS: 700 kB\nVmSwap: 100 kB\n");
    writeFile(procRoot + QStringLiteral("/55/smaps_rollup"),
              "Rss: 600 kB\nPss: broken\nSwap: 80 kB\nSwapPss: 25 kB\n");

    const auto snapshot = memonitor::MemorySampler::collect(
        procRoot, temporaryDirectory.filePath(QStringLiteral("sys/block")));
    QCOMPARE(snapshot.processes.size(), 1);
    const auto& process = snapshot.processes.constFirst();
    QVERIFY(!process.pssExact);
    QVERIFY(process.swapPssExact);
    QVERIFY(process.approximate);
    QCOMPARE(process.rssBytes, std::uint64_t(600U * 1024U));
    QCOMPARE(process.pssBytes, std::uint64_t(600U * 1024U));
    QCOMPARE(process.swapBytes, std::uint64_t(80U * 1024U));
    QCOMPARE(process.swapPssBytes, std::uint64_t(25U * 1024U));
}

void MemorySamplerTest::parsesMemoryByteCounts_data()
{
    QTest::addColumn<QByteArray>("value");
    QTest::addColumn<bool>("readable");
    QTest::addColumn<qulonglong>("bytes");

    QTest::newRow("zero") << QByteArray("0 kB") << true << qulonglong(0);
    QTest::newRow("above-32-bit") << QByteArray("4294967296 kB") << true << qulonglong(4398046511104ULL);
    QTest::newRow("largest-kibibytes") << QByteArray("18014398509481983 kB") << true << qulonglong(18446744073709550592ULL);
    const QList<QByteArray> invalidValues{
        "", "1", "1 MB", "1 kB trailing", "+1 kB", "-1 kB", "1.5 kB",
        "18014398509481984 kB", "18446744073709551616 kB",
    };
    for (const auto& value : invalidValues) {
        QTest::newRow(value.isEmpty() ? "empty" : value.constData()) << value << false << qulonglong(0);
    }
}

void MemorySamplerTest::parsesMemoryByteCounts()
{
    QFETCH(QByteArray, value);
    QFETCH(bool, readable);
    QFETCH(qulonglong, bytes);
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    createProcRoot(temporaryDirectory,
                   QByteArray("MemTotal: 18014398509481983 kB\nMemAvailable: ") + value
                       + "\nMemFree: " + value + "\nSwapTotal: 18014398509481983 kB\nSwapFree: " + value + '\n');
    const QString procRoot = temporaryDirectory.filePath(QStringLiteral("proc"));
    writeFile(procRoot + QStringLiteral("/42/stat"), processStat(42, "byte-counts", 4200));
    writeFile(procRoot + QStringLiteral("/42/status"), "Name: byte-counts\nVmRSS: 2 kB\nVmSwap: 3 kB\n");
    writeFile(procRoot + QStringLiteral("/42/smaps_rollup"),
              QByteArray("Pss: ") + value + "\nSwapPss: " + value + '\n');

    const auto snapshot = memonitor::MemorySampler::collect(
        procRoot, temporaryDirectory.filePath(QStringLiteral("sys/block")));
    QCOMPARE(snapshot.memoryInfoComplete, readable);
    QCOMPARE(snapshot.memoryFreeReadable, readable);
    QCOMPARE(snapshot.swapInfoComplete, readable);
    if (readable) {
        QCOMPARE(snapshot.memoryAvailableBytes, static_cast<std::uint64_t>(bytes));
        QCOMPARE(snapshot.memoryFreeBytes, static_cast<std::uint64_t>(bytes));
        QCOMPARE(snapshot.swapFreeBytes, static_cast<std::uint64_t>(bytes));
    }
    QCOMPARE(snapshot.processes.size(), 1);
    const auto& process = snapshot.processes.constFirst();
    QCOMPARE(process.pssExact, readable);
    QCOMPARE(process.swapPssExact, readable);
    QCOMPARE(process.pssBytes, readable ? static_cast<std::uint64_t>(bytes) : std::uint64_t(2048));
    QCOMPARE(process.swapPssBytes, readable ? static_cast<std::uint64_t>(bytes) : std::uint64_t(3072));
}

void MemorySamplerTest::checksProcessIdentity_data()
{
    QTest::addColumn<QByteArray>("stat");
    QTest::addColumn<bool>("readable");
    QTest::addColumn<qulonglong>("startTime");

    QTest::newRow("parentheses-and-spaces") << processStat(42, "worker (child) )", 4200) << true << qulonglong(4200);
    QTest::newRow("above-32-bit") << processStat(42, "worker", 4294967297ULL) << true << qulonglong(4294967297ULL);
    QTest::newRow("zero-start-time") << processStat(42, "worker", 0) << true << qulonglong(0);
    QTest::newRow("maximum-start-time") << processStat(42, "worker", std::numeric_limits<std::uint64_t>::max())
                                        << true << std::numeric_limits<qulonglong>::max();
    QTest::newRow("missing") << QByteArray() << false << qulonglong(0);
    QTest::newRow("incomplete") << QByteArray("42 (worker) S 1 2 3\n") << false << qulonglong(0);
    QTest::newRow("wrong-pid") << processStat(43, "replacement", 4200) << false << qulonglong(0);
    QByteArray overflow = processStat(42, "worker", 4200);
    overflow.replace("4200 20", "18446744073709551616 20");
    QTest::newRow("overflow") << overflow << false << qulonglong(0);
    QByteArray signedValue = processStat(42, "worker", 4200);
    signedValue.replace("4200 20", "+4200 20");
    QTest::newRow("signed") << signedValue << false << qulonglong(0);
}

void MemorySamplerTest::checksProcessIdentity()
{
    QFETCH(QByteArray, stat);
    QFETCH(bool, readable);
    QFETCH(qulonglong, startTime);
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    createProcRoot(temporaryDirectory, "MemTotal: 4096 kB\nMemAvailable: 1024 kB\n");
    const QString procRoot = temporaryDirectory.filePath(QStringLiteral("proc"));
    writeFile(procRoot + QStringLiteral("/42/status"), "Name: worker\nVmRSS: 2 kB\n");
    if (!stat.isEmpty()) {
        writeFile(procRoot + QStringLiteral("/42/stat"), stat);
    }

    const auto snapshot = memonitor::MemorySampler::collect(
        procRoot, temporaryDirectory.filePath(QStringLiteral("sys/block")));
    QCOMPARE(snapshot.processes.size(), readable ? 1 : 0);
    QCOMPARE(snapshot.processesSkipped, readable ? 0 : 1);
    if (readable) {
        QCOMPARE(snapshot.processes.constFirst().startTimeTicks, static_cast<std::uint64_t>(startTime));
    }
}

void MemorySamplerTest::rejectsChangedProcessIdentity_data()
{
    QTest::addColumn<bool>("removeStat");
    QTest::newRow("reused-pid") << false;
    QTest::newRow("exited-process") << true;
}

void MemorySamplerTest::rejectsChangedProcessIdentity()
{
    QFETCH(bool, removeStat);
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    createProcRoot(temporaryDirectory, "MemTotal: 4096 kB\nMemAvailable: 1024 kB\n");
    const QString procRoot = temporaryDirectory.filePath(QStringLiteral("proc"));
    const QString statPath = procRoot + QStringLiteral("/42/stat");
    const QByteArray commandPath = QFile::encodeName(procRoot + QStringLiteral("/42/cmdline"));
    writeFile(statPath, processStat(42, "before", 4200));
    writeFile(procRoot + QStringLiteral("/42/status"), "Name: before\nVmRSS: 2 kB\n");
    writeFile(procRoot + QStringLiteral("/42/smaps_rollup"), "Pss: 1 kB\nSwapPss: 0 kB\n");
    QCOMPARE(::mkfifo(commandPath.constData(), S_IRUSR | S_IWUSR), 0);

    bool identityChanged = false;
    std::jthread writer([&](std::stop_token stopToken) {
        int descriptor = -1;
        while (!stopToken.stop_requested()) {
            descriptor = ::open(commandPath.constData(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
            if (descriptor >= 0) {
                break;
            }
            if (errno != ENXIO && errno != EINTR) {
                return;
            }
            std::this_thread::yield();
        }
        if (descriptor < 0) {
            return;
        }
        if (removeStat) {
            identityChanged = QFile::remove(statPath);
        } else {
            writeFile(statPath, processStat(42, "after", 4300));
            identityChanged = true;
        }
        constexpr char command[] = "before";
        const auto written = ::write(descriptor, command, sizeof(command));
        identityChanged = identityChanged && written == static_cast<ssize_t>(sizeof(command));
        ::close(descriptor);
    });
    const auto snapshot = memonitor::MemorySampler::collect(
        procRoot, temporaryDirectory.filePath(QStringLiteral("sys/block")));
    writer.request_stop();
    writer.join();

    QVERIFY(identityChanged);
    QVERIFY(snapshot.processes.isEmpty());
    QCOMPARE(snapshot.processesSkipped, 1);
}

void MemorySamplerTest::reportsIncompleteMemoryInfo()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    createProcRoot(temporaryDirectory, "MemTotal: not-a-number\nMemAvailable: missing\n");
    const QString procRoot = temporaryDirectory.filePath(QStringLiteral("proc"));
    const QString sysRoot = temporaryDirectory.filePath(QStringLiteral("sys/block"));
    QDir().mkpath(procRoot + QStringLiteral("/123"));

    const memonitor::MemorySnapshot snapshot = memonitor::MemorySampler::collect(procRoot, sysRoot);
    QVERIFY(snapshot.memoryInfoReadable);
    QVERIFY(!snapshot.memoryInfoComplete);
    QCOMPARE(snapshot.memoryTotalBytes, std::uint64_t(0));
    QCOMPARE(snapshot.processesSkipped, 1);
}

void MemorySamplerTest::reportsMissingDiagnostics()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    createProcRoot(temporaryDirectory,
                   "MemTotal: 1000 kB\nMemAvailable: 400 kB\nSwapTotal: 2048 kB\nSwapFree: 1024 kB\n");
    const QString procRoot = temporaryDirectory.filePath(QStringLiteral("proc"));
    const QString sysRoot = temporaryDirectory.filePath(QStringLiteral("sys/block"));
    writeFile(sysRoot + QStringLiteral("/zram0/mm_stat"), "8192 4096 5120 0\n");

    const memonitor::MemorySnapshot snapshot = memonitor::MemorySampler::collect(procRoot, sysRoot);
    QVERIFY(snapshot.memoryInfoComplete);
    QVERIFY(snapshot.swapInfoComplete);
    QVERIFY(!snapshot.memoryFreeReadable);
    QCOMPARE(snapshot.memoryFreeBytes, std::uint64_t(0));
    QVERIFY(!snapshot.memoryPressureReadable);
    QCOMPARE(snapshot.memoryPressureSomeAvg10, 0.0);
    QCOMPARE(snapshot.memoryPressureFullAvg10, 0.0);
    QVERIFY(!snapshot.oomKillCountReadable);
    QCOMPARE(snapshot.oomKillCount, std::uint64_t(0));
    QCOMPARE(snapshot.zramDevices.size(), 1);
    QVERIFY(snapshot.zramDevices.constFirst().statisticsReadable);
    QVERIFY(!snapshot.zramDevices.constFirst().ioStatisticsReadable);
    QCOMPARE(snapshot.zramDevices.constFirst().failedWrites, std::uint64_t(0));
}

void MemorySamplerTest::parsesMemoryFree_data()
{
    QTest::addColumn<QByteArray>("meminfo");
    QTest::addColumn<bool>("memoryInfoComplete");
    QTest::addColumn<bool>("freeReadable");
    QTest::addColumn<qulonglong>("freeBytes");

    QTest::newRow("valid") << QByteArray("MemTotal: 1000 kB\nMemAvailable: 400 kB\nMemFree: 100 kB\n") << true << true << qulonglong(102400);
    QTest::newRow("zero-free") << QByteArray("MemTotal: 1000 kB\nMemAvailable: 400 kB\nMemFree: 0 kB\n") << true << true << qulonglong(0);
    QTest::newRow("free-equals-total") << QByteArray("MemTotal: 1000 kB\nMemAvailable: 1000 kB\nMemFree: 1000 kB\n") << true << true << qulonglong(1024000);
    QTest::newRow("missing-available") << QByteArray("MemTotal: 1000 kB\nMemFree: 100 kB\n") << false << true << qulonglong(102400);
    QTest::newRow("invalid-available") << QByteArray("MemTotal: 1000 kB\nMemAvailable: broken\nMemFree: 100 kB\n") << false << true << qulonglong(102400);
    QTest::newRow("missing-total") << QByteArray("MemAvailable: 400 kB\nMemFree: 100 kB\n") << false << false << qulonglong(0);
    QTest::newRow("invalid-total") << QByteArray("MemTotal: broken\nMemAvailable: 400 kB\nMemFree: 100 kB\n") << false << false << qulonglong(0);
    QTest::newRow("free-exceeds-total") << QByteArray("MemTotal: 1000 kB\nMemAvailable: 400 kB\nMemFree: 1001 kB\n") << true << false << qulonglong(0);
    QTest::newRow("invalid-free") << QByteArray("MemTotal: 1000 kB\nMemAvailable: 400 kB\nMemFree: broken\n") << true << false << qulonglong(0);
    QTest::newRow("free-overflow") << QByteArray("MemTotal: 1000 kB\nMemAvailable: 400 kB\nMemFree: 18446744073709551615 kB\n") << true << false << qulonglong(0);
}

void MemorySamplerTest::parsesMemoryFree()
{
    QFETCH(QByteArray, meminfo);
    QFETCH(bool, memoryInfoComplete);
    QFETCH(bool, freeReadable);
    QFETCH(qulonglong, freeBytes);
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    createProcRoot(temporaryDirectory, meminfo);
    const QString procRoot = temporaryDirectory.filePath(QStringLiteral("proc"));
    writeFile(procRoot + QStringLiteral("/pressure/memory"), "some avg10=3.50\nfull avg10=1.25\n");
    writeFile(procRoot + QStringLiteral("/vmstat"), "oom_kill 9\n");

    const memonitor::MemorySnapshot snapshot = memonitor::MemorySampler::collect(
        procRoot, temporaryDirectory.filePath(QStringLiteral("sys/block")));
    QCOMPARE(snapshot.memoryInfoComplete, memoryInfoComplete);
    QCOMPARE(snapshot.memoryFreeReadable, freeReadable);
    if (freeReadable) {
        QCOMPARE(snapshot.memoryFreeBytes, static_cast<std::uint64_t>(freeBytes));
    }
    QVERIFY(snapshot.memoryPressureReadable);
    QCOMPARE(snapshot.memoryPressureSomeAvg10, 3.50);
    QCOMPARE(snapshot.memoryPressureFullAvg10, 1.25);
    QVERIFY(snapshot.oomKillCountReadable);
    QCOMPARE(snapshot.oomKillCount, std::uint64_t(9));
}

void MemorySamplerTest::parsesMemoryPressure_data()
{
    QTest::addColumn<QByteArray>("pressure");
    QTest::addColumn<bool>("readable");
    QTest::addColumn<double>("someAverage");
    QTest::addColumn<double>("fullAverage");

    QTest::newRow("zero") << QByteArray("some avg10=0.00\nfull avg10=0.00\n") << true << 0.0 << 0.0;
    QTest::newRow("hundred") << QByteArray("some avg10=100.00\nfull avg10=100.00\n") << true << 100.0 << 100.0;
    QTest::newRow("reordered-fields") << QByteArray("full\tavg60=99.00 avg10=2.25 total=123\nsome total=456 avg300=88.00 avg10=12.50\n") << true << 12.50 << 2.25;
    QTest::newRow("empty") << QByteArray() << false << 0.0 << 0.0;
    QTest::newRow("missing-full") << QByteArray("some avg10=3.50\n") << false << 0.0 << 0.0;
    QTest::newRow("missing-some") << QByteArray("full avg10=1.25\n") << false << 0.0 << 0.0;
    QTest::newRow("missing-some-avg10") << QByteArray("some avg60=3.50\nfull avg10=1.25\n") << false << 0.0 << 0.0;
    QTest::newRow("missing-full-avg10") << QByteArray("some avg10=3.50\nfull avg60=1.25\n") << false << 0.0 << 0.0;

    const QList<QByteArray> invalidAverages{"NaN", "inf", "-inf", "1e999", "-0.01", "100.01", "broken", ""};
    for (const QByteArray& average : invalidAverages) {
        const QByteArray someRow = QByteArray("invalid-some-") + average;
        QTest::newRow(someRow.constData())
            << (QByteArray("some avg10=") + average + "\nfull avg10=1.25\n") << false << 0.0 << 0.0;
        const QByteArray fullRow = QByteArray("invalid-full-") + average;
        QTest::newRow(fullRow.constData())
            << (QByteArray("some avg10=3.50\nfull avg10=") + average + "\n") << false << 0.0 << 0.0;
    }
}

void MemorySamplerTest::parsesMemoryPressure()
{
    QFETCH(QByteArray, pressure);
    QFETCH(bool, readable);
    QFETCH(double, someAverage);
    QFETCH(double, fullAverage);
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    createProcRoot(temporaryDirectory, "MemTotal: 1000 kB\nMemAvailable: 400 kB\nMemFree: 100 kB\n");
    const QString procRoot = temporaryDirectory.filePath(QStringLiteral("proc"));
    writeFile(procRoot + QStringLiteral("/pressure/memory"), pressure);
    writeFile(procRoot + QStringLiteral("/vmstat"), "oom_kill 9\n");

    const memonitor::MemorySnapshot snapshot = memonitor::MemorySampler::collect(
        procRoot, temporaryDirectory.filePath(QStringLiteral("sys/block")));
    QCOMPARE(snapshot.memoryPressureReadable, readable);
    QCOMPARE(snapshot.memoryPressureSomeAvg10, someAverage);
    QCOMPARE(snapshot.memoryPressureFullAvg10, fullAverage);
    QVERIFY(snapshot.memoryInfoComplete);
    QVERIFY(snapshot.memoryFreeReadable);
    QCOMPARE(snapshot.memoryFreeBytes, std::uint64_t(102400));
    QVERIFY(snapshot.oomKillCountReadable);
    QCOMPARE(snapshot.oomKillCount, std::uint64_t(9));
}

void MemorySamplerTest::parsesOomKillCount_data()
{
    QTest::addColumn<QByteArray>("vmstat");
    QTest::addColumn<bool>("readable");
    QTest::addColumn<qulonglong>("count");

    QTest::newRow("valid") << QByteArray("pgpgin 1\noom_kill\t17\npgpgout 2\n") << true << qulonglong(17);
    QTest::newRow("zero") << QByteArray("oom_kill 0\n") << true << qulonglong(0);
    QTest::newRow("maximum") << QByteArray("oom_kill 18446744073709551615\n") << true << std::numeric_limits<qulonglong>::max();
    QTest::newRow("empty") << QByteArray() << false << qulonglong(0);
    QTest::newRow("missing-key") << QByteArray("oom_kill_allocating_task 17\npgpgin 100\n") << false << qulonglong(0);
    QTest::newRow("missing-value") << QByteArray("oom_kill\n") << false << qulonglong(0);
    QTest::newRow("invalid-value") << QByteArray("oom_kill broken\n") << false << qulonglong(0);
    QTest::newRow("negative") << QByteArray("oom_kill -1\n") << false << qulonglong(0);
    QTest::newRow("overflow") << QByteArray("oom_kill 18446744073709551616\n") << false << qulonglong(0);
    QTest::newRow("extra-field") << QByteArray("oom_kill 17 garbage\n") << false << qulonglong(0);
}

void MemorySamplerTest::parsesOomKillCount()
{
    QFETCH(QByteArray, vmstat);
    QFETCH(bool, readable);
    QFETCH(qulonglong, count);
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    createProcRoot(temporaryDirectory, "MemTotal: 1000 kB\nMemAvailable: 400 kB\nMemFree: 100 kB\n");
    const QString procRoot = temporaryDirectory.filePath(QStringLiteral("proc"));
    writeFile(procRoot + QStringLiteral("/pressure/memory"), "some avg10=3.50\nfull avg10=1.25\n");
    writeFile(procRoot + QStringLiteral("/vmstat"), vmstat);

    const memonitor::MemorySnapshot snapshot = memonitor::MemorySampler::collect(
        procRoot, temporaryDirectory.filePath(QStringLiteral("sys/block")));
    QCOMPARE(snapshot.oomKillCountReadable, readable);
    QCOMPARE(snapshot.oomKillCount, static_cast<std::uint64_t>(count));
    QVERIFY(snapshot.memoryInfoComplete);
    QVERIFY(snapshot.memoryFreeReadable);
    QCOMPARE(snapshot.memoryFreeBytes, std::uint64_t(102400));
    QVERIFY(snapshot.memoryPressureReadable);
    QCOMPARE(snapshot.memoryPressureSomeAvg10, 3.50);
    QCOMPARE(snapshot.memoryPressureFullAvg10, 1.25);
}

void MemorySamplerTest::parsesZramIoStatistics_data()
{
    QTest::addColumn<QByteArray>("ioStat");
    QTest::addColumn<bool>("mmStatReadable");
    QTest::addColumn<bool>("ioStatReadable");
    QTest::addColumn<qulonglong>("failedWrites");

    QTest::newRow("second-column") << QByteArray("73 19 0 0\n") << true << true << qulonglong(19);
    QTest::newRow("zero-writes") << QByteArray("73 0 0 0\n") << true << true << qulonglong(0);
    QTest::newRow("two-columns") << QByteArray("73\t19\n") << true << true << qulonglong(19);
    QTest::newRow("invalid-mm-stat") << QByteArray("73 19 0 0\n") << false << true << qulonglong(19);
    QTest::newRow("empty") << QByteArray() << true << false << qulonglong(0);
    QTest::newRow("missing-second-column") << QByteArray("73\n") << true << false << qulonglong(0);
    QTest::newRow("invalid-second-column") << QByteArray("73 broken 0 0\n") << true << false << qulonglong(0);
    QTest::newRow("negative") << QByteArray("73 -1 0 0\n") << true << false << qulonglong(0);
    QTest::newRow("overflow") << QByteArray("73 18446744073709551616 0 0\n") << true << false << qulonglong(0);
}

void MemorySamplerTest::parsesZramIoStatistics()
{
    QFETCH(QByteArray, ioStat);
    QFETCH(bool, mmStatReadable);
    QFETCH(bool, ioStatReadable);
    QFETCH(qulonglong, failedWrites);
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    createProcRoot(temporaryDirectory, "MemTotal: 1000 kB\nMemAvailable: 400 kB\n");
    const QString sysRoot = temporaryDirectory.filePath(QStringLiteral("sys/block"));
    writeFile(sysRoot + QStringLiteral("/zram0/mm_stat"), mmStatReadable ? "8192 4096 5120 0\n" : "broken\n");
    writeFile(sysRoot + QStringLiteral("/zram0/io_stat"), ioStat);

    const memonitor::MemorySnapshot snapshot = memonitor::MemorySampler::collect(
        temporaryDirectory.filePath(QStringLiteral("proc")), sysRoot);
    QCOMPARE(snapshot.zramDevices.size(), 1);
    const memonitor::ZramDevice& zram = snapshot.zramDevices.constFirst();
    QCOMPARE(zram.statisticsReadable, mmStatReadable);
    QCOMPARE(zram.ioStatisticsReadable, ioStatReadable);
    QCOMPARE(zram.failedWrites, static_cast<std::uint64_t>(failedWrites));
    if (mmStatReadable) {
        QCOMPARE(zram.originalDataBytes, std::uint64_t(8192));
        QCOMPARE(zram.compressedDataBytes, std::uint64_t(4096));
        QCOMPARE(zram.memoryUsedBytes, std::uint64_t(5120));
    }
    QCOMPARE(snapshot.memoryUsedBytes, std::uint64_t(600U * 1024U));
}

void MemorySamplerTest::readsDiagnosticsWithoutMeminfo()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString procRoot = temporaryDirectory.filePath(QStringLiteral("proc"));
    const QString sysRoot = temporaryDirectory.filePath(QStringLiteral("sys/block"));
    writeFile(procRoot + QStringLiteral("/pressure/memory"), "some avg10=3.50\nfull avg10=1.25\n");
    writeFile(procRoot + QStringLiteral("/vmstat"), "oom_kill 9\n");
    writeFile(sysRoot + QStringLiteral("/zram0/io_stat"), "73 19 0 0\n");

    const memonitor::MemorySnapshot snapshot = memonitor::MemorySampler::collect(procRoot, sysRoot);
    QVERIFY(!snapshot.memoryInfoReadable);
    QVERIFY(!snapshot.memoryInfoComplete);
    QVERIFY(!snapshot.memoryFreeReadable);
    QVERIFY(snapshot.memoryPressureReadable);
    QCOMPARE(snapshot.memoryPressureSomeAvg10, 3.50);
    QCOMPARE(snapshot.memoryPressureFullAvg10, 1.25);
    QVERIFY(snapshot.oomKillCountReadable);
    QCOMPARE(snapshot.oomKillCount, std::uint64_t(9));
    QCOMPARE(snapshot.zramDevices.size(), 1);
    QVERIFY(!snapshot.zramDevices.constFirst().statisticsReadable);
    QVERIFY(snapshot.zramDevices.constFirst().ioStatisticsReadable);
    QCOMPARE(snapshot.zramDevices.constFirst().failedWrites, std::uint64_t(19));
}

void MemorySamplerTest::parsesZramMemoryStatistics_data()
{
    QTest::addColumn<QByteArray>("mmStat");
    QTest::addColumn<bool>("readable");
    QTest::addColumn<qulonglong>("memoryUsed");

    QTest::newRow("above-32-bit") << QByteArray("8589934592 4294967296 5368709120 6442450944\n")
                                 << true << qulonglong(5368709120ULL);
    QTest::newRow("maximum") << QByteArray("18446744073709551615 18446744073709551615 18446744073709551615\n")
                             << true << std::numeric_limits<qulonglong>::max();
    QTest::newRow("empty") << QByteArray() << false << qulonglong(0);
    QTest::newRow("incomplete") << QByteArray("8192 4096\n") << false << qulonglong(0);
    QTest::newRow("signed-original") << QByteArray("+8192 4096 5120\n") << false << qulonglong(0);
    QTest::newRow("signed-compressed") << QByteArray("8192 +4096 5120\n") << false << qulonglong(0);
    QTest::newRow("negative-memory") << QByteArray("8192 4096 -1\n") << false << qulonglong(0);
    QTest::newRow("overflow") << QByteArray("8192 4096 18446744073709551616\n") << false << qulonglong(0);
}

void MemorySamplerTest::parsesZramMemoryStatistics()
{
    QFETCH(QByteArray, mmStat);
    QFETCH(bool, readable);
    QFETCH(qulonglong, memoryUsed);
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    createProcRoot(temporaryDirectory, "MemTotal: 4096 kB\nMemAvailable: 1024 kB\n");
    const QString sysRoot = temporaryDirectory.filePath(QStringLiteral("sys/block"));
    writeFile(sysRoot + QStringLiteral("/zram0/mm_stat"), mmStat);
    const auto snapshot = memonitor::MemorySampler::collect(
        temporaryDirectory.filePath(QStringLiteral("proc")), sysRoot);
    QCOMPARE(snapshot.zramDevices.size(), 1);
    QCOMPARE(snapshot.zramDevices.constFirst().statisticsReadable, readable);
    QCOMPARE(snapshot.zramDevices.constFirst().memoryUsedBytes, static_cast<std::uint64_t>(memoryUsed));
}

void MemorySamplerTest::doesNotDoubleCountZramMemory()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    createProcRoot(temporaryDirectory,
                   "MemTotal: 1000 kB\nMemAvailable: 400 kB\nMemFree: 32 kB\nSwapTotal: 4096 kB\nSwapFree: 16 kB\n");
    const QString procRoot = temporaryDirectory.filePath(QStringLiteral("proc"));
    const QString sysRoot = temporaryDirectory.filePath(QStringLiteral("sys/block"));
    const memonitor::MemorySnapshot baseline = memonitor::MemorySampler::collect(procRoot, sysRoot);
    writeFile(sysRoot + QStringLiteral("/zram0/disksize"), "4194304\n");
    writeFile(sysRoot + QStringLiteral("/zram0/mm_stat"), "4177920 204800 307200 0\n");
    writeFile(sysRoot + QStringLiteral("/zram1/mm_stat"), "262144 65536 131072 0\n");

    const memonitor::MemorySnapshot snapshot = memonitor::MemorySampler::collect(procRoot, sysRoot);
    QVERIFY(snapshot.memoryInfoComplete);
    QVERIFY(snapshot.memoryFreeReadable);
    QVERIFY(snapshot.swapInfoComplete);
    QCOMPARE(snapshot.zramDevices.size(), 2);
    QVERIFY(snapshot.zramDevices.at(0).statisticsReadable);
    QVERIFY(snapshot.zramDevices.at(1).statisticsReadable);
    QCOMPARE(snapshot.zramDevices.at(0).memoryUsedBytes, std::uint64_t(307200));
    QCOMPARE(snapshot.memoryAvailableBytes, std::uint64_t(400U * 1024U));
    QCOMPARE(snapshot.memoryUsedBytes, std::uint64_t(600U * 1024U));
    QCOMPARE(snapshot.memoryFreeBytes, std::uint64_t(32U * 1024U));
    QCOMPARE(snapshot.swapFreeBytes, std::uint64_t(16U * 1024U));
    QCOMPARE(snapshot.memoryUsedBytes, snapshot.memoryTotalBytes - snapshot.memoryAvailableBytes);
    QCOMPARE(snapshot.memoryUsedBytes, baseline.memoryUsedBytes);
    QCOMPARE(snapshot.memoryAvailableBytes, baseline.memoryAvailableBytes);
}

void MemorySamplerTest::appliesStrictWarningBoundary()
{
    QCOMPARE(memonitor::warningThresholdBytes(1000U), std::uint64_t(300U));
    QVERIFY(!memonitor::exceedsWarningThreshold(300U, 1000U));
    QVERIFY(!memonitor::exceedsWarningThreshold(299U, 1000U));
    QVERIFY(memonitor::exceedsWarningThreshold(301U, 1000U));
    QVERIFY(memonitor::exceedsWarningThreshold(std::numeric_limits<std::uint64_t>::max(),
                                               std::numeric_limits<std::uint64_t>::max()));
}

void MemorySamplerTest::appliesCustomWarningBoundaries_data()
{
    QTest::addColumn<qulonglong>("total");
    QTest::addColumn<unsigned>("percent");
    QTest::addColumn<qulonglong>("threshold");
    QTest::addColumn<bool>("fractional");

    QTest::newRow("one-percent") << qulonglong(1000) << 1U << qulonglong(10) << false;
    QTest::newRow("custom") << qulonglong(1000) << 75U << qulonglong(750) << false;
    QTest::newRow("fractional") << qulonglong(101) << 33U << qulonglong(33) << true;
    QTest::newRow("less-than-one-byte") << qulonglong(1) << 1U << qulonglong(0) << true;
    QTest::newRow("above-32-bit") << qulonglong(10000000000ULL) << 30U << qulonglong(3000000000ULL) << false;
    QTest::newRow("maximum-default") << std::numeric_limits<qulonglong>::max()
                                    << 30U << qulonglong(5534023222112865484ULL) << true;
    QTest::newRow("maximum-half") << std::numeric_limits<qulonglong>::max()
                                 << 50U << qulonglong(9223372036854775807ULL) << true;
    QTest::newRow("large-integer-half") << qulonglong(18446744073709551614ULL)
                                       << 50U << qulonglong(9223372036854775807ULL) << false;
    QTest::newRow("maximum-percent") << std::numeric_limits<qulonglong>::max()
                                    << 100U << std::numeric_limits<qulonglong>::max() << false;
}

void MemorySamplerTest::appliesCustomWarningBoundaries()
{
    QFETCH(qulonglong, total);
    QFETCH(unsigned, percent);
    QFETCH(qulonglong, threshold);
    QFETCH(bool, fractional);

    QCOMPARE(memonitor::warningThresholdBytes(total, percent), static_cast<std::uint64_t>(threshold));
    QVERIFY(!memonitor::exceedsWarningThreshold(threshold, total, percent));
    QCOMPARE(memonitor::belowAvailableThreshold(threshold, total, percent), fractional);
    if (threshold > 0) {
        QVERIFY(memonitor::belowAvailableThreshold(threshold - 1, total, percent));
        QVERIFY(!memonitor::exceedsWarningThreshold(threshold - 1, total, percent));
    }
    if (threshold < std::numeric_limits<qulonglong>::max()) {
        QVERIFY(memonitor::exceedsWarningThreshold(threshold + 1, total, percent));
        QVERIFY(!memonitor::belowAvailableThreshold(threshold + 1, total, percent));
    }
    QCOMPARE(memonitor::warningThresholdBytes(0, percent), std::uint64_t(0));
    QVERIFY(!memonitor::exceedsWarningThreshold(1, 0, percent));
    QVERIFY(!memonitor::belowAvailableThreshold(0, 0, percent));
}

void MemorySamplerTest::commandLineInformationWorksWithoutDisplay()
{
    const QString executable = QCoreApplication::applicationDirPath() + QStringLiteral("/memonitor");
    if (!QFileInfo::exists(executable)) {
        QSKIP("memonitor executable was not built alongside the tests");
    }
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("memonitor-invalid-platform"));
    environment.remove(QStringLiteral("DISPLAY"));
    environment.remove(QStringLiteral("WAYLAND_DISPLAY"));
    for (const auto& option : {QStringLiteral("--help"), QStringLiteral("--version")}) {
        QProcess process;
        process.setProcessEnvironment(environment);
        process.start(executable, {option});
        QVERIFY(process.waitForFinished(15000));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), EXIT_SUCCESS);
        const QByteArray output = process.readAllStandardOutput();
        if (option == QStringLiteral("--help")) {
            QVERIFY(output.contains("--dump"));
        } else {
            QVERIFY(output.contains("MemMonitor"));
        }
        QVERIFY2(process.readAllStandardError().isEmpty(), "CLI information must not initialize a GUI platform");
    }
}

void MemorySamplerTest::dumpFailsWhenOutputCannotBeWritten()
{
    const QString executable = QCoreApplication::applicationDirPath() + QStringLiteral("/memonitor");
    if (!QFileInfo::exists(executable) || !QFileInfo::exists(QStringLiteral("/dev/full"))) {
        QSKIP("memonitor executable or /dev/full is unavailable");
    }
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("memonitor-invalid-platform"));
    QProcess process;
    process.setProcessEnvironment(environment);
    process.setStandardOutputFile(QStringLiteral("/dev/full"));
    process.start(executable, {QStringLiteral("--dump")});
    QVERIFY(process.waitForFinished(15000));
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    QCOMPARE(process.exitCode(), EXIT_FAILURE);
    QVERIFY(process.readAllStandardError().contains("Could not write memory snapshot"));
}

QTEST_GUILESS_MAIN(MemorySamplerTest)

#include "memory_sampler_test.moc"
