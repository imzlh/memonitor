#pragma once

#include "memory_sampler.h"
#include "window_tracker.h"

#include <QFutureWatcher>
#include <QHash>
#include <QMainWindow>
#include <QSet>
#include <QVector>

class QCloseEvent;
class QLabel;
class QLineEdit;
class QPoint;
class QTableWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QProgressBar;
class QSystemTrayIcon;
class QTimer;
class QWidget;
class GuiSmokeTest;

namespace memonitor {

class MemoryPieChart;

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(QWidget* parent = nullptr, bool enableWindowTracking = true);
    ~MainWindow() override;

private:
    friend class ::GuiSmokeTest;

    void buildUi();
    void requestRefresh();
    void applySnapshot(const MemorySnapshot& snapshot);
    void updateCards(const MemorySnapshot& snapshot);
    void updateProcessTree(const MemorySnapshot& snapshot);
    void updateZramTable(const MemorySnapshot& snapshot);
    void scheduleProcessTreeUpdate();
    void applyProcessFilter();
    void showProcessContextMenu(const QPoint& position);
    void terminateProcessItem(qint64 pid, std::uint64_t startTimeTicks, bool includeChildren, bool force);
    void checkWarnings(const MemorySnapshot& snapshot);
    void showWarningNotification(const QString& title, const QString& message);
    void showSettings();
    void loadSettings();
    void saveSettings();
    void showAbout();
    void closeEvent(QCloseEvent* event) override;

    MemoryPieChart* memoryChart_ = nullptr;
    QLabel* physicalValue_ = nullptr;
    QLabel* physicalDetail_ = nullptr;
    QProgressBar* physicalProgress_ = nullptr;
    QLabel* zramValue_ = nullptr;
    QLabel* zramDetail_ = nullptr;
    QLabel* swapValue_ = nullptr;
    QLabel* swapDetail_ = nullptr;
    QLabel* pressureValue_ = nullptr;
    QLabel* pressureDetail_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* processScopeLabel_ = nullptr;
    QLabel* tableSummaryLabel_ = nullptr;
    QLabel* processEmptyLabel_ = nullptr;
    QLabel* zramSummaryLabel_ = nullptr;
    QLineEdit* processFilter_ = nullptr;
    QTreeWidget* processTree_ = nullptr;
    QHash<QString, QTreeWidgetItem*> processItems_;
    QHash<QString, bool> expansionBeforeSearch_;
    QString scrollAnchorBeforeSearch_;
    int scrollOffsetBeforeSearch_ = 0;
    int scrollPositionBeforeSearch_ = 0;
    QTableWidget* zramTable_ = nullptr;
    WindowTracker* windowTracker_ = nullptr;
    QVector<WindowInfo> windows_;
    QSystemTrayIcon* trayIcon_ = nullptr;
    QTimer* refreshTimer_ = nullptr;
    QTimer* processTreeIdleTimer_ = nullptr;
    QFutureWatcher<MemorySnapshot>* refreshWatcher_ = nullptr;
    QSet<QString> warnedProcesses_;
    MemorySnapshot latestSnapshot_;
    int refreshIntervalMs_ = 1500;
    int defaultProcessScope_ = 0;
    int processWarningPercent_ = 30;
    int availableMemoryWarningMode_ = 0;
    int availableMemoryWarningPercent_ = 10;
    int availableMemoryWarningMiB_ = 1024;
    bool notificationsEnabled_ = true;
    bool availableMemoryWarningEnabled_ = false;
    bool availableMemoryWarningActive_ = false;
    bool closeToTray_ = true;
    bool hasSnapshot_ = false;
    bool refreshInFlight_ = false;
    bool processTreeUpdatePending_ = false;
    bool updatingProcessTree_ = false;
    bool searchActive_ = false;
};

} // namespace memonitor
