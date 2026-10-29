#include "FatiguePanel.hpp"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <numeric>
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

QString mpa(double value) { return QStringLiteral("%1 MPa").arg(value, 0, 'f', 1); }

}  // namespace

FatiguePanel::FatiguePanel(QWidget* parent) : QWidget(parent) {
    const sa::SnCurve sn;
    const sa::MeanStressCorrection mean;

    // --- Calibration and S-N curve ------------------------------------------------
    scaleSpin_ = makeSpin(0.001, 1e6, 1.0, 3, tr(" MPa/unit"));
    scaleSpin_->setToolTip(tr("Stress per unit of the signal, e.g. from a strain gauge or an FE "
                              "transfer function. Without it the result is relative damage."));
    shapeCombo_ = new QComboBox;
    shapeCombo_->addItem(tr("Two slopes (EN 1993-1-9)"), int(sa::SnCurve::Shape::TwoSlope));
    shapeCombo_->addItem(tr("Endurance limit"), int(sa::SnCurve::Shape::EnduranceLimit));
    shapeCombo_->addItem(tr("Single slope, no limit"), int(sa::SnCurve::Shape::SingleSlope));
    slopeSpin_ = makeSpin(1.0, 20.0, sn.slope, 1, QString());
    refRangeSpin_ = makeSpin(0.1, 1e5, sn.refRange, 1, tr(" MPa"));
    refRangeSpin_->setToolTip(tr("Detail category: stress range at the reference number of cycles"));
    refCyclesSpin_ = makeSpin(1e3, 1e12, sn.refCycles, 0, QString());
    kneeCyclesSpin_ = makeSpin(1e3, 1e12, sn.kneeCycles, 0, QString());
    slope2Spin_ = makeSpin(1.0, 30.0, sn.slope2, 1, QString());
    cutoffCyclesSpin_ = makeSpin(1e3, 1e14, sn.cutoffCycles, 0, QString());
    limitsLabel_ = new QLabel;

    auto* curveBox = new QGroupBox(tr("Calibration and S-N curve"));
    auto* curve = new QFormLayout(curveBox);
    curve->addRow(tr("Stress scale:"), scaleSpin_);
    curve->addRow(tr("Shape:"), shapeCombo_);
    curve->addRow(tr("Slope m:"), slopeSpin_);
    curve->addRow(tr("Detail category Δσ_C:"), refRangeSpin_);
    curve->addRow(tr("  at cycles:"), refCyclesSpin_);
    curve->addRow(tr("Knee N_D:"), kneeCyclesSpin_);
    curve->addRow(tr("Slope m₂ (below knee):"), slope2Spin_);
    curve->addRow(tr("Cut-off N_L:"), cutoffCyclesSpin_);
    curve->addRow(tr("Limits:"), limitsLabel_);

    // --- Mean stress ----------------------------------------------------------------
    meanCombo_ = new QComboBox;
    meanCombo_->addItem(tr("None (welded joints)"), int(sa::MeanStressCorrection::Method::None));
    meanCombo_->addItem(tr("Goodman"), int(sa::MeanStressCorrection::Method::Goodman));
    meanCombo_->addItem(tr("Gerber"), int(sa::MeanStressCorrection::Method::Gerber));
    meanCombo_->setToolTip(tr("Tensile mean stress shortens life; compressive means are not "
                              "credited. Welded joints carry high residual stresses, so EN 1993-1-9 "
                              "and IIW apply no correction."));
    strengthSpin_ = makeSpin(1.0, 1e5, mean.ultimateStrength, 0, tr(" MPa"));

    auto* meanBox = new QGroupBox(tr("Mean-stress correction"));
    auto* meanForm = new QFormLayout(meanBox);
    meanForm->addRow(tr("Method:"), meanCombo_);
    meanForm->addRow(tr("Ultimate strength R_m:"), strengthSpin_);

    // --- Results ----------------------------------------------------------------------
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
    layout->addWidget(curveBox);
    layout->addWidget(meanBox);
    layout->addWidget(resultBox);
    layout->addWidget(damagePlot_, 1);

    for (QDoubleSpinBox* spin : {scaleSpin_, slopeSpin_, refRangeSpin_, refCyclesSpin_,
                                 kneeCyclesSpin_, slope2Spin_, cutoffCyclesSpin_, strengthSpin_}) {
        connect(spin, &QDoubleSpinBox::valueChanged, this, &FatiguePanel::updateInputs);
    }
    for (QComboBox* combo : {shapeCombo_, meanCombo_}) {
        connect(combo, &QComboBox::currentIndexChanged, this, &FatiguePanel::updateInputs);
    }
    updateInputs();
}

sa::SnCurve FatiguePanel::snCurve() const {
    sa::SnCurve c;
    c.shape = static_cast<sa::SnCurve::Shape>(shapeCombo_->currentData().toInt());
    c.slope = slopeSpin_->value();
    c.refRange = refRangeSpin_->value();
    c.refCycles = refCyclesSpin_->value();
    c.kneeCycles = kneeCyclesSpin_->value();
    c.slope2 = slope2Spin_->value();
    c.cutoffCycles = cutoffCyclesSpin_->value();
    return c;
}

sa::MeanStressCorrection FatiguePanel::meanStressCorrection() const {
    sa::MeanStressCorrection c;
    c.method = static_cast<sa::MeanStressCorrection::Method>(meanCombo_->currentData().toInt());
    c.ultimateStrength = strengthSpin_->value();
    return c;
}

void FatiguePanel::updateInputs() {
    const sa::SnCurve c = snCurve();
    using Shape = sa::SnCurve::Shape;
    kneeCyclesSpin_->setEnabled(c.shape != Shape::SingleSlope);
    slope2Spin_->setEnabled(c.shape == Shape::TwoSlope);
    cutoffCyclesSpin_->setEnabled(c.shape == Shape::TwoSlope);
    strengthSpin_->setEnabled(meanStressCorrection().method != sa::MeanStressCorrection::Method::None);

    switch (c.shape) {
        case Shape::TwoSlope:
            limitsLabel_->setText(tr("Δσ_D = %1, Δσ_L = %2").arg(mpa(sa::kneeRange(c)), mpa(sa::cutoffRange(c))));
            break;
        case Shape::EnduranceLimit:
            limitsLabel_->setText(tr("Δσ_D = %1").arg(mpa(sa::kneeRange(c))));
            break;
        case Shape::SingleSlope:
            limitsLabel_->setText(tr("none: every cycle counts"));
            break;
    }
    emit changed();
}

void FatiguePanel::showFrame(const sa::AnalysisFrame& frame) {
    const sa::RainflowMatrix& matrix = frame.rainflow;
    const sa::SnCurve curve = snCurve();
    const double scale = scaleSpin_->value();
    const std::vector<double> perRange = sa::damageByRange(matrix, curve, meanStressCorrection(), scale);
    const double damage = std::accumulate(perRange.begin(), perRange.end(), 0.0);

    timeLabel_->setText(QString::fromStdString(sa::formatDuration(frame.fatigueSeconds)));
    cyclesLabel_->setText(QString::number(matrix.totalCycles(), 'f', 0));
    maxRangeLabel_->setText(QStringLiteral("%1 MPa").arg(matrix.maxRange() * scale, 0, 'g', 4));
    if (damage > 0.0 && frame.fatigueSeconds > 0.0) {
        damageLabel_->setText(QString::number(damage, 'g', 3));
        rateLabel_->setText(QString::number(damage / frame.fatigueSeconds * 3600.0, 'g', 3));
        lifeLabel_->setText(QString::fromStdString(sa::formatDuration(frame.fatigueSeconds / damage)));
    } else {
        const bool belowLimit = matrix.totalCycles() > 0.0 && sa::cutoffRange(curve) > 0.0;
        damageLabel_->setText(belowLimit ? tr("0 (all ranges below the limit)") : QStringLiteral("0"));
        rateLabel_->setText(QStringLiteral("—"));
        lifeLabel_->setText(belowLimit ? tr("unlimited") : QStringLiteral("—"));
    }

    // Share of the total damage per range bin, up to the largest range seen, with
    // the S-N limits marked.
    std::vector<PlotWidget::Marker> limits;
    if (curve.shape != sa::SnCurve::Shape::SingleSlope) {
        limits.push_back({sa::kneeRange(curve), QStringLiteral("Δσ_D")});
    }
    if (curve.shape == sa::SnCurve::Shape::TwoSlope) {
        limits.push_back({sa::cutoffRange(curve), QStringLiteral("Δσ_L"), QColor(230, 140, 40), Qt::DashLine, 1});
    }
    damagePlot_->setMarkers(std::move(limits));
    if (damage <= 0.0 || matrix.rangeBinWidth() <= 0.0) {
        damagePlot_->setData({}, 0.0, 1.0);
        return;
    }
    const auto usedBins = std::min(matrix.rangeBins(),
                                   static_cast<std::size_t>(matrix.maxRange() / matrix.rangeBinWidth()) + 1);
    std::vector<float> share(usedBins);
    for (std::size_t i = 0; i < usedBins; ++i) {
        share[i] = static_cast<float>(100.0 * perRange[i] / damage);
    }
    const double width = matrix.rangeBinWidth() * scale;
    damagePlot_->setData(std::move(share), 0.5 * width, width);
}
