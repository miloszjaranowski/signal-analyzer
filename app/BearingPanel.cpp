#include "BearingPanel.hpp"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QSpinBox>
#include <QVBoxLayout>

#include <stdexcept>

QColor faultColor(sa::BearingFault fault) {
    // Mid-saturation hues that read on light and dark palettes.
    switch (fault) {
        case sa::BearingFault::Cage: return {170, 120, 220};
        case sa::BearingFault::OuterRace: return {70, 150, 240};
        case sa::BearingFault::InnerRace: return {80, 190, 110};
        case sa::BearingFault::RollingElement: return {230, 90, 150};
    }
    return Qt::gray;
}

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

BearingPanel::BearingPanel(QWidget* parent) : QWidget(parent) {
    const sa::BearingGeometry defaults;  // SKF 6205

    elementsSpin_ = new QSpinBox;
    elementsSpin_->setRange(1, 200);
    elementsSpin_->setValue(defaults.rollingElements);
    ballSpin_ = makeSpin(0.01, 1000.0, defaults.ballDiameter, 2, tr(" mm"));
    pitchSpin_ = makeSpin(0.01, 5000.0, defaults.pitchDiameter, 2, tr(" mm"));
    angleSpin_ = makeSpin(0.0, 89.0, defaults.contactAngleDeg, 1, tr(" °"));
    rpmSpin_ = makeSpin(0.0, 1'000'000.0, 3000.0, 0, tr(" rpm"));
    rpmSpin_->setSingleStep(100.0);

    auto* geometryBox = new QGroupBox(tr("Geometry (default: SKF 6205)"));
    auto* geometry = new QFormLayout(geometryBox);
    geometry->addRow(tr("Rolling elements:"), elementsSpin_);
    geometry->addRow(tr("Ball diameter d:"), ballSpin_);
    geometry->addRow(tr("Pitch diameter D:"), pitchSpin_);
    geometry->addRow(tr("Contact angle:"), angleSpin_);
    geometry->addRow(tr("Shaft speed:"), rpmSpin_);

    auto* resultsBox = new QGroupBox(tr("Defect frequencies"));
    auto* results = new QFormLayout(resultsBox);
    for (std::size_t i = 0; i < sa::kBearingFaults.size(); ++i) {
        const sa::BearingFault fault = sa::kBearingFaults[i];
        auto* name = new QLabel(QStringLiteral("<span style=\"color:%1\">&#9632;</span> %2")
                                    .arg(faultColor(fault).name(),
                                         QString::fromUtf8(sa::shortName(fault))));
        name->setToolTip(tr("Defect on the %1").arg(QString::fromUtf8(sa::description(fault))));
        resultLabels_[i] = new QLabel;
        resultLabels_[i]->setTextInteractionFlags(Qt::TextSelectableByMouse);
        results->addRow(name, resultLabels_[i]);
    }
    markCheck_ = new QCheckBox(tr("Mark on envelope spectrum"));
    markCheck_->setChecked(true);
    results->addRow(markCheck_);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(geometryBox);
    layout->addWidget(resultsBox);
    layout->addStretch();

    connect(elementsSpin_, &QSpinBox::valueChanged, this, &BearingPanel::recalculate);
    for (QDoubleSpinBox* spin : {ballSpin_, pitchSpin_, angleSpin_, rpmSpin_}) {
        connect(spin, &QDoubleSpinBox::valueChanged, this, &BearingPanel::recalculate);
    }
    connect(markCheck_, &QCheckBox::toggled, this, &BearingPanel::changed);
    recalculate();
}

bool BearingPanel::markOnEnvelope() const { return markCheck_->isChecked(); }

void BearingPanel::recalculate() {
    sa::BearingGeometry g;
    g.rollingElements = elementsSpin_->value();
    g.ballDiameter = ballSpin_->value();
    g.pitchDiameter = pitchSpin_->value();
    g.contactAngleDeg = angleSpin_->value();
    const double shaftHz = rpmSpin_->value() / 60.0;

    try {
        frequencies_ = sa::bearingFrequencies(g, shaftHz);
    } catch (const std::invalid_argument&) {
        frequencies_.reset();
    }

    // Flag the geometry instead of fighting the user while they type.
    const QString invalid = frequencies_ ? QString() : QStringLiteral("color: #d33;");
    ballSpin_->setStyleSheet(invalid);
    pitchSpin_->setStyleSheet(invalid);

    for (std::size_t i = 0; i < sa::kBearingFaults.size(); ++i) {
        if (!frequencies_) {
            resultLabels_[i]->setText(tr("d must be < D"));
            continue;
        }
        const double hz = sa::frequencyOf(*frequencies_, sa::kBearingFaults[i]);
        // Order = multiple of shaft speed, independent of rpm.
        const QString order =
            shaftHz > 0.0 ? QStringLiteral("%1×").arg(hz / shaftHz, 0, 'f', 3) : QStringLiteral("—");
        resultLabels_[i]->setText(QStringLiteral("%1 Hz  (%2)").arg(hz, 0, 'f', 2).arg(order));
    }
    emit changed();
}
