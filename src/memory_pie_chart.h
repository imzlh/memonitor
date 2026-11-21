#pragma once

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QWidget>

#include <cstdint>

class GuiSmokeTest;

namespace memonitor {

class MemoryPieChart final : public QWidget {
public:
    explicit MemoryPieChart(QWidget* parent = nullptr);

    void setMemory(std::uint64_t totalBytes,
                   std::uint64_t availableBytes,
                   std::uint64_t zramBytes,
                   std::uint64_t zramLogicalBytes,
                   std::uint64_t zramCapacityBytes,
                   bool zramStatisticsComplete = true);

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    friend class ::GuiSmokeTest;

    enum class Segment {
        None,
        Available,
        Regular,
        Zram,
        ZramLogical
    };

    [[nodiscard]] QRectF chartRect() const;
    [[nodiscard]] QFont legendFont() const;
    [[nodiscard]] QRectF legendRect() const;
    [[nodiscard]] QRectF legendItemRect(int index) const;
    [[nodiscard]] qreal captionHeight() const;
    [[nodiscard]] Segment segmentAt(const QPointF& point) const;
    [[nodiscard]] QColor segmentColor(Segment segment) const;
    [[nodiscard]] QString segmentName(Segment segment) const;
    [[nodiscard]] QString tooltipFor(Segment segment) const;
    void updateTooltip();

    std::uint64_t totalBytes_ = 0;
    std::uint64_t availableBytes_ = 0;
    std::uint64_t zramRamBytes_ = 0;
    std::uint64_t zramReportedBytes_ = 0;
    std::uint64_t zramLogicalBytes_ = 0;
    std::uint64_t zramCapacityBytes_ = 0;
    std::uint64_t regularBytes_ = 0;
    std::uint64_t usedBytes_ = 0;
    bool hasSnapshot_ = false;
    bool zramValueClamped_ = false;
    bool zramStatisticsComplete_ = true;
    Segment hoveredSegment_ = Segment::None;
};

} // namespace memonitor
