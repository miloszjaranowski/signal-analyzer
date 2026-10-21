#pragma once

#include <QColor>
#include <QString>
#include <QWidget>

#include <vector>

// Minimal, fast line plot drawn with QPainter (no QtCharts dependency).
// Large series are decimated to min/max per pixel column, so a 2048-point
// waveform or 8193-bin spectrum renders in well under a millisecond and
// short spikes are never lost.
class PlotWidget : public QWidget {
    Q_OBJECT

public:
    // Vertical line with a short label near the top, e.g. a fault harmonic.
    struct Marker {
        double x = 0.0;
        QString label;
        QColor color = QColor(230, 140, 40);  // reads on light and dark palettes
        Qt::PenStyle style = Qt::DashLine;
        int labelRow = 0;  // 0 = first text line under the top edge, 1 = second, ...
    };

    explicit PlotWidget(QString title, QString xUnit, QString yUnit, QWidget* parent = nullptr);

    // y[i] is plotted at x = xStart + i * xStep.
    void setData(std::vector<float> y, double xStart, double xStep);
    void setMarkers(std::vector<Marker> markers);
    void setTitle(const QString& title);
    void setYUnit(const QString& unit);
    void setFixedYRange(double lo, double hi);
    void setAutoScale(bool symmetric);
    void setXMax(double xMax);  // 0 = full data range

    QSize sizeHint() const override { return {800, 260}; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void updateAutoRange();

    QString title_;
    QString xUnit_;
    QString yUnit_;
    std::vector<float> y_;
    std::vector<Marker> markers_;
    double xStart_ = 0.0;
    double xStep_ = 1.0;
    double xMax_ = 0.0;

    bool autoScale_ = true;
    bool symmetric_ = false;
    double yLo_ = -1.0;
    double yHi_ = 1.0;
};
