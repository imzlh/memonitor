#pragma once

#include <QObject>
#include <QToolButton>

#include <array>

class QWidget;

namespace memonitor {

class WindowCaptionButton final : public QToolButton {
public:
    enum class Action { Minimize, Maximize, Close };

    explicit WindowCaptionButton(Action action, QWidget* parent = nullptr);
    void setRestore(bool restore);

protected:
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;
    bool event(QEvent* event) override;

private:
    void updateIcon();

    Action action_;
    bool restore_ = false;
};

class WindowChrome final : public QObject {
public:
    WindowChrome(QWidget* window, QWidget* header, QWidget* title,
                 WindowCaptionButton* minimizeButton, WindowCaptionButton* maximizeButton,
                 WindowCaptionButton* closeButton);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void toggleMaximized();
    void updateWindowButtons();
    void updateResizeHandles();

    QWidget* window_;
    QWidget* header_;
    QWidget* title_;
    WindowCaptionButton* minimizeButton_;
    WindowCaptionButton* maximizeButton_;
    WindowCaptionButton* closeButton_;
    std::array<QWidget*, 8> resizeHandles_{};
};

} // namespace memonitor
