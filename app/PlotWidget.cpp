#include "PlotWidget.hpp"

#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kLeft = 64;
constexpr int kRight = 14;
constexpr int kTop = 26;
constexpr int kBottom = 30;
constexpr int kGridDivisions = 5;

QString formatTick(double v) {
    const double a = std::abs(v);
    if (a >= 1000.0) return QString::number(v, 'f', 0);
    if (a >= 10.0) return QString::number(v, 'f', 1);
    if (a >= 0.1 || a == 0.0) return QString::number(v, 'f', 2);
    return QString::number(v, 'g', 2);
}

}  // namespace

PlotWidget::PlotWidget(QString title, QString xUnit, QString yUnit, QWidget* parent)
    : QWidget(parent), title_(std::move(title)), xUnit_(std::move(xUnit)), yUnit_(std::move(yUnit)) {
    setMinimumHeight(180);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void PlotWidget::setData(std::vector<float> y, double xStart, double xStep) {
    y_ = std::move(y);
    xStart_ = xStart;
    xStep_ = xStep > 0.0 ? xStep : 1.0;
    if (autoScale_) {
        updateAutoRange();
    }
    update();
}

void PlotWidget::setYUnit(const QString& unit) {
    yUnit_ = unit;
    update();
}

void PlotWidget::setFixedYRange(double lo, double hi) {
    autoScale_ = false;
    yLo_ = lo;
    yHi_ = hi;
    update();
}

void PlotWidget::setAutoScale(bool symmetric) {
    autoScale_ = true;
    symmetric_ = symmetric;
    updateAutoRange();
    update();
}

void PlotWidget::setXMax(double xMax) {
    xMax_ = xMax;
    update();
}

void PlotWidget::updateAutoRange() {
    if (y_.empty()) {
        return;
    }
    const auto [minIt, maxIt] = std::minmax_element(y_.begin(), y_.end());
    double lo = *minIt;
    double hi = *maxIt;
    if (symmetric_) {
        const double m = std::max(std::abs(lo), std::abs(hi));
        lo = -m;
        hi = m;
    }
    const double margin = std::max(1e-6, (hi - lo) * 0.1);
    lo -= symmetric_ ? margin : 0.0;
    hi += margin;

    // Expand immediately, shrink slowly: avoids a jittery axis.
    constexpr double kShrink = 0.05;
    yLo_ = lo < yLo_ ? lo : yLo_ + (lo - yLo_) * kShrink;
    yHi_ = hi > yHi_ ? hi : yHi_ + (hi - yHi_) * kShrink;
}

void PlotWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    const QPalette& pal = palette();
    p.fillRect(rect(), pal.color(QPalette::Base));

    const QRectF area(kLeft, kTop, width() - kLeft - kRight, height() - kTop - kBottom);
    if (area.width() < 10 || area.height() < 10) {
        return;
    }

    const double xLast = y_.empty() ? 1.0 : xStart_ + xStep_ * double(y_.size() - 1);
    const double x0 = xStart_;
    const double x1 = (xMax_ > x0 && xMax_ < xLast) ? xMax_ : std::max(xLast, x0 + 1e-9);
    const double y0 = yLo_;
    const double y1 = yHi_ > yLo_ ? yHi_ : yLo_ + 1.0;

    auto toPx = [&](double x, double y) {
        return QPointF(area.left() + (x - x0) / (x1 - x0) * area.width(),
                       area.bottom() - (y - y0) / (y1 - y0) * area.height());
    };

    // Grid and tick labels.
    QPen gridPen(pal.color(QPalette::Mid));
    gridPen.setStyle(Qt::DotLine);
    p.setPen(gridPen);
    for (int i = 0; i <= kGridDivisions; ++i) {
        const double fx = area.left() + area.width() * i / kGridDivisions;
        const double fy = area.top() + area.height() * i / kGridDivisions;
        p.drawLine(QPointF(fx, area.top()), QPointF(fx, area.bottom()));
        p.drawLine(QPointF(area.left(), fy), QPointF(area.right(), fy));
    }

    p.setPen(pal.color(QPalette::Text));
    const QFontMetrics fm = p.fontMetrics();
    for (int i = 0; i <= kGridDivisions; ++i) {
        const double xv = x0 + (x1 - x0) * i / kGridDivisions;
        const double yv = y1 - (y1 - y0) * i / kGridDivisions;
        const QString xs = formatTick(xv);
        const QString ys = formatTick(yv);
        const double fx = area.left() + area.width() * i / kGridDivisions;
        const double fy = area.top() + area.height() * i / kGridDivisions;
        p.drawText(QPointF(fx - fm.horizontalAdvance(xs) / 2.0, area.bottom() + fm.height()), xs);
        p.drawText(QPointF(area.left() - fm.horizontalAdvance(ys) - 6, fy + fm.ascent() / 2.0), ys);
    }

    QFont bold = p.font();
    bold.setBold(true);
    p.setFont(bold);
    p.drawText(QPointF(area.left(), kTop - 8), title_);
    p.setFont(font());
    const QString units = QStringLiteral("x: %1   y: %2").arg(xUnit_, yUnit_);
    p.drawText(QPointF(area.right() - fm.horizontalAdvance(units), kTop - 8), units);

    p.setPen(pal.color(QPalette::Mid));
    p.drawRect(area);

    if (y_.size() < 2) {
        p.setPen(pal.color(QPalette::PlaceholderText));
        p.drawText(area, Qt::AlignCenter, tr("Waiting for data..."));
        return;
    }

    // Visible index range.
    const auto lastIdx = static_cast<std::ptrdiff_t>(
        std::min<double>(double(y_.size() - 1), std::ceil((x1 - x0) / xStep_)));
    const std::ptrdiff_t count = lastIdx + 1;
    const int columns = static_cast<int>(area.width());

    QPolygonF line;
    if (count <= 2 * columns) {
        line.reserve(static_cast<int>(count));
        for (std::ptrdiff_t i = 0; i < count; ++i) {
            line << toPx(x0 + double(i) * xStep_, y_[static_cast<std::size_t>(i)]);
        }
    } else {
        // Min/max decimation per pixel column.
        line.reserve(columns * 2);
        for (int c = 0; c < columns; ++c) {
            const auto begin = static_cast<std::size_t>(count * c / columns);
            const auto end = std::max(begin + 1, static_cast<std::size_t>(count * (c + 1) / columns));
            const auto [mn, mx] = std::minmax_element(y_.begin() + static_cast<std::ptrdiff_t>(begin),
                                                      y_.begin() + static_cast<std::ptrdiff_t>(end));
            const double x = x0 + double(begin) * xStep_;
            line << toPx(x, *mn) << toPx(x, *mx);
        }
    }

    p.setClipRect(area);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen dataPen(pal.color(QPalette::Highlight));
    dataPen.setWidthF(1.3);
    p.setPen(dataPen);
    p.drawPolyline(line);
}
