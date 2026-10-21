#pragma once

#include <QColor>
#include <QWidget>

#include <array>
#include <optional>

#include "sa/Bearing.hpp"

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;

// Colour of a defect frequency, shared by the panel legend and the plot markers.
QColor faultColor(sa::BearingFault fault);

// Bearing geometry and shaft speed in, characteristic defect frequencies out.
class BearingPanel : public QWidget {
    Q_OBJECT

public:
    explicit BearingPanel(QWidget* parent = nullptr);

    // Empty while the inputs are inconsistent (e.g. ball larger than pitch diameter).
    const std::optional<sa::BearingFrequencies>& frequencies() const { return frequencies_; }
    bool markOnEnvelope() const;

signals:
    void changed();

private:
    void recalculate();

    QSpinBox* elementsSpin_ = nullptr;
    QDoubleSpinBox* ballSpin_ = nullptr;
    QDoubleSpinBox* pitchSpin_ = nullptr;
    QDoubleSpinBox* angleSpin_ = nullptr;
    QDoubleSpinBox* rpmSpin_ = nullptr;
    QCheckBox* markCheck_ = nullptr;
    std::array<QLabel*, sa::kBearingFaults.size()> resultLabels_{};

    std::optional<sa::BearingFrequencies> frequencies_;
};
