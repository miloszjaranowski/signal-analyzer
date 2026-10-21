#pragma once

#include <QImage>
#include <QWidget>

#include <deque>
#include <span>
#include <vector>

// Scrolling spectrogram (waterfall): one row per analysis frame, newest on top,
// frequency on x with the same margins as PlotWidget so it lines up with the
// spectrum plot. Rows are stored in dB; each new row is rendered once into a
// ring of image lines (max-decimated per pixel column, so narrow tones never
// disappear). The image is only rebuilt when the width, frequency range or
// dB range changes.
class SpectrogramWidget : public QWidget {
    Q_OBJECT

public:
    explicit SpectrogramWidget(QWidget* parent = nullptr);

    // `amplitude` is a single-sided amplitude spectrum (sa::AnalysisFrame::spectrum),
    // `rowSeconds` the time between consecutive rows (hop size / sample rate).
    // A different bin count or bin width starts a new history.
    void addSpectrum(std::span<const float> amplitude, double binHz, double rowSeconds);
    void clear();
    void setXMax(double xMax);  // 0 = Nyquist
    void setDbRange(double lo, double hi);

    QSize sizeHint() const override { return {800, 260}; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    static constexpr int kHistoryRows = 300;

    double visibleMaxHz() const;
    void rebuildImage(int columns);
    void renderRow(const std::vector<float>& db);
    void drawColorBar(QPainter& p, const QRectF& area) const;

    std::deque<std::vector<float>> rows_;  // dB, newest at back
    double binHz_ = 0.0;
    double rowSeconds_ = 0.0;
    double xMax_ = 0.0;
    double dbLo_ = -100.0;
    double dbHi_ = 10.0;

    // Ring of rendered rows: the newest is at line imageHead_, older ones follow
    // downwards and wrap around to line 0.
    QImage image_;
    int imageHead_ = 0;
    bool imageValid_ = false;
};
