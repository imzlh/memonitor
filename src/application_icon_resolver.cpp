#include "application_icon_resolver.h"

#include <QCache>
#include <QDataStream>
#include <QDir>
#include <QFileInfo>
#include <QIODevice>
#include <QMutex>
#include <QMutexLocker>
#include <QSettings>
#include <QSet>
#include <QStandardPaths>
#include <QStringList>
#include <QUrl>
#include <QVector>

#include <array>
#include <chrono>

namespace memonitor {
namespace {

struct IconReference final {
    QString value;
    QString baseDirectory;
};

struct CachedIcon final {
    QIcon icon;
    std::chrono::steady_clock::time_point cachedAt;
};

bool isDesktopFileName(const QString& value)
{
    return value.endsWith(QStringLiteral(".desktop"), Qt::CaseInsensitive);
}

QString withoutDesktopSuffix(QString value)
{
    if (isDesktopFileName(value)) {
        value.chop(8);
    }
    return value;
}

QString localPathForReference(const QString& reference)
{
    QString value = reference.trimmed();
    if (value.startsWith(QStringLiteral("file:"), Qt::CaseInsensitive)) {
        const QUrl url(value);
        if (url.isLocalFile()) {
            value = url.toLocalFile();
        }
    } else if (value.startsWith(QStringLiteral("~/"))) {
        value = QDir(QDir::homePath()).filePath(value.mid(2));
    }
    return value;
}

bool looksLikePath(const QString& value)
{
    const QString trimmed = value.trimmed();
    return trimmed.startsWith(QStringLiteral(":/"))
        || trimmed.startsWith(QStringLiteral("file:"), Qt::CaseInsensitive)
        || trimmed.startsWith(QStringLiteral("~/"))
        || trimmed.contains(QLatin1Char('/'));
}

void appendUnique(QStringList& values, const QString& value)
{
    const QString trimmed = value.trimmed();
    if (!trimmed.isEmpty() && !values.contains(trimmed)) {
        values.push_back(trimmed);
    }
}

QString stableFilePath(const QString& path)
{
    const QFileInfo info(path);
    const QString canonicalPath = info.canonicalFilePath();
    return canonicalPath.isEmpty() ? info.absoluteFilePath() : canonicalPath;
}

QString locateDesktopFile(const QString& identifier)
{
    const QString value = identifier.trimmed();
    if (value.isEmpty()) {
        return {};
    }

    const QString localPath = localPathForReference(value);
    if (looksLikePath(value)) {
        const QFileInfo info(localPath);
        if (info.isFile() && isDesktopFileName(info.fileName())) {
            return stableFilePath(info.absoluteFilePath());
        }
        return {};
    }

    QStringList names;
    appendUnique(names, value);
    if (!isDesktopFileName(value)) {
        appendUnique(names, value + QStringLiteral(".desktop"));
    }

    const QString baseName = QFileInfo(value).fileName();
    if (baseName != value) {
        appendUnique(names, baseName);
        if (!isDesktopFileName(baseName)) {
            appendUnique(names, baseName + QStringLiteral(".desktop"));
        }
    }

    for (const QString& name : names) {
        const QStringList matches = QStandardPaths::locateAll(
            QStandardPaths::ApplicationsLocation, name, QStandardPaths::LocateFile);
        for (const QString& match : matches) {
            const QFileInfo info(match);
            if (info.isFile()) {
                return stableFilePath(info.filePath());
            }
        }
    }
    return {};
}

QString readDesktopIcon(const QString& desktopPath)
{
    QSettings settings(desktopPath, QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("Desktop Entry"));
    return settings.value(QStringLiteral("Icon")).toString().trimmed();
}

QStringList localFileCandidates(const IconReference& reference)
{
    const QString value = localPathForReference(reference.value);
    QStringList candidates;
    if (value.isEmpty()) {
        return candidates;
    }

    if (!reference.baseDirectory.isEmpty() && !QDir::isAbsolutePath(value)
        && !value.startsWith(QStringLiteral(":/"))) {
        appendUnique(candidates, QDir(reference.baseDirectory).filePath(value));
    }
    appendUnique(candidates, value);
    return candidates;
}

QIcon iconFromFile(const IconReference& reference)
{
    for (const QString& path : localFileCandidates(reference)) {
        const bool isQtResource = path.startsWith(QStringLiteral(":/"));
        if (!isQtResource && !QFileInfo(path).isFile()) {
            continue;
        }

        const QIcon icon(path);
        if (!icon.isNull()) {
            return icon;
        }
    }
    return {};
}

void appendDesktopIdentifiers(QStringList& identifiers, const QString& value)
{
    const QString trimmed = value.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }

    if (looksLikePath(trimmed)) {
        if (isDesktopFileName(trimmed)) {
            appendUnique(identifiers, trimmed);
        }
        const QString pathBaseName = QFileInfo(localPathForReference(trimmed)).fileName();
        if (!pathBaseName.isEmpty() && pathBaseName != QStringLiteral(".")
            && pathBaseName != QStringLiteral("..")) {
            appendUnique(identifiers, pathBaseName);
            appendUnique(identifiers, withoutDesktopSuffix(pathBaseName));
        }
        return;
    }

    appendUnique(identifiers, trimmed);
    appendUnique(identifiers, withoutDesktopSuffix(QFileInfo(trimmed).fileName()));
}

void collectDesktopIconReferences(const QString& identifier,
                                  QVector<IconReference>& references,
                                  QSet<QString>& visitedDesktopFiles)
{
    const QString desktopPath = locateDesktopFile(identifier);
    if (desktopPath.isEmpty()) {
        return;
    }

    const QString visitKey = stableFilePath(desktopPath);
    if (visitedDesktopFiles.contains(visitKey)) {
        return;
    }
    visitedDesktopFiles.insert(visitKey);

    const QString iconReference = readDesktopIcon(desktopPath);
    if (iconReference.isEmpty()) {
        return;
    }

    // A desktop Icon= may point at another desktop entry. Follow that entry
    // before treating the value as a theme name.
    if (isDesktopFileName(iconReference)) {
        collectDesktopIconReferences(iconReference, references, visitedDesktopFiles);
    }

    references.push_back({iconReference, QFileInfo(desktopPath).absolutePath()});
}

void appendThemeCandidates(QStringList& candidates, const QString& value)
{
    const QString trimmed = value.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }

    if (looksLikePath(trimmed)) {
        const QString pathBaseName = QFileInfo(localPathForReference(trimmed)).fileName();
        if (!pathBaseName.isEmpty() && pathBaseName != QStringLiteral(".")
            && pathBaseName != QStringLiteral("..")) {
            appendThemeCandidates(candidates, pathBaseName);
        }
        return;
    }

    appendUnique(candidates, trimmed);

    const QString fileName = QFileInfo(trimmed).fileName();
    appendUnique(candidates, fileName);

    const QString stem = withoutDesktopSuffix(fileName);
    appendUnique(candidates, stem);

    appendUnique(candidates, trimmed.toCaseFolded());
    appendUnique(candidates, fileName.toCaseFolded());
    appendUnique(candidates, stem.toCaseFolded());

    const QString foldedStem = stem.toCaseFolded();
    if (foldedStem.startsWith(QStringLiteral("org.kde."))) {
        appendUnique(candidates, stem.mid(8));
        appendUnique(candidates, foldedStem.mid(8));
    }
}

QIcon iconFromTheme(const QStringList& candidates)
{
    for (const QString& candidate : candidates) {
        const QIcon icon = QIcon::fromTheme(candidate);
        if (!icon.isNull()) {
            return icon;
        }
    }
    return {};
}

void appendKdeAliases(QStringList& candidates, const QStringList& identifiers)
{
    struct KdeAlias final {
        const char* identifier;
        const char* iconName;
    };

    static constexpr std::array<KdeAlias, 18> aliases{{
        {"org.kde.ark", "ark"},
        {"org.kde.dolphin", "system-file-manager"},
        {"org.kde.discover", "plasmadiscover"},
        {"org.kde.gwenview", "gwenview"},
        {"org.kde.kate", "kate"},
        {"org.kde.kcalc", "kcalc"},
        {"org.kde.kmail", "kmail"},
        {"org.kde.konsole", "utilities-terminal"},
        {"org.kde.korganizer", "korganizer"},
        {"org.kde.krdc", "krdc"},
        {"org.kde.krunner", "krunner"},
        {"org.kde.okular", "okular"},
        {"org.kde.plasmashell", "plasmashell"},
        {"org.kde.spectacle", "spectacle"},
        {"org.kde.systemsettings", "systemsettings"},
        {"kdesystemsettings", "systemsettings"},
        {"konsole", "utilities-terminal"},
        {"dolphin", "system-file-manager"},
    }};

    for (const QString& identifier : identifiers) {
        const QString key = withoutDesktopSuffix(QFileInfo(identifier).fileName()).toCaseFolded();
        for (const KdeAlias& alias : aliases) {
            if (key == QLatin1String(alias.identifier)) {
                appendUnique(candidates, QString::fromLatin1(alias.iconName));
            }
        }
    }
}

QByteArray cacheKey(const ApplicationIconRequest& request)
{
    QByteArray key;
    QDataStream stream(&key, QIODevice::WriteOnly);
    stream << request.desktopFileName << request.iconName << request.resourceClass
           << request.resourceName << request.executableName << QIcon::themeName()
           << QIcon::themeSearchPaths();
    return key;
}

QMutex& iconCacheMutex()
{
    static QMutex mutex;
    return mutex;
}

QCache<QByteArray, CachedIcon>& iconCache()
{
    static QCache<QByteArray, CachedIcon> cache(512);
    return cache;
}

QIcon resolveUncached(const ApplicationIconRequest& request)
{
    QStringList desktopIdentifiers;
    appendDesktopIdentifiers(desktopIdentifiers, request.desktopFileName);
    appendDesktopIdentifiers(desktopIdentifiers, request.iconName);
    appendDesktopIdentifiers(desktopIdentifiers, request.resourceClass);
    appendDesktopIdentifiers(desktopIdentifiers, request.resourceName);
    appendDesktopIdentifiers(desktopIdentifiers, request.executableName);

    QVector<IconReference> desktopIconReferences;
    QSet<QString> visitedDesktopFiles;
    for (const QString& identifier : desktopIdentifiers) {
        collectDesktopIconReferences(identifier, desktopIconReferences, visitedDesktopFiles);
    }

    for (const IconReference& reference : desktopIconReferences) {
        const QIcon icon = iconFromFile(reference);
        if (!icon.isNull()) {
            return icon;
        }
    }

    const QStringList rawValues{
        request.iconName,
        request.resourceClass,
        request.resourceName,
        request.executableName,
        request.desktopFileName,
    };
    for (const QString& value : rawValues) {
        const QIcon icon = iconFromFile({value, {}});
        if (!icon.isNull()) {
            return icon;
        }
    }

    QStringList themeCandidates;
    for (const IconReference& reference : desktopIconReferences) {
        appendThemeCandidates(themeCandidates, reference.value);
    }
    for (const QString& value : rawValues) {
        appendThemeCandidates(themeCandidates, value);
    }
    appendKdeAliases(themeCandidates, desktopIdentifiers);

    return iconFromTheme(themeCandidates);
}

} // namespace

QIcon ApplicationIconResolver::resolve(const ApplicationIconRequest& request)
{
    constexpr auto cacheLifetime = std::chrono::minutes(1);
    const QByteArray key = cacheKey(request);
    const auto now = std::chrono::steady_clock::now();

    QMutexLocker locker(&iconCacheMutex());
    auto& cache = iconCache();
    if (const CachedIcon* cached = cache.object(key)) {
        if (now - cached->cachedAt < cacheLifetime) {
            return cached->icon;
        }
        cache.remove(key);
    }

    const QIcon icon = resolveUncached(request);
    cache.insert(key, new CachedIcon{icon, std::chrono::steady_clock::now()});
    return icon;
}

} // namespace memonitor
