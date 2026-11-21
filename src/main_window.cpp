#include "main_window.h"

#include "application_icon_resolver.h"
#include "memory_pie_chart.h"
#include "monitor_policy.h"
#include "window_chrome.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QBrush>
#include <QCloseEvent>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFormLayout>
#include <QFont>
#include <QFontMetrics>
#include <QFuture>
#include <QHash>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QProgressBar>
#include <QPalette>
#include <QPainter>
#include <QPersistentModelIndex>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSet>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStyle>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QSystemTrayIcon>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidget>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sys/types.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace memonitor {
namespace {

constexpr int kProcessKindRole = Qt::UserRole + 10;
constexpr int kProcessPidRole = Qt::UserRole + 11;
constexpr int kProcessStartTimeRole = Qt::UserRole + 12;
constexpr int kProcessSearchRole = Qt::UserRole + 13;
constexpr int kProcessIdentityRole = Qt::UserRole + 14;
constexpr int kApplicationOwnerPidRole = Qt::UserRole + 15;
constexpr int kMemoryShareRole = Qt::UserRole + 16;

enum class ProcessItemKind {
    Process,
    FilteredParent,
    Window,
    Application,
};

QVariant numericData(std::uint64_t value)
{
    return QVariant::fromValue<qulonglong>(static_cast<qulonglong>(value));
}

int boundedSetting(const QSettings& settings, const QString& key, int fallback, int minimum, int maximum)
{
    bool valid = false;
    const qlonglong value = settings.value(key, fallback).toLongLong(&valid);
    return valid ? static_cast<int>(std::clamp(value, static_cast<qlonglong>(minimum),
                                             static_cast<qlonglong>(maximum))) : fallback;
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

QString formatPercent(std::uint64_t value, std::uint64_t total)
{
    if (total == 0) {
        return QStringLiteral("--");
    }
    const double percent = static_cast<double>(value) * 100.0 / static_cast<double>(total);
    return QStringLiteral("%1%").arg(QString::number(percent, 'f', percent >= 10.0 ? 1 : 2));
}

QString formatMemoryValue(std::uint64_t bytes, bool exact)
{
    return exact ? formatBytes(bytes) : QStringLiteral("≈ %1").arg(formatBytes(bytes));
}

class NumericTableItem final : public QTableWidgetItem {
public:
    explicit NumericTableItem(const QString& text, std::uint64_t value)
        : QTableWidgetItem(text)
    {
        setData(Qt::UserRole, numericData(value));
        setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    }

    bool operator<(const QTableWidgetItem& other) const override
    {
        return data(Qt::UserRole).toULongLong() < other.data(Qt::UserRole).toULongLong();
    }
};

class ProcessTreeItem final : public QTreeWidgetItem {
public:
    explicit ProcessTreeItem(QTreeWidget* parent)
        : QTreeWidgetItem(parent)
    {
    }

    explicit ProcessTreeItem(QTreeWidgetItem* parent)
        : QTreeWidgetItem(parent)
    {
    }

    bool operator<(const QTreeWidgetItem& other) const override
    {
        const int column = treeWidget() == nullptr ? 0 : treeWidget()->sortColumn();
        if (column >= 1 && column <= 4) {
            return data(column, Qt::UserRole).toULongLong() < other.data(column, Qt::UserRole).toULongLong();
        }
        return text(column).localeAwareCompare(other.text(column)) < 0;
    }
};

class ProcessTreeWidget final : public QTreeWidget {
public:
    explicit ProcessTreeWidget(QWidget* parent)
        : QTreeWidget(parent)
    {
        connect(header(), &QHeaderView::sectionResized, this, [this](int section) {
            if (section != 0 && !resizingName_) {
                resizeNameColumn();
            }
        });
    }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QTreeWidget::resizeEvent(event);
        resizeNameColumn();
    }

private:
    void resizeNameColumn()
    {
        if (resizingName_) {
            return;
        }
        resizingName_ = true;
        int available = viewport()->width();
        for (int column = 1; column < columnCount(); ++column) {
            if (!isColumnHidden(column)) {
                available -= columnWidth(column);
            }
        }
        const int minimumNameWidth = fontMetrics().horizontalAdvance(QStringLiteral("应用窗口标题与所属进程")) + 64;
        setColumnWidth(0, std::max(minimumNameWidth, available));
        resizingName_ = false;
    }

    bool resizingName_ = false;
};

class MemoryUsageDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QStyledItemDelegate::paint(painter, option, index);
        const double share = std::clamp(index.data(kMemoryShareRole).toDouble(), 0.0, 1.0);
        if (share <= 0.0) {
            return;
        }
        const QRectF track = option.rect.adjusted(4, option.rect.height() - 3, -4, -1);
        QColor color = option.palette.color(option.state & QStyle::State_Selected
                                                ? QPalette::HighlightedText : QPalette::Highlight);
        color.setAlpha(160);
        painter->save();
        painter->setClipRect(option.rect);
        painter->setPen(Qt::NoPen);
        painter->setBrush(color);
        painter->drawRoundedRect(QRectF(track.left(), track.top(),
                                       std::max(2.0, track.width() * share), track.height()), 1.0, 1.0);
        painter->restore();
    }
};

void setNumericItem(QTableWidget* table, int row, int column, const QString& text, std::uint64_t value)
{
    auto* item = dynamic_cast<NumericTableItem*>(table->item(row, column));
    if (item == nullptr) {
        table->setItem(row, column, new NumericTableItem(text, value));
    } else {
        item->setText(text);
        item->setData(Qt::UserRole, numericData(value));
    }
}

void setTextItem(QTableWidget* table, int row, int column, const QString& text)
{
    QTableWidgetItem* item = table->item(row, column);
    if (item == nullptr) {
        table->setItem(row, column, new QTableWidgetItem(text));
    } else {
        item->setText(text);
        item->setData(Qt::UserRole, QVariant());
    }
}

void setNumericItem(QTreeWidgetItem* item, int column, const QString& text, std::uint64_t value)
{
    item->setText(column, text);
    item->setData(column, Qt::UserRole, numericData(value));
    item->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
}

QString processIdentity(qint64 pid, std::uint64_t startTime)
{
    return QStringLiteral("%1:%2").arg(pid).arg(startTime);
}

bool readProcessStartTime(qint64 pid, std::uint64_t& startTime)
{
    QFile statFile(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!statFile.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray stat = statFile.readAll();
    const qsizetype closingName = stat.lastIndexOf(')');
    if (closingName < 0) {
        return false;
    }
    const QList<QByteArray> fields = stat.mid(closingName + 1).simplified().split(' ');
    if (fields.size() <= 19) {
        return false;
    }
    bool ok = false;
    const std::uint64_t parsed = fields.at(19).toULongLong(&ok);
    if (!ok) {
        return false;
    }
    startTime = parsed;
    return true;
}

bool sendSignalToProcess(qint64 pid, std::uint64_t startTime, int signal, QString& error)
{
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
    const long rawPidFd = ::syscall(SYS_pidfd_open, static_cast<pid_t>(pid), 0U);
    if (rawPidFd < 0) {
        error = QStringLiteral("PID %1: %2").arg(pid).arg(QString::fromLocal8Bit(std::strerror(errno)));
        return false;
    }
    const int pidFd = static_cast<int>(rawPidFd);

    std::uint64_t currentStartTime = 0;
    if (!readProcessStartTime(pid, currentStartTime) || currentStartTime != startTime) {
        ::close(pidFd);
        error = QStringLiteral("PID %1 已退出或已被复用，未发送信号").arg(pid);
        return false;
    }

    const long result = ::syscall(SYS_pidfd_send_signal, pidFd, signal, nullptr, 0U);
    const int savedErrno = errno;
    ::close(pidFd);
    if (result != 0) {
        error = QStringLiteral("PID %1: %2").arg(pid).arg(QString::fromLocal8Bit(std::strerror(savedErrno)));
        return false;
    }
    return true;
#else
    Q_UNUSED(pid)
    Q_UNUSED(startTime)
    Q_UNUSED(signal)
    error = QStringLiteral("当前系统头文件不支持 pidfd，拒绝使用可能命中复用 PID 的方式结束进程");
    return false;
#endif
}

QWidget* makeMetric(const QString& title, QLabel*& value, QLabel*& detail)
{
    auto* widget = new QWidget;
    auto* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(0, 4, 0, 4);
    layout->setSpacing(3);

    auto* heading = new QHBoxLayout;
    auto* titleLabel = new QLabel(title);
    QFont titleFont = titleLabel->font();
    titleFont.setWeight(QFont::DemiBold);
    titleLabel->setFont(titleFont);
    value = new QLabel(QStringLiteral("--"));
    QFont valueFont = value->font();
    valueFont.setWeight(QFont::DemiBold);
    value->setFont(valueFont);
    value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    heading->addWidget(titleLabel);
    heading->addStretch();
    heading->addWidget(value);
    detail = new QLabel(QStringLiteral("等待采样"));
    detail->setTextFormat(Qt::PlainText);
    QFont detailFont = detail->font();
    detailFont.setPointSizeF(std::max(8.0, detailFont.pointSizeF() - 1.0));
    detail->setFont(detailFont);
    detail->setWordWrap(true);
    detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QPalette detailPalette = detail->palette();
    detailPalette.setColor(QPalette::WindowText, detailPalette.color(QPalette::PlaceholderText));
    detail->setPalette(detailPalette);

    layout->addLayout(heading);
    layout->addWidget(detail);
    return widget;
}

bool processInScope(const ProcessMemory& process, int scope, std::uint64_t currentUid)
{
    if (scope == 0) {
        return process.uid == currentUid;
    }
    if (scope == 2) {
        return process.uid != currentUid
            && process.uid != std::numeric_limits<std::uint64_t>::max();
    }
    return true;
}

QIcon resolveProcessIcon(const ProcessMemory* process, const WindowInfo* window)
{
    ApplicationIconRequest request;
    if (window != nullptr) {
        request.desktopFileName = window->desktopFileName;
        request.iconName = window->iconName;
        request.resourceClass = window->resourceClass.isEmpty() ? window->appId : window->resourceClass;
        request.resourceName = window->resourceName;
    }
    if (process != nullptr) {
        request.executableName = process->executable;
        if (request.resourceClass.isEmpty()) {
            request.resourceClass = process->name;
        }
    }
    return ApplicationIconResolver::resolve(request);
}

} // namespace

MainWindow::MainWindow(QWidget* parent, bool enableWindowTracking)
    : QMainWindow(parent)
    , refreshTimer_(new QTimer(this))
    , processTreeIdleTimer_(new QTimer(this))
    , refreshWatcher_(new QFutureWatcher<MemorySnapshot>(this))
{
    setWindowTitle(QStringLiteral("MemMonitor"));
    setWindowFlag(Qt::FramelessWindowHint);
    setWindowIcon(QIcon::fromTheme(QStringLiteral("utilities-system-monitor"),
                                  style()->standardIcon(QStyle::SP_ComputerIcon)));
    setMinimumSize(800, 600);
    resize(1120, 760);
    loadSettings();
    buildUi();

    if (enableWindowTracking) {
        windowTracker_ = new WindowTracker([this](const QVector<WindowInfo>& windows) {
            if (windows_ == windows) {
                return;
            }
            windows_ = windows;
            if (hasSnapshot_) {
                scheduleProcessTreeUpdate();
            }
        }, this);
    }

    connect(refreshTimer_, &QTimer::timeout, this, &MainWindow::requestRefresh);
    connect(refreshWatcher_, &QFutureWatcher<MemorySnapshot>::finished, this, [this]() {
        refreshInFlight_ = false;
        applySnapshot(refreshWatcher_->result());
    });

    processTreeIdleTimer_->setSingleShot(true);
    processTreeIdleTimer_->setInterval(900);
    connect(processTreeIdleTimer_, &QTimer::timeout, this, [this]() {
        if (processTreeUpdatePending_ && hasSnapshot_
            && !processTree_->verticalScrollBar()->isSliderDown()) {
            processTreeUpdatePending_ = false;
            updateProcessTree(latestSnapshot_);
        }
    });

    refreshTimer_->start(refreshIntervalMs_);
    requestRefresh();
}

MainWindow::~MainWindow()
{
    refreshTimer_->stop();
    if (refreshWatcher_->isRunning()) {
        refreshWatcher_->cancel();
        refreshWatcher_->waitForFinished();
    }
}

void MainWindow::buildUi()
{
    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* headerWidget = new QWidget(central);
    headerWidget->setObjectName(QStringLiteral("windowHeader"));
    headerWidget->setFixedHeight(40);
    auto* header = new QHBoxLayout(headerWidget);
    header->setContentsMargins(18, 0, 0, 0);
    header->setSpacing(4);
    auto* title = new QLabel(QStringLiteral("MemMonitor"), headerWidget);
    title->setObjectName(QStringLiteral("windowTitleLabel"));
    title->setContentsMargins(4, 4, 0, 0);
    QFont titleFont = title->font();
    titleFont.setFamilies({QStringLiteral("Noto Sans Mono"), QStringLiteral("Hack"), QStringLiteral("DejaVu Sans Mono"), QStringLiteral("monospace")});
    titleFont.setStyleHint(QFont::Monospace);
    titleFont.setPointSizeF(std::max(15.0, titleFont.pointSizeF() + 5.0));
    titleFont.setWeight(QFont::Normal);
    titleFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.5);
    title->setFont(titleFont);
    header->addWidget(title);
    header->addStretch(1);

    auto* refreshButton = new QPushButton;
    refreshButton->setIcon(QIcon::fromTheme(QStringLiteral("view-refresh"), style()->standardIcon(QStyle::SP_BrowserReload)));
    refreshButton->setToolTip(QStringLiteral("立即刷新"));
    refreshButton->setAccessibleName(QStringLiteral("立即刷新"));
    refreshButton->setShortcut(QKeySequence::Refresh);
    refreshButton->setFlat(true);
    refreshButton->setFixedSize(32, 32);
    connect(refreshButton, &QPushButton::clicked, this, &MainWindow::requestRefresh);
    header->addWidget(refreshButton);

    auto* settingsButton = new QPushButton;
    settingsButton->setObjectName(QStringLiteral("settingsButton"));
    settingsButton->setIcon(QIcon::fromTheme(QStringLiteral("settings-configure"), style()->standardIcon(QStyle::SP_FileDialogDetailedView)));
    settingsButton->setToolTip(QStringLiteral("设置（Ctrl+,）"));
    settingsButton->setAccessibleName(QStringLiteral("设置"));
    settingsButton->setShortcut(QKeySequence(QStringLiteral("Ctrl+,")));
    settingsButton->setFlat(true);
    settingsButton->setFixedSize(32, 32);
    connect(settingsButton, &QPushButton::clicked, this, &MainWindow::showSettings);
    header->addWidget(settingsButton);

    auto* aboutButton = new QPushButton;
    aboutButton->setObjectName(QStringLiteral("aboutButton"));
    aboutButton->setIcon(QIcon::fromTheme(QStringLiteral("help-about"), style()->standardIcon(QStyle::SP_MessageBoxInformation)));
    aboutButton->setToolTip(QStringLiteral("关于"));
    aboutButton->setAccessibleName(QStringLiteral("关于"));
    aboutButton->setFlat(true);
    aboutButton->setFixedSize(32, 32);
    connect(aboutButton, &QPushButton::clicked, this, &MainWindow::showAbout);
    header->addWidget(aboutButton);
    header->addSpacing(12);

    auto* windowControls = new QWidget(headerWidget);
    windowControls->setObjectName(QStringLiteral("windowControls"));
    auto* controlsLayout = new QHBoxLayout(windowControls);
    controlsLayout->setContentsMargins(0, 0, 0, 0);
    controlsLayout->setSpacing(0);
    auto* minimizeButton = new WindowCaptionButton(WindowCaptionButton::Action::Minimize, windowControls);
    minimizeButton->setObjectName(QStringLiteral("minimizeWindowButton"));
    auto* maximizeButton = new WindowCaptionButton(WindowCaptionButton::Action::Maximize, windowControls);
    maximizeButton->setObjectName(QStringLiteral("maximizeWindowButton"));
    auto* closeButton = new WindowCaptionButton(WindowCaptionButton::Action::Close, windowControls);
    closeButton->setObjectName(QStringLiteral("closeWindowButton"));
    controlsLayout->addWidget(minimizeButton);
    controlsLayout->addWidget(maximizeButton);
    controlsLayout->addWidget(closeButton);
    header->addWidget(windowControls);
    root->addWidget(headerWidget);

    auto* content = new QWidget(central);
    auto* contentRoot = new QVBoxLayout(content);
    contentRoot->setContentsMargins(18, 8, 18, 12);
    contentRoot->setSpacing(8);
    auto* mainSplitter = new QSplitter(Qt::Horizontal, content);
    mainSplitter->setObjectName(QStringLiteral("mainSplitter"));
    mainSplitter->setChildrenCollapsible(false);
    mainSplitter->setHandleWidth(1);

    auto* leftPanel = new QWidget(mainSplitter);
    leftPanel->setMinimumWidth(320);
    leftPanel->setMaximumWidth(340);
    auto* leftRoot = new QVBoxLayout(leftPanel);
    leftRoot->setContentsMargins(0, 0, 8, 0);
    leftRoot->setSpacing(8);
    auto* overviewScroll = new QScrollArea(leftPanel);
    overviewScroll->setObjectName(QStringLiteral("overviewScroll"));
    overviewScroll->setFrameShape(QFrame::NoFrame);
    overviewScroll->setWidgetResizable(true);
    overviewScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* overview = new QWidget(leftPanel);
    overview->setObjectName(QStringLiteral("memoryOverview"));
    auto* overviewLayout = new QVBoxLayout(overview);
    overviewLayout->setContentsMargins(0, 4, 0, 0);
    overviewLayout->setSpacing(8);
    auto* overviewTitle = new QLabel(QStringLiteral("整体内存占用"), overview);
    QFont overviewFont = overviewTitle->font();
    overviewFont.setWeight(QFont::DemiBold);
    overviewTitle->setFont(overviewFont);
    overviewLayout->addWidget(overviewTitle);
    auto* memoryText = new QWidget(overview);
    auto* memoryTextLayout = new QVBoxLayout(memoryText);
    memoryTextLayout->setContentsMargins(0, 0, 0, 0);
    memoryTextLayout->setSpacing(5);
    auto* memoryCaption = new QLabel(QStringLiteral("物理内存"));
    QFont captionFont = memoryCaption->font();
    captionFont.setWeight(QFont::DemiBold);
    memoryCaption->setFont(captionFont);
    physicalValue_ = new QLabel(QStringLiteral("--"));
    QFont totalFont = physicalValue_->font();
    totalFont.setPointSize(totalFont.pointSize() + 7);
    totalFont.setWeight(QFont::DemiBold);
    physicalValue_->setFont(totalFont);
    physicalDetail_ = new QLabel(QStringLiteral("等待第一次采样"));
    physicalDetail_->setWordWrap(true);
    QFont physicalDetailFont = physicalDetail_->font();
    physicalDetailFont.setPointSizeF(std::max(8.0, physicalDetailFont.pointSizeF() - 1.0));
    physicalDetail_->setFont(physicalDetailFont);
    physicalDetail_->setTextFormat(Qt::PlainText);
    physicalDetail_->setToolTip(QStringLiteral("估算可用是 MemAvailable，包含内核认为可以回收的部分缓存；空闲是 MemFree。zram 占用的 RAM 已计入物理消耗，不重复扣减。估算可用不保证分配成功或不会 OOM。"));
    physicalProgress_ = new QProgressBar;
    physicalProgress_->setObjectName(QStringLiteral("memoryProgress"));
    physicalProgress_->setRange(0, 1000);
    physicalProgress_->setTextVisible(false);
    physicalProgress_->setFixedHeight(10);
    physicalProgress_->setAccessibleName(QStringLiteral("物理内存使用率"));
    memoryTextLayout->addWidget(memoryCaption);
    memoryTextLayout->addWidget(physicalValue_);
    memoryTextLayout->addWidget(physicalProgress_);
    memoryTextLayout->addWidget(physicalDetail_);
    overviewLayout->addWidget(memoryText);
    memoryChart_ = new MemoryPieChart(overview);
    memoryChart_->setObjectName(QStringLiteral("memoryPieChart"));
    memoryChart_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    overviewLayout->addWidget(memoryChart_);
    auto* metricsLayout = new QVBoxLayout;
    metricsLayout->setContentsMargins(0, 5, 0, 0);
    metricsLayout->setSpacing(4);
    metricsLayout->addWidget(makeMetric(QStringLiteral("zram 实际占用"), zramValue_, zramDetail_));
    metricsLayout->addWidget(makeMetric(QStringLiteral("交换区剩余"), swapValue_, swapDetail_));
    metricsLayout->addWidget(makeMetric(QStringLiteral("内存压力"), pressureValue_, pressureDetail_));
    swapValue_->setObjectName(QStringLiteral("swapFreeValue"));
    pressureValue_->setObjectName(QStringLiteral("memoryPressureValue"));
    overviewLayout->addLayout(metricsLayout);
    overviewLayout->addStretch(1);
    overviewScroll->setWidget(overview);
    leftRoot->addWidget(overviewScroll);

    statusLabel_ = new QLabel(QStringLiteral("正在读取内存数据…"), content);
    statusLabel_->setObjectName(QStringLiteral("statusLabel"));
    statusLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont statusFont = statusLabel_->font();
    statusFont.setPointSizeF(std::max(8.0, statusFont.pointSizeF() - 1.0));
    statusLabel_->setFont(statusFont);
    QPalette statusPalette = statusLabel_->palette();
    statusPalette.setColor(QPalette::WindowText, statusPalette.color(QPalette::PlaceholderText));
    statusLabel_->setPalette(statusPalette);

    auto* rightPanel = new QWidget(mainSplitter);
    auto* rightRoot = new QVBoxLayout(rightPanel);
    rightRoot->setContentsMargins(8, 0, 0, 0);
    rightRoot->setSpacing(6);
    auto* tabs = new QTabWidget(rightPanel);
    tabs->setDocumentMode(true);
    tabs->tabBar()->setExpanding(false);

    auto* processPage = new QWidget;
    auto* processRoot = new QVBoxLayout(processPage);
    processRoot->setContentsMargins(0, 6, 0, 0);
    processRoot->setSpacing(6);
    auto* processHeader = new QHBoxLayout;
    processScopeLabel_ = new QLabel(QStringLiteral("我的进程"));
    processScopeLabel_->setObjectName(QStringLiteral("processScopeLabel"));
    processScopeLabel_->setToolTip(QStringLiteral("在设置中更改显示的进程范围"));
    QFont sectionFont = processScopeLabel_->font();
    sectionFont.setWeight(QFont::DemiBold);
    processScopeLabel_->setFont(sectionFont);
    processHeader->addWidget(processScopeLabel_);
    processHeader->addStretch(1);
    tableSummaryLabel_ = new QLabel(QStringLiteral("等待采样"));
    tableSummaryLabel_->setObjectName(QStringLiteral("processSummary"));
    tableSummaryLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    tableSummaryLabel_->setPalette(statusPalette);
    processHeader->addWidget(tableSummaryLabel_);
    processFilter_ = new QLineEdit(processPage);
    processFilter_->setObjectName(QStringLiteral("processFilter"));
    processFilter_->setPlaceholderText(QStringLiteral("搜索进程、PID 或窗口"));
    processFilter_->setClearButtonEnabled(true);
    processFilter_->setToolTip(QStringLiteral("搜索进程名、PID、用户、命令行和窗口标题"));
    processFilter_->setAccessibleName(QStringLiteral("搜索应用和进程"));
    const QIcon searchIcon = QIcon::fromTheme(QStringLiteral("edit-find"));
    if (!searchIcon.isNull()) {
        processFilter_->addAction(searchIcon, QLineEdit::LeadingPosition);
    }
    processFilter_->setMinimumWidth(180);
    processFilter_->setMaximumWidth(250);
    connect(processFilter_, &QLineEdit::textChanged, this, &MainWindow::applyProcessFilter);
    processHeader->addSpacing(8);
    processHeader->addWidget(processFilter_, 1);
    processRoot->addLayout(processHeader);

    processTree_ = new ProcessTreeWidget(processPage);
    processTree_->setObjectName(QStringLiteral("processTree"));
    processTree_->setColumnCount(6);
    processTree_->setHeaderLabels({
        QStringLiteral("应用 / 进程"), QStringLiteral("PID"), QStringLiteral("内存"),
        QStringLiteral("交换"), QStringLiteral("总足迹"), QStringLiteral("用户"),
    });
    processTree_->headerItem()->setToolTip(2, QStringLiteral("驻留 PSS：按比例分摊共享页后的物理内存占用"));
    processTree_->headerItem()->setToolTip(3, QStringLiteral("SwapPss：交换区中归属该进程的逻辑数据量，包含 zram 中的数据；不是压缩后占用的 RAM"));
    processTree_->headerItem()->setToolTip(4, QStringLiteral("PSS + SwapPss：驻留内存与逻辑交换量之和"));
    for (int column = 2; column <= 4; ++column) {
        processTree_->headerItem()->setToolTip(column, processTree_->headerItem()->toolTip(column)
                                                   + QStringLiteral("；≈ 表示含近似采样"));
    }
    processTree_->setAlternatingRowColors(true);
    processTree_->setSelectionBehavior(QAbstractItemView::SelectRows);
    processTree_->setSelectionMode(QAbstractItemView::SingleSelection);
    processTree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    processTree_->setSortingEnabled(false);
    processTree_->setUniformRowHeights(true);
    processTree_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    processTree_->setIconSize(QSize(20, 20));
    processTree_->setTextElideMode(Qt::ElideRight);
    processTree_->setContextMenuPolicy(Qt::CustomContextMenu);
    processTree_->setAccessibleName(QStringLiteral("应用和进程内存列表"));
    processTree_->setItemDelegateForColumn(2, new MemoryUsageDelegate(processTree_));
    processTree_->setToolTip(QStringLiteral("应用行显示窗口标题，展开后查看所属进程树；右键进程可结束任务。"));
    processTree_->header()->setStretchLastSection(false);
    processTree_->header()->setSectionResizeMode(QHeaderView::Interactive);
    const QFontMetrics columnMetrics(processTree_->font());
    processTree_->setColumnWidth(1, columnMetrics.horizontalAdvance(QStringLiteral("99 个进程")) + 16);
    for (int column = 2; column <= 5; ++column) {
        const QString sample = column == 5 ? QStringLiteral("用户名") : QStringLiteral("999.9 MiB");
        processTree_->setColumnWidth(column, columnMetrics.horizontalAdvance(sample) + 16);
    }
    processTree_->header()->setSortIndicator(4, Qt::DescendingOrder);
    connect(processTree_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this]() {
        if (!updatingProcessTree_) {
            processTreeIdleTimer_->start();
        }
    });
    connect(processTree_->verticalScrollBar(), &QScrollBar::sliderPressed,
            processTreeIdleTimer_, qOverload<>(&QTimer::stop));
    connect(processTree_->verticalScrollBar(), &QScrollBar::sliderReleased,
            processTreeIdleTimer_, qOverload<>(&QTimer::start));
    connect(processTree_, &QTreeWidget::customContextMenuRequested,
            this, &MainWindow::showProcessContextMenu);
    processEmptyLabel_ = new QLabel(processTree_->viewport());
    processEmptyLabel_->setObjectName(QStringLiteral("processEmptyState"));
    processEmptyLabel_->setAlignment(Qt::AlignCenter);
    processEmptyLabel_->setWordWrap(true);
    processEmptyLabel_->setPalette(statusPalette);
    processEmptyLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto* emptyLayout = new QVBoxLayout(processTree_->viewport());
    emptyLayout->setContentsMargins(24, 24, 24, 24);
    emptyLayout->addWidget(processEmptyLabel_, 0, Qt::AlignCenter);
    processEmptyLabel_->hide();
    processRoot->addWidget(processTree_, 1);
    tabs->addTab(processPage, QStringLiteral("进程"));
    auto* findAction = new QAction(this);
    findAction->setShortcut(QKeySequence::Find);
    connect(findAction, &QAction::triggered, this, [this, tabs, processPage]() {
        tabs->setCurrentWidget(processPage);
        processFilter_->setFocus(Qt::ShortcutFocusReason);
        processFilter_->selectAll();
    });
    addAction(findAction);
    auto* clearSearchAction = new QAction(processFilter_);
    clearSearchAction->setShortcut(QKeySequence(Qt::Key_Escape));
    clearSearchAction->setShortcutContext(Qt::WidgetShortcut);
    connect(clearSearchAction, &QAction::triggered, processFilter_, &QLineEdit::clear);
    processFilter_->addAction(clearSearchAction);

    auto* zramPage = new QWidget;
    auto* zramRoot = new QVBoxLayout(zramPage);
    zramRoot->setContentsMargins(0, 6, 0, 0);
    zramRoot->setSpacing(9);
    auto* zramHeader = new QHBoxLayout;
    auto* zramTitle = new QLabel(QStringLiteral("zram 设备"));
    zramTitle->setFont(sectionFont);
    zramHeader->addWidget(zramTitle);
    zramHeader->addStretch(1);
    zramSummaryLabel_ = new QLabel(QStringLiteral("等待采样"));
    zramHeader->addWidget(zramSummaryLabel_);
    zramRoot->addLayout(zramHeader);

    zramTable_ = new QTableWidget(0, 7, zramPage);
    zramTable_->setHorizontalHeaderLabels({
        QStringLiteral("设备"), QStringLiteral("算法"), QStringLiteral("容量上限"),
        QStringLiteral("逻辑数据"), QStringLiteral("压缩数据"), QStringLiteral("实际占用"), QStringLiteral("压缩比"),
    });
    zramTable_->setObjectName(QStringLiteral("zramTable"));
    zramTable_->horizontalHeaderItem(2)->setToolTip(QStringLiteral("允许存放的逻辑容量上限；不是已占用的物理内存"));
    zramTable_->horizontalHeaderItem(3)->setToolTip(QStringLiteral("压缩前的原始数据量"));
    zramTable_->horizontalHeaderItem(4)->setToolTip(QStringLiteral("压缩后的数据量"));
    zramTable_->horizontalHeaderItem(5)->setToolTip(QStringLiteral("实际使用的物理 RAM，包含压缩数据与管理开销；来自 mm_stat 的 mem_used_total"));
    zramTable_->horizontalHeaderItem(6)->setToolTip(QStringLiteral("逻辑数据量 ÷ 压缩数据量"));
    zramTable_->setAlternatingRowColors(true);
    zramTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    zramTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    zramTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    zramTable_->setShowGrid(false);
    zramTable_->setTextElideMode(Qt::ElideRight);
    zramTable_->verticalHeader()->setVisible(false);
    zramTable_->verticalHeader()->setDefaultSectionSize(30);
    zramTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    zramTable_->horizontalHeader()->setStretchLastSection(false);
    zramTable_->horizontalHeader()->setMinimumSectionSize(
        QFontMetrics(zramTable_->font()).horizontalAdvance(QStringLiteral("999.9 MiB")) + 12);
    for (int column = 0; column < zramTable_->columnCount(); ++column) {
        const Qt::Alignment alignment = column < 2 ? Qt::AlignLeft | Qt::AlignVCenter : Qt::AlignRight | Qt::AlignVCenter;
        zramTable_->horizontalHeaderItem(column)->setTextAlignment(alignment);
        if (column >= 2 && column <= 5) {
            zramTable_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::Stretch);
        }
    }
    zramRoot->addWidget(zramTable_, 1);
    auto* zramNote = new QLabel(QStringLiteral("逻辑数据为压缩前大小；实际占用含管理开销，已计入物理内存。"));
    zramNote->setWordWrap(true);
    zramNote->setFont(statusFont);
    zramNote->setPalette(statusPalette);
    zramRoot->addWidget(zramNote);
    tabs->addTab(zramPage, QStringLiteral("zram"));
    rightRoot->addWidget(tabs, 1);

    mainSplitter->addWidget(leftPanel);
    mainSplitter->addWidget(rightPanel);
    mainSplitter->setStretchFactor(0, 35);
    mainSplitter->setStretchFactor(1, 65);
    mainSplitter->setSizes({360, 680});
    contentRoot->addWidget(mainSplitter, 1);
    contentRoot->addWidget(statusLabel_);
    root->addWidget(content, 1);
    setCentralWidget(central);
    new WindowChrome(this, headerWidget, title, minimizeButton, maximizeButton, closeButton);

    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        trayIcon_ = new QSystemTrayIcon(this);
        QIcon trayIcon = QIcon::fromTheme(QStringLiteral("utilities-system-monitor"));
        if (trayIcon.isNull()) {
            trayIcon = style()->standardIcon(QStyle::SP_ComputerIcon);
        }
        trayIcon_->setIcon(trayIcon);
        trayIcon_->setToolTip(QStringLiteral("MemMonitor"));

        auto* menu = new QMenu(this);
        auto* showAction = menu->addAction(QStringLiteral("显示窗口"));
        connect(showAction, &QAction::triggered, this, [this]() {
            showNormal();
            raise();
            activateWindow();
        });
        auto* refreshAction = menu->addAction(QStringLiteral("立即刷新"));
        connect(refreshAction, &QAction::triggered, this, &MainWindow::requestRefresh);
        menu->addSeparator();
        auto* quitAction = menu->addAction(QStringLiteral("退出"));
        connect(quitAction, &QAction::triggered, qApp, &QApplication::quit);
        trayIcon_->setContextMenu(menu);
        trayIcon_->show();
    }
}

void MainWindow::requestRefresh()
{
    if (refreshInFlight_) {
        return;
    }

    refreshInFlight_ = true;
    refreshWatcher_->setFuture(QtConcurrent::run([]() {
        return MemorySampler::collect();
    }));
}

void MainWindow::applySnapshot(const MemorySnapshot& snapshot)
{
    latestSnapshot_ = snapshot;
    hasSnapshot_ = true;
    updateCards(snapshot);
    scheduleProcessTreeUpdate();
    updateZramTable(snapshot);
    checkWarnings(snapshot);

    int approximateProcesses = 0;
    for (const ProcessMemory& process : snapshot.processes) {
        if (process.approximate) {
            ++approximateProcesses;
        }
    }
    const QString zramState = snapshot.zramDevices.isEmpty()
        ? QStringLiteral("未检测到 zram")
        : QStringLiteral("%1 个 zram 设备").arg(snapshot.zramDevices.size());
    const QString windowState = windowTracker_ == nullptr
        ? QStringLiteral("窗口标题不可用")
        : windowTracker_->isAvailable() ? QStringLiteral("KWin 窗口已关联") : QStringLiteral("KWin 窗口不可用");
    const QString windowCount = windowTracker_ != nullptr && windowTracker_->isAvailable()
        ? QStringLiteral("%1 个窗口").arg(windows_.size())
        : QStringLiteral("窗口标题不可用");
    statusLabel_->setText(QStringLiteral("更新于 %1  ·  %2 个进程  ·  %3  ·  %4")
                              .arg(snapshot.timestamp.toString(QStringLiteral("HH:mm:ss")))
                              .arg(snapshot.processes.size())
                              .arg(zramState)
                              .arg(windowCount));
    statusLabel_->setToolTip(QStringLiteral("进程数据：%1 个近似采样，%2 个读取时已退出或无权限；物理内存：%3；交换区：%4；%5")
                                 .arg(approximateProcesses)
                                 .arg(snapshot.processesSkipped)
                                 .arg(snapshot.memoryInfoComplete ? QStringLiteral("完整") : QStringLiteral("不完整"))
                                 .arg(snapshot.swapInfoComplete ? QStringLiteral("完整") : QStringLiteral("不完整"))
                                 .arg(windowState));
}

void MainWindow::updateCards(const MemorySnapshot& snapshot)
{
    if (snapshot.memoryInfoComplete) {
        const double ratio = static_cast<double>(snapshot.memoryUsedBytes)
            / static_cast<double>(snapshot.memoryTotalBytes);
        const int progress = static_cast<int>(std::min(1000.0, std::max(0.0, ratio * 1000.0 + 0.5)));
        physicalProgress_->setValue(progress);
        physicalValue_->setText(QStringLiteral("%1 / %2")
                                    .arg(formatBytes(snapshot.memoryUsedBytes), formatBytes(snapshot.memoryTotalBytes)));
        physicalDetail_->setText(QStringLiteral("估算可用 %1 · 空闲 %2")
                                     .arg(formatBytes(snapshot.memoryAvailableBytes),
                                          snapshot.memoryFreeReadable ? formatBytes(snapshot.memoryFreeBytes)
                                                                      : QStringLiteral("不可读")));
    } else {
        physicalProgress_->setValue(0);
        physicalValue_->setText(QStringLiteral("不可用"));
        physicalDetail_->setText(snapshot.memoryFreeReadable
                                    ? QStringLiteral("估算可用不可读 · 空闲 %1").arg(formatBytes(snapshot.memoryFreeBytes))
                                    : QStringLiteral("无法完整读取 /proc/meminfo"));
    }

    std::uint64_t zramMemory = 0;
    std::uint64_t zramOriginal = 0;
    std::uint64_t zramCompressed = 0;
    std::uint64_t zramCapacity = 0;
    std::uint64_t zramFailedWrites = 0;
    bool zramCapacityComplete = true;
    int readable = 0;
    for (const ZramDevice& device : snapshot.zramDevices) {
        if (device.ioStatisticsReadable) {
            zramFailedWrites += device.failedWrites;
        }
        zramCapacityComplete = zramCapacityComplete && device.statisticsReadable && device.diskSizeBytes > 0;
        if (!device.statisticsReadable) {
            continue;
        }
        ++readable;
        zramMemory += device.memoryUsedBytes;
        zramOriginal += device.originalDataBytes;
        zramCompressed += device.compressedDataBytes;
        zramCapacity += device.diskSizeBytes;
    }
    memoryChart_->setMemory(snapshot.memoryInfoComplete ? snapshot.memoryTotalBytes : 0,
                            snapshot.memoryAvailableBytes, zramMemory, zramOriginal,
                            zramCapacityComplete ? zramCapacity : 0,
                            readable == snapshot.zramDevices.size());

    if (snapshot.zramDevices.isEmpty()) {
        zramValue_->setText(QStringLiteral("--"));
        zramDetail_->setText(QStringLiteral("没有活动 zram 设备"));
        zramDetail_->setToolTip(QString());
    } else if (readable == 0) {
        zramValue_->setText(QStringLiteral("不可用"));
        zramDetail_->setText(QStringLiteral("无法读取任何 zram 的 mm_stat"));
        zramDetail_->setToolTip(QString());
    } else {
        zramValue_->setText(readable == snapshot.zramDevices.size() ? formatBytes(zramMemory)
                                                                  : QStringLiteral("部分 %1").arg(formatBytes(zramMemory)));
        zramDetail_->setText(QStringLiteral("逻辑 %1 / %2")
                                 .arg(formatBytes(zramOriginal), zramCapacityComplete
                                          ? formatBytes(zramCapacity) : QStringLiteral("容量不可读")));
        const QString swapText = snapshot.swapInfoComplete ? formatBytes(snapshot.swapUsedBytes) : QStringLiteral("不可用");
        zramDetail_->setToolTip(QStringLiteral("zram 实际 RAM 已包含在物理已用中。\n压缩前数据 %1，压缩数据 %2。逻辑使用率不等同于交换槽位使用率。\n交换区已用 %3；%4/%5 个 zram 设备可读。")
                                  .arg(formatBytes(zramOriginal), formatBytes(zramCompressed), swapText)
                                  .arg(readable).arg(snapshot.zramDevices.size()));
    }

    if (snapshot.swapInfoComplete && snapshot.swapTotalBytes > 0) {
        swapValue_->setText(formatBytes(snapshot.swapFreeBytes));
        swapDetail_->setText(QStringLiteral("已用 %1 / %2 · %3")
                                 .arg(formatBytes(snapshot.swapUsedBytes), formatBytes(snapshot.swapTotalBytes),
                                      formatPercent(snapshot.swapUsedBytes, snapshot.swapTotalBytes)));
    } else {
        swapValue_->setText(QStringLiteral("--"));
        swapDetail_->setText(snapshot.swapInfoComplete ? QStringLiteral("未启用交换区")
                                                       : QStringLiteral("无法读取交换区统计"));
    }
    const QString swapExplanation = QStringLiteral("所有交换区的剩余逻辑槽位（SwapFree），包含用作交换区的 zram；不是额外物理 RAM。zram 快满时，系统继续换出匿名页的余地会减少；这不能通过从 MemAvailable 再减一次 zram RAM 来修正。");
    swapValue_->setToolTip(swapExplanation);
    swapDetail_->setToolTip(swapExplanation);

    pressureValue_->setText(snapshot.memoryPressureReadable
                               ? QStringLiteral("阻塞 %1%")
                                     .arg(QString::number(snapshot.memoryPressureSomeAvg10, 'f', 1))
                               : QStringLiteral("不可读"));
    const QString oomText = snapshot.oomKillCountReadable
        ? QStringLiteral("OOM 击杀 %1").arg(snapshot.oomKillCount)
        : QStringLiteral("OOM 计数不可读");
    pressureDetail_->setText(snapshot.memoryPressureReadable
                                ? QStringLiteral("完全阻塞 %1% · %2")
                                      .arg(QString::number(snapshot.memoryPressureFullAvg10, 'f', 1), oomText)
                                : oomText);
    const QString pressureExplanation = QStringLiteral("PSI 最近 10 秒平均值：部分阻塞为至少一个任务因内存压力停顿的时间比例；完全阻塞为所有非空闲任务同时停顿的比例。它表示已经发生的压力，不预测剩余可分配容量。\nOOM 击杀为本次开机以来内核杀死的进程数，不包含应用自身的分配失败、堆上限或用户态内存管理程序的终止。");
    pressureValue_->setToolTip(pressureExplanation);
    pressureDetail_->setToolTip(pressureExplanation);
    if (zramFailedWrites > 0) {
        zramDetail_->setToolTip(zramDetail_->toolTip()
                               + QStringLiteral("\nzram 设备累计写失败 %1 次，不代表当前仍在失败。")
                                     .arg(zramFailedWrites));
    }
}

void MainWindow::updateProcessTree(const MemorySnapshot& snapshot)
{
    if (processTree_ == nullptr) {
        return;
    }

    processTreeIdleTimer_->stop();
    processTreeUpdatePending_ = false;
    updatingProcessTree_ = true;
    const int scrollPosition = processTree_->verticalScrollBar()->value();
    const QPersistentModelIndex scrollAnchor(processTree_->indexAt(QPoint(0, 0)));
    const int scrollAnchorOffset = processTree_->visualRect(scrollAnchor).top();
    const QString filter = processFilter_->text().trimmed();
    const bool restoreAfterSearch = searchActive_ && filter.isEmpty();
    if (!searchActive_ && !filter.isEmpty()) {
        expansionBeforeSearch_.clear();
        for (auto iterator = processItems_.cbegin(); iterator != processItems_.cend(); ++iterator) {
            expansionBeforeSearch_.insert(iterator.key(), iterator.value()->isExpanded());
        }
        QTreeWidgetItem* anchor = processTree_->itemAt(QPoint(0, 0));
        scrollAnchorBeforeSearch_ = anchor == nullptr ? QString() : anchor->data(0, kProcessIdentityRole).toString();
        scrollOffsetBeforeSearch_ = scrollAnchorOffset;
        scrollPositionBeforeSearch_ = scrollPosition;
    }
    searchActive_ = !filter.isEmpty();
    const QString currentIdentity = processTree_->currentItem() == nullptr ? QString()
        : processTree_->currentItem()->data(0, kProcessIdentityRole).toString();
    const QList<QTreeWidgetItem*> selectedItems = processTree_->selectedItems();
    const QString selectedIdentity = selectedItems.isEmpty() ? QString()
        : selectedItems.constFirst()->data(0, kProcessIdentityRole).toString();
    const bool hadTree = processTree_->topLevelItemCount() > 0;
    QSet<QString> liveIdentities;
    QSet<QTreeWidgetItem*> newItems;
    const auto acquireItem = [this, &liveIdentities, &newItems](const QString& identity) {
        liveIdentities.insert(identity);
        auto* item = static_cast<ProcessTreeItem*>(processItems_.value(identity, nullptr));
        if (item == nullptr) {
            item = new ProcessTreeItem(static_cast<QTreeWidget*>(nullptr));
            item->setData(0, kProcessIdentityRole, identity);
            processItems_.insert(identity, item);
            newItems.insert(item);
        }
        return item;
    };
    const auto attachItem = [this](QTreeWidgetItem* item, QTreeWidgetItem* parent) {
        if (item->treeWidget() == processTree_ && item->parent() == parent) {
            return;
        }
        if (QTreeWidgetItem* oldParent = item->parent()) {
            oldParent->takeChild(oldParent->indexOfChild(item));
        } else if (item->treeWidget() == processTree_) {
            processTree_->takeTopLevelItem(processTree_->indexOfTopLevelItem(item));
        }
        if (parent != nullptr) {
            parent->addChild(item);
        } else {
            processTree_->addTopLevelItem(item);
        }
    };

    processTree_->setUpdatesEnabled(false);
    processTree_->setSortingEnabled(false);
    const int scope = defaultProcessScope_;
    const std::uint64_t currentUid = static_cast<std::uint64_t>(::getuid());
    QHash<qint64, const ProcessMemory*> processesByPid;
    QHash<qint64, ProcessTreeItem*> itemsByPid;
    processesByPid.reserve(snapshot.processes.size());
    itemsByPid.reserve(snapshot.processes.size());
    QSet<qint64> inScopePids;
    QSet<qint64> visibleProcessPids;

    for (const ProcessMemory& process : snapshot.processes) {
        processesByPid.insert(process.pid, &process);
        if (processInScope(process, scope, currentUid)) {
            inScopePids.insert(process.pid);
            visibleProcessPids.insert(process.pid);
        }
    }
    for (const ProcessMemory& process : snapshot.processes) {
        if (!inScopePids.contains(process.pid)) {
            continue;
        }
        QSet<qint64> ancestry;
        qint64 parentPid = process.ppid;
        while (parentPid > 0 && !ancestry.contains(parentPid)) {
            ancestry.insert(parentPid);
            const ProcessMemory* parent = processesByPid.value(parentPid, nullptr);
            if (parent == nullptr) {
                break;
            }
            visibleProcessPids.insert(parentPid);
            parentPid = parent->ppid;
        }
    }

    for (const ProcessMemory& process : snapshot.processes) {
        if (!visibleProcessPids.contains(process.pid)) {
            continue;
        }

        const bool isFilteredParent = !inScopePids.contains(process.pid);
        auto* item = acquireItem(processIdentity(process.pid, process.startTimeTicks));
        item->setText(0, process.name);
        item->setData(0, kProcessKindRole,
                      static_cast<int>(isFilteredParent ? ProcessItemKind::FilteredParent : ProcessItemKind::Process));
        item->setData(0, kProcessPidRole, process.pid);
        item->setData(0, kProcessStartTimeRole, numericData(process.startTimeTicks));
        item->setData(0, kProcessIdentityRole, processIdentity(process.pid, process.startTimeTicks));
        item->setData(0, kProcessSearchRole, isFilteredParent
                          ? QString()
                          : QStringList{process.name, QString::number(process.pid), process.userName,
                                        QString::number(process.uid), process.command, process.executable}.join(QLatin1Char('\n')));
        item->setToolTip(0, isFilteredParent
                                 ? QStringLiteral("PID %1  ·  用户 %2\n此父进程位于当前筛选范围之外，仅用于保留父子关系。")
                                       .arg(process.pid)
                                       .arg(process.userName)
                                 : QStringLiteral("%1\nPID %2  ·  PPID %3\n%4%5")
                                       .arg(process.name)
                                       .arg(process.pid)
                                       .arg(process.ppid)
                                       .arg(process.command)
                                       .arg(process.pssExact ? QString() : QStringLiteral("\nPSS 为近似值，未用于告警")));
        setNumericItem(item, 1, QString::number(process.pid), static_cast<std::uint64_t>(process.pid));
        if (isFilteredParent) {
            item->setText(2, QString());
            item->setText(3, QString());
            item->setText(4, QString());
            item->setData(2, kMemoryShareRole, 0.0);
            item->setToolTip(2, QString());
            item->setToolTip(3, QString());
            item->setToolTip(4, QString());
        } else {
            const bool footprintExact = process.pssExact && process.swapPssExact;
            setNumericItem(item, 2, formatMemoryValue(process.pssBytes, process.pssExact), process.pssBytes);
            setNumericItem(item, 3, formatMemoryValue(process.swapPssBytes, process.swapPssExact), process.swapPssBytes);
            setNumericItem(item, 4, formatMemoryValue(process.footprintBytes(), footprintExact), process.footprintBytes());
            item->setData(2, kMemoryShareRole, snapshot.memoryInfoComplete && snapshot.memoryTotalBytes > 0
                              ? static_cast<double>(process.pssBytes) / static_cast<double>(snapshot.memoryTotalBytes) : 0.0);
            item->setToolTip(2, QStringLiteral("%1物理内存的 %2；条形长度表示占物理内存的比例。\n%3")
                                   .arg(process.pssExact ? QStringLiteral("占") : QStringLiteral("约占"),
                                        formatPercent(process.pssBytes, snapshot.memoryTotalBytes),
                                        process.pssExact ? QStringLiteral("驻留 PSS，按比例分摊共享页。")
                                                         : QStringLiteral("≈ 无法读取精确 PSS，使用 RSS 近似值，共享页可能重复；此值不触发告警。")));
            item->setToolTip(3, process.swapPssExact
                                   ? QStringLiteral("SwapPss：按共享比例分摊的逻辑交换量，不是压缩后 RAM。")
                                   : QStringLiteral("≈ 无法读取 SwapPss，使用 Swap 或 VmSwap 近似值，共享交换页可能重复。"));
            item->setToolTip(4, footprintExact
                                   ? QStringLiteral("驻留 PSS + 逻辑 SwapPss，不代表压缩后的实际 RAM。")
                                   : QStringLiteral("≈ 驻留与交换量之和包含近似采样，共享页可能重复。"));
        }
        item->setText(5, process.userName.isEmpty()
                             ? QStringLiteral("UID %1").arg(process.uid)
                             : process.userName);
        item->setToolTip(5, process.command);
        const bool highMemory = !isFilteredParent && snapshot.memoryInfoComplete && process.pssExact
            && exceedsWarningThreshold(process.residentBytes(), snapshot.memoryTotalBytes,
                                       static_cast<unsigned>(processWarningPercent_));
        const QBrush foreground = isFilteredParent
            ? QBrush(processTree_->palette().color(QPalette::PlaceholderText))
            : highMemory ? QBrush(processTree_->palette().color(QPalette::BrightText)) : QBrush();
        item->setForeground(2, foreground);
        item->setForeground(0, foreground);
        itemsByPid.insert(process.pid, item);
    }

    QHash<qint64, QString> appKeyByWindowPid;
    QHash<QString, QString> appNameByKey;
    QHash<QString, QVector<WindowInfo>> windowsByApp;
    const auto appKeyForWindow = [&processesByPid](const WindowInfo& window) {
        const ProcessMemory* process = processesByPid.value(window.pid, nullptr);
        if (process != nullptr) {
            return QStringLiteral("window-process:%1").arg(processIdentity(process->pid, process->startTimeTicks));
        }
        return QStringLiteral("pid:%1").arg(window.pid);
    };

    for (const WindowInfo& window : windows_) {
        if (!inScopePids.contains(window.pid) || !itemsByPid.contains(window.pid)) {
            continue;
        }
        const QString appKey = appKeyByWindowPid.value(window.pid, appKeyForWindow(window));
        appKeyByWindowPid.insert(window.pid, appKey);
        windowsByApp[appKey].push_back(window);
        if (!appNameByKey.contains(appKey)) {
            const ProcessMemory* process = processesByPid.value(window.pid, nullptr);
            const QString name = process == nullptr || process->name.isEmpty()
                ? (window.appId.isEmpty() ? QStringLiteral("应用") : window.appId)
                : process->name;
            appNameByKey.insert(appKey, name);
        }
    }

    QHash<QString, QSet<qint64>> processPidsByApp;
    QHash<qint64, QString> processAppKey;
    for (const ProcessMemory& process : snapshot.processes) {
        if (!inScopePids.contains(process.pid)) {
            continue;
        }

        QString appKey;
        QSet<qint64> visited;
        qint64 currentPid = process.pid;
        while (currentPid > 0 && !visited.contains(currentPid)) {
            visited.insert(currentPid);
            if (appKeyByWindowPid.contains(currentPid)) {
                appKey = appKeyByWindowPid.value(currentPid);
                break;
            }
            const ProcessMemory* current = processesByPid.value(currentPid, nullptr);
            if (current == nullptr) {
                break;
            }
            currentPid = current->ppid;
        }
        if (!appKey.isEmpty()) {
            processPidsByApp[appKey].insert(process.pid);
            processAppKey.insert(process.pid, appKey);
        }
    }

    const auto saturatingAdd = [](std::uint64_t left, std::uint64_t right) {
        constexpr std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
        return right > maximum - left ? maximum : left + right;
    };

    QSet<qint64> appAssignedPids;
    QStringList appKeys = processPidsByApp.keys();
    std::sort(appKeys.begin(), appKeys.end(), [&appNameByKey](const QString& left, const QString& right) {
        return appNameByKey.value(left).localeAwareCompare(appNameByKey.value(right)) < 0;
    });
    for (const QString& appKey : appKeys) {
        const QSet<qint64>& appPids = processPidsByApp.value(appKey);
        const QVector<WindowInfo>& appWindows = windowsByApp.value(appKey);
        std::uint64_t pss = 0;
        std::uint64_t swapPss = 0;
        std::uint64_t footprint = 0;
        bool pssExact = true;
        bool swapPssExact = true;
        QStringList searchParts{appNameByKey.value(appKey), appKey};
        for (const qint64 pid : appPids) {
            const ProcessMemory* process = processesByPid.value(pid, nullptr);
            if (process == nullptr) {
                continue;
            }
            pss = saturatingAdd(pss, process->pssBytes);
            swapPss = saturatingAdd(swapPss, process->swapPssBytes);
            footprint = saturatingAdd(footprint, process->footprintBytes());
            pssExact = pssExact && process->pssExact;
            swapPssExact = swapPssExact && process->swapPssExact;
            searchParts << process->name << QString::number(process->pid) << process->command;
            appAssignedPids.insert(pid);
        }
        for (const WindowInfo& window : appWindows) {
            searchParts << window.title << window.appId << window.resourceName;
        }

        auto* applicationItem = acquireItem(QStringLiteral("application:%1").arg(appKey));
        const QString appName = appNameByKey.value(appKey, QStringLiteral("应用"));
        const QStringList windowTitles = [&appWindows]() {
            QStringList titles;
            titles.reserve(appWindows.size());
            for (const WindowInfo& window : appWindows) {
                titles.push_back(window.title);
            }
            return titles;
        }();
        const QString windowSummary = windowTitles.join(QStringLiteral(" / "));
        applicationItem->setText(0, windowSummary);
        applicationItem->setData(0, kProcessKindRole, static_cast<int>(ProcessItemKind::Application));
        applicationItem->setData(0, kProcessIdentityRole, QStringLiteral("application:%1").arg(appKey));
        applicationItem->setData(0, kProcessSearchRole, searchParts.join(QLatin1Char('\n')));
        applicationItem->setToolTip(0, QStringLiteral("%1\n窗口：%2\n%3 个进程\n组内内存为各进程采样值之和，展开查看明细。%4")
                                       .arg(appName)
                                       .arg(windowTitles.join(QStringLiteral("\n")))
                                       .arg(appPids.size())
                                       .arg(pssExact && swapPssExact ? QString()
                                                                   : QStringLiteral("\n≈ 表示组内含近似采样，共享页可能重复。")));
        const WindowInfo* representativeWindow = appWindows.isEmpty() ? nullptr : &appWindows.constFirst();
        const ProcessMemory* firstProcess = representativeWindow == nullptr ? nullptr
            : processesByPid.value(representativeWindow->pid, nullptr);
        applicationItem->setData(0, kApplicationOwnerPidRole, firstProcess == nullptr ? qint64{0} : firstProcess->pid);
        const QIcon appIcon = firstProcess != nullptr && firstProcess->pid == QCoreApplication::applicationPid()
            ? windowIcon() : resolveProcessIcon(firstProcess, representativeWindow);
        applicationItem->setIcon(0, appIcon);
        setNumericItem(applicationItem, 1, QStringLiteral("%1 个进程").arg(appPids.size()),
                       static_cast<std::uint64_t>(appPids.size()));
        setNumericItem(applicationItem, 2, formatMemoryValue(pss, pssExact), pss);
        applicationItem->setData(2, kMemoryShareRole, snapshot.memoryInfoComplete && snapshot.memoryTotalBytes > 0
                                   ? static_cast<double>(pss) / static_cast<double>(snapshot.memoryTotalBytes) : 0.0);
        applicationItem->setToolTip(2, QStringLiteral("组内进程合计占物理内存的 %1；条形长度表示占物理内存的比例。%2")
                                          .arg(formatPercent(pss, snapshot.memoryTotalBytes),
                                               pssExact ? QString()
                                                        : QStringLiteral("\n≈ 包含 RSS 近似值，共享页可能重复，展开查看各进程。")));
        setNumericItem(applicationItem, 3, formatMemoryValue(swapPss, swapPssExact), swapPss);
        setNumericItem(applicationItem, 4, formatMemoryValue(footprint, pssExact && swapPssExact), footprint);
        applicationItem->setToolTip(3, swapPssExact
                                          ? QStringLiteral("组内逻辑 SwapPss 之和，不是压缩后 RAM。")
                                          : QStringLiteral("≈ 组内交换量包含 Swap 或 VmSwap 近似值，共享页可能重复。"));
        applicationItem->setToolTip(4, pssExact && swapPssExact
                                          ? QStringLiteral("组内驻留 PSS + 逻辑 SwapPss 之和。")
                                          : QStringLiteral("≈ 组内驻留与交换量之和包含近似采样，共享页可能重复。"));
        applicationItem->setText(5, firstProcess == nullptr || firstProcess->userName.isEmpty()
                                     ? QString()
                                     : firstProcess->userName);
        attachItem(applicationItem, nullptr);
        if (newItems.contains(applicationItem)) {
            applicationItem->setExpanded(true);
        }

        for (const qint64 pid : appPids) {
            ProcessTreeItem* item = itemsByPid.value(pid, nullptr);
            if (item == nullptr) {
                continue;
            }
            const ProcessMemory* process = processesByPid.value(pid, nullptr);
            const bool parentInApp = process != nullptr && appPids.contains(process->ppid);
            ProcessTreeItem* parentItem = parentInApp ? itemsByPid.value(process->ppid, nullptr) : nullptr;
            if (parentItem != nullptr && parentItem != item) {
                attachItem(item, parentItem);
            } else {
                attachItem(item, applicationItem);
            }
            item->setIcon(0, QIcon());
        }
    }

    QSet<qint64> backgroundPids;
    for (const qint64 pid : visibleProcessPids) {
        if (!appAssignedPids.contains(pid)) {
            backgroundPids.insert(pid);
        }
    }
    for (const qint64 pid : backgroundPids) {
        ProcessTreeItem* item = itemsByPid.value(pid, nullptr);
        if (item == nullptr) {
            continue;
        }
        const ProcessMemory* process = processesByPid.value(pid, nullptr);
        const bool parentInBackground = process != nullptr && backgroundPids.contains(process->ppid);
        ProcessTreeItem* parentItem = parentInBackground ? itemsByPid.value(process->ppid, nullptr) : nullptr;
        if (parentItem != nullptr && parentItem != item) {
            attachItem(item, parentItem);
            item->setIcon(0, QIcon());
        } else {
            const QIcon processIcon = resolveProcessIcon(process, nullptr);
            item->setIcon(0, processIcon);
            attachItem(item, nullptr);
            if (newItems.contains(item) && !hadTree) {
                item->setExpanded(true);
            }
        }
    }

    QList<QTreeWidgetItem*> removedItems;
    for (auto iterator = processItems_.begin(); iterator != processItems_.end();) {
        if (liveIdentities.contains(iterator.key())) {
            ++iterator;
            continue;
        }
        QTreeWidgetItem* item = iterator.value();
        if (QTreeWidgetItem* parent = item->parent()) {
            parent->takeChild(parent->indexOfChild(item));
        } else if (item->treeWidget() == processTree_) {
            processTree_->takeTopLevelItem(processTree_->indexOfTopLevelItem(item));
        }
        removedItems.push_back(item);
        iterator = processItems_.erase(iterator);
    }
    qDeleteAll(removedItems);
    if (!currentIdentity.isEmpty() && !liveIdentities.contains(currentIdentity)) {
        processTree_->setCurrentItem(nullptr);
    }
    if (!selectedIdentity.isEmpty() && !liveIdentities.contains(selectedIdentity)) {
        processTree_->clearSelection();
    }

    if (restoreAfterSearch) {
        for (auto iterator = expansionBeforeSearch_.cbegin(); iterator != expansionBeforeSearch_.cend(); ++iterator) {
            if (QTreeWidgetItem* item = processItems_.value(iterator.key(), nullptr)) {
                item->setExpanded(iterator.value());
            }
        }
        expansionBeforeSearch_.clear();
    }
    QSet<QString> titleMatchedApps;
    QSet<QString> titleMatchedIdentities;
    if (!filter.isEmpty()) {
        for (const WindowInfo& window : windows_) {
            if (inScopePids.contains(window.pid)
                && QStringList{window.title, window.appId, window.resourceName}
                       .join(QLatin1Char('\n')).contains(filter, Qt::CaseInsensitive)) {
                const QString appKey = appKeyByWindowPid.value(window.pid);
                if (!appKey.isEmpty()) {
                    titleMatchedApps.insert(appKey);
                    titleMatchedIdentities.insert(QStringLiteral("application:%1").arg(appKey));
                }
            }
        }
    }
    const auto applyVisibility = [&filter, &titleMatchedIdentities](QTreeWidgetItem* item, bool inheritedMatch, auto&& self) -> bool {
        const bool matchesApplication = item->data(0, kProcessKindRole).toInt() == static_cast<int>(ProcessItemKind::Application)
            && titleMatchedIdentities.contains(item->data(0, kProcessIdentityRole).toString());
        bool descendantMatches = false;
        for (int index = 0; index < item->childCount(); ++index) {
            descendantMatches = self(item->child(index), inheritedMatch || matchesApplication, self) || descendantMatches;
        }
        const bool ownMatch = inheritedMatch || matchesApplication || filter.isEmpty()
            || item->data(0, kProcessSearchRole).toString().contains(filter, Qt::CaseInsensitive);
        const bool visible = ownMatch || descendantMatches;
        item->setHidden(!visible);
        if (!filter.isEmpty() && descendantMatches) {
            item->setExpanded(true);
        }
        return visible;
    };
    for (int index = 0; index < processTree_->topLevelItemCount(); ++index) {
        applyVisibility(processTree_->topLevelItem(index), false, applyVisibility);
    }

    processTree_->setSortingEnabled(true);

    processTree_->doItemsLayout();
    if (restoreAfterSearch) {
        if (QTreeWidgetItem* item = processItems_.value(scrollAnchorBeforeSearch_, nullptr)) {
            processTree_->scrollToItem(item, QAbstractItemView::PositionAtTop);
            processTree_->verticalScrollBar()->setValue(processTree_->verticalScrollBar()->value() - scrollOffsetBeforeSearch_);
        } else {
            processTree_->verticalScrollBar()->setValue(scrollPositionBeforeSearch_);
        }
        scrollAnchorBeforeSearch_.clear();
    } else if (scrollAnchor.isValid()) {
        processTree_->scrollTo(scrollAnchor, QAbstractItemView::PositionAtTop);
        processTree_->verticalScrollBar()->setValue(processTree_->verticalScrollBar()->value() - scrollAnchorOffset);
    } else {
        processTree_->verticalScrollBar()->setValue(scrollPosition);
    }
    processTree_->setUpdatesEnabled(true);
    processTree_->viewport()->update();
    updatingProcessTree_ = false;

    int scopeCount = 0;
    int matchCount = 0;
    QSet<QString> matchedApps;
    for (const ProcessMemory& process : snapshot.processes) {
        if (!processInScope(process, scope, currentUid)) {
            continue;
        }
        ++scopeCount;
        const bool matches = filter.isEmpty() || titleMatchedApps.contains(processAppKey.value(process.pid))
            || QStringList{process.name, QString::number(process.pid), process.userName,
                           QString::number(process.uid), process.command, process.executable}
                   .join(QLatin1Char('\n')).contains(filter, Qt::CaseInsensitive);
        if (matches) {
            ++matchCount;
            if (processAppKey.contains(process.pid)) {
                matchedApps.insert(processAppKey.value(process.pid));
            }
        }
    }
    tableSummaryLabel_->setText(filter.isEmpty()
                                    ? QStringLiteral("%1 个进程 · %2 个应用").arg(scopeCount).arg(processPidsByApp.size())
                                    : QStringLiteral("匹配 %1 个进程 · %2 个应用").arg(matchCount).arg(matchedApps.size()));
    const QStringList scopeNames{QStringLiteral("我的进程"), QStringLiteral("所有进程"), QStringLiteral("其他用户 / 系统")};
    processScopeLabel_->setText(scopeNames.value(scope));
    const bool empty = matchCount == 0 && matchedApps.isEmpty();
    processEmptyLabel_->setText(filter.isEmpty()
                                   ? QStringLiteral("当前范围内没有可显示的进程\n可在设置中调整进程范围")
                                   : QStringLiteral("没有匹配的应用或进程\n换个关键词，或按 Esc 清空搜索"));
    processEmptyLabel_->setVisible(empty);
}

void MainWindow::scheduleProcessTreeUpdate()
{
    if (!hasSnapshot_ || processTree_ == nullptr) {
        return;
    }
    processTreeUpdatePending_ = true;
    if (processTreeIdleTimer_->isActive() || processTree_->verticalScrollBar()->isSliderDown()) {
        return;
    }
    processTreeUpdatePending_ = false;
    updateProcessTree(latestSnapshot_);
}

void MainWindow::updateZramTable(const MemorySnapshot& snapshot)
{
    zramTable_->setUpdatesEnabled(false);
    if (snapshot.zramDevices.isEmpty()) {
        if (zramTable_->columnSpan(0, 0) == 1) {
            zramTable_->clearContents();
        }
        zramTable_->setRowCount(1);
        setTextItem(zramTable_, 0, 0, QStringLiteral("未检测到 zram 设备"));
        zramTable_->setSpan(0, 0, 1, zramTable_->columnCount());
        zramSummaryLabel_->setText(QStringLiteral("没有设备"));
        zramTable_->setUpdatesEnabled(true);
        return;
    }

    QString selectedDevice;
    if (zramTable_->currentRow() >= 0 && zramTable_->item(zramTable_->currentRow(), 0) != nullptr) {
        selectedDevice = zramTable_->item(zramTable_->currentRow(), 0)->text();
    }
    if (zramTable_->columnSpan(0, 0) > 1) {
        zramTable_->clearSpans();
        zramTable_->clearContents();
    }
    const int rowCount = static_cast<int>(snapshot.zramDevices.size());
    zramTable_->setRowCount(rowCount);
    int readable = 0;
    int selectedRow = -1;
    for (int row = 0; row < rowCount; ++row) {
        const ZramDevice& device = snapshot.zramDevices.at(row);
        if (device.name == selectedDevice) {
            selectedRow = row;
        }
        setTextItem(zramTable_, row, 0, device.name);
        const QString writeFailures = device.ioStatisticsReadable
            ? QString::number(device.failedWrites) : QStringLiteral("不可读");
        const QString memoryLimit = !device.statisticsReadable ? QStringLiteral("不可读")
            : device.memoryLimitBytes == 0 ? QStringLiteral("未设置") : formatBytes(device.memoryLimitBytes);
        zramTable_->item(row, 0)->setToolTip(QStringLiteral("设备累计写失败：%1\nzram RAM 限额：%2\n历史写失败计数不代表当前仍在失败；RAM 限额与设备逻辑容量不同。")
                                              .arg(writeFailures, memoryLimit));
        setTextItem(zramTable_, row, 1, device.compressionAlgorithm.isEmpty()
                       ? QStringLiteral("不可读") : device.compressionAlgorithm);
        if (device.diskSizeBytes > 0) {
            setNumericItem(zramTable_, row, 2, formatBytes(device.diskSizeBytes), device.diskSizeBytes);
        } else {
            setTextItem(zramTable_, row, 2, QStringLiteral("不可读"));
        }
        if (!device.statisticsReadable) {
            for (int column = 3; column <= 5; ++column) {
                setTextItem(zramTable_, row, column, QStringLiteral("不可读"));
            }
            setTextItem(zramTable_, row, 6, QStringLiteral("--"));
            zramTable_->item(row, 6)->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            continue;
        }

        ++readable;
        setNumericItem(zramTable_, row, 3, formatBytes(device.originalDataBytes), device.originalDataBytes);
        setNumericItem(zramTable_, row, 4, formatBytes(device.compressedDataBytes), device.compressedDataBytes);
        setNumericItem(zramTable_, row, 5, formatBytes(device.memoryUsedBytes), device.memoryUsedBytes);
        const QString ratio = device.compressedDataBytes == 0
            ? QStringLiteral("--")
            : QStringLiteral("%1×").arg(QString::number(static_cast<double>(device.originalDataBytes)
                                                         / static_cast<double>(device.compressedDataBytes), 'f', 2));
        setTextItem(zramTable_, row, 6, ratio);
        zramTable_->item(row, 6)->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    }
    if (selectedRow >= 0) {
        if (zramTable_->currentRow() != selectedRow) {
            zramTable_->setCurrentCell(selectedRow, 0);
        }
    } else if (!selectedDevice.isEmpty()) {
        zramTable_->setCurrentItem(nullptr);
        zramTable_->clearSelection();
    }
    zramSummaryLabel_->setText(QStringLiteral("%1/%2 个设备可读").arg(readable).arg(snapshot.zramDevices.size()));
    zramTable_->setUpdatesEnabled(true);
}

void MainWindow::applyProcessFilter()
{
    if (hasSnapshot_) {
        updateProcessTree(latestSnapshot_);
    }
}

void MainWindow::showProcessContextMenu(const QPoint& position)
{
    QTreeWidgetItem* clickedItem = processTree_->itemAt(position);
    if (clickedItem == nullptr) {
        return;
    }

    const int clickedKind = clickedItem->data(0, kProcessKindRole).toInt();
    if (clickedKind == static_cast<int>(ProcessItemKind::FilteredParent)) {
        return;
    }
    QTreeWidgetItem* item = clickedItem;
    while (item != nullptr
           && item->data(0, kProcessKindRole).toInt() != static_cast<int>(ProcessItemKind::Process)) {
        item = item->parent();
    }
    const bool isApplication = clickedKind == static_cast<int>(ProcessItemKind::Application);
    if (item == nullptr && (isApplication || clickedKind == static_cast<int>(ProcessItemKind::Window))) {
        const qint64 windowPid = clickedItem->data(0, isApplication ? kApplicationOwnerPidRole : kProcessPidRole).toLongLong();
        const auto findProcess = [windowPid](QTreeWidgetItem* candidate, auto&& self) -> QTreeWidgetItem* {
            if (candidate->data(0, kProcessKindRole).toInt() == static_cast<int>(ProcessItemKind::Process)
                && candidate->data(0, kProcessPidRole).toLongLong() == windowPid) {
                return candidate;
            }
            for (int index = 0; index < candidate->childCount(); ++index) {
                if (QTreeWidgetItem* result = self(candidate->child(index), self); result != nullptr) {
                    return result;
                }
            }
            return nullptr;
        };
        for (int index = 0; index < processTree_->topLevelItemCount() && item == nullptr; ++index) {
            item = findProcess(processTree_->topLevelItem(index), findProcess);
        }
    }
    if (item == nullptr) {
        return;
    }

    const qint64 pid = item->data(0, kProcessPidRole).toLongLong();
    const std::uint64_t startTimeTicks = item->data(0, kProcessStartTimeRole).toULongLong();
    processTree_->setCurrentItem(clickedItem);
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("processContextMenu"));
    const QIcon stopIcon = QIcon::fromTheme(QStringLiteral("process-stop"),
                                          style()->standardIcon(QStyle::SP_BrowserStop));
    if (isApplication) {
        QAction* terminateAppAction = menu.addAction(stopIcon, QStringLiteral("结束应用"));
        menu.addSeparator();
        QAction* forceAppAction = menu.addAction(QStringLiteral("强制结束应用"));
        QAction* selected = menu.exec(processTree_->viewport()->mapToGlobal(position));
        if (selected == terminateAppAction || selected == forceAppAction) {
            terminateProcessItem(pid, startTimeTicks, true, selected == forceAppAction);
        }
        return;
    }
    const QString targetName = clickedKind == static_cast<int>(ProcessItemKind::Window)
        ? QStringLiteral("所属进程")
        : QStringLiteral("进程");
    QAction* terminateAction = menu.addAction(stopIcon, QStringLiteral("结束%1").arg(targetName));
    QAction* terminateTreeAction = menu.addAction(QStringLiteral("结束%1及子进程").arg(targetName));
    menu.addSeparator();
    QAction* forceAction = menu.addAction(QStringLiteral("强制结束%1").arg(targetName));
    QAction* forceTreeAction = menu.addAction(QStringLiteral("强制结束%1及子进程").arg(targetName));
    QAction* selected = menu.exec(processTree_->viewport()->mapToGlobal(position));
    if (selected == terminateAction) {
        terminateProcessItem(pid, startTimeTicks, false, false);
    } else if (selected == terminateTreeAction) {
        terminateProcessItem(pid, startTimeTicks, true, false);
    } else if (selected == forceAction) {
        terminateProcessItem(pid, startTimeTicks, false, true);
    } else if (selected == forceTreeAction) {
        terminateProcessItem(pid, startTimeTicks, true, true);
    }
}

void MainWindow::terminateProcessItem(qint64 rootPid,
                                      std::uint64_t rootStartTime,
                                      bool includeChildren,
                                      bool force)
{
    ProcessMemory rootProcess;
    bool rootFound = false;
    for (const ProcessMemory& process : latestSnapshot_.processes) {
        if (process.pid == rootPid && process.startTimeTicks == rootStartTime) {
            rootProcess = process;
            rootFound = true;
            break;
        }
    }
    if (!rootFound) {
        QMessageBox::warning(this, QStringLiteral("进程已变化"),
                             QStringLiteral("该进程已退出或 PID 已被复用，请等待刷新后重试。"));
        return;
    }

    struct ProcessTarget final {
        qint64 pid = 0;
        std::uint64_t startTimeTicks = 0;
    };
    QVector<ProcessTarget> targets;
    QSet<qint64> visited;
    const auto appendProcessTree = [this, includeChildren, &targets, &visited](const ProcessMemory& process, auto&& self) -> void {
        if (visited.contains(process.pid)) {
            return;
        }
        visited.insert(process.pid);
        if (includeChildren) {
            for (const ProcessMemory& candidate : latestSnapshot_.processes) {
                if (candidate.ppid == process.pid) {
                    self(candidate, self);
                }
            }
        }
        targets.push_back({process.pid, process.startTimeTicks});
    };
    appendProcessTree(rootProcess, appendProcessTree);

    const QString signalName = force ? QStringLiteral("SIGKILL") : QStringLiteral("SIGTERM");
    const QString scope = includeChildren
        ? QStringLiteral("进程及其 %1 个子进程").arg(targets.size() - 1)
        : QStringLiteral("该进程");
    const QString question = QStringLiteral("结束 %1（PID %2）？\n将向%3发送 %4。未保存的数据可能丢失。")
                                 .arg(rootProcess.name).arg(rootPid).arg(scope, signalName);
    const QMessageBox::StandardButton answer = QMessageBox::warning(
        this, QStringLiteral("确认结束任务"), question,
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) {
        return;
    }

    const int signal = force ? SIGKILL : SIGTERM;
    QStringList errors;
    int sent = 0;
    for (const ProcessTarget& process : targets) {
        QString error;
        if (sendSignalToProcess(process.pid, process.startTimeTicks, signal, error)) {
            ++sent;
        } else {
            errors.push_back(error);
        }
    }

    QString result = QStringLiteral("已发送 %1 个信号。向子进程发送信号后，父进程可能会重启或重新创建它们。")
                         .arg(sent);
    if (!errors.isEmpty()) {
        result += QStringLiteral("\n\n未能发送：\n%1").arg(errors.mid(0, 8).join(QLatin1Char('\n')));
        if (errors.size() > 8) {
            result += QStringLiteral("\n另有 %1 个错误未显示。此进程可能需要更高权限。")
                          .arg(errors.size() - 8);
        }
    }
    QMessageBox::information(this, QStringLiteral("结束任务"), result);
    QTimer::singleShot(250, this, &MainWindow::requestRefresh);
}

void MainWindow::checkWarnings(const MemorySnapshot& snapshot)
{
    if (!notificationsEnabled_ || !snapshot.memoryInfoReadable || !snapshot.memoryInfoComplete
        || snapshot.memoryTotalBytes == 0 || snapshot.memoryAvailableBytes > snapshot.memoryTotalBytes) {
        return;
    }

    QSet<QString> activeHighProcesses;
    QStringList newWarnings;
    for (const ProcessMemory& process : snapshot.processes) {
        const QString identity = processIdentity(process.pid, process.startTimeTicks);
        if (!process.pssExact) {
            if (warnedProcesses_.contains(identity)) {
                activeHighProcesses.insert(identity);
            }
            continue;
        }
        if (!exceedsWarningThreshold(process.residentBytes(), snapshot.memoryTotalBytes,
                                     static_cast<unsigned>(processWarningPercent_))) {
            continue;
        }

        activeHighProcesses.insert(identity);
        if (warnedProcesses_.contains(identity)) {
            continue;
        }
        warnedProcesses_.insert(identity);
        newWarnings.push_back(QStringLiteral("%1 (PID %2) 的精确驻留 PSS 为 %3，已高于物理内存的 %4%。")
                                   .arg(process.name)
                                   .arg(process.pid)
                                   .arg(formatBytes(process.residentBytes()))
                                   .arg(processWarningPercent_));
    }

    for (auto iterator = warnedProcesses_.begin(); iterator != warnedProcesses_.end();) {
        if (!activeHighProcesses.contains(*iterator)) {
            iterator = warnedProcesses_.erase(iterator);
        } else {
            ++iterator;
        }
    }

    const bool lowAvailable = availableMemoryWarningEnabled_
        && (availableMemoryWarningMode_ == 0
                ? belowAvailableThreshold(snapshot.memoryAvailableBytes, snapshot.memoryTotalBytes,
                                          static_cast<unsigned>(availableMemoryWarningPercent_))
                : snapshot.memoryAvailableBytes
                    < static_cast<std::uint64_t>(availableMemoryWarningMiB_) * 1024U * 1024U);
    if (lowAvailable && !availableMemoryWarningActive_) {
        const QString threshold = availableMemoryWarningMode_ == 0
            ? QStringLiteral("物理内存的 %1%").arg(availableMemoryWarningPercent_)
            : QStringLiteral("%1 MiB").arg(availableMemoryWarningMiB_);
        newWarnings.push_back(QStringLiteral("系统估算可用内存（MemAvailable）为 %1，已低于%2。此值包含内核估算可回收的内存，不与 zram RAM 再次相加减。")
                                  .arg(formatBytes(snapshot.memoryAvailableBytes), threshold));
    }
    availableMemoryWarningActive_ = lowAvailable;

    if (!newWarnings.isEmpty()) {
        showWarningNotification(QStringLiteral("MemMonitor 实际内存警告"), newWarnings.join(QStringLiteral("\n")));
    }
}

void MainWindow::showWarningNotification(const QString& title, const QString& message)
{
    if (trayIcon_ != nullptr && QSystemTrayIcon::supportsMessages()) {
        trayIcon_->showMessage(title, message, QSystemTrayIcon::Warning, 9000);
        return;
    }

    QDBusInterface notifications(QStringLiteral("org.freedesktop.Notifications"),
                                 QStringLiteral("/org/freedesktop/Notifications"),
                                 QStringLiteral("org.freedesktop.Notifications"),
                                 QDBusConnection::sessionBus());
    if (notifications.isValid()) {
        const QVariantMap hints{{QStringLiteral("urgency"), QVariant::fromValue<uchar>(2)}};
        notifications.asyncCall(QStringLiteral("Notify"), QStringLiteral("MemMonitor"), uint(0), QString(),
                                title, message.toHtmlEscaped(), QStringList{}, hints, 9000);
    }
}

void MainWindow::showSettings()
{
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("settingsDialog"));
    dialog.setWindowTitle(QStringLiteral("设置"));
    dialog.setMinimumWidth(380);

    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    auto* refreshInterval = new QSpinBox(&dialog);
    refreshInterval->setObjectName(QStringLiteral("refreshIntervalSetting"));
    refreshInterval->setRange(500, 10000);
    refreshInterval->setSingleStep(500);
    refreshInterval->setSuffix(QStringLiteral(" ms"));
    refreshInterval->setValue(refreshIntervalMs_);
    form->addRow(QStringLiteral("刷新间隔"), refreshInterval);

    auto* processScope = new QComboBox(&dialog);
    processScope->setObjectName(QStringLiteral("defaultProcessScopeSetting"));
    processScope->addItem(QStringLiteral("我的进程"), 0);
    processScope->addItem(QStringLiteral("所有进程"), 1);
    processScope->addItem(QStringLiteral("其他用户 / 系统"), 2);
    processScope->setCurrentIndex(defaultProcessScope_);
    form->addRow(QStringLiteral("进程范围"), processScope);

    auto* notifications = new QCheckBox(QStringLiteral("启用内存告警通知"), &dialog);
    notifications->setObjectName(QStringLiteral("notificationsSetting"));
    notifications->setToolTip(QStringLiteral("控制进程 PSS 和估算可用内存告警；恢复后再次越过阈值会重新通知。"));
    notifications->setChecked(notificationsEnabled_);
    form->addRow(QString(), notifications);

    auto* processWarning = new QSpinBox(&dialog);
    processWarning->setObjectName(QStringLiteral("processWarningPercentSetting"));
    processWarning->setRange(1, 100);
    processWarning->setSuffix(QStringLiteral(" %"));
    processWarning->setValue(processWarningPercent_);
    processWarning->setToolTip(QStringLiteral("单进程精确驻留 PSS 严格高于此比例时通知并标色；SwapPss 和近似 RSS 不触发告警。"));
    form->addRow(QStringLiteral("进程 PSS 高于"), processWarning);

    auto* availableWarning = new QCheckBox(QStringLiteral("估算可用内存不足时通知"), &dialog);
    availableWarning->setObjectName(QStringLiteral("availableMemoryWarningSetting"));
    availableWarning->setChecked(availableMemoryWarningEnabled_);
    form->addRow(QString(), availableWarning);

    auto* availableThreshold = new QWidget(&dialog);
    auto* availableLayout = new QHBoxLayout(availableThreshold);
    availableLayout->setContentsMargins(0, 0, 0, 0);
    auto* availablePercent = new QSpinBox(availableThreshold);
    availablePercent->setObjectName(QStringLiteral("availableMemoryWarningPercentSetting"));
    availablePercent->setRange(1, 100);
    availablePercent->setSuffix(QStringLiteral(" %"));
    availablePercent->setValue(availableMemoryWarningPercent_);
    auto* availableMiB = new QSpinBox(availableThreshold);
    availableMiB->setObjectName(QStringLiteral("availableMemoryWarningMiBSetting"));
    availableMiB->setRange(1, 1048576);
    availableMiB->setSuffix(QStringLiteral(" MiB"));
    availableMiB->setValue(availableMemoryWarningMiB_);
    auto* availableMode = new QComboBox(availableThreshold);
    availableMode->setObjectName(QStringLiteral("availableMemoryWarningModeSetting"));
    availableMode->addItem(QStringLiteral("百分比"), 0);
    availableMode->addItem(QStringLiteral("MiB"), 1);
    availableMode->setCurrentIndex(availableMemoryWarningMode_);
    availableLayout->addWidget(availablePercent, 1);
    availableLayout->addWidget(availableMiB, 1);
    availableLayout->addWidget(availableMode);
    availableThreshold->setToolTip(QStringLiteral("有效完整采样的 MemAvailable 严格低于阈值时通知，不与 zram RAM 相加减；恢复到阈值后重新触发。"));
    form->addRow(QStringLiteral("估算可用低于"), availableThreshold);

    const auto updateWarningControls = [notifications, processWarning, availableWarning, availableThreshold,
                                        availableMode, availablePercent, availableMiB]() {
        processWarning->setEnabled(notifications->isChecked());
        availableWarning->setEnabled(notifications->isChecked());
        availableThreshold->setEnabled(notifications->isChecked() && availableWarning->isChecked());
        availablePercent->setVisible(availableMode->currentData().toInt() == 0);
        availableMiB->setVisible(availableMode->currentData().toInt() == 1);
    };
    connect(notifications, &QCheckBox::toggled, &dialog, updateWarningControls);
    connect(availableWarning, &QCheckBox::toggled, &dialog, updateWarningControls);
    connect(availableMode, &QComboBox::currentIndexChanged, &dialog, updateWarningControls);
    updateWarningControls();

    auto* closeToTray = new QCheckBox(QStringLiteral("关闭窗口时最小化到托盘"), &dialog);
    closeToTray->setObjectName(QStringLiteral("closeToTraySetting"));
    closeToTray->setChecked(closeToTray_);
    closeToTray->setEnabled(trayIcon_ != nullptr);
    if (trayIcon_ == nullptr) {
        closeToTray->setToolTip(QStringLiteral("当前桌面环境未提供系统托盘"));
    }
    form->addRow(QString(), closeToTray);
    layout->addLayout(form);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("保存"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    auto* testNotification = buttons->addButton(QStringLiteral("测试通知"), QDialogButtonBox::ActionRole);
    testNotification->setObjectName(QStringLiteral("testNotificationButton"));
    connect(testNotification, &QPushButton::clicked, &dialog, [this]() {
        showWarningNotification(QStringLiteral("MemMonitor 测试通知"),
                                QStringLiteral("这是一条测试通知，用于确认桌面通知能否显示，不表示当前内存不足。"));
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    refreshIntervalMs_ = refreshInterval->value();
    const bool scopeChanged = defaultProcessScope_ != processScope->currentData().toInt();
    defaultProcessScope_ = processScope->currentData().toInt();
    const bool processWarningChanged = processWarningPercent_ != processWarning->value();
    const bool warningSettingsChanged = processWarningChanged
        || notificationsEnabled_ != notifications->isChecked()
        || availableMemoryWarningEnabled_ != availableWarning->isChecked()
        || availableMemoryWarningMode_ != availableMode->currentData().toInt()
        || availableMemoryWarningPercent_ != availablePercent->value()
        || availableMemoryWarningMiB_ != availableMiB->value();
    if (warningSettingsChanged) {
        warnedProcesses_.clear();
        availableMemoryWarningActive_ = false;
    }
    notificationsEnabled_ = notifications->isChecked();
    processWarningPercent_ = processWarning->value();
    availableMemoryWarningEnabled_ = availableWarning->isChecked();
    availableMemoryWarningMode_ = availableMode->currentData().toInt();
    availableMemoryWarningPercent_ = availablePercent->value();
    availableMemoryWarningMiB_ = availableMiB->value();
    closeToTray_ = closeToTray->isChecked();
    refreshTimer_->setInterval(refreshIntervalMs_);
    if (scopeChanged || processWarningChanged) {
        applyProcessFilter();
    }
    saveSettings();
    if (warningSettingsChanged && hasSnapshot_) {
        checkWarnings(latestSnapshot_);
    }
}

void MainWindow::loadSettings()
{
    QSettings settings(QStringLiteral("MemMonitor"), QStringLiteral("MemMonitor"));
    refreshIntervalMs_ = std::clamp(settings.value(QStringLiteral("refreshIntervalMs"), 1500).toInt(), 500, 10000);
    defaultProcessScope_ = settings.value(QStringLiteral("defaultProcessScope"), 0).toInt();
    if (defaultProcessScope_ < 0 || defaultProcessScope_ > 2) {
        defaultProcessScope_ = 0;
    }
    notificationsEnabled_ = settings.value(QStringLiteral("notificationsEnabled"), true).toBool();
    processWarningPercent_ = boundedSetting(settings, QStringLiteral("processWarningPercent"), 30, 1, 100);
    availableMemoryWarningEnabled_ = settings.value(QStringLiteral("availableMemoryWarningEnabled"), false).toBool();
    availableMemoryWarningMode_ = boundedSetting(settings, QStringLiteral("availableMemoryWarningMode"), 0, 0, 1);
    availableMemoryWarningPercent_ = boundedSetting(settings, QStringLiteral("availableMemoryWarningPercent"), 10, 1, 100);
    availableMemoryWarningMiB_ = boundedSetting(settings, QStringLiteral("availableMemoryWarningMiB"), 1024, 1, 1048576);
    closeToTray_ = settings.value(QStringLiteral("closeToTray"), true).toBool();
}

void MainWindow::saveSettings()
{
    QSettings settings(QStringLiteral("MemMonitor"), QStringLiteral("MemMonitor"));
    settings.setValue(QStringLiteral("refreshIntervalMs"), refreshIntervalMs_);
    settings.setValue(QStringLiteral("defaultProcessScope"), defaultProcessScope_);
    settings.setValue(QStringLiteral("notificationsEnabled"), notificationsEnabled_);
    settings.setValue(QStringLiteral("processWarningPercent"), processWarningPercent_);
    settings.setValue(QStringLiteral("availableMemoryWarningEnabled"), availableMemoryWarningEnabled_);
    settings.setValue(QStringLiteral("availableMemoryWarningMode"), availableMemoryWarningMode_);
    settings.setValue(QStringLiteral("availableMemoryWarningPercent"), availableMemoryWarningPercent_);
    settings.setValue(QStringLiteral("availableMemoryWarningMiB"), availableMemoryWarningMiB_);
    settings.setValue(QStringLiteral("closeToTray"), closeToTray_);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (closeToTray_ && trayIcon_ != nullptr) {
        hide();
        event->ignore();
        return;
    }
    event->accept();
    qApp->quit();
}

void MainWindow::showAbout()
{
    QMessageBox::about(this, QStringLiteral("关于 MemMonitor"),
                       QStringLiteral("MemMonitor %1\n\n"
                                      "使用 Linux /proc 读取进程 PSS、RSS 和 SwapPss，使用 zram mm_stat 读取压缩前后和实际内存占用。\n\n"
                                      "PSS 按共享页比例分摊；zram 的 mem_used_total 已包含在物理内存使用量中，不会重复相加。")
                           .arg(QCoreApplication::applicationVersion()));
}

} // namespace memonitor
