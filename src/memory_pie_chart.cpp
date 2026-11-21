#include "memory_pie_chart.h"

#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPaintEvent>
#include <QToolTip>

#include <algorithm>
#include <cmath>

namespace memonitor {
namespace {

QString formatBytes(std::uint64_t bytes)
{
    static const QStringList units{
        QStringLiteral("B"), QStringLiteral("KiB"), QStringLiteral("MiB"),
        QStringLiteral("GiB"), QStringLiteral("TiB")};
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

} // namespace

MemoryPieChart::MemoryPieChart(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setAttribute(Qt::WA_Hover, true);
    setMinimumSize(minimumSizeHint());
    setAccessibleName(QStringLiteral("整体内存分布"));
    setAccessibleDescription(QStringLiteral("外圈显示物理 RAM 分布，内圈显示 zram 压缩前数据占设备容量的比例，两圈分别计算。"));
}

void MemoryPieChart::setMemory(std::uint64_t totalBytes,
                               std::uint64_t availableBytes,
                               std::uint64_t zramBytes,
                               std::uint64_t zramLogicalBytes,
                               std::uint64_t zramCapacityBytes,
                               bool zramStatisticsComplete)
{
    totalBytes_ = totalBytes;
    availableBytes_ = std::min(availableBytes, totalBytes);
    usedBytes_ = totalBytes_ - availableBytes_;
    zramStatisticsComplete_ = zramStatisticsComplete;
    zramReportedBytes_ = zramBytes;
    zramValueClamped_ = zramStatisticsComplete_ && zramBytes > usedBytes_;
    zramRamBytes_ = zramStatisticsComplete_ ? std::min(zramBytes, usedBytes_) : 0;
    zramLogicalBytes_ = zramLogicalBytes;
    zramCapacityBytes_ = zramStatisticsComplete_ ? zramCapacityBytes : 0;
    regularBytes_ = usedBytes_ - zramRamBytes_;
    hasSnapshot_ = totalBytes_ > 0;
    hoveredSegment_ = Segment::None;
    updateTooltip();
    update();
}

QSize MemoryPieChart::sizeHint() const
{
    return {320, 315};
}

QSize MemoryPieChart::minimumSizeHint() const
{
    return {280, 280};
}

QRectF MemoryPieChart::chartRect() const
{
    const qreal legendHeight = QFontMetrics(legendFont()).height() * 4.0 + 14.0 + captionHeight();
    const qreal side = std::max<qreal>(64.0,
        std::min({200.0, height() - legendHeight - 24.0, width() - 12.0}));
    const qreal top = (height() - side - legendHeight - 12.0) / 2.0;
    return QRectF((width() - side) / 2.0, top, side, side);
}

QFont MemoryPieChart::legendFont() const
{
    QFont result = font();
    result.setPointSizeF(std::max(8.0, result.pointSizeF() - 2.0));
    return result;
}

QRectF MemoryPieChart::legendRect() const
{
    const qreal rowHeight = QFontMetrics(legendFont()).height() * 2.0 + 4.0;
    const qreal legendHeight = rowHeight * 2.0 + 6.0;
    const qreal legendWidth = std::min<qreal>(280.0, width() - 12.0);
    return QRectF((width() - legendWidth) / 2.0, chartRect().bottom() + 12.0,
                  legendWidth, legendHeight);
}

QRectF MemoryPieChart::legendItemRect(int index) const
{
    const QRectF bounds = legendRect();
    const qreal columnWidth = (bounds.width() - 12.0) / 2.0;
    const qreal rowHeight = (bounds.height() - 6.0) / 2.0;
    return QRectF(bounds.left() + (index % 2) * (columnWidth + 12.0),
                  bounds.top() + (index / 2) * (rowHeight + 6.0), columnWidth, rowHeight);
}

qreal MemoryPieChart::captionHeight() const
{
    return QFontMetrics(legendFont()).height() + 10.0;
}

QColor MemoryPieChart::segmentColor(Segment segment) const
{
    const bool dark = palette().color(QPalette::Window).lightness() < 128;
    switch (segment) {
    case Segment::Available:
        return dark ? QColor(QStringLiteral("#576574")) : QColor(QStringLiteral("#8b98a5"));
    case Segment::Regular:
        return dark ? QColor(QStringLiteral("#46a6bd")) : QColor(QStringLiteral("#2786a1"));
    case Segment::Zram:
        return dark ? QColor(QStringLiteral("#e6a84a")) : QColor(QStringLiteral("#c47a12"));
    case Segment::ZramLogical:
        return dark ? QColor(QStringLiteral("#b58ac7")) : QColor(QStringLiteral("#8757a1"));
    case Segment::None:
        break;
    }
    return palette().color(QPalette::WindowText);
}

QString MemoryPieChart::segmentName(Segment segment) const
{
    switch (segment) {
    case Segment::Available: return QStringLiteral("估算可用内存");
    case Segment::Regular: return zramStatisticsComplete_ ? QStringLiteral("其他已用内存")
                                                         : QStringLiteral("物理已用（包含 zram）");
    case Segment::Zram: return QStringLiteral("zram 实际 RAM");
    case Segment::ZramLogical: return QStringLiteral("zram 逻辑数据");
    case Segment::None: break;
    }
    return QString();
}

QString MemoryPieChart::tooltipFor(Segment segment) const
{
    if (!hasSnapshot_ || totalBytes_ == 0) {
        return QStringLiteral("等待有效的物理内存采样");
    }

    std::uint64_t bytes = 0;
    QString explanation;
    const QString capacityText = zramCapacityBytes_ > 0 ? formatBytes(zramCapacityBytes_)
                                                       : QStringLiteral("未启用或容量不可读");
    switch (segment) {
    case Segment::Available:
        bytes = availableBytes_;
        explanation = QStringLiteral("MemAvailable：内核估算无需交换即可供新应用使用的物理内存，包含部分可回收缓存。zram 的 RAM 分配已经反映在该值中；它不是分配成功或不会 OOM 的保证。");
        break;
    case Segment::Regular:
        bytes = regularBytes_;
        explanation = zramStatisticsComplete_
            ? QStringLiteral("物理已用量中扣除 zram 实际 RAM 后的其他部分；zram 实际 RAM 仍包含在整体物理已用中。")
            : QStringLiteral("zram 统计未完整读到，无法单独分摊；此块包含 zram 在内的全部物理已用量。");
        break;
    case Segment::Zram:
        if (!zramStatisticsComplete_) {
            return QStringLiteral("zram 实际 RAM：统计不完整，未单独绘制。已读设备占用 %1；不能当作全部设备的实际值。")
                .arg(formatBytes(zramReportedBytes_));
        }
        bytes = zramRamBytes_;
        explanation = QStringLiteral("zram 实际 RAM；它已经包含在 MemTotal 的物理已用量中，本图将其从普通物理已用中单独标出，不会重复相加。逻辑数据为 %1。")
            .arg(formatBytes(zramLogicalBytes_));
        break;
    case Segment::ZramLogical:
        if (!zramStatisticsComplete_) {
            return QStringLiteral("zram 逻辑数据：统计不完整，无法计算完整的逻辑使用率。");
        }
        bytes = zramLogicalBytes_;
        return QStringLiteral("zram 逻辑数据（内圈）\n%1 / %2（%3）\n压缩前的数据量 / zram 设备逻辑容量。与外圈分别计算，不作为额外物理 RAM；实际 RAM 为 %4。交换槽位的余量另见交换区指标。")
            .arg(formatBytes(bytes), capacityText,
                 formatPercent(bytes, zramCapacityBytes_), formatBytes(zramReportedBytes_));
    case Segment::None:
        if (!zramStatisticsComplete_) {
            return QStringLiteral("物理已用 %1 / %2（%3）\nzram 统计不完整，无法单独分摊。外圈显示估算可用和包含 zram 的物理已用，内圈未绘制。")
                .arg(formatBytes(usedBytes_), formatBytes(totalBytes_), formatPercent(usedBytes_, totalBytes_));
        }
        return QStringLiteral("物理已用 %1 / %2（%3）\n外圈：估算可用、其他已用和 zram 实际 RAM，相加等于物理总量。\n内圈：zram 逻辑数据 %4 / %5（%6），不与物理容量相加。\n估算可用不保证后续分配成功，需结合交换区余量和 PSI 判断压力。")
            .arg(formatBytes(usedBytes_), formatBytes(totalBytes_),
                 formatPercent(usedBytes_, totalBytes_), formatBytes(zramLogicalBytes_),
                 capacityText, formatPercent(zramLogicalBytes_, zramCapacityBytes_));
    }

    QString result = QStringLiteral("%1\n%2 / %3 (%4)\n%5")
        .arg(segmentName(segment), formatBytes(bytes), formatBytes(totalBytes_),
             formatPercent(bytes, totalBytes_), explanation);
    if (segment == Segment::Zram && zramValueClamped_) {
        result += QStringLiteral("\n设备采样实际值 %1 超出本次物理已用估算；读数并非同一时刻，图形按已用量封顶。")
                      .arg(formatBytes(zramReportedBytes_));
    }
    return result;
}

MemoryPieChart::Segment MemoryPieChart::segmentAt(const QPointF& point) const
{
    const QRectF bounds = chartRect();
    const QPointF center = bounds.center();
    const qreal outerRadius = bounds.width() / 2.0;
    const qreal dx = point.x() - center.x();
    const qreal dy = point.y() - center.y();
    const qreal distance = std::hypot(dx, dy);
    if (distance > outerRadius || totalBytes_ == 0) {
        return Segment::None;
    }
    if (distance < outerRadius * 0.78) {
        return zramCapacityBytes_ > 0 && distance >= outerRadius * 0.58 && distance <= outerRadius * 0.68
            ? Segment::ZramLogical : Segment::None;
    }

    double angle = std::atan2(dy, dx) * 180.0 / 3.14159265358979323846;
    if (angle < 0.0) {
        angle += 360.0;
    }
    const double availableAngle = static_cast<double>(availableBytes_) * 360.0 / static_cast<double>(totalBytes_);
    const double regularAngle = static_cast<double>(regularBytes_) * 360.0 / static_cast<double>(totalBytes_);
    if (angle < availableAngle) {
        return Segment::Available;
    }
    if (angle < availableAngle + regularAngle) {
        return Segment::Regular;
    }
    return Segment::Zram;
}

void MemoryPieChart::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);

    const QColor textColor = palette().color(QPalette::WindowText);
    const QColor mutedColor = palette().color(QPalette::PlaceholderText);
    const QRectF bounds = chartRect();
    const QRectF centerRect = bounds.adjusted(bounds.width() * 0.21,
                                              bounds.height() * 0.21,
                                              -bounds.width() * 0.21,
                                              -bounds.height() * 0.21);

    if (hasSnapshot_ && totalBytes_ > 0) {
        const Segment segments[] = {
            Segment::Available, Segment::Regular, Segment::Zram};
        const std::uint64_t values[] = {
            availableBytes_, regularBytes_, zramRamBytes_};
        std::size_t lastNonzero = 0;
        for (std::size_t index = 0; index < 3; ++index) {
            if (values[index] > 0) {
                lastNonzero = index;
            }
        }
        int startAngle = 0;
        int drawnAngle = 0;
        for (std::size_t index = 0; index < 3; ++index) {
            if (values[index] == 0) {
                continue;
            }
            const int spanAngle = index == lastNonzero
                ? 360 * 16 - drawnAngle
                : static_cast<int>(std::llround(static_cast<double>(values[index]) * 360.0 * 16.0
                                                 / static_cast<double>(totalBytes_)));
            const QColor base = segmentColor(segments[index]);
            const QColor fill = hoveredSegment_ == segments[index] ? base.lighter(118) : base;
            painter.setPen(Qt::NoPen);
            painter.setBrush(fill);
            painter.drawPie(bounds, startAngle, -spanAngle);
            startAngle -= spanAngle;
            drawnAngle += spanAngle;
        }

        painter.setPen(Qt::NoPen);
        painter.setBrush(palette().color(QPalette::Window));
        painter.drawEllipse(bounds.adjusted(bounds.width() * 0.11, bounds.height() * 0.11,
                                            -bounds.width() * 0.11, -bounds.height() * 0.11));
        if (zramCapacityBytes_ > 0) {
            const QRectF innerRing = bounds.adjusted(bounds.width() * 0.16, bounds.height() * 0.16,
                                                     -bounds.width() * 0.16, -bounds.height() * 0.16);
            QColor trackColor = palette().color(QPalette::Mid);
            trackColor.setAlpha(90);
            painter.setBrush(trackColor);
            painter.drawEllipse(innerRing);
            const double ratio = static_cast<double>(std::min(zramLogicalBytes_, zramCapacityBytes_))
                / static_cast<double>(zramCapacityBytes_);
            const QColor logicalColor = segmentColor(Segment::ZramLogical);
            painter.setBrush(hoveredSegment_ == Segment::ZramLogical ? logicalColor.lighter(118) : logicalColor);
            painter.drawPie(innerRing, 90 * 16, -static_cast<int>(std::llround(ratio * 360.0 * 16.0)));
        }
        painter.setBrush(palette().color(QPalette::Window));
        painter.drawEllipse(centerRect);

        const double usedPercent = static_cast<double>(usedBytes_) * 100.0
            / static_cast<double>(totalBytes_);
        QFont valueFont = font();
        valueFont.setPointSize(std::max(11, valueFont.pointSize() + 6));
        valueFont.setWeight(QFont::DemiBold);
        painter.setFont(valueFont);
        painter.setPen(textColor);
        const QFontMetrics valueMetrics(valueFont);
        QFont labelFont = font();
        labelFont.setPointSize(std::max(8, labelFont.pointSize() - 1));
        const QFontMetrics labelMetrics(labelFont);
        constexpr qreal labelGap = 6.0;
        const qreal textHeight = static_cast<qreal>(valueMetrics.height() + labelMetrics.height()) + labelGap;
        const qreal textTop = centerRect.center().y() - textHeight / 2.0;
        const QRectF valueRect(centerRect.left(), textTop, centerRect.width(), valueMetrics.height());
        painter.drawText(valueRect, Qt::AlignCenter,
                         QStringLiteral("%1%").arg(QString::number(usedPercent, 'f', 1)));
        painter.setFont(labelFont);
        painter.setPen(mutedColor);
        const QRectF labelRect(centerRect.left(), textTop + valueMetrics.height() + labelGap,
                               centerRect.width(), labelMetrics.height());
        painter.drawText(labelRect, Qt::AlignCenter, QStringLiteral("物理已用"));
    } else {
        painter.setPen(QPen(palette().color(QPalette::Mid), 8.0, Qt::SolidLine, Qt::RoundCap));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(bounds.adjusted(5, 5, -5, -5));
        painter.setPen(mutedColor);
        painter.drawText(bounds, Qt::AlignCenter, QStringLiteral("等待采样"));
    }

    const Segment legendSegments[] = {
        Segment::Available, Segment::Regular, Segment::Zram, Segment::ZramLogical};
    const QString legendNames[] = {
        QStringLiteral("估算可用"), zramStatisticsComplete_ ? QStringLiteral("其他已用") : QStringLiteral("物理已用"),
        QStringLiteral("zram 实际"), QStringLiteral("zram 逻辑")};
    const std::uint64_t legendValues[] = {
        availableBytes_, regularBytes_, zramRamBytes_, zramLogicalBytes_};

    for (int row = 0; row < 4; ++row) {
        const Segment segment = legendSegments[row];
        const QRectF itemRect = legendItemRect(row);
        const bool hovered = hoveredSegment_ == segment;
        constexpr qreal markerSize = 7.0;
        QFont nameFont = legendFont();
        nameFont.setWeight(hovered ? QFont::DemiBold : QFont::Normal);
        const QFontMetrics nameMetrics(nameFont);
        const qreal lineHeight = nameMetrics.height();
        painter.setPen(Qt::NoPen);
        painter.setBrush(segmentColor(segment));
        painter.drawRoundedRect(QRectF(itemRect.left(), itemRect.top() + (lineHeight - markerSize) / 2.0,
                                      markerSize, markerSize), 1.5, 1.5);
        painter.setFont(nameFont);
        painter.setPen(textColor);
        const QString name = legendNames[row];
        const QRectF nameRect(itemRect.left() + 13.0, itemRect.top(), itemRect.width() - 13.0, lineHeight);
        painter.drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                         nameMetrics.elidedText(name, Qt::ElideRight, static_cast<int>(nameRect.width())));

        const QFont amountFont = legendFont();
        painter.setFont(amountFont);
        painter.setPen(mutedColor);
        const QString amount = !hasSnapshot_ ? QStringLiteral("--")
            : !zramStatisticsComplete_ && row >= 2 ? QStringLiteral("统计不完整")
            : row == 2 && zramValueClamped_ ? QStringLiteral("%1 · 图形封顶").arg(formatBytes(zramReportedBytes_))
            : row == 3 ? QStringLiteral("%1 · %2").arg(formatBytes(zramLogicalBytes_),
                                                      formatPercent(zramLogicalBytes_, zramCapacityBytes_))
                       : QStringLiteral("%1 · %2").arg(formatBytes(legendValues[row]),
                                                      formatPercent(legendValues[row], totalBytes_));
        const QRectF amountRect(itemRect.left() + 13.0, itemRect.top() + lineHeight + 2.0,
                                itemRect.width() - 13.0, lineHeight);
        painter.drawText(amountRect, Qt::AlignLeft | Qt::AlignVCenter,
                         QFontMetrics(amountFont).elidedText(amount, Qt::ElideRight, static_cast<int>(amountRect.width())));
    }
    painter.setFont(legendFont());
    painter.setPen(mutedColor);
    const QRectF caption(0.0, legendRect().bottom() + 10.0, width(), captionHeight() - 10.0);
    painter.drawText(caption, Qt::AlignCenter, zramStatisticsComplete_
                         ? QStringLiteral("外圈 RAM · 内圈 zram 逻辑使用率")
                         : QStringLiteral("zram 统计不完整 · 无法单独分摊"));
}

void MemoryPieChart::mouseMoveEvent(QMouseEvent* event)
{
    Segment segment = segmentAt(event->position());
    const Segment legendSegments[] = {
        Segment::Available, Segment::Regular, Segment::Zram, Segment::ZramLogical};
    for (int row = 0; row < 4; ++row) {
        if (legendItemRect(row).contains(event->position())) {
            segment = legendSegments[row];
            break;
        }
    }
    if (segment != hoveredSegment_) {
        hoveredSegment_ = segment;
        update();
        updateTooltip();
    } else if (segment != Segment::None) {
        QToolTip::showText(event->globalPosition().toPoint(), tooltipFor(segment), this);
    }
    QWidget::mouseMoveEvent(event);
}

void MemoryPieChart::leaveEvent(QEvent* event)
{
    hoveredSegment_ = Segment::None;
    QToolTip::hideText();
    update();
    QWidget::leaveEvent(event);
}

void MemoryPieChart::updateTooltip()
{
    if (hoveredSegment_ != Segment::None) {
        setToolTip(tooltipFor(hoveredSegment_));
    } else if (hasSnapshot_) {
        setToolTip(tooltipFor(Segment::None));
    } else {
        setToolTip(QStringLiteral("等待有效的物理内存采样"));
    }
}

} // namespace memonitor
