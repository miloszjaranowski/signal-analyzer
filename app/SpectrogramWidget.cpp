#include "SpectrogramWidget.hpp"

#include <QPainter>

#include <algorithm>
#include <array>
#include <cmath>

#include "PlotFrame.hpp"
#include "sa/Dsp.hpp"

namespace {

// Control points of an "inferno"-like perceptual colormap: dark = quiet, bright = loud.
struct ColorStop {
    double t;
    int r, g, b;
};
constexpr std::array<ColorStop, 9> kStops{{{0.000, 0, 0, 4},
                                           {0.125, 31, 12, 72},
                                           {0.250, 85, 15, 109},
                                           {0.375, 136, 34, 106},
                                           {0.500, 186, 54, 85},
                                           {0.625, 227, 89, 51},
                                           {0.750, 249, 140, 10},
                                           {0.875, 249, 201, 50},
                                           {1.000, 252, 255, 164}}};

std::array<QRgb, 256> makeColormap() {
    std::array<QRgb, 256> lut{};
    std::size_t s = 0;
    for (std::size_t i = 0; i < lut.size(); ++i) {
        const double t = double(i) / double(lut.size() - 1);
        while (s + 2 < kStops.size() && t > kStops[s + 1].t) {
            ++s;
        }
        const ColorStop& a = kStops[s];
        const ColorStop& b = kStops[s + 1];
        const double f = (t - a.t) / (b.t - a.t);
        auto mix = [f](int x, int y) { return static_cast<int>(std::lround(x + (y - x) * f)); };
        lut[i] = qRgb(mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b));
    }
    return lut;
}

const std::array<QRgb, 256> kColormap = makeColormap();

}  // namespace

SpectrogramWidget::SpectrogramWidget(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(180);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void SpectrogramWidget::addSpectrum(std::span<const float> amplitude, double binHz,
                                    double rowSeconds) {
    if (amplitude.size() < 2 || binHz <= 0.0) {
        return;
    }
    if (!rows_.empty() && (amplitude.size() != rows_.back().size() || binHz != binHz_)) {
        clear();  // new FFT size or sample rate: old rows are on a different axis
    }
    binHz_ = binHz;
    rowSeconds_ = rowSeconds;

    // Recycle the oldest row's storage once the history is full.
    std::vector<float> db;
    if (rows_.size() == static_cast<std::size_t>(kHistoryRows)) {
        db = std::move(rows_.front());
        rows_.pop_front();
    }
    db.resize(amplitude.size());
    std::transform(amplitude.begin(), amplitude.end(), db.begin(), sa::amplitudeToDb);
    rows_.push_back(std::move(db));

    if (imageValid_) {
        renderRow(rows_.back());
    }
    update();
}

void SpectrogramWidget::clear() {
    rows_.clear();
    imageValid_ = false;
    update();
}

void SpectrogramWidget::setXMax(double xMax) {
    xMax_ = xMax;
    imageValid_ = false;
    update();
}

void SpectrogramWidget::setDbRange(double lo, double hi) {
    dbLo_ = lo;
    dbHi_ = hi > lo ? hi : lo + 1.0;
    imageValid_ = false;
    update();
}

double SpectrogramWidget::visibleMaxHz() const {
    const double nyquist = rows_.empty() ? 0.0 : binHz_ * double(rows_.back().size() - 1);
    return (xMax_ > 0.0 && xMax_ < nyquist) ? xMax_ : nyquist;
}

void SpectrogramWidget::rebuildImage(int columns) {
    if (image_.width() != columns) {
        image_ = QImage(columns, kHistoryRows, QImage::Format_RGB32);
    }
    image_.fill(kColormap.front());
    imageHead_ = 0;
    for (const auto& row : rows_) {
        renderRow(row);  // oldest first, so the newest ends up on top
    }
    imageValid_ = true;
}

void SpectrogramWidget::renderRow(const std::vector<float>& db) {
    imageHead_ = (imageHead_ + kHistoryRows - 1) % kHistoryRows;
    auto* line = reinterpret_cast<QRgb*>(image_.scanLine(imageHead_));

    // Pixel column c covers bins [c * binsPerColumn, (c + 1) * binsPerColumn),
    // matching how PlotWidget maps x to pixels.
    const int columns = image_.width();
    const double binsPerColumn = visibleMaxHz() / binHz_ / columns;
    const std::size_t bins = db.size();
    const double scale = double(kColormap.size() - 1) / (dbHi_ - dbLo_);
    for (int c = 0; c < columns; ++c) {
        const auto begin = std::min(bins - 1, static_cast<std::size_t>(c * binsPerColumn));
        const auto end =
            std::clamp(static_cast<std::size_t>((c + 1) * binsPerColumn), begin + 1, bins);
        const float v = *std::max_element(db.begin() + static_cast<std::ptrdiff_t>(begin),
                                          db.begin() + static_cast<std::ptrdiff_t>(end));
        const int idx = std::clamp(static_cast<int>((v - dbLo_) * scale), 0,
                                   static_cast<int>(kColormap.size() - 1));
        line[c] = kColormap[static_cast<std::size_t>(idx)];
    }
}

void SpectrogramWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    const QPalette& pal = palette();
    p.fillRect(rect(), pal.color(QPalette::Base));

    const QRectF area = plot::dataArea(*this);
    if (area.width() < 10 || area.height() < 10) {
        return;
    }

    if (!rows_.empty()) {
        const int columns = static_cast<int>(area.width());
        if (!imageValid_ || image_.width() != columns) {
            rebuildImage(columns);
        }
        // Lines [head, end) hold the newest rows, [0, head) the oldest ones.
        const double rowHeight = area.height() / kHistoryRows;
        const int newer = kHistoryRows - imageHead_;
        p.drawImage(QRectF(area.left(), area.top(), area.width(), newer * rowHeight), image_,
                    QRectF(0, imageHead_, columns, newer));
        if (imageHead_ > 0) {
            p.drawImage(QRectF(area.left(), area.top() + newer * rowHeight, area.width(),
                               imageHead_ * rowHeight),
                        image_, QRectF(0, 0, columns, imageHead_));
        }
    }

    const double span = kHistoryRows * rowSeconds_;
    plot::drawAxes(p, pal, area,
                   {.x0 = 0.0,
                    .x1 = rows_.empty() ? 1.0 : visibleMaxHz(),
                    .yBottom = span > 0.0 ? span : 1.0,
                    .yTop = 0.0,
                    .title = tr("Spectrogram"),
                    .xUnit = tr("Hz"),
                    .yUnit = tr("s ago"),
                    .grid = false});
    drawColorBar(p, area);

    if (rows_.empty()) {
        p.setPen(pal.color(QPalette::PlaceholderText));
        p.drawText(area, Qt::AlignCenter, tr("Waiting for data..."));
    }
}

void SpectrogramWidget::drawColorBar(QPainter& p, const QRectF& area) const {
    static const QImage strip = [] {
        QImage img(static_cast<int>(kColormap.size()), 1, QImage::Format_RGB32);
        std::copy(kColormap.begin(), kColormap.end(), reinterpret_cast<QRgb*>(img.scanLine(0)));
        return img;
    }();

    // Centred in the header line, between the title and the units.
    constexpr double kWidth = 120.0;
    constexpr double kHeight = 8.0;
    const double baseline = area.top() - 8;
    const QRectF bar(area.center().x() - kWidth / 2, baseline - kHeight, kWidth, kHeight);
    p.drawImage(bar, strip);

    const QString lo = QString::number(dbLo_, 'f', 0);
    const QString hi = tr("%1 dB").arg(dbHi_, 0, 'f', 0);
    const QFontMetrics fm = p.fontMetrics();
    p.setPen(palette().color(QPalette::Text));
    p.drawText(QPointF(bar.left() - 6 - fm.horizontalAdvance(lo), baseline), lo);
    p.drawText(QPointF(bar.right() + 6, baseline), hi);
}
