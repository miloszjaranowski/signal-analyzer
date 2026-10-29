#pragma once

#include <QWidget>

#include "sa/Fatigue.hpp"

namespace sa {
struct AnalysisFrame;
}

class PlotWidget;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;

// Rainflow + Palmgren-Miner: stress calibration, S-N curve and mean-stress
// correction in; accumulated damage, damage rate and estimated life out. Damage
// is computed here from the frame's rainflow matrix, so every input can change
// while running without recounting.
class FatiguePanel : public QWidget {
    Q_OBJECT

public:
    explicit FatiguePanel(QWidget* parent = nullptr);

    void showFrame(const sa::AnalysisFrame& frame);
    // Estimated life of any channel's frame with the current settings, e.g. "3 days".
    QString lifeText(const sa::AnalysisFrame& frame) const;

signals:
    void changed();
    void resetRequested();

private:
    sa::SnCurve snCurve() const;
    sa::MeanStressCorrection meanStressCorrection() const;
    void updateInputs();  // enables the fields the chosen models use, shows derived limits
    static QString lifeText(double damage, const sa::AnalysisFrame& frame, const sa::SnCurve& curve);

    QDoubleSpinBox* scaleSpin_ = nullptr;
    QComboBox* shapeCombo_ = nullptr;
    QDoubleSpinBox* slopeSpin_ = nullptr;
    QDoubleSpinBox* refRangeSpin_ = nullptr;
    QDoubleSpinBox* refCyclesSpin_ = nullptr;
    QDoubleSpinBox* kneeCyclesSpin_ = nullptr;
    QDoubleSpinBox* slope2Spin_ = nullptr;
    QDoubleSpinBox* cutoffCyclesSpin_ = nullptr;
    QLabel* limitsLabel_ = nullptr;
    QComboBox* meanCombo_ = nullptr;
    QDoubleSpinBox* strengthSpin_ = nullptr;

    QGroupBox* resultBox_ = nullptr;
    QLabel* timeLabel_ = nullptr;
    QLabel* cyclesLabel_ = nullptr;
    QLabel* maxRangeLabel_ = nullptr;
    QLabel* damageLabel_ = nullptr;
    QLabel* rateLabel_ = nullptr;
    QLabel* lifeLabel_ = nullptr;

    PlotWidget* damagePlot_ = nullptr;
};
