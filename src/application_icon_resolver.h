#pragma once

#include <QIcon>
#include <QString>

namespace memonitor {

struct ApplicationIconRequest final {
    QString desktopFileName;
    QString iconName;
    QString resourceClass;
    QString resourceName;
    QString executableName;
};

class ApplicationIconResolver final {
public:
    [[nodiscard]] static QIcon resolve(const ApplicationIconRequest& request);
};

} // namespace memonitor
