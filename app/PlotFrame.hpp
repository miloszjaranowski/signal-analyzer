#pragma once

#include <QRectF>
#include <QString>

class QPainter;
class QPalette;
class QWidget;

// Plot chrome shared by PlotWidget and SpectrogramWidget: margins, grid, tick
// labels, title and border. Using the same margins everywhere keeps the
// spectrum and the spectrogram aligned pixel for pixel.
namespace plot {

struct Axes {
    double x0 = 0.0;       // value at the left edge
    double x1 = 1.0;       // value at the right edge
    double yBottom = 0.0;  // value at the bottom edge
    double yTop = 1.0;     // value at the top edge
    QString title;
    QString xUnit;
    QString yUnit;
    bool grid = true;
};

// Data area of `widget`, inside the margins.
QRectF dataArea(const QWidget& widget);

// Draws everything except the data. Leaves the painter's pen and font changed.
void drawAxes(QPainter& p, const QPalette& pal, const QRectF& area, const Axes& axes);

}  // namespace plot
