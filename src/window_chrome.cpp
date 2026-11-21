#include "window_chrome.h"

#include <QAbstractButton>
#include <QEvent>
#include <QIcon>
#include <QIconEngine>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QStyle>
#include <QStyleOptionFocusRect>
#include <QWidget>
#include <QWindow>

#include <algorithm>

namespace memonitor {
namespace {

constexpr int kResizeMargin = 6;
constexpr int kResizeCorner = 12;
constexpr std::array<Qt::Edges, 8> kResizeEdges{
    Qt::TopEdge, Qt::TopEdge | Qt::RightEdge, Qt::RightEdge,
    Qt::BottomEdge | Qt::RightEdge, Qt::BottomEdge,
    Qt::BottomEdge | Qt::LeftEdge, Qt::LeftEdge, Qt::TopEdge | Qt::LeftEdge,
};
constexpr std::array<Qt::CursorShape, 8> kResizeCursors{
    Qt::SizeVerCursor, Qt::SizeBDiagCursor, Qt::SizeHorCursor, Qt::SizeFDiagCursor,
    Qt::SizeVerCursor, Qt::SizeBDiagCursor, Qt::SizeHorCursor, Qt::SizeFDiagCursor,
};

void paintCaptionGlyph(QPainter& painter, const QRect& rectangle,
                       WindowCaptionButton::Action action, bool restore, const QColor& color)
{
    painter.save();
    painter.translate(rectangle.center() + QPointF(0.5, 0.5));
    painter.scale(rectangle.width() / 16.0, rectangle.height() / 16.0);
    painter.setPen(QPen(color, 1.0));
    painter.setBrush(Qt::NoBrush);
    if (action == WindowCaptionButton::Action::Minimize) {
        painter.drawLine(QPointF(-5, 0.5), QPointF(5, 0.5));
    } else if (action == WindowCaptionButton::Action::Close) {
        painter.setRenderHint(QPainter::Antialiasing);
        painter.drawLine(QPointF(-4.5, -4.5), QPointF(4.5, 4.5));
        painter.drawLine(QPointF(4.5, -4.5), QPointF(-4.5, 4.5));
    } else if (restore) {
        QPainterPath back;
        back.moveTo(-2.5, -3.5);
        back.lineTo(-2.5, -5.5);
        back.lineTo(5.5, -5.5);
        back.lineTo(5.5, 2.5);
        back.lineTo(3.5, 2.5);
        painter.drawPath(back);
        painter.drawRect(QRectF(-5.5, -2.5, 8.0, 8.0));
    } else {
        painter.drawRect(QRectF(-4.5, -4.5, 9.0, 9.0));
    }
    painter.restore();
}

class CaptionIconEngine final : public QIconEngine {
public:
    CaptionIconEngine(WindowCaptionButton::Action action, bool restore, const QColor& color)
        : action_(action), restore_(restore), color_(color)
    {
    }

    QIconEngine* clone() const override { return new CaptionIconEngine(*this); }

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode, QIcon::State) override
    {
        paintCaptionGlyph(*painter, rect, action_, restore_, color_);
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
    {
        QPixmap result(size);
        result.fill(Qt::transparent);
        QPainter painter(&result);
        paint(&painter, QRect(QPoint(), size), mode, state);
        return result;
    }

private:
    WindowCaptionButton::Action action_;
    bool restore_;
    QColor color_;
};

} // namespace

WindowCaptionButton::WindowCaptionButton(Action action, QWidget* parent)
    : QToolButton(parent), action_(action)
{
    setAutoRaise(true);
    setAttribute(Qt::WA_Hover);
    setFixedSize(46, 40);
    setIconSize(QSize(16, 16));
    updateIcon();
}

void WindowCaptionButton::setRestore(bool restore)
{
    if (restore_ != restore) {
        restore_ = restore;
        updateIcon();
    }
}

void WindowCaptionButton::updateIcon()
{
    setIcon(QIcon(new CaptionIconEngine(action_, restore_, palette().color(QPalette::WindowText))));
    update();
}

void WindowCaptionButton::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    const bool hover = underMouse() && isEnabled();
    QColor foreground = palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled, QPalette::WindowText);
    if (action_ == Action::Close && (hover || isDown())) {
        painter.fillRect(rect(), isDown() ? QColor(196, 43, 28) : QColor(232, 17, 35));
        foreground = Qt::white;
    } else if (hover || isDown()) {
        QColor background = palette().color(QPalette::WindowText);
        background.setAlpha(isDown() ? 42 : 24);
        painter.fillRect(rect(), background);
    }
    const QRect glyphRect((width() - 16) / 2, (height() - 16) / 2, 16, 16);
    paintCaptionGlyph(painter, glyphRect, action_, restore_, foreground);
    if (hasFocus()) {
        QStyleOptionFocusRect option;
        option.initFrom(this);
        option.rect = rect().adjusted(3, 3, -3, -3);
        style()->drawPrimitive(QStyle::PE_FrameFocusRect, &option, &painter, this);
    }
}

void WindowCaptionButton::changeEvent(QEvent* event)
{
    QToolButton::changeEvent(event);
    if (event->type() == QEvent::PaletteChange) {
        updateIcon();
    }
}

bool WindowCaptionButton::event(QEvent* event)
{
    const bool handled = QToolButton::event(event);
    if (event->type() == QEvent::HoverEnter || event->type() == QEvent::HoverLeave
        || event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut) {
        update();
    }
    return handled;
}

WindowChrome::WindowChrome(QWidget* window, QWidget* header, QWidget* title,
                           WindowCaptionButton* minimizeButton, WindowCaptionButton* maximizeButton,
                           WindowCaptionButton* closeButton)
    : QObject(window)
    , window_(window)
    , header_(header)
    , title_(title)
    , minimizeButton_(minimizeButton)
    , maximizeButton_(maximizeButton)
    , closeButton_(closeButton)
{
    window_->installEventFilter(this);
    header_->installEventFilter(this);
    title_->installEventFilter(this);

    connect(minimizeButton_, &QAbstractButton::clicked, window_, &QWidget::showMinimized);
    connect(maximizeButton_, &QAbstractButton::clicked, this, &WindowChrome::toggleMaximized);
    connect(closeButton_, &QAbstractButton::clicked, window_, &QWidget::close);

    // Separate handles keep resize cursors at the outer edge and leave content,
    // scrolling and dialogs alone. They hide when maximized or full screen.
    for (std::size_t index = 0; index < resizeHandles_.size(); ++index) {
        auto* handle = new QWidget(window_);
        handle->setObjectName(QStringLiteral("windowResizeHandle%1").arg(index));
        handle->setCursor(kResizeCursors[index]);
        handle->installEventFilter(this);
        resizeHandles_[index] = handle;
    }
    updateWindowButtons();
    updateResizeHandles();
}

bool WindowChrome::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == window_) {
        switch (event->type()) {
        case QEvent::Show:
        case QEvent::WindowStateChange:
            updateWindowButtons();
            updateResizeHandles();
            break;
        case QEvent::Resize:
            updateResizeHandles();
            break;
        case QEvent::StyleChange:
        case QEvent::PaletteChange:
            updateWindowButtons();
            break;
        default:
            break;
        }
    }

    if (watched == header_ || watched == title_) {
        if (event->type() == QEvent::MouseButtonDblClick
            || event->type() == QEvent::MouseButtonPress) {
            const auto* mouseEvent = static_cast<QMouseEvent*>(event);
            const QPoint headerPosition = watched == title_
                ? title_->mapTo(header_, mouseEvent->position().toPoint())
                : mouseEvent->position().toPoint();
            const QWidget* child = header_->childAt(headerPosition);
            // Ignored button events can propagate to the header. Only its
            // title and blank space should initiate window operations.
            if (child != nullptr && child != title_) {
                return QObject::eventFilter(watched, event);
            }
            if (mouseEvent->button() == Qt::LeftButton && event->type() == QEvent::MouseButtonDblClick) {
                toggleMaximized();
                return true;
            } else if (mouseEvent->button() == Qt::LeftButton && window_->windowHandle() != nullptr) {
                window_->windowHandle()->startSystemMove();
                return true;
            }
        }
    }

    if (event->type() == QEvent::MouseButtonPress) {
        const auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (mouseEvent->button() == Qt::LeftButton && !window_->isMaximized()
            && !window_->isFullScreen() && window_->windowHandle() != nullptr) {
            for (std::size_t index = 0; index < resizeHandles_.size(); ++index) {
                if (watched == resizeHandles_[index]) {
                    window_->windowHandle()->startSystemResize(kResizeEdges[index]);
                    return true;
                }
            }
        }
    }

    return QObject::eventFilter(watched, event);
}

void WindowChrome::toggleMaximized()
{
    if (window_->isMaximized() || window_->isFullScreen()) {
        window_->showNormal();
    } else {
        window_->showMaximized();
    }
}

void WindowChrome::updateWindowButtons()
{
    minimizeButton_->setToolTip(QStringLiteral("最小化"));
    minimizeButton_->setAccessibleName(QStringLiteral("最小化"));

    const bool restore = window_->isMaximized() || window_->isFullScreen();
    maximizeButton_->setRestore(restore);
    const QString maximizeText = restore ? QStringLiteral("还原") : QStringLiteral("最大化");
    maximizeButton_->setToolTip(maximizeText);
    maximizeButton_->setAccessibleName(maximizeText);

    closeButton_->setToolTip(QStringLiteral("关闭"));
    closeButton_->setAccessibleName(QStringLiteral("关闭"));
}

void WindowChrome::updateResizeHandles()
{
    const int width = window_->width();
    const int height = window_->height();
    const int horizontalSpan = std::max(0, width - 2 * kResizeCorner);
    const int verticalSpan = std::max(0, height - 2 * kResizeCorner);
    const std::array<QRect, 8> rectangles{
        QRect(kResizeCorner, 0, horizontalSpan, kResizeMargin),
        QRect(width - kResizeCorner, 0, kResizeCorner, kResizeCorner),
        QRect(width - kResizeMargin, kResizeCorner, kResizeMargin, verticalSpan),
        QRect(width - kResizeCorner, height - kResizeCorner, kResizeCorner, kResizeCorner),
        QRect(kResizeCorner, height - kResizeMargin, horizontalSpan, kResizeMargin),
        QRect(0, height - kResizeCorner, kResizeCorner, kResizeCorner),
        QRect(0, kResizeCorner, kResizeMargin, verticalSpan),
        QRect(0, 0, kResizeCorner, kResizeCorner),
    };
    const bool resizable = !window_->isMaximized() && !window_->isFullScreen()
        && !window_->isMinimized();
    for (std::size_t index = 0; index < resizeHandles_.size(); ++index) {
        resizeHandles_[index]->setGeometry(rectangles[index]);
        resizeHandles_[index]->setVisible(resizable);
        resizeHandles_[index]->raise();
    }
}

} // namespace memonitor
