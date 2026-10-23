#include "FatiguePanel.hpp"

#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <vector>

#include "PlotWidget.hpp"
#include "sa/Pipeline.hpp"

namespace {

QDoubleSpinBox* makeSpin(double lo, double hi, double value, int decimals, const QString& suffix) {
    auto* spin = new QDoubleSpinBox;
    spin->setRange(lo, hi);
    spin->setDecimals(decimals);
    spin->setValue(value);
    spin->setSuffix(suffix);
    return spin;
}

}  // namespace

FatiguePanel::FatiguePanel(QWidget* parent) : QWidget(parent) {
    const sa::SnCurve defaults;

    scaleSpin_ = makeSpin(0.001, 1e6, 1.0, 3, tr(" MPa/unit"));
    scaleSpin_->setToolTip(tr("Stress per unit of the signal, e.g. from a strain gauge or an FE "
                              "transfer function. Without it the result is relative damage."));
    slopeSpin_ = makeSpin(1.0, 20.0, defaults.slope, 1, QString());
    refRangeSpin_ = makeSpin(0.1, 1e5, defaults.refRange, 1, tr(" MPa"));
    refCyclesSpin_ = makeSpin(1e3, 1e12, defaults.refCycles, 0, QString());

    auto* inputBox = new QGroupBox(tr("Calibration and S-N curve"));
    auto* inputs = new QFormLayout(inputBox);
    inputs->addRow(tr("Stress scale:"), scaleSpin_);
    inputs->addRow(tr("Slope m:"), slopeSpin_);
    inputs->addRow(tr("Reference range:"), refRangeSpin_);
    inputs->addRow(tr("Cycles at reference:"), refCyclesSpin_);
    auto* note = new QLabel(tr("N = N_ref · (Δσ_ref / Δσ)^m, no endurance limit. The signal "
                               "is used as is: without a stress calibration D is relative."));
    note->setWordWrap(true);
    note->setEnabled(false);  // muted secondary text
    inputs->addRow(note);

    timeLabel_ = new QLabel;
    cyclesLabel_ = new QLabel;
    maxRangeLabel_ = new QLabel;
    damageLabel_ = new QLabel;
    rateLabel_ = new QLabel;
    lifeLabel_ = new QLabel;
    QFont bold = lifeLabel_->font();
    bold.setBold(true);
    lifeLabel_->setFont(bold);

    auto* resultBox = new QGroupBox(tr("Palmgren-Miner"));
    auto* results = new QFormLayout(resultBox);
    results->addRow(tr("Signal time:"), timeLabel_);
    results->addRow(tr("Rainflow cycles:"), cyclesLabel_);
    results->addRow(tr("Max stress range:"), maxRangeLabel_);
    results->addRow(tr("Damage D:"), damageLabel_);
    results->addRow(tr("Damage per hour:"), rateLabel_);
    results->addRow(tr("Estimated life:"), lifeLabel_);
    auto* resetButton = new QPushButton(tr("Reset counting"));
    connect(resetButton, &QPushButton::clicked, this, &FatiguePanel::resetRequested);
    results->addRow(resetButton);

    damagePlot_ = new PlotWidget(tr("Damage by stress range"), tr("MPa"), tr("% of D"));
    damagePlot_->setAutoScale(/*symmetric=*/false);
    // Fill the dock instead of widening it to the plot's 800 px size hint.
    damagePlot_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(inputBox);
    layout->addWidget(resultBox);
    layout->addWidget(damagePlot_, 1);

    for (QDoubleSpinBox* spin : {scaleSpin_, slopeSpin_, refRangeSpin_, refCyclesSpin_}) {
        connect(spin, &QDoubleSpinBox::valueChanged, this, &FatiguePanel::changed);
    }
}

sa::SnCurve FatiguePanel::snCurve() const {
    return {slopeSpin_->value(), refRangeSpin_->value(), refCyclesSpin_->value()};
}

void FatiguePanel::showFrame(const sa::AnalysisFrame& frame) {
    const sa::RangeHistogram& h = frame.rainflow;
    const sa::SnCurve curve = snCurve();
    const double scale = scaleSpin_->value();
    const double damage = sa::minerDamage(h, curve, scale);

    timeLabel_->setText(QString::fromStdString(sa::formatDuration(frame.fatigueSeconds)));
    cyclesLabel_->setText(QString::number(h.totalCycles(), 'f', 0));
    maxRangeLabel_->setText(QStringLiteral("%1 MPa").arg(h.maxRange() * scale, 0, 'g', 4));
    damageLabel_->setText(QString::number(damage, 'g', 3));
    if (damage > 0.0 && frame.fatigueSeconds > 0.0) {
        rateLabel_->setText(QString::number(damage / frame.fatigueSeconds * 3600.0, 'g', 3));
        lifeLabel_->setText(QString::fromStdString(sa::formatDuration(frame.fatigueSeconds / damage)));
    } else {
        rateLabel_->setText(QStringLiteral("—"));
        lifeLabel_->setText(QStringLiteral("—"));
    }

    // Share of the total damage per range bin, up to the largest range seen.
    if (damage <= 0.0 || h.binWidth() <= 0.0) {
        damagePlot_->setData({}, 0.0, 1.0);
        return;
    }
    const auto usedBins = std::min(h.counts().size(),
                                   static_cast<std::size_t>(h.maxRange() / h.binWidth()) + 1);
    std::vector<float> share(usedBins);
    for (std::size_t i = 0; i < usedBins; ++i) {
        share[i] = static_cast<float>(
            100.0 * sa::cycleDamage(h.meanRange(i) * scale, h.counts()[i], curve) / damage);
    }
    damagePlot_->setData(std::move(share), 0.5 * h.binWidth() * scale, h.binWidth() * scale);
}
