#include "window_tracker.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVirtualObject>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QPointer>
#include <QTemporaryFile>
#include <QTimer>
#include <QVariant>
#include <QUuid>

#include <utility>

namespace memonitor {
namespace {

constexpr auto kWindowInterface = "org.memonitor.WindowList";
constexpr auto kScriptingInterface = "org.kde.kwin.Scripting";
constexpr auto kScriptInterface = "org.kde.kwin.Script";
constexpr int kDbusTimeoutMs = 5000;
constexpr int kScriptRunTimeoutMs = 15000;
constexpr int kRetryDelayMs = 2000;
constexpr int kFirstUpdateTimeoutMs = 7000;

Q_LOGGING_CATEGORY(windowTrackerLog, "memonitor.windowtracker")

QString jsonString(const QJsonObject& object, const QString& key)
{
    const QJsonValue value = object.value(key);
    return value.isString() ? value.toString().trimmed() : QString();
}

QDBusPendingCallWatcher* sendCall(const QDBusConnection& bus,
                                  const QString& path,
                                  const QString& interface,
                                  const QString& method,
                                  const QVariantList& arguments,
                                  QObject* parent,
                                  int timeoutMs = kDbusTimeoutMs)
{
    QDBusMessage message = QDBusMessage::createMethodCall(
        QStringLiteral("org.kde.KWin"), path, interface, method);
    message.setArguments(arguments);
    return new QDBusPendingCallWatcher(bus.asyncCall(message, timeoutMs), parent);
}

QDBusPendingCallWatcher* sendUnload(const QDBusConnection& bus,
                                    const QString& scriptName,
                                    QObject* parent)
{
    QDBusMessage message = QDBusMessage::createMethodCall(
        QStringLiteral("org.kde.KWin"),
        QStringLiteral("/Scripting"),
        QString::fromLatin1(kScriptingInterface),
        QStringLiteral("unloadScript"));
    message.setArguments({scriptName});
    auto* watcher = new QDBusPendingCallWatcher(bus.asyncCall(message, kDbusTimeoutMs), parent);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, watcher, &QObject::deleteLater);
    return watcher;
}

class WindowListReceiver final : public QDBusVirtualObject {
public:
    WindowListReceiver(WindowTracker::UpdateHandler handler, QObject* parent)
        : QDBusVirtualObject(parent)
        , handler_(std::move(handler))
    {
    }

    QString introspect(const QString&) const override
    {
        return QStringLiteral(
            "<node><interface name=\"org.memonitor.WindowList\">"
            "<method name=\"update\"><arg type=\"s\" direction=\"in\"/></method>"
            "</interface></node>");
    }

    bool handleMessage(const QDBusMessage& message, const QDBusConnection& connection) override
    {
        if (message.interface() != QString::fromLatin1(kWindowInterface)
            || message.member() != QStringLiteral("update")
            || message.arguments().size() != 1
            || !message.arguments().constFirst().canConvert<QString>()) {
            return false;
        }

        const QJsonDocument document = QJsonDocument::fromJson(message.arguments().constFirst().toString().toUtf8());
        if (!document.isArray()) {
            connection.send(message.createErrorReply(QDBusError::InvalidArgs, QStringLiteral("Expected a window array")));
            return true;
        }

        QVector<WindowInfo> windows;
        const QJsonArray entries = document.array();
        windows.reserve(entries.size());
        for (const QJsonValue& entry : entries) {
            if (!entry.isObject()) {
                continue;
            }
            const QJsonObject object = entry.toObject();
            bool pidOk = false;
            const qint64 pid = object.value(QStringLiteral("pid")).toVariant().toLongLong(&pidOk);
            const QString title = object.value(QStringLiteral("title")).toString().trimmed();
            if (!pidOk || pid <= 0 || title.isEmpty()) {
                continue;
            }

            // appId was the only application identifier in the original
            // protocol. Prefer the explicit KWin fields, but keep both sides
            // populated so older consumers and older scripts continue to
            // group windows in the same way.
            const QString legacyAppId = jsonString(object, QStringLiteral("appId"));
            QString resourceClass = jsonString(object, QStringLiteral("resourceClass"));
            if (resourceClass.isEmpty()) {
                resourceClass = legacyAppId;
            }
            QString appId = legacyAppId;
            if (appId.isEmpty()) {
                appId = resourceClass;
            }

            WindowInfo windowInfo;
            windowInfo.pid = pid;
            windowInfo.title = title;
            windowInfo.appId = appId;
            windowInfo.resourceName = jsonString(object, QStringLiteral("resourceName"));
            windowInfo.desktopFileName = jsonString(object, QStringLiteral("desktopFileName"));
            windowInfo.iconName = jsonString(object, QStringLiteral("iconName"));
            windowInfo.resourceClass = resourceClass;
            windows.push_back(std::move(windowInfo));
        }

        if (handler_) {
            handler_(windows);
        }
        if (message.isReplyRequired()) {
            connection.send(message.createReply());
        }
        return true;
    }

private:
    WindowTracker::UpdateHandler handler_;
};

QString windowScript(const QString& serviceName)
{
    return QStringLiteral(
        "var stringifyWindowValue = function(value) {\n"
        "    try {\n"
        "        return value === undefined || value === null ? \"\" : String(value);\n"
        "    } catch (error) {\n"
        "        return \"\";\n"
        "    }\n"
        "};\n"
        "var readIconName = function(window) {\n"
        "    try {\n"
        "        var iconName = window.iconName;\n"
        "        if (iconName !== undefined && iconName !== null) {\n"
        "            return stringifyWindowValue(iconName);\n"
        "        }\n"
        "        var icon = window.icon;\n"
        "        if (!icon) {\n"
        "            return \"\";\n"
        "        }\n"
        "        if (typeof icon.name === \"function\") {\n"
        "            return stringifyWindowValue(icon.name());\n"
        "        }\n"
        "        return stringifyWindowValue(icon.name);\n"
        "    } catch (error) {\n"
        "        return \"\";\n"
        "    }\n"
        "};\n"
        "var publishWindows = function() {\n"
        "    var windows = [];\n"
        "    var clients = workspace.windowList();\n"
        "    for (var i = 0; i < clients.length; ++i) {\n"
        "        var window = clients[i];\n"
        "        if (!window || window.pid <= 0 || window.skipTaskbar || !window.caption) {\n"
        "            continue;\n"
        "        }\n"
        "        var resourceClass = stringifyWindowValue(window.resourceClass);\n"
        "        var resourceName = stringifyWindowValue(window.resourceName);\n"
        "        var desktopFileName = stringifyWindowValue(window.desktopFileName);\n"
        "        var iconName = readIconName(window);\n"
        "        windows.push({ pid: window.pid, title: String(window.caption), "
        "appId: resourceClass, "
        "resourceClass: resourceClass, "
        "resourceName: resourceName, "
        "desktopFileName: desktopFileName, "
        "iconName: iconName });\n"
        "    }\n"
        "    callDBus(\"%1\", \"/WindowList\", \"org.memonitor.WindowList\", \"update\", JSON.stringify(windows));\n"
        "};\n"
        "var publishTimer = new QTimer();\n"
        "publishTimer.interval = 2000;\n"
        "publishTimer.timeout.connect(publishWindows);\n"
        "publishTimer.start();\n"
        "publishWindows();\n")
        .arg(serviceName);
}

} // namespace

WindowTracker::WindowTracker(UpdateHandler handler, QObject* parent)
    : QObject(parent)
    , handler_(std::move(handler))
{
    const QString suffix = QString::number(QCoreApplication::applicationPid());
    serviceName_ = QStringLiteral("org.memonitor.MemMonitor.p%1.%2")
                       .arg(suffix, QStringLiteral("x%1").arg(QUuid::createUuid().toString(QUuid::Id128)));

    retryTimer_ = new QTimer(this);
    retryTimer_->setSingleShot(true);
    connect(retryTimer_, &QTimer::timeout, this, &WindowTracker::beginSetup);

    updateTimer_ = new QTimer(this);
    updateTimer_->setSingleShot(true);
    connect(updateTimer_, &QTimer::timeout, this, [this]() {
        failSetup(attempt_, QStringLiteral("KWin stopped sending window updates"));
    });

    beginSetup();
}

WindowTracker::~WindowTracker()
{
    stopping_ = true;
    retryTimer_->stop();
    updateTimer_->stop();

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (loadRequested_) {
        sendUnload(bus, scriptName_, QCoreApplication::instance());
    } else {
        removeScriptFile();
    }
    if (receiver_ != nullptr) {
        bus.unregisterObject(QStringLiteral("/WindowList"));
    }
    if (serviceRegistered_) {
        bus.unregisterService(serviceName_);
    }
}

bool WindowTracker::isAvailable() const
{
    return scriptLoaded_ && updateReceived_ && updateTimer_->isActive();
}

void WindowTracker::beginSetup()
{
    if (stopping_ || loadRequested_ || scriptLoaded_) {
        return;
    }

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        qCWarning(windowTrackerLog) << "Session D-Bus is not connected; retrying";
        scheduleRetry();
        return;
    }

    if (!serviceRegistered_) {
        if (!bus.registerService(serviceName_)) {
            qCWarning(windowTrackerLog) << "Could not register D-Bus service" << serviceName_;
            scheduleRetry();
            return;
        }
        serviceRegistered_ = true;
    }

    if (receiver_ == nullptr) {
        receiver_ = new WindowListReceiver(
            [this](const QVector<WindowInfo>& windows) {
                if (stopping_ || !loadRequested_) {
                    return;
                }
                updateReceived_ = true;
                if (scriptLoaded_) {
                    updateTimer_->start(kFirstUpdateTimeoutMs);
                }
                if (handler_) {
                    handler_(windows);
                }
            },
            this);
        if (!bus.registerVirtualObject(QStringLiteral("/WindowList"), receiver_)) {
            qCWarning(windowTrackerLog) << "Could not register the window-list D-Bus object";
            delete receiver_;
            receiver_ = nullptr;
            bus.unregisterService(serviceName_);
            serviceRegistered_ = false;
            scheduleRetry();
            return;
        }
    }

    ++attempt_;
    const quint64 attempt = attempt_;
    scriptName_ = QStringLiteral("memonitor-window-provider-p%1-%2")
                      .arg(QCoreApplication::applicationPid())
                      .arg(QUuid::createUuid().toString(QUuid::Id128));

    QTemporaryFile scriptFile;
    scriptFile.setFileTemplate(QStringLiteral("%1/memonitor-kwin-XXXXXX.js")
                                   .arg(QDir::tempPath()));
    scriptFile.setAutoRemove(false);
    if (!scriptFile.open()) {
        qCWarning(windowTrackerLog) << "Could not create the temporary KWin script";
        scheduleRetry();
        return;
    }

    const QByteArray contents = windowScript(serviceName_).toUtf8();
    if (scriptFile.write(contents) != static_cast<qint64>(contents.size()) || !scriptFile.flush()) {
        qCWarning(windowTrackerLog) << "Could not write the temporary KWin script";
        scriptFile.remove();
        scheduleRetry();
        return;
    }
    scriptPath_ = scriptFile.fileName();
    scriptFile.close();

    loadRequested_ = true;
    updateReceived_ = false;
    scriptLoaded_ = false;
    const QString attemptName = scriptName_;
    const QString attemptPath = scriptPath_;
    const QPointer<WindowTracker> tracker(this);
    QObject* watcherParent = QCoreApplication::instance();
    auto* loadWatcher = sendCall(bus,
                                 QStringLiteral("/Scripting"),
                                 QString::fromLatin1(kScriptingInterface),
                                 QStringLiteral("loadScript"),
                                 {attemptPath, attemptName},
                                 watcherParent);
    connect(loadWatcher,
            &QDBusPendingCallWatcher::finished,
            loadWatcher,
            [tracker, bus, loadWatcher, attempt, attemptName, attemptPath](QDBusPendingCallWatcher*) {
                const QDBusPendingReply<int> loadReply = *loadWatcher;
                const QDBusError loadError = loadWatcher->error();
                loadWatcher->deleteLater();
                if (!tracker || tracker->stopping_ || attempt != tracker->attempt_) {
                    QFile::remove(attemptPath);
                    sendUnload(bus, attemptName, QCoreApplication::instance());
                    return;
                }
                if (!loadReply.isValid() || loadReply.value() < 0) {
                    tracker->failSetup(attempt,
                                       loadError.isValid() ? loadError.message() : QStringLiteral("KWin rejected loadScript"));
                    return;
                }

                tracker->scriptId_ = loadReply.value();
                tracker->scriptObjectPath_ = QStringLiteral("/Scripting/Script%1").arg(tracker->scriptId_);
                auto* runWatcher = sendCall(bus,
                                            tracker->scriptObjectPath_,
                                            QString::fromLatin1(kScriptInterface),
                                            QStringLiteral("run"),
                                            {},
                                            QCoreApplication::instance(),
                                            kScriptRunTimeoutMs);
                connect(runWatcher,
                        &QDBusPendingCallWatcher::finished,
                        runWatcher,
                        [tracker, runWatcher, bus, attempt, attemptName, attemptPath](QDBusPendingCallWatcher*) {
                            const QDBusPendingReply<> runReply = *runWatcher;
                            const QDBusError runError = runWatcher->error();
                            runWatcher->deleteLater();
                            if (!tracker || tracker->stopping_ || attempt != tracker->attempt_) {
                                QFile::remove(attemptPath);
                                sendUnload(bus, attemptName, QCoreApplication::instance());
                                return;
                            }
                            if (!runReply.isValid()) {
                                tracker->failSetup(attempt,
                                                   runError.isValid() ? runError.message() : QStringLiteral("KWin failed to start the script"));
                                return;
                            }

                            tracker->scriptLoaded_ = true;
                            tracker->removeScriptFile();
                            tracker->updateTimer_->start(kFirstUpdateTimeoutMs);
                        });
            });
}

void WindowTracker::scheduleRetry()
{
    if (!stopping_ && retryTimer_ != nullptr && !retryTimer_->isActive()) {
        retryTimer_->start(kRetryDelayMs);
    }
}

void WindowTracker::failSetup(quint64 attempt, const QString& reason)
{
    if (stopping_ || attempt != attempt_) {
        return;
    }

    qCWarning(windowTrackerLog) << "KWin window tracking setup failed:" << reason;
    retryTimer_->stop();
    updateTimer_->stop();
    if (loadRequested_) {
        sendUnload(QDBusConnection::sessionBus(), scriptName_, QCoreApplication::instance());
    }
    removeScriptFile();
    scriptObjectPath_.clear();
    scriptId_ = -1;
    scriptLoaded_ = false;
    updateReceived_ = false;
    loadRequested_ = false;
    if (handler_) {
        handler_({});
    }
    scheduleRetry();
}

void WindowTracker::removeScriptFile()
{
    if (!scriptPath_.isEmpty()) {
        QFile::remove(scriptPath_);
        scriptPath_.clear();
    }
}

} // namespace memonitor
