#include "main_window.h"
#include "memory_pie_chart.h"

#include <QAbstractButton>
#include <QCoreApplication>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDBusConnection>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFormLayout>
#include <QHash>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPersistentModelIndex>
#include <QProgressBar>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QSplitter>
#include <QSystemTrayIcon>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimeZone>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QComboBox>
#include <QTemporaryDir>
#include <QWidget>
#include <QtTest/QtTest>

#include <algorithm>
#include <array>
#include <csignal>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <unistd.h>

namespace {

constexpr int kProcessPidRole = Qt::UserRole + 11;
constexpr int kProcessStartTimeRole = Qt::UserRole + 12;
constexpr std::uint64_t kKiB = 1024;
constexpr std::uint64_t kMiB = kKiB * kKiB;
constexpr std::uint64_t kGiB = kKiB * kKiB * kKiB;

memonitor::MemorySnapshot syntheticSnapshot(int processCount)
{
    memonitor::MemorySnapshot snapshot;
    snapshot.timestamp = QDateTime::fromSecsSinceEpoch(1700000000, QTimeZone::utc());
    snapshot.memoryTotalBytes = 16 * kGiB;
    snapshot.memoryUsedBytes = 4 * kGiB;
    snapshot.memoryAvailableBytes = 12 * kGiB;
    snapshot.swapTotalBytes = 32 * kGiB;
    snapshot.swapUsedBytes = 8 * kGiB;
    snapshot.swapFreeBytes = 24 * kGiB;
    snapshot.memoryInfoReadable = true;
    snapshot.memoryInfoComplete = true;
    snapshot.swapInfoComplete = true;
    snapshot.processes.reserve(processCount);
    for (int processIndex = 0; processIndex < processCount; ++processIndex) {
        memonitor::ProcessMemory process;
        process.pid = 1000000 + processIndex;
        process.ppid = processIndex == 0 ? -1
            : processIndex % 16 == 0 ? 1000000
                                    : 1000000 + (processIndex / 16) * 16;
        process.uid = static_cast<std::uint64_t>(::getuid());
        process.userName = QStringLiteral("gui-fixture-user");
        process.name = QStringLiteral("memonitor-gui-fixture-worker");
        process.executable = QStringLiteral("/nonexistent/memonitor-gui-fixture-worker");
        process.command = QStringLiteral("memonitor-gui-fixture-worker --pid %1").arg(process.pid);
        process.pssBytes = std::uint64_t{512} * kKiB
            + static_cast<std::uint64_t>(processIndex) * std::uint64_t{128};
        process.rssBytes = process.pssBytes;
        process.swapPssBytes = std::uint64_t{64} * kKiB;
        process.swapBytes = process.swapPssBytes;
        process.startTimeTicks = std::uint64_t{100000} + static_cast<std::uint64_t>(processIndex);
        process.pssExact = true;
        process.swapPssExact = true;
        snapshot.processes.push_back(process);
    }
    return snapshot;
}

memonitor::WindowInfo syntheticWindow(qint64 pid)
{
    memonitor::WindowInfo window;
    window.pid = pid;
    window.title = QStringLiteral("Regression document — Window title only");
    window.appId = QStringLiteral("memonitor-gui-fixture");
    return window;
}

QTreeWidgetItem* findProcess(QTreeWidget* tree, qint64 pid)
{
    for (QTreeWidgetItemIterator iterator(tree); *iterator != nullptr; ++iterator) {
        if ((*iterator)->data(0, kProcessPidRole).toLongLong() == pid) {
            return *iterator;
        }
    }
    return nullptr;
}

} // namespace

class NotificationReceiver final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")

public:
    struct Notification {
        QString application;
        uint replacesId;
        QString icon;
        QStringList actions;
        QVariantMap hints;
        int timeout;
    };

    QStringList receivedBodies;
    QStringList receivedSummaries;
    QVector<Notification> receivedNotifications;

public slots:
    uint Notify(const QString& application, uint replacesId, const QString& icon,
                const QString& summary, const QString& body,
                const QStringList& actions, const QVariantMap& hints, int timeout)
    {
        receivedSummaries.push_back(summary);
        receivedBodies.push_back(body);
        receivedNotifications.push_back({application, replacesId, icon, actions, hints, timeout});
        return static_cast<uint>(receivedBodies.size());
    }
};

class GuiSmokeTest final : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void createsNativeMonitorAndRefreshes();
    void windowChromeKeepsControlsAndResizeHandlesInPlace();
    void windowChromeUpdatesMaximizeAndMinimizeState();
    void windowCloseButtonHidesToTray();
    void memoryRingsKeepPhysicalAndZramScalesSeparate();
    void memoryDiagnosticsDistinguishEstimateSwapAndPressure();
    void overviewFitsDarkAndLightPalettes_data();
    void overviewFitsDarkAndLightPalettes();
    void savedProcessScopeFiltersSnapshotAndKeepsSearch();
    void searchEmptyStateAndKeyboardNavigation();
    void clearingSearchRestoresExpansionAndScroll();
    void applicationContextMenuTargetsOwner();
    void terminationSignalsOwnedHelper_data();
    void terminationSignalsOwnedHelper();
    void zramRefreshKeepsDeviceSelectionAndClearsEmptySpan();
    void warningSettingsPersistCancelAndClamp();
    void warningNotificationsUsePlainTextAndRearm();
    void windowTitlesAndChildProcessIcons();
    void approximateProcessValuesRemainVisibleAndSortable();
    void windowOwnersWithSharedAppIdRemainIndependent();
    void inPlaceRefreshRetainsRowsAndInvalidatesExitedIdentities();
    void scrollingDefersOnlyTheTreeAndAppliesLatestSnapshot();
    void largeProcessTreeWarmRefresh_data();
    void largeProcessTreeWarmRefresh();

private:
    static void stopAutomaticRefresh(memonitor::MainWindow& window);

    std::unique_ptr<QTemporaryDir> settingsDirectory_;
    QSettings::Format originalSettingsFormat_ = QSettings::NativeFormat;
};

void GuiSmokeTest::init()
{
    settingsDirectory_ = std::make_unique<QTemporaryDir>();
    QVERIFY(settingsDirectory_->isValid());
    originalSettingsFormat_ = QSettings::defaultFormat();
    QSettings::setDefaultFormat(QSettings::IniFormat);
    const QString settingsRoot = QDir::cleanPath(settingsDirectory_->path()) + QDir::separator();
    for (const QSettings::Format format : {QSettings::NativeFormat, QSettings::IniFormat}) {
        QSettings::setPath(format, QSettings::UserScope, settingsDirectory_->path());
        QSettings::setPath(format, QSettings::SystemScope,
                           settingsDirectory_->filePath(QStringLiteral("system")));
        for (const QSettings::Scope scope : {QSettings::UserScope, QSettings::SystemScope}) {
            const QSettings scopedSettings(format, scope, QStringLiteral("MemMonitor"), QStringLiteral("MemMonitor"));
            QVERIFY2(QDir::cleanPath(scopedSettings.fileName()).startsWith(settingsRoot),
                     qPrintable(scopedSettings.fileName()));
        }
    }
    QSettings savedSettings(QStringLiteral("MemMonitor"), QStringLiteral("MemMonitor"));
    QCOMPARE(savedSettings.format(), QSettings::NativeFormat);
    QVERIFY2(QDir::cleanPath(savedSettings.fileName()).startsWith(settingsRoot),
             qPrintable(savedSettings.fileName()));
    savedSettings.clear();
    savedSettings.setValue(QStringLiteral("notificationsEnabled"), false);
    savedSettings.setValue(QStringLiteral("closeToTray"), false);
    savedSettings.sync();
    QCOMPARE(savedSettings.status(), QSettings::NoError);
}

void GuiSmokeTest::cleanup()
{
    QSettings::setDefaultFormat(originalSettingsFormat_);
    settingsDirectory_.reset();
}

void GuiSmokeTest::stopAutomaticRefresh(memonitor::MainWindow& window)
{
    window.refreshTimer_->stop();
    if (window.refreshWatcher_->isRunning()) {
        window.refreshWatcher_->waitForFinished();
    }
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    window.refreshTimer_->stop();
    window.processTreeIdleTimer_->stop();
    window.processTreeUpdatePending_ = false;
    window.notificationsEnabled_ = false;
    window.windows_.clear();
}

void GuiSmokeTest::createsNativeMonitorAndRefreshes()
{
    QSettings savedSettings(QStringLiteral("MemMonitor"), QStringLiteral("MemMonitor"));
    savedSettings.remove(QStringLiteral("closeToTray"));
    savedSettings.sync();
    QCOMPARE(savedSettings.status(), QSettings::NoError);
    QVERIFY(!savedSettings.contains(QStringLiteral("closeToTray")));

    memonitor::MainWindow window(nullptr, false);
    QVERIFY(window.closeToTray_);
    delete window.trayIcon_;
    window.trayIcon_ = nullptr;
    window.show();

    QVERIFY(window.isVisible());
    QCOMPARE(window.findChildren<QTabWidget*>().size(), 1);
    QCOMPARE(window.findChildren<QTableWidget*>().size(), 1);
    QCOMPARE(window.findChildren<QTreeWidget*>().size(), 1);
    QSplitter* mainSplitter = window.findChild<QSplitter*>(QStringLiteral("mainSplitter"));
    QVERIFY(mainSplitter != nullptr);

    QProgressBar* memoryProgress = window.findChild<QProgressBar*>(QStringLiteral("memoryProgress"));
    QVERIFY(memoryProgress != nullptr);
    QWidget* memoryChart = window.findChild<QWidget*>(QStringLiteral("memoryPieChart"));
    QVERIFY(memoryChart != nullptr);
    QVERIFY(window.findChildren<QComboBox*>().isEmpty());
    QCOMPARE(window.defaultProcessScope_, 0);
    QCOMPARE(window.findChildren<QLineEdit*>().size(), 1);
    QLineEdit* processFilter = window.findChild<QLineEdit*>(QStringLiteral("processFilter"));
    QVERIFY(processFilter != nullptr);
    QTreeWidget* processTree = window.findChild<QTreeWidget*>(QStringLiteral("processTree"));
    QVERIFY(processTree != nullptr);
    QCOMPARE(processFilter->parentWidget(), processTree->parentWidget());
    QVERIFY(window.findChild<QTabWidget*>()->indexOf(processTree->parentWidget()) >= 0);
    QPushButton* settingsButton = window.findChild<QPushButton*>(QStringLiteral("settingsButton"));
    QVERIFY(settingsButton != nullptr);
    QTimer modalTimeout;
    modalTimeout.setSingleShot(true);
    modalTimeout.setInterval(2000);
    connect(&modalTimeout, &QTimer::timeout, &window, []() {
        if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
            dialog->reject();
        }
    });
    bool settingsApplied = false;
    bool closeToTrayReadBack = false;
    bool scopeLabelReadBack = false;
    QTimer::singleShot(0, &window, [&settingsApplied, &closeToTrayReadBack, &scopeLabelReadBack]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (dialog == nullptr) {
            return;
        }
        auto* refreshInterval = dialog->findChild<QSpinBox*>(QStringLiteral("refreshIntervalSetting"));
        auto* defaultScope = dialog->findChild<QComboBox*>(QStringLiteral("defaultProcessScopeSetting"));
        auto* notifications = dialog->findChild<QCheckBox*>(QStringLiteral("notificationsSetting"));
        auto* closeToTray = dialog->findChild<QCheckBox*>(QStringLiteral("closeToTraySetting"));
        auto* form = dialog->findChild<QFormLayout*>();
        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        if (refreshInterval == nullptr || defaultScope == nullptr || notifications == nullptr
            || closeToTray == nullptr || form == nullptr || buttons == nullptr
            || buttons->button(QDialogButtonBox::Ok) == nullptr) {
            dialog->reject();
            return;
        }
        const auto* scopeLabel = qobject_cast<QLabel*>(form->labelForField(defaultScope));
        scopeLabelReadBack = scopeLabel != nullptr && scopeLabel->text() == QStringLiteral("进程范围");
        refreshInterval->setValue(2500);
        defaultScope->setCurrentIndex(1);
        notifications->setChecked(false);
        closeToTrayReadBack = closeToTray->isChecked() && !closeToTray->isEnabled();
        settingsApplied = true;
        buttons->button(QDialogButtonBox::Ok)->click();
    });
    modalTimeout.start();
    settingsButton->click();
    modalTimeout.stop();
    QVERIFY(settingsApplied);
    QVERIFY(closeToTrayReadBack);
    QVERIFY(scopeLabelReadBack);
    QVERIFY(window.closeToTray_);
    QCOMPARE(window.defaultProcessScope_, 1);
    QCOMPARE(window.refreshIntervalMs_, 2500);
    QCOMPARE(window.refreshTimer_->interval(), 2500);
    savedSettings.sync();
    QCOMPARE(savedSettings.status(), QSettings::NoError);
    QCOMPARE(savedSettings.value(QStringLiteral("refreshIntervalMs")).toInt(), 2500);
    QCOMPARE(savedSettings.value(QStringLiteral("defaultProcessScope")).toInt(), 1);
    QVERIFY(!savedSettings.value(QStringLiteral("notificationsEnabled")).toBool());
    QVERIFY(savedSettings.value(QStringLiteral("closeToTray")).toBool());

    QVariantMap settingsBeforeCancel;
    for (const QString& key : savedSettings.allKeys()) {
        settingsBeforeCancel.insert(key, savedSettings.value(key));
    }
    bool settingsCancelled = false;
    QTimer::singleShot(0, &window, [&settingsCancelled]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (dialog == nullptr) {
            return;
        }
        auto* refreshInterval = dialog->findChild<QSpinBox*>(QStringLiteral("refreshIntervalSetting"));
        auto* defaultScope = dialog->findChild<QComboBox*>(QStringLiteral("defaultProcessScopeSetting"));
        auto* notifications = dialog->findChild<QCheckBox*>(QStringLiteral("notificationsSetting"));
        auto* closeToTray = dialog->findChild<QCheckBox*>(QStringLiteral("closeToTraySetting"));
        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        if (refreshInterval == nullptr || defaultScope == nullptr || notifications == nullptr
            || closeToTray == nullptr || buttons == nullptr || buttons->button(QDialogButtonBox::Cancel) == nullptr) {
            dialog->reject();
            return;
        }
        refreshInterval->setValue(10000);
        defaultScope->setCurrentIndex(0);
        notifications->setChecked(true);
        closeToTray->setChecked(false);
        settingsCancelled = true;
        buttons->button(QDialogButtonBox::Cancel)->click();
    });
    modalTimeout.start();
    settingsButton->click();
    modalTimeout.stop();
    QVERIFY(settingsCancelled);
    QCOMPARE(window.refreshIntervalMs_, 2500);
    QCOMPARE(window.refreshTimer_->interval(), 2500);
    QCOMPARE(window.defaultProcessScope_, 1);
    QVERIFY(!window.notificationsEnabled_);
    QVERIFY(window.closeToTray_);
    savedSettings.sync();
    QCOMPARE(savedSettings.status(), QSettings::NoError);
    QVariantMap settingsAfterCancel;
    for (const QString& key : savedSettings.allKeys()) {
        settingsAfterCancel.insert(key, savedSettings.value(key));
    }
    QCOMPARE(settingsAfterCancel, settingsBeforeCancel);
    {
        memonitor::MainWindow restoredWindow(nullptr, false);
        QVERIFY(restoredWindow.findChildren<QComboBox*>().isEmpty());
        QCOMPARE(restoredWindow.defaultProcessScope_, 1);
        QCOMPARE(restoredWindow.refreshIntervalMs_, 2500);
        QCOMPARE(restoredWindow.refreshTimer_->interval(), 2500);
        QVERIFY(restoredWindow.closeToTray_);
    }

    QLabel* status = window.findChild<QLabel*>(QStringLiteral("statusLabel"));
    QVERIFY(status != nullptr);
    QElapsedTimer refreshTimeout;
    refreshTimeout.start();
    while ((!status->text().contains(QStringLiteral("更新于")) || memoryProgress->value() == 0
            || processTree->topLevelItemCount() == 0)
           && refreshTimeout.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    QVERIFY(status->text().contains(QStringLiteral("更新于")));
    QVERIFY(memoryProgress->value() > 0);
    QVERIFY(processTree->topLevelItemCount() > 0);

    QLabel* processSummary = window.findChild<QLabel*>(QStringLiteral("processSummary"));
    QVERIFY(processSummary != nullptr);
    processFilter->setText(QString::number(QCoreApplication::applicationPid()));
    QVERIFY(processSummary->text().startsWith(QStringLiteral("匹配")));
    processFilter->clear();

    window.resize(900, 700);
    QCoreApplication::processEvents();
    QVERIFY(memoryProgress->isVisible());
    QVERIFY(mainSplitter->sizes().at(0) < mainSplitter->sizes().at(1));

    window.resize(1200, 760);
    QCoreApplication::processEvents();
    QVERIFY(memoryProgress->isVisible());
    QVERIFY(mainSplitter->sizes().at(0) < mainSplitter->sizes().at(1));
    QVERIFY(mainSplitter->sizes().at(0) <= 340);
}

void GuiSmokeTest::windowChromeKeepsControlsAndResizeHandlesInPlace()
{
    memonitor::MainWindow window(nullptr, false);
    stopAutomaticRefresh(window);
    QVERIFY(window.windowFlags().testFlag(Qt::FramelessWindowHint));
    auto* header = window.findChild<QWidget*>(QStringLiteral("windowHeader"));
    QVERIFY(header != nullptr);
    auto* title = window.findChild<QLabel*>(QStringLiteral("windowTitleLabel"));
    QVERIFY(title != nullptr);
    QCOMPARE(title->text(), window.windowTitle());
    QVERIFY(header->isAncestorOf(title));
    auto* minimizeButton = window.findChild<QAbstractButton*>(QStringLiteral("minimizeWindowButton"));
    QVERIFY(minimizeButton != nullptr);
    auto* maximizeButton = window.findChild<QAbstractButton*>(QStringLiteral("maximizeWindowButton"));
    QVERIFY(maximizeButton != nullptr);
    auto* closeButton = window.findChild<QAbstractButton*>(QStringLiteral("closeWindowButton"));
    QVERIFY(closeButton != nullptr);
    auto* settingsButton = window.findChild<QAbstractButton*>(QStringLiteral("settingsButton"));
    QVERIFY(settingsButton != nullptr);
    auto* aboutButton = window.findChild<QAbstractButton*>(QStringLiteral("aboutButton"));
    QVERIFY(aboutButton != nullptr);
    auto* mainSplitter = window.findChild<QSplitter*>(QStringLiteral("mainSplitter"));
    QVERIFY(mainSplitter != nullptr);
    auto* processFilter = window.findChild<QLineEdit*>(QStringLiteral("processFilter"));
    QVERIFY(processFilter != nullptr);
    auto* processTree = window.findChild<QTreeWidget*>(QStringLiteral("processTree"));
    QVERIFY(processTree != nullptr);

    std::array<QWidget*, 8> resizeHandles{};
    for (std::size_t index = 0; index < resizeHandles.size(); ++index) {
        resizeHandles[index] = window.findChild<QWidget*>(QStringLiteral("windowResizeHandle%1").arg(index));
        QVERIFY2(resizeHandles[index] != nullptr,
                 qPrintable(QStringLiteral("Missing windowResizeHandle%1").arg(index)));
    }
    const std::array<QAbstractButton*, 5> headerButtons{
        minimizeButton, maximizeButton, closeButton, settingsButton, aboutButton,
    };
    const std::array<QWidget*, 9> unobscuredWidgets{
        title, minimizeButton, maximizeButton, closeButton, settingsButton, aboutButton,
        mainSplitter, processFilter, processTree->viewport(),
    };
    window.show();
    for (const QSize windowSize : {QSize(900, 700), QSize(1200, 760)}) {
        window.resize(windowSize);
        QCoreApplication::processEvents();
        QCOMPARE(window.size(), windowSize);
        const QRect headerBounds(header->mapTo(&window, QPoint()), header->size());
        const QRect titleBounds(title->mapTo(&window, QPoint()), title->size());
        QVERIFY(window.rect().contains(headerBounds));
        QCOMPARE(headerBounds.top(), 0);
        QCOMPARE(headerBounds.left(), 0);
        QCOMPARE(headerBounds.right(), window.rect().right());
        QCOMPARE(headerBounds.height(), 40);
        QVERIFY(headerBounds.contains(titleBounds));
        QVERIFY(headerBounds.bottom() < mainSplitter->mapTo(&window, QPoint()).y());
        for (QAbstractButton* button : headerButtons) {
            QVERIFY(header->isAncestorOf(button));
            QVERIFY(button->isVisible());
            QVERIFY(button->isEnabled());
            QVERIFY(!button->icon().isNull());
            QVERIFY(!button->icon().pixmap(24, 24).isNull());
            const QRect buttonBounds(button->mapTo(&window, QPoint()), button->size());
            QVERIFY(headerBounds.contains(buttonBounds));
            QVERIFY(buttonBounds.top() <= titleBounds.center().y());
            QVERIFY(buttonBounds.bottom() >= titleBounds.center().y());
        }
        QVERIFY(title->mapTo(&window, title->rect().center()).x()
                < minimizeButton->mapTo(&window, minimizeButton->rect().center()).x());
        QVERIFY(minimizeButton->mapTo(&window, minimizeButton->rect().center()).x()
                < maximizeButton->mapTo(&window, maximizeButton->rect().center()).x());
        QVERIFY(maximizeButton->mapTo(&window, maximizeButton->rect().center()).x()
                < closeButton->mapTo(&window, closeButton->rect().center()).x());

        const QRect minimizeBounds(minimizeButton->mapTo(&window, QPoint()), minimizeButton->size());
        const QRect maximizeBounds(maximizeButton->mapTo(&window, QPoint()), maximizeButton->size());
        const QRect closeBounds(closeButton->mapTo(&window, QPoint()), closeButton->size());
        QCOMPARE(closeBounds.top(), 0);
        QCOMPARE(closeBounds.right(), window.rect().right());
        QCOMPARE(minimizeBounds.size(), QSize(46, 40));
        QCOMPARE(maximizeBounds.size(), minimizeBounds.size());
        QCOMPARE(closeBounds.size(), minimizeBounds.size());
        QCOMPARE(minimizeBounds.top(), closeBounds.top());
        QCOMPARE(maximizeBounds.top(), closeBounds.top());
        QCOMPARE(maximizeBounds.left(), minimizeBounds.right() + 1);
        QCOMPARE(closeBounds.left(), maximizeBounds.right() + 1);

        for (QWidget* handle : resizeHandles) {
            QVERIFY(handle->isVisible());
            const QRect bounds(handle->mapTo(&window, QPoint()), handle->size());
            QVERIFY(!bounds.isEmpty());
            QVERIFY(window.rect().contains(bounds));
            QWidget* hit = window.childAt(bounds.center());
            QVERIFY(hit == handle || handle->isAncestorOf(hit));
        }
        const QRect bounds = window.rect();
        const std::array<QPoint, 8> edgePoints{
            bounds.topLeft(), QPoint(bounds.center().x(), bounds.top()), bounds.topRight(),
            QPoint(bounds.right(), bounds.center().y()), bounds.bottomRight(),
            QPoint(bounds.center().x(), bounds.bottom()), bounds.bottomLeft(),
            QPoint(bounds.left(), bounds.center().y()),
        };
        for (const QPoint& point : edgePoints) {
            QWidget* hit = window.childAt(point);
            QVERIFY(std::any_of(resizeHandles.cbegin(), resizeHandles.cend(), [hit](QWidget* handle) {
                return hit == handle || handle->isAncestorOf(hit);
            }));
        }
        for (QWidget* widget : unobscuredWidgets) {
            QVERIFY(widget->isVisible());
            QWidget* hit = window.childAt(widget->mapTo(&window, widget->rect().center()));
            QVERIFY2(hit == widget || widget->isAncestorOf(hit), qPrintable(widget->objectName()));
        }
    }
}

void GuiSmokeTest::windowChromeUpdatesMaximizeAndMinimizeState()
{
    memonitor::MainWindow window(nullptr, false);
    stopAutomaticRefresh(window);
    auto* header = window.findChild<QWidget*>(QStringLiteral("windowHeader"));
    QVERIFY(header != nullptr);
    auto* title = window.findChild<QLabel*>(QStringLiteral("windowTitleLabel"));
    QVERIFY(title != nullptr);
    auto* maximizeButton = window.findChild<QAbstractButton*>(QStringLiteral("maximizeWindowButton"));
    QVERIFY(maximizeButton != nullptr);
    auto* minimizeButton = window.findChild<QAbstractButton*>(QStringLiteral("minimizeWindowButton"));
    QVERIFY(minimizeButton != nullptr);
    std::array<QWidget*, 8> resizeHandles{};
    for (std::size_t index = 0; index < resizeHandles.size(); ++index) {
        resizeHandles[index] = window.findChild<QWidget*>(QStringLiteral("windowResizeHandle%1").arg(index));
        QVERIFY(resizeHandles[index] != nullptr);
    }
    window.show();
    QCoreApplication::processEvents();
    QVERIFY(!window.isMaximized());
    QCOMPARE(maximizeButton->accessibleName(), QStringLiteral("最大化"));
    QCOMPARE(maximizeButton->toolTip(), QStringLiteral("最大化"));
    QCOMPARE(minimizeButton->accessibleName(), QStringLiteral("最小化"));
    QCOMPARE(minimizeButton->toolTip(), QStringLiteral("最小化"));
    const QImage maximizeIcon = maximizeButton->icon().pixmap(24, 24).toImage();
    QVERIFY(!maximizeIcon.isNull());

    QTest::mouseClick(maximizeButton, Qt::LeftButton);
    QVERIFY(QTest::qWaitFor([&]() { return window.isMaximized(); }, 1000));
    QVERIFY(QTest::qWaitFor([&]() {
        return maximizeButton->accessibleName() == QStringLiteral("还原");
    }, 1000));
    QCOMPARE(maximizeButton->accessibleName(), QStringLiteral("还原"));
    QVERIFY(QTest::qWaitFor([&]() {
        return maximizeButton->toolTip() == QStringLiteral("还原");
    }, 1000));
    QCOMPARE(maximizeButton->toolTip(), QStringLiteral("还原"));
    const QImage restoreIcon = maximizeButton->icon().pixmap(24, 24).toImage();
    QVERIFY(!restoreIcon.isNull());
    QVERIFY(restoreIcon != maximizeIcon);
    for (QWidget* handle : resizeHandles) {
        QVERIFY(handle->isHidden());
    }

    QTest::mouseClick(maximizeButton, Qt::LeftButton);
    QVERIFY(QTest::qWaitFor([&]() { return !window.isMaximized(); }, 1000));
    QVERIFY(QTest::qWaitFor([&]() {
        return maximizeButton->accessibleName() == QStringLiteral("最大化");
    }, 1000));
    QCOMPARE(maximizeButton->accessibleName(), QStringLiteral("最大化"));
    QVERIFY(QTest::qWaitFor([&]() {
        return maximizeButton->toolTip() == QStringLiteral("最大化");
    }, 1000));
    QCOMPARE(maximizeButton->toolTip(), QStringLiteral("最大化"));
    QCOMPARE(maximizeButton->icon().pixmap(24, 24).toImage(), maximizeIcon);
    for (QWidget* handle : resizeHandles) {
        QVERIFY(handle->isVisible());
    }

    QPoint headerPoint(-1, -1);
    for (int horizontal = 1; horizontal < header->width(); ++horizontal) {
        const QPoint candidate(horizontal, header->height() / 2);
        if (header->childAt(candidate) == nullptr
            && window.childAt(header->mapTo(&window, candidate)) == header) {
            headerPoint = candidate;
            break;
        }
    }
    QVERIFY(header->rect().contains(headerPoint));
    QCOMPARE(window.childAt(header->mapTo(&window, headerPoint)), header);
    for (QWidget* target : {header, static_cast<QWidget*>(title)}) {
        const QPoint point = target == header ? headerPoint : title->rect().center();
        QTest::mouseDClick(target, Qt::LeftButton, Qt::NoModifier, point);
        QTest::mouseRelease(target, Qt::LeftButton, Qt::NoModifier, point);
        QVERIFY(QTest::qWaitFor([&]() { return window.isMaximized(); }, 1000));
        QVERIFY(QTest::qWaitFor([&]() {
            return maximizeButton->accessibleName() == QStringLiteral("还原");
        }, 1000));
        QCOMPARE(maximizeButton->accessibleName(), QStringLiteral("还原"));
        QVERIFY(QTest::qWaitFor([&]() {
            return maximizeButton->toolTip() == QStringLiteral("还原");
        }, 1000));
        QCOMPARE(maximizeButton->toolTip(), QStringLiteral("还原"));
        QCOMPARE(maximizeButton->icon().pixmap(24, 24).toImage(), restoreIcon);
        for (QWidget* handle : resizeHandles) {
            QVERIFY(handle->isHidden());
        }

        QTest::mouseDClick(target, Qt::LeftButton, Qt::NoModifier, point);
        QTest::mouseRelease(target, Qt::LeftButton, Qt::NoModifier, point);
        QVERIFY(QTest::qWaitFor([&]() { return !window.isMaximized(); }, 1000));
        QVERIFY(QTest::qWaitFor([&]() {
            return maximizeButton->accessibleName() == QStringLiteral("最大化");
        }, 1000));
        QCOMPARE(maximizeButton->accessibleName(), QStringLiteral("最大化"));
        QVERIFY(QTest::qWaitFor([&]() {
            return maximizeButton->toolTip() == QStringLiteral("最大化");
        }, 1000));
        QCOMPARE(maximizeButton->toolTip(), QStringLiteral("最大化"));
        QCOMPARE(maximizeButton->icon().pixmap(24, 24).toImage(), maximizeIcon);
        for (QWidget* handle : resizeHandles) {
            QVERIFY(handle->isVisible());
        }
    }

    QTest::mouseClick(minimizeButton, Qt::LeftButton);
    QVERIFY(QTest::qWaitFor([&]() { return window.isMinimized(); }, 1000));
    for (QWidget* handle : resizeHandles) {
        QVERIFY(handle->isHidden());
    }
    window.showNormal();
    QVERIFY(QTest::qWaitFor([&]() {
        return window.isVisible() && !window.isMinimized() && !window.isMaximized();
    }, 1000));
    QCOMPARE(maximizeButton->accessibleName(), QStringLiteral("最大化"));
    QCOMPARE(maximizeButton->toolTip(), QStringLiteral("最大化"));
    QCOMPARE(maximizeButton->icon().pixmap(24, 24).toImage(), maximizeIcon);
    for (QWidget* handle : resizeHandles) {
        QVERIFY(handle->isVisible());
    }
}

void GuiSmokeTest::windowCloseButtonHidesToTray()
{
    const bool quitOnLastWindowClosed = qApp->quitOnLastWindowClosed();
    const auto restoreQuitPolicy = qScopeGuard([quitOnLastWindowClosed]() {
        qApp->setQuitOnLastWindowClosed(quitOnLastWindowClosed);
    });
    qApp->setQuitOnLastWindowClosed(false);
    memonitor::MainWindow window(nullptr, false);
    stopAutomaticRefresh(window);
    window.closeToTray_ = true;
    if (window.trayIcon_ == nullptr) {
        window.trayIcon_ = new QSystemTrayIcon(&window);
    }
    auto* closeButton = window.findChild<QAbstractButton*>(QStringLiteral("closeWindowButton"));
    QVERIFY(closeButton != nullptr);
    QCOMPARE(closeButton->accessibleName(), QStringLiteral("关闭"));
    QCOMPARE(closeButton->toolTip(), QStringLiteral("关闭"));
    window.show();
    QCoreApplication::processEvents();
    QVERIFY(window.isVisible());
    QSignalSpy lastWindowClosedSpy(qApp, &QGuiApplication::lastWindowClosed);
    QVERIFY(lastWindowClosedSpy.isValid());
    QTest::mouseClick(closeButton, Qt::LeftButton);
    QVERIFY(QTest::qWaitFor([&]() { return !window.isVisible(); }, 1000));
    QCOMPARE(lastWindowClosedSpy.count(), 0);
    window.showNormal();
    QVERIFY(QTest::qWaitFor([&]() { return window.isVisible(); }, 1000));
}

void GuiSmokeTest::memoryRingsKeepPhysicalAndZramScalesSeparate()
{
    memonitor::MemoryPieChart chart;
    chart.resize(320, 300);
    chart.setMemory(8 * kGiB, 2 * kGiB, kGiB, 6 * kGiB, 8 * kGiB);
    QCOMPARE(chart.availableBytes_, 2 * kGiB);
    QCOMPARE(chart.usedBytes_, 6 * kGiB);
    QCOMPARE(chart.regularBytes_, 5 * kGiB);
    QCOMPARE(chart.availableBytes_ + chart.regularBytes_ + chart.zramRamBytes_, 8 * kGiB);
    const QString physicalTooltip = chart.tooltipFor(memonitor::MemoryPieChart::Segment::None);
    QVERIFY(physicalTooltip.contains(QStringLiteral("75.0%")));
    QVERIFY(!physicalTooltip.contains(QStringLiteral("组合已用")));
    const QString zramTooltip = chart.tooltipFor(memonitor::MemoryPieChart::Segment::ZramLogical);
    QVERIFY(zramTooltip.contains(QStringLiteral("6.00 GiB / 8.00 GiB")));
    QVERIFY(zramTooltip.contains(QStringLiteral("75.0%")));

    const QRectF bounds = chart.chartRect();
    QVERIFY(bounds.width() <= 200.0);
    const QPointF center = bounds.center();
    const qreal radius = bounds.width() / 2.0;
    const auto pointAt = [center, radius](double degrees, double fraction) {
        const double angle = degrees * 3.14159265358979323846 / 180.0;
        return center + QPointF(std::cos(angle) * radius * fraction,
                                std::sin(angle) * radius * fraction);
    };
    QCOMPARE(chart.segmentAt(pointAt(45.0, 0.9)), memonitor::MemoryPieChart::Segment::Available);
    QCOMPARE(chart.segmentAt(pointAt(180.0, 0.9)), memonitor::MemoryPieChart::Segment::Regular);
    QCOMPARE(chart.segmentAt(pointAt(340.0, 0.9)), memonitor::MemoryPieChart::Segment::Zram);
    QCOMPARE(chart.segmentAt(pointAt(45.0, 0.63)), memonitor::MemoryPieChart::Segment::ZramLogical);
    QCOMPARE(chart.segmentAt(center), memonitor::MemoryPieChart::Segment::None);
    QVERIFY(!chart.grab().isNull());

    // Changing compressed data must not change the physical percentage.
    chart.setMemory(8 * kGiB, 2 * kGiB, kGiB, 7 * kGiB, 8 * kGiB);
    QCOMPARE(chart.usedBytes_, 6 * kGiB);
    QVERIFY(chart.tooltipFor(memonitor::MemoryPieChart::Segment::ZramLogical)
                .contains(QStringLiteral("87.5%")));
    QVERIFY(chart.tooltipFor(memonitor::MemoryPieChart::Segment::None)
                .contains(QStringLiteral("物理已用 6.00 GiB / 8.00 GiB（75.0%）")));

    chart.setMemory(8 * kGiB, 7 * kGiB, 2 * kGiB, 3 * kGiB, 8 * kGiB);
    QVERIFY(chart.zramValueClamped_);
    QCOMPARE(chart.zramRamBytes_, kGiB);
    QCOMPARE(chart.zramReportedBytes_, 2 * kGiB);
    QVERIFY(chart.tooltipFor(memonitor::MemoryPieChart::Segment::ZramLogical)
                .contains(QStringLiteral("实际 RAM 为 2.00 GiB")));
    QVERIFY(chart.tooltipFor(memonitor::MemoryPieChart::Segment::Zram)
                .contains(QStringLiteral("设备采样实际值 2.00 GiB")));
    QCOMPARE(chart.regularBytes_, std::uint64_t(0));
    QCOMPARE(chart.availableBytes_ + chart.regularBytes_ + chart.zramRamBytes_, 8 * kGiB);

    chart.setMemory(8 * kGiB, 8 * kGiB, 0, 0, 0);
    QCOMPARE(chart.segmentAt(pointAt(45.0, 0.63)), memonitor::MemoryPieChart::Segment::None);
    chart.setMemory(0, 0, 0, 0, 0);
    QVERIFY(!chart.hasSnapshot_);
    QVERIFY(!chart.grab().isNull());
}

void GuiSmokeTest::memoryDiagnosticsDistinguishEstimateSwapAndPressure()
{
    memonitor::MainWindow window(nullptr, false);
    stopAutomaticRefresh(window);
    auto snapshot = syntheticSnapshot(0);
    snapshot.memoryFreeBytes = 512 * 1024 * kKiB;
    snapshot.memoryFreeReadable = true;
    snapshot.swapUsedBytes = snapshot.swapTotalBytes - 128 * 1024 * kKiB;
    snapshot.swapFreeBytes = 128 * 1024 * kKiB;
    snapshot.memoryPressureReadable = true;
    snapshot.memoryPressureSomeAvg10 = 21.5;
    snapshot.memoryPressureFullAvg10 = 8.0;
    snapshot.oomKillCountReadable = true;
    snapshot.oomKillCount = 3;
    memonitor::ZramDevice device;
    device.name = QStringLiteral("zram0");
    device.diskSizeBytes = 32 * kGiB;
    device.originalDataBytes = 31 * kGiB;
    device.memoryUsedBytes = kGiB;
    device.compressedDataBytes = 900 * 1024 * kKiB;
    device.statisticsReadable = true;
    device.ioStatisticsReadable = true;
    device.failedWrites = 4;
    snapshot.zramDevices = {device};
    window.applySnapshot(snapshot);
    QVERIFY(window.physicalDetail_->text().contains(QStringLiteral("估算可用 12.0 GiB")));
    QVERIFY(window.physicalDetail_->text().contains(QStringLiteral("空闲 512 MiB")));
    QCOMPARE(window.swapValue_->text(), QStringLiteral("128 MiB"));
    QVERIFY(window.swapDetail_->text().contains(QStringLiteral("99.6%")));
    QCOMPARE(window.pressureValue_->text(), QStringLiteral("阻塞 21.5%"));
    QCOMPARE(window.pressureDetail_->text(), QStringLiteral("完全阻塞 8.0% · OOM 击杀 3"));
    QVERIFY(window.zramDetail_->toolTip().contains(QStringLiteral("写失败 4 次")));
    QCOMPARE(window.memoryChart_->zramCapacityBytes_, 32 * kGiB);
    QCOMPARE(window.memoryChart_->usedBytes_, 4 * kGiB);

    window.resize(800, 600);
    window.show();
    QCoreApplication::processEvents();
    QVERIFY(window.memoryChart_->chartRect().width() >= 144.0);
    QScrollArea* overviewScroll = window.findChild<QScrollArea*>(QStringLiteral("overviewScroll"));
    QVERIFY(overviewScroll != nullptr);
    overviewScroll->ensureWidgetVisible(window.pressureDetail_);
    QCoreApplication::processEvents();
    const QPoint pressureBottom = window.pressureDetail_->mapTo(overviewScroll->viewport(),
                                                               QPoint(0, window.pressureDetail_->height()));
    QVERIFY(pressureBottom.y() <= overviewScroll->viewport()->height());

    snapshot.memoryFreeReadable = false;
    snapshot.swapInfoComplete = false;
    snapshot.memoryPressureReadable = false;
    snapshot.oomKillCountReadable = false;
    snapshot.zramDevices[0].diskSizeBytes = 0;
    snapshot.zramDevices[0].ioStatisticsReadable = false;
    window.applySnapshot(snapshot);
    QVERIFY(window.physicalDetail_->text().contains(QStringLiteral("空闲 不可读")));
    QCOMPARE(window.swapValue_->text(), QStringLiteral("--"));
    QCOMPARE(window.pressureValue_->text(), QStringLiteral("不可读"));
    QCOMPARE(window.pressureDetail_->text(), QStringLiteral("OOM 计数不可读"));
    QCOMPARE(window.memoryChart_->zramCapacityBytes_, std::uint64_t(0));
    QVERIFY(!window.zramDetail_->toolTip().contains(QStringLiteral("写失败")));

    memonitor::ZramDevice unreadableDevice;
    unreadableDevice.name = QStringLiteral("zram1");
    snapshot.zramDevices.push_back(unreadableDevice);
    window.applySnapshot(snapshot);
    QVERIFY(window.zramValue_->text().startsWith(QStringLiteral("部分")));
    QVERIFY(!window.memoryChart_->zramStatisticsComplete_);
    QCOMPARE(window.memoryChart_->zramRamBytes_, std::uint64_t(0));
    QCOMPARE(window.memoryChart_->regularBytes_, snapshot.memoryUsedBytes);
    QVERIFY(window.memoryChart_->tooltipFor(memonitor::MemoryPieChart::Segment::Regular)
                .contains(QStringLiteral("包含 zram")));
    QVERIFY(window.memoryChart_->tooltipFor(memonitor::MemoryPieChart::Segment::Zram)
                .contains(QStringLiteral("统计不完整")));

    snapshot.memoryInfoComplete = false;
    snapshot.memoryFreeReadable = true;
    window.applySnapshot(snapshot);
    QVERIFY(window.physicalDetail_->text().contains(QStringLiteral("空闲 512 MiB")));
    QVERIFY(window.physicalDetail_->text().contains(QStringLiteral("估算可用不可读")));
}

void GuiSmokeTest::overviewFitsDarkAndLightPalettes_data()
{
    QTest::addColumn<bool>("dark");
    QTest::addColumn<QSize>("windowSize");
    QTest::newRow("dark-default") << true << QSize(1120, 760);
    QTest::newRow("light-default") << false << QSize(1120, 760);
    QTest::newRow("dark-minimum") << true << QSize(800, 600);
    QTest::newRow("light-minimum") << false << QSize(800, 600);
}

void GuiSmokeTest::overviewFitsDarkAndLightPalettes()
{
    QFETCH(bool, dark);
    QFETCH(QSize, windowSize);
    const QPalette savedPalette = qApp->palette();
    const auto restorePalette = qScopeGuard([savedPalette]() { qApp->setPalette(savedPalette); });
    QPalette palette = savedPalette;
    const QColor text = dark ? QColor(239, 240, 241) : QColor(35, 38, 41);
    const QColor background = dark ? QColor(35, 38, 41) : QColor(239, 240, 241);
    palette.setColor(QPalette::Window, background);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::Button, background);
    palette.setColor(QPalette::Base, dark ? QColor(27, 30, 32) : QColor(252, 252, 252));
    palette.setColor(QPalette::AlternateBase, dark ? QColor(42, 46, 50) : QColor(247, 247, 247));
    palette.setColor(QPalette::PlaceholderText, dark ? QColor(161, 169, 177) : QColor(98, 104, 110));
    palette.setColor(QPalette::Mid, dark ? QColor(94, 101, 109) : QColor(188, 191, 196));
    palette.setColor(QPalette::Highlight, QColor(61, 174, 233));
    palette.setColor(QPalette::HighlightedText, QColor(Qt::white));
    qApp->setPalette(palette);

    memonitor::MainWindow window(nullptr, false);
    stopAutomaticRefresh(window);
    auto snapshot = syntheticSnapshot(3);
    snapshot.memoryFreeReadable = true;
    snapshot.memoryFreeBytes = kGiB;
    snapshot.memoryPressureReadable = true;
    snapshot.memoryPressureSomeAvg10 = 2.5;
    snapshot.memoryPressureFullAvg10 = 0.1;
    snapshot.oomKillCountReadable = true;
    memonitor::ZramDevice zram;
    zram.name = QStringLiteral("zram0");
    zram.compressionAlgorithm = QStringLiteral("zstd");
    zram.diskSizeBytes = 32 * kGiB;
    zram.originalDataBytes = 8 * kGiB;
    zram.compressedDataBytes = 900 * 1024 * kKiB;
    zram.memoryUsedBytes = kGiB;
    zram.statisticsReadable = true;
    snapshot.zramDevices = {zram};
    window.windows_ = {syntheticWindow(snapshot.processes.constFirst().pid)};
    window.applySnapshot(snapshot);
    window.resize(windowSize);
    window.show();
    QCoreApplication::processEvents();

    const auto* chart = window.memoryChart_;
    QCOMPARE(chart->palette().color(QPalette::Window), background);
    QVERIFY(chart->chartRect().width() >= 144.0);
    QVERIFY(chart->chartRect().width() <= 200.0);
    QVERIFY(chart->legendRect().bottom() < chart->height());
    QVERIFY(chart->legendRect().left() >= 0.0);
    QVERIFY(chart->legendRect().right() <= chart->width());
    auto* splitter = window.findChild<QSplitter*>(QStringLiteral("mainSplitter"));
    QVERIFY(splitter != nullptr);
    QVERIFY(splitter->sizes().constFirst() <= 340);
    auto* scroll = window.findChild<QScrollArea*>(QStringLiteral("overviewScroll"));
    QVERIFY(scroll != nullptr);
    scroll->ensureWidgetVisible(window.pressureDetail_);
    QCoreApplication::processEvents();
    QVERIFY(window.pressureDetail_->mapTo(scroll->viewport(), QPoint(0, window.pressureDetail_->height())).y()
            <= scroll->viewport()->height());
    scroll->verticalScrollBar()->setValue(0);
    const QPixmap rendered = window.grab();
    QVERIFY(!rendered.isNull());
    const QString screenshotRoot = qEnvironmentVariable("MEMONITOR_TEST_SCREENSHOTS");
    if (!screenshotRoot.isEmpty()) {
        QVERIFY(QDir().mkpath(screenshotRoot));
        const QString filename = QStringLiteral("overview-%1-%2x%3.png")
                                     .arg(dark ? QStringLiteral("dark") : QStringLiteral("light"))
                                     .arg(windowSize.width()).arg(windowSize.height());
        QVERIFY(rendered.save(QDir(screenshotRoot).filePath(filename)));
    }
}

void GuiSmokeTest::savedProcessScopeFiltersSnapshotAndKeepsSearch()
{
    QSettings savedSettings(QStringLiteral("MemMonitor"), QStringLiteral("MemMonitor"));
    savedSettings.setValue(QStringLiteral("defaultProcessScope"), 1);
    savedSettings.sync();
    QCOMPARE(savedSettings.status(), QSettings::NoError);

    memonitor::MainWindow window(nullptr, false);
    stopAutomaticRefresh(window);
    QCOMPARE(window.defaultProcessScope_, 1);
    auto snapshot = syntheticSnapshot(3);
    for (auto& process : snapshot.processes) {
        process.ppid = -1;
    }
    snapshot.processes[0].name = QStringLiteral("fixture-owned-target");
    snapshot.processes[1].name = QStringLiteral("fixture-owned-other");
    snapshot.processes[2].name = QStringLiteral("fixture-foreign-user");
    snapshot.processes[2].uid = snapshot.processes.at(0).uid == 0 ? std::uint64_t{1} : std::uint64_t{0};
    snapshot.processes[2].userName = QStringLiteral("fixture-other-user");
    window.applySnapshot(snapshot);
    QTreeWidget* tree = window.processTree_;
    QCOMPARE(tree->topLevelItemCount(), 3);
    QVERIFY(findProcess(tree, snapshot.processes.at(2).pid) != nullptr);

    QTimer modalTimeout;
    modalTimeout.setSingleShot(true);
    connect(&modalTimeout, &QTimer::timeout, &window, []() {
        if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
            dialog->reject();
        }
    });
    bool scopeSaved = false;
    QTimer::singleShot(0, &window, [&scopeSaved]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (dialog == nullptr) {
            return;
        }
        auto* scope = dialog->findChild<QComboBox*>(QStringLiteral("defaultProcessScopeSetting"));
        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        if (scope == nullptr || scope->findData(0) < 0 || buttons == nullptr
            || buttons->button(QDialogButtonBox::Ok) == nullptr) {
            dialog->reject();
            return;
        }
        scope->setCurrentIndex(scope->findData(0));
        scopeSaved = true;
        buttons->button(QDialogButtonBox::Ok)->click();
    });
    modalTimeout.start(2000);
    window.showSettings();
    modalTimeout.stop();
    QVERIFY(scopeSaved);
    QCOMPARE(window.defaultProcessScope_, 0);
    QCOMPARE(tree->topLevelItemCount(), 2);
    QVERIFY(findProcess(tree, snapshot.processes.at(2).pid) == nullptr);
    savedSettings.sync();
    QCOMPARE(savedSettings.status(), QSettings::NoError);
    QCOMPARE(savedSettings.value(QStringLiteral("defaultProcessScope")).toInt(), 0);

    QTreeWidgetItem* target = findProcess(tree, snapshot.processes.at(0).pid);
    QTreeWidgetItem* other = findProcess(tree, snapshot.processes.at(1).pid);
    QVERIFY(target != nullptr);
    QVERIFY(other != nullptr);
    window.processFilter_->setText(snapshot.processes.at(0).name);
    QVERIFY(!target->isHidden());
    QVERIFY(other->isHidden());
    window.processFilter_->setText(snapshot.processes.at(2).name);
    QVERIFY(target->isHidden());
    QVERIFY(other->isHidden());
    QVERIFY(findProcess(tree, snapshot.processes.at(2).pid) == nullptr);
    window.processFilter_->clear();
    QVERIFY(!target->isHidden());
    QVERIFY(!other->isHidden());
    QCOMPARE(tree->topLevelItemCount(), 2);
}

void GuiSmokeTest::searchEmptyStateAndKeyboardNavigation()
{
    memonitor::MainWindow window(nullptr, false);
    stopAutomaticRefresh(window);
    window.show();
    window.activateWindow();
    auto snapshot = syntheticSnapshot(2);
    window.applySnapshot(snapshot);
    QVERIFY(!window.processEmptyLabel_->isVisible());
    QCOMPARE(window.processScopeLabel_->text(), QStringLiteral("我的进程"));

    window.processFilter_->setText(QStringLiteral("fixture-that-does-not-exist"));
    QVERIFY(window.processEmptyLabel_->isVisible());
    QVERIFY(window.processEmptyLabel_->text().contains(QStringLiteral("没有匹配")));
    for (int index = 0; index < window.processTree_->topLevelItemCount(); ++index) {
        QVERIFY(window.processTree_->topLevelItem(index)->isHidden());
    }

    auto* tabs = window.findChild<QTabWidget*>();
    QVERIFY(tabs != nullptr);
    tabs->setCurrentIndex(1);
    QCoreApplication::processEvents();
    QTest::keyClick(&window, Qt::Key_F, Qt::ControlModifier);
    QCOMPARE(tabs->currentIndex(), 0);
    QCoreApplication::processEvents();
    QVERIFY(window.processFilter_->hasFocus());
    QCOMPARE(window.processFilter_->selectedText(), window.processFilter_->text());
    QTest::keyClick(window.processFilter_, Qt::Key_Escape);
    QVERIFY(window.processFilter_->text().isEmpty());
    QVERIFY(!window.processEmptyLabel_->isVisible());

    snapshot.processes.clear();
    window.applySnapshot(snapshot);
    QVERIFY(window.processEmptyLabel_->isVisible());
    QVERIFY(window.processEmptyLabel_->text().contains(QStringLiteral("当前范围")));
    window.defaultProcessScope_ = 1;
    window.applyProcessFilter();
    QCOMPARE(window.processScopeLabel_->text(), QStringLiteral("所有进程"));
}

void GuiSmokeTest::applicationContextMenuTargetsOwner()
{
    memonitor::MainWindow window(nullptr, false);
    stopAutomaticRefresh(window);
    window.show();
    auto snapshot = syntheticSnapshot(2);
    snapshot.processes[0].name = QStringLiteral("fixture-application-owner");
    snapshot.processes[1].name = QStringLiteral("fixture-application-child");
    window.windows_ = {syntheticWindow(snapshot.processes.constFirst().pid)};
    window.applySnapshot(snapshot);
    QCoreApplication::processEvents();
    QTreeWidgetItem* application = window.processTree_->topLevelItem(0);
    QVERIFY(application != nullptr);
    const QRect itemRect = window.processTree_->visualItemRect(application);
    QVERIFY(!itemRect.isEmpty());
    bool menuOpened = false;
    bool actionsCorrect = false;
    QTimer menuTimeout;
    menuTimeout.setSingleShot(true);
    connect(&menuTimeout, &QTimer::timeout, &window, []() {
        if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
            menu->close();
        }
    });
    QTimer::singleShot(0, &window, [&menuOpened, &actionsCorrect]() {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (menu == nullptr) {
            return;
        }
        menuOpened = true;
        QStringList labels;
        for (const QAction* action : menu->actions()) {
            if (!action->isSeparator()) {
                labels.push_back(action->text());
            }
        }
        actionsCorrect = labels == QStringList{QStringLiteral("结束应用"), QStringLiteral("强制结束应用")};
        menu->close();
    });
    menuTimeout.start(2000);
    window.showProcessContextMenu(itemRect.center());
    menuTimeout.stop();
    QVERIFY(menuOpened);
    QVERIFY(actionsCorrect);
    QCOMPARE(window.processTree_->currentItem(), application);

    bool confirmationObserved = false;
    bool confirmationHasOwner = false;
    QTimer dismissConfirmation;
    dismissConfirmation.setInterval(10);
    connect(&dismissConfirmation, &QTimer::timeout, &window, [&]() {
        if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            confirmationObserved = true;
            confirmationHasOwner = dialog->text().contains(snapshot.processes.at(0).name)
                && dialog->text().contains(QString::number(snapshot.processes.at(0).pid));
            dialog->done(QMessageBox::Cancel);
        }
    });
    QTimer::singleShot(0, &window, []() {
        if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
            menu->setActiveAction(menu->actions().constFirst());
            QTest::keyClick(menu, Qt::Key_Return);
        }
    });
    menuTimeout.start(2000);
    dismissConfirmation.start();
    window.showProcessContextMenu(itemRect.center());
    dismissConfirmation.stop();
    menuTimeout.stop();
    QVERIFY(confirmationObserved);
    QVERIFY(confirmationHasOwner);
}

void GuiSmokeTest::terminationSignalsOwnedHelper_data()
{
    QTest::addColumn<bool>("force");
    QTest::addColumn<bool>("staleIdentity");
    QTest::newRow("sigterm") << false << false;
    QTest::newRow("sigkill") << true << false;
    QTest::newRow("changed-identity-is-rejected") << false << true;
}

void GuiSmokeTest::terminationSignalsOwnedHelper()
{
    QFETCH(bool, force);
    QFETCH(bool, staleIdentity);
    QProcess helper;
    helper.start(QStringLiteral("/bin/sleep"), {QStringLiteral("30")});
    QVERIFY(helper.waitForStarted(2000));
    const qint64 helperPid = helper.processId();
    const auto live = memonitor::MemorySampler::collect();
    const auto sampledHelper = std::find_if(live.processes.cbegin(), live.processes.cend(), [helperPid](const auto& process) {
        return process.pid == helperPid;
    });
    QVERIFY(sampledHelper != live.processes.cend());
    QVERIFY(sampledHelper->startTimeTicks > 0);
    auto snapshot = syntheticSnapshot(0);
    snapshot.processes = {*sampledHelper};
    if (staleIdentity) {
        ++snapshot.processes[0].startTimeTicks;
    }
    memonitor::MainWindow window(nullptr, false);
    stopAutomaticRefresh(window);
    window.applySnapshot(snapshot);

    bool confirmed = false;
    bool resultObserved = false;
    bool timedOut = false;
    QString resultText;
    QTimer dialogAutomation;
    dialogAutomation.setInterval(10);
    connect(&dialogAutomation, &QTimer::timeout, &window, [&]() {
        auto* message = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (message == nullptr) {
            return;
        }
        if (message->windowTitle() == QStringLiteral("确认结束任务")) {
            confirmed = message->text().contains(QString::number(helperPid));
            if (auto* button = message->button(QMessageBox::Yes)) {
                button->click();
            }
        } else if (message->windowTitle() == QStringLiteral("结束任务")) {
            resultText = message->text();
            resultObserved = true;
            message->accept();
        } else {
            message->reject();
        }
    });
    QTimer dialogTimeout;
    dialogTimeout.setSingleShot(true);
    connect(&dialogTimeout, &QTimer::timeout, &window, [&]() {
        timedOut = true;
        if (auto* message = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            message->reject();
        }
    });
    dialogAutomation.start();
    dialogTimeout.start(3000);
    window.terminateProcessItem(helperPid, snapshot.processes.constFirst().startTimeTicks, false, force);
    dialogAutomation.stop();
    dialogTimeout.stop();
    QVERIFY(!timedOut);
    QVERIFY(confirmed);
    QVERIFY(resultObserved);
    if (staleIdentity) {
        QVERIFY(resultText.contains(QStringLiteral("已发送 0 个信号")));
        QVERIFY(resultText.contains(QStringLiteral("未发送信号")));
        QCOMPARE(helper.state(), QProcess::Running);
        QVERIFY(!helper.waitForFinished(100));
        helper.terminate();
        QVERIFY(helper.waitForFinished(2000));
    } else {
        QVERIFY2(resultText.contains(QStringLiteral("已发送 1 个信号")), qPrintable(resultText));
        QVERIFY(helper.state() == QProcess::NotRunning || helper.waitForFinished(2000));
        QCOMPARE(helper.exitStatus(), QProcess::CrashExit);
        QCOMPARE(helper.exitCode(), force ? SIGKILL : SIGTERM);
    }
}

void GuiSmokeTest::clearingSearchRestoresExpansionAndScroll()
{
    memonitor::MainWindow window(nullptr, false);
    stopAutomaticRefresh(window);
    window.show();
    auto snapshot = syntheticSnapshot(160);
    window.windows_ = {syntheticWindow(snapshot.processes.constFirst().pid)};
    window.applySnapshot(snapshot);
    QCoreApplication::processEvents();
    QTreeWidget* tree = window.processTree_;
    QTreeWidgetItem* owner = findProcess(tree, snapshot.processes.at(0).pid);
    QTreeWidgetItem* parent = findProcess(tree, snapshot.processes.at(16).pid);
    QVERIFY(owner != nullptr);
    QVERIFY(parent != nullptr);
    owner->setExpanded(true);
    parent->setExpanded(false);
    tree->scrollToItem(parent, QAbstractItemView::PositionAtTop);
    QCoreApplication::processEvents();
    const int originalScroll = tree->verticalScrollBar()->value();
    window.processTreeIdleTimer_->stop();
    window.processFilter_->setText(QString::number(snapshot.processes.at(17).pid));
    QVERIFY(parent->isExpanded());
    snapshot.processes[17].pssBytes += 4096;
    window.updateProcessTree(snapshot);
    QVERIFY(parent->isExpanded());
    window.processFilter_->clear();
    QVERIFY(!parent->isExpanded());
    QVERIFY(owner->isExpanded());
    QCOMPARE(tree->verticalScrollBar()->value(), originalScroll);

    QTreeWidgetItem* application = owner->parent();
    QVERIFY(application != nullptr);
    application->setExpanded(false);
    window.processFilter_->setText(window.windows_.constFirst().title);
    QVERIFY(application->isExpanded());
    window.processFilter_->clear();
    QVERIFY(!application->isExpanded());
}

void GuiSmokeTest::zramRefreshKeepsDeviceSelectionAndClearsEmptySpan()
{
    memonitor::MainWindow window(nullptr, false);
    stopAutomaticRefresh(window);
    auto snapshot = syntheticSnapshot(0);
    window.applySnapshot(snapshot);
    QCOMPARE(window.zramTable_->columnSpan(0, 0), 7);

    memonitor::ZramDevice first;
    first.name = QStringLiteral("zram0");
    first.compressionAlgorithm = QStringLiteral("zstd");
    first.statisticsReadable = true;
    first.diskSizeBytes = 32 * kGiB;
    first.originalDataBytes = 8 * kGiB;
    first.compressedDataBytes = 2 * kGiB;
    first.memoryUsedBytes = 3 * kGiB;
    auto second = first;
    second.name = QStringLiteral("zram1");
    snapshot.zramDevices = {first, second};
    window.applySnapshot(snapshot);
    QCOMPARE(window.zramTable_->columnSpan(0, 0), 1);
    QCOMPARE(window.zramTable_->rowCount(), 2);
    window.zramTable_->setCurrentCell(1, 0);
    const QPersistentModelIndex selectedIndex(window.zramTable_->model()->index(1, 0));
    QSignalSpy resetSpy(window.zramTable_->model(), &QAbstractItemModel::modelReset);
    snapshot.zramDevices[1].memoryUsedBytes += 4096;
    window.applySnapshot(snapshot);
    QVERIFY(selectedIndex.isValid());
    QCOMPARE(window.zramTable_->currentItem()->text(), QStringLiteral("zram1"));
    QCOMPARE(resetSpy.count(), 0);
    QCOMPARE(window.zramTable_->item(1, 5)->data(Qt::UserRole).toULongLong(), second.memoryUsedBytes + 4096);
    std::swap(snapshot.zramDevices[0], snapshot.zramDevices[1]);
    window.applySnapshot(snapshot);
    QCOMPARE(window.zramTable_->currentRow(), 0);
    QCOMPARE(window.zramTable_->currentItem()->text(), QStringLiteral("zram1"));

    snapshot.zramDevices.removeFirst();
    window.applySnapshot(snapshot);
    QVERIFY(window.zramTable_->selectedItems().isEmpty());
    QVERIFY(window.zramTable_->currentItem() == nullptr);
    snapshot.zramDevices.clear();
    window.applySnapshot(snapshot);
    QCOMPARE(window.zramTable_->columnSpan(0, 0), 7);
    snapshot.zramDevices = {first};
    window.applySnapshot(snapshot);
    QCOMPARE(window.zramTable_->columnSpan(0, 0), 1);
    QCOMPARE(window.zramTable_->item(0, 0)->text(), QStringLiteral("zram0"));
    QCOMPARE(window.zramTable_->item(0, 6)->text(), QStringLiteral("4.00×"));
}

void GuiSmokeTest::warningSettingsPersistCancelAndClamp()
{
    memonitor::MainWindow window(nullptr, false);
    stopAutomaticRefresh(window);
    QCOMPARE(window.processWarningPercent_, 30);
    QVERIFY(!window.availableMemoryWarningEnabled_);
    QCOMPARE(window.availableMemoryWarningMode_, 0);
    QCOMPARE(window.availableMemoryWarningPercent_, 10);
    QCOMPARE(window.availableMemoryWarningMiB_, 1024);
    window.hasSnapshot_ = false;
    window.warnedProcesses_.insert(QStringLiteral("stale-warning"));
    window.availableMemoryWarningActive_ = true;

    QTimer modalTimeout;
    modalTimeout.setSingleShot(true);
    connect(&modalTimeout, &QTimer::timeout, &window, []() {
        if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
            dialog->reject();
        }
    });
    bool controlsChecked = false;
    QTimer::singleShot(0, &window, [&controlsChecked]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (dialog == nullptr) {
            return;
        }
        auto rejectOnFailure = qScopeGuard([dialog]() { dialog->reject(); });
        auto* notifications = dialog->findChild<QCheckBox*>(QStringLiteral("notificationsSetting"));
        auto* process = dialog->findChild<QSpinBox*>(QStringLiteral("processWarningPercentSetting"));
        auto* available = dialog->findChild<QCheckBox*>(QStringLiteral("availableMemoryWarningSetting"));
        auto* mode = dialog->findChild<QComboBox*>(QStringLiteral("availableMemoryWarningModeSetting"));
        auto* percent = dialog->findChild<QSpinBox*>(QStringLiteral("availableMemoryWarningPercentSetting"));
        auto* mib = dialog->findChild<QSpinBox*>(QStringLiteral("availableMemoryWarningMiBSetting"));
        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        QVERIFY(notifications != nullptr && process != nullptr && available != nullptr
                && mode != nullptr && percent != nullptr && mib != nullptr && buttons != nullptr);
        QVERIFY(buttons->button(QDialogButtonBox::Ok) != nullptr);
        QCOMPARE(process->minimum(), 1);
        QCOMPARE(process->maximum(), 100);
        QCOMPARE(percent->minimum(), 1);
        QCOMPARE(percent->maximum(), 100);
        QCOMPARE(mib->minimum(), 1);
        QCOMPARE(mib->maximum(), 1048576);
        QVERIFY(!process->isEnabled());
        notifications->setChecked(true);
        QVERIFY(process->isEnabled());
        QVERIFY(!percent->isEnabled());
        available->setChecked(true);
        QVERIFY(percent->isEnabled());
        QVERIFY(!percent->isHidden());
        QVERIFY(mib->isHidden());
        mode->setCurrentIndex(mode->findData(1));
        QVERIFY(percent->isHidden());
        QVERIFY(!mib->isHidden());
        process->setValue(45);
        percent->setValue(17);
        mib->setValue(2048);
        controlsChecked = true;
        rejectOnFailure.dismiss();
        buttons->button(QDialogButtonBox::Ok)->click();
    });
    modalTimeout.start(2000);
    window.showSettings();
    modalTimeout.stop();
    QVERIFY(controlsChecked);
    QVERIFY(window.notificationsEnabled_);
    QCOMPARE(window.processWarningPercent_, 45);
    QVERIFY(window.availableMemoryWarningEnabled_);
    QCOMPARE(window.availableMemoryWarningMode_, 1);
    QCOMPARE(window.availableMemoryWarningPercent_, 17);
    QCOMPARE(window.availableMemoryWarningMiB_, 2048);
    QVERIFY(window.warnedProcesses_.isEmpty());
    QVERIFY(!window.availableMemoryWarningActive_);

    QSettings savedSettings(QStringLiteral("MemMonitor"), QStringLiteral("MemMonitor"));
    savedSettings.sync();
    QCOMPARE(savedSettings.status(), QSettings::NoError);
    QCOMPARE(savedSettings.value(QStringLiteral("processWarningPercent")).toInt(), 45);
    QVERIFY(savedSettings.value(QStringLiteral("availableMemoryWarningEnabled")).toBool());
    QCOMPARE(savedSettings.value(QStringLiteral("availableMemoryWarningMode")).toInt(), 1);
    QCOMPARE(savedSettings.value(QStringLiteral("availableMemoryWarningPercent")).toInt(), 17);
    QCOMPARE(savedSettings.value(QStringLiteral("availableMemoryWarningMiB")).toInt(), 2048);
    {
        memonitor::MainWindow restored(nullptr, false);
        QVERIFY(restored.notificationsEnabled_);
        restored.notificationsEnabled_ = false;
        stopAutomaticRefresh(restored);
        QCOMPARE(restored.processWarningPercent_, 45);
        QVERIFY(restored.availableMemoryWarningEnabled_);
        QCOMPARE(restored.availableMemoryWarningMode_, 1);
        QCOMPARE(restored.availableMemoryWarningPercent_, 17);
        QCOMPARE(restored.availableMemoryWarningMiB_, 2048);
    }
    QVariantMap settingsBeforeCancel;
    for (const QString& key : savedSettings.allKeys()) {
        settingsBeforeCancel.insert(key, savedSettings.value(key));
    }
    window.warnedProcesses_.insert(QStringLiteral("retained-warning"));
    window.availableMemoryWarningActive_ = true;
    bool cancelled = false;
    QTimer::singleShot(0, &window, [&cancelled]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (dialog == nullptr) {
            return;
        }
        const auto rejectOnFailure = qScopeGuard([dialog]() { dialog->reject(); });
        auto* notifications = dialog->findChild<QCheckBox*>(QStringLiteral("notificationsSetting"));
        auto* process = dialog->findChild<QSpinBox*>(QStringLiteral("processWarningPercentSetting"));
        auto* available = dialog->findChild<QCheckBox*>(QStringLiteral("availableMemoryWarningSetting"));
        auto* mode = dialog->findChild<QComboBox*>(QStringLiteral("availableMemoryWarningModeSetting"));
        auto* percent = dialog->findChild<QSpinBox*>(QStringLiteral("availableMemoryWarningPercentSetting"));
        auto* mib = dialog->findChild<QSpinBox*>(QStringLiteral("availableMemoryWarningMiBSetting"));
        QVERIFY(notifications != nullptr && process != nullptr && available != nullptr
                && mode != nullptr && percent != nullptr && mib != nullptr);
        process->setValue(99);
        percent->setValue(1);
        mib->setValue(1048576);
        mode->setCurrentIndex(mode->findData(0));
        available->setChecked(false);
        notifications->setChecked(false);
        cancelled = true;
        dialog->reject();
    });
    modalTimeout.start(2000);
    window.showSettings();
    modalTimeout.stop();
    QVERIFY(cancelled);
    QVERIFY(window.notificationsEnabled_);
    QCOMPARE(window.processWarningPercent_, 45);
    QVERIFY(window.availableMemoryWarningEnabled_);
    QCOMPARE(window.availableMemoryWarningMode_, 1);
    QCOMPARE(window.availableMemoryWarningPercent_, 17);
    QCOMPARE(window.availableMemoryWarningMiB_, 2048);
    QVERIFY(window.warnedProcesses_.contains(QStringLiteral("retained-warning")));
    QVERIFY(window.availableMemoryWarningActive_);
    savedSettings.sync();
    QVariantMap settingsAfterCancel;
    for (const QString& key : savedSettings.allKeys()) {
        settingsAfterCancel.insert(key, savedSettings.value(key));
    }
    QCOMPARE(settingsAfterCancel, settingsBeforeCancel);

    savedSettings.setValue(QStringLiteral("notificationsEnabled"), false);
    savedSettings.setValue(QStringLiteral("processWarningPercent"), -7);
    savedSettings.setValue(QStringLiteral("availableMemoryWarningMode"), 25);
    savedSettings.setValue(QStringLiteral("availableMemoryWarningPercent"), 999);
    savedSettings.setValue(QStringLiteral("availableMemoryWarningMiB"), std::numeric_limits<qlonglong>::max());
    savedSettings.sync();
    QCOMPARE(savedSettings.status(), QSettings::NoError);
    {
        memonitor::MainWindow clamped(nullptr, false);
        stopAutomaticRefresh(clamped);
        QCOMPARE(clamped.processWarningPercent_, 1);
        QCOMPARE(clamped.availableMemoryWarningMode_, 1);
        QCOMPARE(clamped.availableMemoryWarningPercent_, 100);
        QCOMPARE(clamped.availableMemoryWarningMiB_, 1048576);
    }
    savedSettings.setValue(QStringLiteral("processWarningPercent"), 999);
    savedSettings.setValue(QStringLiteral("availableMemoryWarningMode"), -1);
    savedSettings.setValue(QStringLiteral("availableMemoryWarningPercent"), -4);
    savedSettings.setValue(QStringLiteral("availableMemoryWarningMiB"), 0);
    savedSettings.sync();
    {
        memonitor::MainWindow clamped(nullptr, false);
        stopAutomaticRefresh(clamped);
        QCOMPARE(clamped.processWarningPercent_, 100);
        QCOMPARE(clamped.availableMemoryWarningMode_, 0);
        QCOMPARE(clamped.availableMemoryWarningPercent_, 1);
        QCOMPARE(clamped.availableMemoryWarningMiB_, 1);
    }
    for (const QString& key : {QStringLiteral("processWarningPercent"), QStringLiteral("availableMemoryWarningMode"),
                               QStringLiteral("availableMemoryWarningPercent"), QStringLiteral("availableMemoryWarningMiB")}) {
        savedSettings.setValue(key, QStringLiteral("invalid"));
    }
    savedSettings.sync();
    memonitor::MainWindow defaults(nullptr, false);
    stopAutomaticRefresh(defaults);
    QCOMPARE(defaults.processWarningPercent_, 30);
    QCOMPARE(defaults.availableMemoryWarningMode_, 0);
    QCOMPARE(defaults.availableMemoryWarningPercent_, 10);
    QCOMPARE(defaults.availableMemoryWarningMiB_, 1024);
}

void GuiSmokeTest::warningNotificationsUsePlainTextAndRearm()
{
    if (!qEnvironmentVariableIsSet("MEMONITOR_TEST_NOTIFICATION_BUS")) {
        QSKIP("Notification delivery is checked in a separate private D-Bus session");
    }
    auto bus = QDBusConnection::sessionBus();
    QVERIFY(bus.isConnected());
    NotificationReceiver receiver;
    QVERIFY(bus.registerService(QStringLiteral("org.freedesktop.Notifications")));
    const auto unregisterService = qScopeGuard([&bus]() {
        bus.unregisterService(QStringLiteral("org.freedesktop.Notifications"));
    });
    QVERIFY(bus.registerObject(QStringLiteral("/org/freedesktop/Notifications"), &receiver,
                               QDBusConnection::ExportAllSlots));
    const auto unregisterObject = qScopeGuard([&bus]() {
        bus.unregisterObject(QStringLiteral("/org/freedesktop/Notifications"));
    });
    memonitor::MainWindow window(nullptr, false);
    stopAutomaticRefresh(window);
    delete window.trayIcon_;
    window.trayIcon_ = nullptr;
    window.notificationsEnabled_ = true;
    window.processWarningPercent_ = 40;
    auto snapshot = syntheticSnapshot(1);
    auto& process = snapshot.processes[0];
    process.name = QStringLiteral("fixture-<b>&\"worker\"</b>");
    const std::uint64_t boundary = snapshot.memoryTotalBytes / 100 * 40
        + snapshot.memoryTotalBytes % 100 * 40 / 100;
    process.pssBytes = boundary;
    window.checkWarnings(snapshot);
    QVERIFY(window.warnedProcesses_.isEmpty());
    process.pssBytes = boundary + 1;
    const auto waitForMessages = [&receiver](qsizetype count) {
        return QTest::qWaitFor([&]() { return receiver.receivedBodies.size() >= count; }, 1000)
            && receiver.receivedBodies.size() == count;
    };
    window.checkWarnings(snapshot);
    QVERIFY(waitForMessages(1));
    QVERIFY(receiver.receivedBodies.constFirst().contains(process.name.toHtmlEscaped()));
    QVERIFY(!receiver.receivedBodies.constFirst().contains(QStringLiteral("<b>")));
    QVERIFY(receiver.receivedBodies.constFirst().contains(QStringLiteral("&lt;b&gt;&amp;&quot;")));
    QVERIFY(receiver.receivedBodies.constFirst().contains(QStringLiteral("40%")));
    QVERIFY(!receiver.receivedBodies.constFirst().contains(QStringLiteral("40%%")));
    QCOMPARE(receiver.receivedSummaries.constFirst(), QStringLiteral("MemMonitor 实际内存警告"));
    const auto& firstNotification = receiver.receivedNotifications.constFirst();
    QCOMPARE(firstNotification.application, QStringLiteral("MemMonitor"));
    QCOMPARE(firstNotification.replacesId, uint(0));
    QVERIFY(firstNotification.icon.isEmpty());
    QVERIFY(firstNotification.actions.isEmpty());
    QCOMPARE(firstNotification.hints.value(QStringLiteral("urgency")).toUInt(), uint(2));
    QCOMPARE(firstNotification.timeout, 9000);
    window.checkWarnings(snapshot);
    QCOMPARE(window.warnedProcesses_.size(), 1);
    QCOMPARE(receiver.receivedBodies.size(), 1);

    const QString originalIdentity = QStringLiteral("%1:%2").arg(process.pid).arg(process.startTimeTicks);
    ++process.startTimeTicks;
    window.checkWarnings(snapshot);
    QVERIFY(waitForMessages(2));
    QCOMPARE(window.warnedProcesses_.size(), 1);
    QVERIFY(!window.warnedProcesses_.contains(originalIdentity));
    QVERIFY(window.warnedProcesses_.contains(QStringLiteral("%1:%2").arg(process.pid).arg(process.startTimeTicks)));
    process.pssExact = false;
    window.checkWarnings(snapshot);
    QCOMPARE(window.warnedProcesses_.size(), 1);
    process.pssExact = true;
    window.checkWarnings(snapshot);
    QCOMPARE(window.warnedProcesses_.size(), 1);
    QCoreApplication::processEvents();
    QCOMPARE(receiver.receivedBodies.size(), 2);

    process.pssBytes = boundary;
    window.checkWarnings(snapshot);
    QVERIFY(window.warnedProcesses_.isEmpty());
    process.pssBytes = boundary + 1;
    window.notificationsEnabled_ = false;
    window.checkWarnings(snapshot);
    QVERIFY(window.warnedProcesses_.isEmpty());
    window.notificationsEnabled_ = true;
    window.checkWarnings(snapshot);
    QVERIFY(waitForMessages(3));

    process.pssBytes = 1024;
    process.swapPssBytes = snapshot.memoryTotalBytes;
    window.checkWarnings(snapshot);
    QVERIFY(window.warnedProcesses_.isEmpty());
    QCOMPARE(receiver.receivedBodies.size(), 3);

    window.processWarningPercent_ = 100;
    window.availableMemoryWarningEnabled_ = true;
    window.availableMemoryWarningPercent_ = 10;
    snapshot.memoryTotalBytes = 100 * kMiB;
    snapshot.memoryAvailableBytes = 10 * kMiB;
    snapshot.memoryUsedBytes = snapshot.memoryTotalBytes - snapshot.memoryAvailableBytes;
    memonitor::ZramDevice zram;
    zram.name = QStringLiteral("zram-fixture");
    zram.statisticsReadable = true;
    zram.memoryUsedBytes = 2 * kMiB;
    snapshot.zramDevices = {zram};
    window.checkWarnings(snapshot);
    QVERIFY(!window.availableMemoryWarningActive_);
    snapshot.memoryAvailableBytes -= 1;
    window.checkWarnings(snapshot);
    QVERIFY(waitForMessages(4));
    QVERIFY(window.availableMemoryWarningActive_);
    QVERIFY(receiver.receivedBodies.constLast().contains(QStringLiteral("MemAvailable")));
    QVERIFY(receiver.receivedBodies.constLast().contains(QStringLiteral("10%")));
    window.checkWarnings(snapshot);
    QCOMPARE(receiver.receivedBodies.size(), 4);
    snapshot.memoryAvailableBytes = 10 * kMiB;
    window.checkWarnings(snapshot);
    QVERIFY(!window.availableMemoryWarningActive_);
    snapshot.memoryAvailableBytes -= 1;
    window.checkWarnings(snapshot);
    QVERIFY(waitForMessages(5));

    snapshot.memoryInfoComplete = false;
    snapshot.memoryAvailableBytes = snapshot.memoryTotalBytes;
    window.checkWarnings(snapshot);
    QVERIFY(window.availableMemoryWarningActive_);
    snapshot.memoryInfoComplete = true;
    snapshot.memoryAvailableBytes = 10 * kMiB - 1;
    window.checkWarnings(snapshot);
    QCoreApplication::processEvents();
    QCOMPARE(receiver.receivedBodies.size(), 5);
    snapshot.memoryAvailableBytes = 10 * kMiB;
    window.checkWarnings(snapshot);
    QVERIFY(!window.availableMemoryWarningActive_);
    snapshot.memoryAvailableBytes -= 1;
    snapshot.memoryInfoReadable = false;
    window.checkWarnings(snapshot);
    QVERIFY(!window.availableMemoryWarningActive_);
    snapshot.memoryInfoReadable = true;
    snapshot.memoryTotalBytes = 0;
    window.checkWarnings(snapshot);
    QVERIFY(!window.availableMemoryWarningActive_);
    snapshot.memoryTotalBytes = 100 * kMiB;
    snapshot.memoryAvailableBytes = snapshot.memoryTotalBytes + 1;
    window.checkWarnings(snapshot);
    QVERIFY(!window.availableMemoryWarningActive_);
    snapshot.memoryAvailableBytes = 10 * kMiB - 1;
    window.checkWarnings(snapshot);
    QVERIFY(waitForMessages(6));

    window.availableMemoryWarningActive_ = false;
    window.availableMemoryWarningMode_ = 1;
    window.availableMemoryWarningMiB_ = 8;
    snapshot.memoryAvailableBytes = 8 * kMiB;
    window.checkWarnings(snapshot);
    QVERIFY(!window.availableMemoryWarningActive_);
    snapshot.memoryAvailableBytes -= 1;
    window.checkWarnings(snapshot);
    QVERIFY(waitForMessages(7));
    QVERIFY(receiver.receivedBodies.constLast().contains(QStringLiteral("8 MiB")));
    window.checkWarnings(snapshot);
    QCOMPARE(receiver.receivedBodies.size(), 7);
    snapshot.memoryAvailableBytes = 8 * kMiB;
    window.checkWarnings(snapshot);
    QVERIFY(!window.availableMemoryWarningActive_);
    snapshot.memoryAvailableBytes -= 1;
    window.notificationsEnabled_ = false;
    window.checkWarnings(snapshot);
    QVERIFY(!window.availableMemoryWarningActive_);
    window.notificationsEnabled_ = true;
    window.checkWarnings(snapshot);
    QVERIFY(waitForMessages(8));

    window.notificationsEnabled_ = true;
    window.processWarningPercent_ = 90;
    window.availableMemoryWarningEnabled_ = false;
    window.availableMemoryWarningActive_ = false;
    snapshot.memoryAvailableBytes = 12 * kMiB;
    snapshot.memoryUsedBytes = snapshot.memoryTotalBytes - snapshot.memoryAvailableBytes;
    process.pssBytes = 50 * kMiB;
    window.applySnapshot(snapshot);
    auto* processItem = findProcess(window.processTree_, process.pid);
    QVERIFY(processItem != nullptr);
    QCOMPARE(processItem->foreground(2).style(), Qt::NoBrush);

    QTimer modalTimeout;
    modalTimeout.setSingleShot(true);
    connect(&modalTimeout, &QTimer::timeout, &window, []() {
        if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
            dialog->reject();
        }
    });
    bool thresholdsSaved = false;
    QTimer::singleShot(0, &window, [&thresholdsSaved]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (dialog == nullptr) {
            return;
        }
        auto rejectOnFailure = qScopeGuard([dialog]() { dialog->reject(); });
        auto* notifications = dialog->findChild<QCheckBox*>(QStringLiteral("notificationsSetting"));
        auto* processThreshold = dialog->findChild<QSpinBox*>(QStringLiteral("processWarningPercentSetting"));
        auto* available = dialog->findChild<QCheckBox*>(QStringLiteral("availableMemoryWarningSetting"));
        auto* mode = dialog->findChild<QComboBox*>(QStringLiteral("availableMemoryWarningModeSetting"));
        auto* percent = dialog->findChild<QSpinBox*>(QStringLiteral("availableMemoryWarningPercentSetting"));
        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        QVERIFY(notifications != nullptr && processThreshold != nullptr && available != nullptr
                && mode != nullptr && percent != nullptr && buttons != nullptr);
        QVERIFY(buttons->button(QDialogButtonBox::Ok) != nullptr);
        notifications->setChecked(true);
        processThreshold->setValue(45);
        available->setChecked(true);
        mode->setCurrentIndex(mode->findData(0));
        percent->setValue(20);
        thresholdsSaved = true;
        rejectOnFailure.dismiss();
        buttons->button(QDialogButtonBox::Ok)->click();
    });
    modalTimeout.start(2000);
    window.showSettings();
    modalTimeout.stop();
    QVERIFY(thresholdsSaved);
    QVERIFY(waitForMessages(9));
    QVERIFY(receiver.receivedBodies.constLast().contains(QStringLiteral("45%")));
    QVERIFY(receiver.receivedBodies.constLast().contains(QStringLiteral("20%")));
    QVERIFY(window.warnedProcesses_.contains(QStringLiteral("%1:%2").arg(process.pid).arg(process.startTimeTicks)));
    QVERIFY(window.availableMemoryWarningActive_);
    QCOMPARE(processItem->foreground(2), QBrush(window.processTree_->palette().color(QPalette::BrightText)));
    window.checkWarnings(snapshot);
    QCoreApplication::processEvents();
    QCOMPARE(receiver.receivedBodies.size(), 9);

    window.notificationsEnabled_ = false;
    const QSet<QString> warningsBeforeTest = window.warnedProcesses_;
    bool testClicked = false;
    QTimer::singleShot(0, &window, [&testClicked]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (dialog == nullptr) {
            return;
        }
        const auto rejectOnExit = qScopeGuard([dialog]() { dialog->reject(); });
        auto* testButton = dialog->findChild<QPushButton*>(QStringLiteral("testNotificationButton"));
        QVERIFY(testButton != nullptr);
        QVERIFY(testButton->isEnabled());
        testButton->click();
        testClicked = true;
    });
    modalTimeout.start(2000);
    window.showSettings();
    modalTimeout.stop();
    QVERIFY(testClicked);
    QVERIFY(waitForMessages(10));
    QCOMPARE(receiver.receivedSummaries.constLast(), QStringLiteral("MemMonitor 测试通知"));
    QVERIFY(receiver.receivedBodies.constLast().contains(QStringLiteral("不表示当前内存不足")));
    QVERIFY(!window.notificationsEnabled_);
    QCOMPARE(window.warnedProcesses_, warningsBeforeTest);
    QVERIFY(window.availableMemoryWarningActive_);
    QCOMPARE(receiver.receivedNotifications.constLast().application, QStringLiteral("MemMonitor"));
    QCOMPARE(receiver.receivedNotifications.constLast().timeout, 9000);
}

void GuiSmokeTest::windowTitlesAndChildProcessIcons()
{
    memonitor::MainWindow window(nullptr, false);
    stopAutomaticRefresh(window);
    auto snapshot = syntheticSnapshot(2);
    snapshot.processes[0].name = QStringLiteral("fixture-browser");
    snapshot.processes[1].name = QStringLiteral("fixture-renderer");
    auto trackedWindow = syntheticWindow(snapshot.processes.constFirst().pid);
    for (const QString& iconName : {QStringLiteral("utilities-terminal"), QStringLiteral("system-file-manager"),
                                    QStringLiteral("text-editor"), QStringLiteral("application-x-executable")}) {
        if (QIcon::hasThemeIcon(iconName) && !QIcon::fromTheme(iconName).pixmap(20, 20).isNull()) {
            trackedWindow.iconName = iconName;
            break;
        }
    }
    window.windows_ = {trackedWindow};
    window.applySnapshot(snapshot);

    QTreeWidget* tree = window.processTree_;
    QCOMPARE(tree->topLevelItemCount(), 1);
    QTreeWidgetItem* windowRow = tree->topLevelItem(0);
    QCOMPARE(windowRow->text(0), trackedWindow.title);
    QCOMPARE(windowRow->childCount(), 1);
    if (!trackedWindow.iconName.isEmpty()) {
        QVERIFY(!windowRow->icon(0).isNull());
        QVERIFY(!windowRow->icon(0).pixmap(20, 20).isNull());
    }
    QTreeWidgetItem* owner = findProcess(tree, snapshot.processes.at(0).pid);
    QTreeWidgetItem* descendant = findProcess(tree, snapshot.processes.at(1).pid);
    QVERIFY(owner != nullptr);
    QVERIFY(descendant != nullptr);
    QCOMPARE(owner->parent(), windowRow);
    QCOMPARE(descendant->parent(), owner);
    QCOMPARE(owner->text(0), snapshot.processes.at(0).name);
    QCOMPARE(descendant->text(0), snapshot.processes.at(1).name);
    QVERIFY(owner->icon(0).isNull());
    QVERIFY(descendant->icon(0).isNull());
    QVERIFY(!owner->text(0).contains(trackedWindow.title));
    QVERIFY(!descendant->text(0).contains(trackedWindow.title));

    window.processFilter_->setText(trackedWindow.title);
    QVERIFY(!windowRow->isHidden());
    QVERIFY(!owner->isHidden());
    QVERIFY(!descendant->isHidden());
    QCOMPARE(window.tableSummaryLabel_->text(), QStringLiteral("匹配 2 个进程 · 1 个应用"));
    QVERIFY(window.processEmptyLabel_->isHidden());
    window.processFilter_->setText(snapshot.processes.at(1).name);
    QVERIFY(!windowRow->isHidden());
    QVERIFY(!owner->isHidden());
    QVERIFY(!descendant->isHidden());
    QCOMPARE(window.tableSummaryLabel_->text(), QStringLiteral("匹配 1 个进程 · 1 个应用"));
    window.processFilter_->clear();

    const QPersistentModelIndex windowIndex(tree->indexFromItem(windowRow));
    const QPersistentModelIndex ownerIndex(tree->indexFromItem(owner));
    const QPersistentModelIndex descendantIndex(tree->indexFromItem(descendant));
    snapshot.processes[1].pssBytes += 4096;
    window.applySnapshot(snapshot);
    QVERIFY(windowIndex.isValid());
    QVERIFY(ownerIndex.isValid());
    QVERIFY(descendantIndex.isValid());
    QCOMPARE(windowRow->text(0), trackedWindow.title);
    QCOMPARE(owner->text(0), snapshot.processes.at(0).name);
    QCOMPARE(descendant->text(0), snapshot.processes.at(1).name);
    QVERIFY(owner->icon(0).isNull());
    QVERIFY(descendant->icon(0).isNull());
}

void GuiSmokeTest::approximateProcessValuesRemainVisibleAndSortable()
{
    memonitor::MainWindow window(nullptr, false);
    stopAutomaticRefresh(window);
    auto snapshot = syntheticSnapshot(3);
    snapshot.processes[0].pssBytes = 3 * kGiB;
    snapshot.processes[1].pssBytes = 2 * kGiB;
    snapshot.processes[1].pssExact = false;
    snapshot.processes[1].approximate = true;
    snapshot.processes[2].swapPssExact = false;
    snapshot.processes[2].approximate = true;
    window.windows_ = {syntheticWindow(snapshot.processes.constFirst().pid)};
    window.applySnapshot(snapshot);
    auto* exact = findProcess(window.processTree_, snapshot.processes.at(0).pid);
    auto* residentEstimate = findProcess(window.processTree_, snapshot.processes.at(1).pid);
    auto* swapEstimate = findProcess(window.processTree_, snapshot.processes.at(2).pid);
    QVERIFY(exact != nullptr);
    QVERIFY(residentEstimate != nullptr);
    QVERIFY(swapEstimate != nullptr);
    auto* application = exact->parent();
    QVERIFY(application != nullptr);
    QVERIFY(!exact->text(2).startsWith(QStringLiteral("≈")));
    QVERIFY(residentEstimate->text(2).startsWith(QStringLiteral("≈")));
    QVERIFY(!residentEstimate->text(3).startsWith(QStringLiteral("≈")));
    QVERIFY(residentEstimate->text(4).startsWith(QStringLiteral("≈")));
    QVERIFY(residentEstimate->toolTip(2).contains(QStringLiteral("RSS")));
    QVERIFY(residentEstimate->toolTip(2).contains(QStringLiteral("不触发告警")));
    QVERIFY(!swapEstimate->text(2).startsWith(QStringLiteral("≈")));
    QVERIFY(swapEstimate->text(3).startsWith(QStringLiteral("≈")));
    QVERIFY(swapEstimate->text(4).startsWith(QStringLiteral("≈")));
    for (int column = 2; column <= 4; ++column) {
        QVERIFY(application->text(column).startsWith(QStringLiteral("≈")));
    }
    const auto residentIndex = QPersistentModelIndex(window.processTree_->indexFromItem(residentEstimate));
    QCOMPARE(residentEstimate->data(2, Qt::UserRole).toULongLong(), 2 * kGiB);
    window.processTree_->sortItems(2, Qt::DescendingOrder);
    QCOMPARE(exact->child(0), residentEstimate);
    QCOMPARE(exact->child(1), swapEstimate);

    snapshot.processes[1].pssExact = true;
    snapshot.processes[1].approximate = false;
    snapshot.processes[2].swapPssExact = true;
    snapshot.processes[2].approximate = false;
    window.applySnapshot(snapshot);
    QVERIFY(residentIndex.isValid());
    QCOMPARE(QPersistentModelIndex(window.processTree_->indexFromItem(residentEstimate)), residentIndex);
    for (int column = 2; column <= 4; ++column) {
        QVERIFY(!residentEstimate->text(column).startsWith(QStringLiteral("≈")));
        QVERIFY(!swapEstimate->text(column).startsWith(QStringLiteral("≈")));
        QVERIFY(!application->text(column).startsWith(QStringLiteral("≈")));
    }
    QVERIFY(!residentEstimate->toolTip(2).contains(QStringLiteral("RSS")));
}

void GuiSmokeTest::windowOwnersWithSharedAppIdRemainIndependent()
{
    memonitor::MainWindow window(nullptr, false);
    stopAutomaticRefresh(window);
    auto snapshot = syntheticSnapshot(4);
    snapshot.processes[2].ppid = -1;
    snapshot.processes[3].ppid = snapshot.processes.at(2).pid;
    auto firstWindow = syntheticWindow(snapshot.processes.at(0).pid);
    firstWindow.title = QStringLiteral("First document — shared application");
    auto secondWindow = syntheticWindow(snapshot.processes.at(2).pid);
    secondWindow.title = QStringLiteral("Second document — shared application");
    auto additionalCaption = firstWindow;
    additionalCaption.title = QStringLiteral("Another view — first owner");
    window.windows_ = {firstWindow, secondWindow, additionalCaption};
    window.applySnapshot(snapshot);

    QTreeWidget* tree = window.processTree_;
    QCOMPARE(tree->topLevelItemCount(), 2);
    for (const int ownerOffset : {0, 2}) {
        const auto& ownerProcess = snapshot.processes.at(ownerOffset);
        const auto& childProcess = snapshot.processes.at(ownerOffset + 1);
        QTreeWidgetItem* owner = findProcess(tree, ownerProcess.pid);
        QTreeWidgetItem* child = findProcess(tree, childProcess.pid);
        QVERIFY(owner != nullptr);
        QVERIFY(child != nullptr);
        QTreeWidgetItem* windowRow = owner->parent();
        QVERIFY(windowRow != nullptr);
        QVERIFY(windowRow->parent() == nullptr);
        QCOMPARE(windowRow->childCount(), 1);
        QCOMPARE(owner->childCount(), 1);
        QCOMPARE(child->parent(), owner);
        const QString expectedTitle = ownerOffset == 0
            ? QStringList{firstWindow.title, additionalCaption.title}.join(QStringLiteral(" / "))
            : secondWindow.title;
        QCOMPARE(windowRow->text(0), expectedTitle);
        QCOMPARE(owner->text(0), ownerProcess.name);
        QCOMPARE(child->text(0), childProcess.name);
        QVERIFY(owner->icon(0).isNull());
        QVERIFY(child->icon(0).isNull());
        QCOMPARE(windowRow->data(1, Qt::UserRole).toULongLong(), std::uint64_t{2});
        QCOMPARE(windowRow->data(2, Qt::UserRole).toULongLong(), ownerProcess.pssBytes + childProcess.pssBytes);
        QCOMPARE(windowRow->data(3, Qt::UserRole).toULongLong(), ownerProcess.swapPssBytes + childProcess.swapPssBytes);
        QCOMPARE(windowRow->data(4, Qt::UserRole).toULongLong(),
                 ownerProcess.footprintBytes() + childProcess.footprintBytes());
    }

    QHash<qint64, int> displayedProcessCounts;
    for (QTreeWidgetItemIterator iterator(tree); *iterator != nullptr; ++iterator) {
        const qint64 pid = (*iterator)->data(0, kProcessPidRole).toLongLong();
        if (pid > 0) {
            ++displayedProcessCounts[pid];
        }
    }
    QCOMPARE(displayedProcessCounts.size(), snapshot.processes.size());
    for (const auto& process : snapshot.processes) {
        QCOMPARE(displayedProcessCounts.value(process.pid), 1);
    }
}

void GuiSmokeTest::inPlaceRefreshRetainsRowsAndInvalidatesExitedIdentities()
{
    memonitor::MainWindow window(nullptr, false);
    stopAutomaticRefresh(window);
    auto snapshot = syntheticSnapshot(5);
    snapshot.processes[2].ppid = snapshot.processes.at(1).pid;
    const qint64 ownerPid = snapshot.processes.at(0).pid;
    const qint64 selectedPid = snapshot.processes.at(1).pid;
    const qint64 removedPid = snapshot.processes.at(3).pid;
    const qint64 reusedPid = snapshot.processes.at(4).pid;
    window.windows_ = {syntheticWindow(ownerPid)};
    window.applySnapshot(snapshot);
    QTreeWidget* tree = window.processTree_;
    QTreeWidgetItem* owner = findProcess(tree, ownerPid);
    QTreeWidgetItem* selected = findProcess(tree, selectedPid);
    QTreeWidgetItem* removed = findProcess(tree, removedPid);
    QTreeWidgetItem* reused = findProcess(tree, reusedPid);
    QVERIFY(owner != nullptr);
    QVERIFY(selected != nullptr);
    QVERIFY(removed != nullptr);
    QVERIFY(reused != nullptr);
    QTreeWidgetItem* application = tree->topLevelItem(0);
    application->setExpanded(true);
    owner->setExpanded(true);
    selected->setExpanded(false);
    tree->setCurrentItem(selected);
    selected->setSelected(true);
    const QPersistentModelIndex applicationIndex(tree->indexFromItem(application));
    const QPersistentModelIndex ownerIndex(tree->indexFromItem(owner));
    const QPersistentModelIndex selectedIndex(tree->indexFromItem(selected));
    const QPersistentModelIndex removedIndex(tree->indexFromItem(removed));
    const QPersistentModelIndex reusedIndex(tree->indexFromItem(reused));
    QSignalSpy resetSpy(tree->model(), &QAbstractItemModel::modelReset);
    QVERIFY(resetSpy.isValid());

    for (int refreshIndex = 0; refreshIndex < 3; ++refreshIndex) {
        snapshot.processes[1].pssBytes += std::uint64_t{64} * kKiB;
        snapshot.processes[1].swapPssBytes += 1024;
        window.applySnapshot(snapshot);
        QCOMPARE(resetSpy.count(), 0);
        QVERIFY(applicationIndex.isValid());
        QVERIFY(ownerIndex.isValid());
        QVERIFY(selectedIndex.isValid());
        QCOMPARE(QPersistentModelIndex(tree->indexFromItem(application)), applicationIndex);
        QCOMPARE(QPersistentModelIndex(tree->indexFromItem(owner)), ownerIndex);
        QCOMPARE(QPersistentModelIndex(tree->indexFromItem(selected)), selectedIndex);
        QCOMPARE(selected->data(2, Qt::UserRole).toULongLong(), snapshot.processes.at(1).pssBytes);
        QCOMPARE(selected->data(3, Qt::UserRole).toULongLong(), snapshot.processes.at(1).swapPssBytes);
        QCOMPARE(tree->currentItem(), selected);
        QCOMPARE(tree->selectedItems().size(), 1);
        QCOMPARE(tree->selectedItems().constFirst(), selected);
        QVERIFY(application->isExpanded());
        QVERIFY(owner->isExpanded());
        QVERIFY(!selected->isExpanded());
    }

    snapshot.processes.removeAt(3);
    snapshot.processes.last().startTimeTicks += 10000;
    snapshot.processes.last().name = QStringLiteral("replacement-process");
    window.applySnapshot(snapshot);
    QCOMPARE(resetSpy.count(), 0);
    QVERIFY(!removedIndex.isValid());
    QVERIFY(!reusedIndex.isValid());
    QVERIFY(findProcess(tree, removedPid) == nullptr);
    QTreeWidgetItem* replacement = findProcess(tree, reusedPid);
    QVERIFY(replacement != nullptr);
    QCOMPARE(replacement->data(0, kProcessStartTimeRole).toULongLong(),
             snapshot.processes.constLast().startTimeTicks);
    QCOMPARE(replacement->text(0), QStringLiteral("replacement-process"));
    QVERIFY(!replacement->isSelected());
    QVERIFY(selectedIndex.isValid());
    QCOMPARE(tree->currentItem(), selected);
    QVERIFY(selected->isSelected());
    QVERIFY(owner->isExpanded());
    QVERIFY(!selected->isExpanded());

    tree->setCurrentItem(replacement);
    const QPersistentModelIndex replacementIndex(tree->indexFromItem(replacement));
    snapshot.processes.last().startTimeTicks += 10000;
    window.applySnapshot(snapshot);
    QVERIFY(!replacementIndex.isValid());
    QTreeWidgetItem* nextReplacement = findProcess(tree, reusedPid);
    QVERIFY(nextReplacement != nullptr);
    QVERIFY(!nextReplacement->isSelected());
    QVERIFY(applicationIndex.isValid());
    QVERIFY(ownerIndex.isValid());
    QVERIFY(selectedIndex.isValid());
    QCOMPARE(resetSpy.count(), 0);
}

void GuiSmokeTest::scrollingDefersOnlyTheTreeAndAppliesLatestSnapshot()
{
    memonitor::MainWindow window(nullptr, false);
    window.show();
    stopAutomaticRefresh(window);
    auto initial = syntheticSnapshot(96);
    for (auto& process : initial.processes) {
        process.ppid = -1;
    }
    initial.processes[0].name = QStringLiteral("initial-tree-row");
    window.applySnapshot(initial);
    QCoreApplication::processEvents();
    QTreeWidget* tree = window.processTree_;
    QScrollBar* scrollBar = tree->verticalScrollBar();
    QCOMPARE(tree->verticalScrollMode(), QAbstractItemView::ScrollPerPixel);
    QCOMPARE(window.processTreeIdleTimer_->interval(), 900);
    window.processTreeIdleTimer_->setInterval(40);
    QVERIFY(scrollBar->maximum() > 0);
    QTreeWidgetItem* watched = findProcess(tree, initial.processes.constFirst().pid);
    QVERIFY(watched != nullptr);
    const QPersistentModelIndex watchedIndex(tree->indexFromItem(watched));
    const QString originalOverview = window.physicalValue_->text();
    QSignalSpy dataSpy(tree->model(), &QAbstractItemModel::dataChanged);
    QSignalSpy resetSpy(tree->model(), &QAbstractItemModel::modelReset);
    QSignalSpy idleSpy(window.processTreeIdleTimer_, &QTimer::timeout);
    QVERIFY(dataSpy.isValid());
    QVERIFY(resetSpy.isValid());
    QVERIFY(idleSpy.isValid());
    QStringList observedNames;
    QObject observationContext;
    connect(tree->model(), &QAbstractItemModel::dataChanged, &observationContext, [&observedNames, &watchedIndex]() {
        observedNames.push_back(watchedIndex.data().toString());
    });

    scrollBar->setValue(std::max(1, scrollBar->maximum() / 3));
    QVERIFY(window.processTreeIdleTimer_->isActive());
    const int pausedScrollPosition = scrollBar->value();
    auto intermediate = initial;
    intermediate.memoryUsedBytes = 8 * kGiB;
    intermediate.memoryAvailableBytes = 8 * kGiB;
    intermediate.processes[0].name = QStringLiteral("intermediate-tree-row");
    intermediate.processes[0].pssBytes += 4096;
    auto intermediateOnly = intermediate.processes.constLast();
    intermediateOnly.pid += 1000;
    intermediateOnly.name = QStringLiteral("intermediate-only-process");
    intermediate.processes.push_back(intermediateOnly);
    window.applySnapshot(intermediate);
    QCOMPARE(window.physicalProgress_->value(), 500);
    QVERIFY(window.physicalValue_->text() != originalOverview);
    QCOMPARE(watchedIndex.data().toString(), QStringLiteral("initial-tree-row"));
    QCOMPARE(watched->data(2, Qt::UserRole).toULongLong(), initial.processes.constFirst().pssBytes);
    QVERIFY(findProcess(tree, intermediateOnly.pid) == nullptr);
    QVERIFY(window.processTreeUpdatePending_);

    intermediate.memoryUsedBytes = 10 * kGiB;
    intermediate.memoryAvailableBytes = 6 * kGiB;
    window.applySnapshot(intermediate);
    QCOMPARE(window.physicalProgress_->value(), 625);
    QCOMPARE(dataSpy.count(), 0);
    QCOMPARE(resetSpy.count(), 0);
    QCOMPARE(scrollBar->value(), pausedScrollPosition);

    scrollBar->setSliderDown(true);
    QVERIFY(!window.processTreeIdleTimer_->isActive());
    auto latest = initial;
    latest.memoryUsedBytes = 12 * kGiB;
    latest.memoryAvailableBytes = 4 * kGiB;
    latest.processes[0].name = QStringLiteral("latest-tree-row");
    latest.processes[0].pssBytes += 8192;
    auto latestOnly = latest.processes.constLast();
    latestOnly.pid += 2000;
    latestOnly.name = QStringLiteral("latest-only-process");
    latest.processes.push_back(latestOnly);
    window.applySnapshot(latest);
    QCOMPARE(window.physicalProgress_->value(), 750);
    QCOMPARE(window.latestSnapshot_.processes.constFirst().name, QStringLiteral("latest-tree-row"));
    QCOMPARE(watchedIndex.data().toString(), QStringLiteral("initial-tree-row"));
    QVERIFY(window.processTreeUpdatePending_);
    QCOMPARE(dataSpy.count(), 0);

    scrollBar->setValue(pausedScrollPosition + 1);
    QVERIFY(window.processTreeIdleTimer_->isActive());
    QVERIFY(QTest::qWaitFor([&idleSpy]() { return idleSpy.count() > 0; }, 2000));
    QVERIFY(scrollBar->isSliderDown());
    QVERIFY(window.processTreeUpdatePending_);
    QCOMPARE(watchedIndex.data().toString(), QStringLiteral("initial-tree-row"));
    QCOMPARE(dataSpy.count(), 0);
    QVERIFY(findProcess(tree, latestOnly.pid) == nullptr);

    scrollBar->setSliderDown(false);
    QVERIFY(window.processTreeIdleTimer_->isActive());
    QCOMPARE(watchedIndex.data().toString(), QStringLiteral("initial-tree-row"));
    QVERIFY(QTest::qWaitFor([&watchedIndex]() {
        return watchedIndex.data().toString() == QStringLiteral("latest-tree-row");
    }, 2000));
    QVERIFY(watchedIndex.isValid());
    QCOMPARE(watched->data(2, Qt::UserRole).toULongLong(), latest.processes.constFirst().pssBytes);
    QVERIFY(findProcess(tree, latestOnly.pid) != nullptr);
    QVERIFY(findProcess(tree, intermediateOnly.pid) == nullptr);
    QVERIFY(!observedNames.contains(QStringLiteral("intermediate-tree-row")));
    QCOMPARE(tree->topLevelItemCount(), latest.processes.size());
    QVERIFY(!window.processTreeUpdatePending_);
    QVERIFY(!window.processTreeIdleTimer_->isActive());
    QCOMPARE(window.physicalProgress_->value(), 750);
    QCOMPARE(resetSpy.count(), 0);
}

void GuiSmokeTest::largeProcessTreeWarmRefresh_data()
{
    QTest::addColumn<int>("processCount");
    QTest::newRow("2000-processes") << 2000;
    QTest::newRow("3000-processes") << 3000;
}

void GuiSmokeTest::largeProcessTreeWarmRefresh()
{
    QFETCH(int, processCount);
    memonitor::MainWindow window(nullptr, false);
    window.show();
    stopAutomaticRefresh(window);
    auto snapshot = syntheticSnapshot(processCount);
    window.windows_ = {syntheticWindow(snapshot.processes.constFirst().pid)};
    window.applySnapshot(snapshot);
    window.processTree_->expandAll();
    QCoreApplication::processEvents();
    window.latestSnapshot_ = snapshot;
    window.hasSnapshot_ = true;
    window.updateProcessTree(snapshot);
    QTreeWidget* tree = window.processTree_;
    QTreeWidgetItem* watched = findProcess(tree, snapshot.processes.constLast().pid);
    QVERIFY(watched != nullptr);
    const QPersistentModelIndex watchedIndex(tree->indexFromItem(watched));
    QSignalSpy resetSpy(tree->model(), &QAbstractItemModel::modelReset);
    QVERIFY(resetSpy.isValid());
    for (int refreshIndex = 0; refreshIndex < 3; ++refreshIndex) {
        for (auto& process : snapshot.processes) {
            process.pssBytes += 4096;
            process.swapPssBytes += 1024;
        }
        window.latestSnapshot_ = snapshot;
        QElapsedTimer refreshTimer;
        refreshTimer.start();
        window.updateProcessTree(snapshot);
        const qint64 elapsedNanoseconds = refreshTimer.nsecsElapsed();
        const double elapsedMilliseconds = static_cast<double>(elapsedNanoseconds) / 1000000.0;
        qInfo().nospace() << "In-place warm refresh: " << processCount << " processes, pass "
                          << refreshIndex + 1 << ", " << elapsedMilliseconds << " ms";
        QVERIFY2(elapsedNanoseconds < 500000000,
                 qPrintable(QStringLiteral("%1-process warm refresh took %2 ms (limit 500 ms)")
                                .arg(processCount).arg(elapsedMilliseconds, 0, 'f', 2)));
        QVERIFY(watchedIndex.isValid());
        QCOMPARE(QPersistentModelIndex(tree->indexFromItem(watched)), watchedIndex);
        QCOMPARE(watched->data(2, Qt::UserRole).toULongLong(), snapshot.processes.constLast().pssBytes);
        QCOMPARE(watched->data(3, Qt::UserRole).toULongLong(), snapshot.processes.constLast().swapPssBytes);
        QCOMPARE(resetSpy.count(), 0);
    }
    int displayedProcesses = 0;
    for (QTreeWidgetItemIterator iterator(tree); *iterator != nullptr; ++iterator) {
        if ((*iterator)->data(0, kProcessPidRole).toLongLong() > 0) {
            ++displayedProcesses;
        }
    }
    QCOMPARE(displayedProcesses, processCount);
}

QTEST_MAIN(GuiSmokeTest)

#include "gui_smoke_test.moc"
