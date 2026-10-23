#pragma once

#include <QWidget>

#include "sa/Fatigue.hpp"

namespace sa {
struct AnalysisFrame;
}

class PlotWidget;
class QDoubleSpinBox;
class QLabel;

// Rainflow + Palmgren-Miner: stress calibration and S-N curve in, accumulated
// damage, damage rate and estimated life out. Damage is computed here from the
// frame's rainflow histogram, so the inputs can change without recounting.
class FatiguePanel : public QWidget {
    Q_OBJECT

public:
    explicit FatiguePanel(QWidget* parent = nullptr);

    void showFrame(const sa::AnalysisFrame& frame);

signals:
    void changed();
    void resetRequested();

private:
    sa::SnCurve snCurve() const;

    QDoubleSpinBox* scaleSpin_ = nullptr;
    QDoubleSpinBox* slopeSpin_ = nullptr;
    QDoubleSpinBox* refRangeSpin_ = nullptr;
    QDoubleSpinBox* refCyclesSpin_ = nullptr;

    QLabel* timeLabel_ = nullptr;
    QLabel* cyclesLabel_ = nullptr;
    QLabel* maxRangeLabel_ = nullptr;
    QLabel* damageLabel_ = nullptr;
    QLabel* rateLabel_ = nullptr;
    QLabel* lifeLabel_ = nullptr;

    PlotWidget* damagePlot_ = nullptr;
};
