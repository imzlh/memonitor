#pragma once

#include <QObject>
#include <QString>
#include <QtGlobal>
#include <QVector>

#include <functional>

class QDBusVirtualObject;
class QTimer;

namespace memonitor {

struct WindowInfo {
    qint64 pid = 0;
    QString title;
    // Kept as the legacy grouping identifier for existing consumers. New
    // code should prefer the explicit KWin metadata below.
    QString appId;
    QString resourceName;
    QString desktopFileName;
    QString iconName;
    QString resourceClass;

    friend bool operator==(const WindowInfo&, const WindowInfo&) = default;
};

class WindowTracker final : public QObject {
public:
    using UpdateHandler = std::function<void(const QVector<WindowInfo>&)>;

    explicit WindowTracker(UpdateHandler handler, QObject* parent = nullptr);
    ~WindowTracker() override;

    [[nodiscard]] bool isAvailable() const;

private:
    void beginSetup();
    void scheduleRetry();
    void failSetup(quint64 attempt, const QString& reason);
    void removeScriptFile();

    UpdateHandler handler_;
    QDBusVirtualObject* receiver_ = nullptr;
    QTimer* retryTimer_ = nullptr;
    QTimer* updateTimer_ = nullptr;
    QString serviceName_;
    QString scriptName_;
    QString scriptPath_;
    QString scriptObjectPath_;
    int scriptId_ = -1;
    quint64 attempt_ = 0;
    bool loadRequested_ = false;
    bool serviceRegistered_ = false;
    bool scriptLoaded_ = false;
    bool updateReceived_ = false;
    bool stopping_ = false;
};

} // namespace memonitor
