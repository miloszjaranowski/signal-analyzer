#pragma once

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
    explicit PlotWidget(QString title, QString xUnit, QString yUnit, QWidget* parent = nullptr);

    // y[i] is plotted at x = xStart + i * xStep.
    void setData(std::vector<float> y, double xStart, double xStep);
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
    double xStart_ = 0.0;
    double xStep_ = 1.0;
    double xMax_ = 0.0;

    bool autoScale_ = true;
    bool symmetric_ = false;
    double yLo_ = -1.0;
    double yHi_ = 1.0;
};
