#pragma once

#include <QMainWindow>

#include <cstdint>
#include <memory>

#include "sa/Pipeline.hpp"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTimer;
class BearingPanel;
class FatiguePanel;
class PlotWidget;
class SpectrogramWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void toggleAcquisition();
    void refresh();
    void updateSpectrumScale();
    void applyEnvelopeBand();

private:
    void startPipeline();
    void stopPipeline();
    void setControlsRunning(bool running);
    void showFrame(const sa::AnalysisFrame& frame);

    std::unique_ptr<sa::Pipeline> pipeline_;
    std::uint64_t lastFrameIndex_ = 0;
    std::shared_ptr<const sa::AnalysisFrame> shownFrame_;  // redrawn when the scale changes

    QSpinBox* portSpin_ = nullptr;
    QComboBox* fftCombo_ = nullptr;
    QCheckBox* dbCheck_ = nullptr;
    QDoubleSpinBox* maxFreqSpin_ = nullptr;
    QPushButton* startButton_ = nullptr;
    QDoubleSpinBox* envLoSpin_ = nullptr;
    QDoubleSpinBox* envHiSpin_ = nullptr;
    QDoubleSpinBox* envMaxFreqSpin_ = nullptr;

    PlotWidget* waveformPlot_ = nullptr;
    PlotWidget* envelopePlot_ = nullptr;
    PlotWidget* spectrumPlot_ = nullptr;
    SpectrogramWidget* spectrogram_ = nullptr;
    BearingPanel* bearingPanel_ = nullptr;
    FatiguePanel* fatiguePanel_ = nullptr;

    QLabel* rmsLabel_ = nullptr;
    QLabel* peakLabel_ = nullptr;
    QLabel* crestLabel_ = nullptr;
    QLabel* domFreqLabel_ = nullptr;
    QLabel* envPeakLabel_ = nullptr;
    QLabel* bearingLabel_ = nullptr;
    QLabel* rateLabel_ = nullptr;
    QLabel* linkLabel_ = nullptr;

    QTimer* refreshTimer_ = nullptr;
};
