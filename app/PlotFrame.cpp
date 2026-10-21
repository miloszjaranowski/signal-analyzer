#include "PlotFrame.hpp"

#include <QPainter>
#include <QPalette>
#include <QWidget>

#include <cmath>

namespace plot {

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

QRectF dataArea(const QWidget& widget) {
    return QRectF(kLeft, kTop, widget.width() - kLeft - kRight, widget.height() - kTop - kBottom);
}

void drawAxes(QPainter& p, const QPalette& pal, const QRectF& area, const Axes& axes) {
    if (axes.grid) {
        QPen gridPen(pal.color(QPalette::Mid));
        gridPen.setStyle(Qt::DotLine);
        p.setPen(gridPen);
        for (int i = 0; i <= kGridDivisions; ++i) {
            const double fx = area.left() + area.width() * i / kGridDivisions;
            const double fy = area.top() + area.height() * i / kGridDivisions;
            p.drawLine(QPointF(fx, area.top()), QPointF(fx, area.bottom()));
            p.drawLine(QPointF(area.left(), fy), QPointF(area.right(), fy));
        }
    }

    p.setPen(pal.color(QPalette::Text));
    const QFontMetrics fm = p.fontMetrics();
    for (int i = 0; i <= kGridDivisions; ++i) {
        const double xv = axes.x0 + (axes.x1 - axes.x0) * i / kGridDivisions;
        const double yv = axes.yTop + (axes.yBottom - axes.yTop) * i / kGridDivisions;
        const QString xs = formatTick(xv);
        const QString ys = formatTick(yv);
        const double fx = area.left() + area.width() * i / kGridDivisions;
        const double fy = area.top() + area.height() * i / kGridDivisions;
        p.drawText(QPointF(fx - fm.horizontalAdvance(xs) / 2.0, area.bottom() + fm.height()), xs);
        p.drawText(QPointF(area.left() - fm.horizontalAdvance(ys) - 6, fy + fm.ascent() / 2.0), ys);
    }

    const QFont base = p.font();
    QFont bold = base;
    bold.setBold(true);
    p.setFont(bold);
    p.drawText(QPointF(area.left(), area.top() - 8), axes.title);
    p.setFont(base);
    const QString units = QStringLiteral("x: %1   y: %2").arg(axes.xUnit, axes.yUnit);
    p.drawText(QPointF(area.right() - fm.horizontalAdvance(units), area.top() - 8), units);

    p.setPen(pal.color(QPalette::Mid));
    p.drawRect(area);
}

}  // namespace plot
