#pragma once

#include <QMainWindow>

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "sa/Pipeline.hpp"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QPushButton;
class QSpinBox;
class QTimer;
class BearingPanel;
class ChannelTable;
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
    void selectChannel(std::uint16_t channel);

private:
    void startPipeline();
    void stopPipeline();
    void setControlsRunning(bool running);
    void showFrame(const sa::AnalysisFrame& frame);  // plots and panels: the selected channel
    void updateChannelTable();                       // all channels, from the caches below
    QString bearingText(const sa::AnalysisFrame& frame) const;

    std::unique_ptr<sa::Pipeline> pipeline_;
    std::uint16_t channel_ = 0;     // shown in the plots
    std::uint64_t lastFrameIndex_ = 0;  // of channel_
    std::shared_ptr<const sa::AnalysisFrame> shownFrame_;  // redrawn when the scale changes

    // Latest state of every channel; kept after Stop so the table still reacts
    // to changed bearing or fatigue settings.
    std::map<std::uint16_t, std::shared_ptr<const sa::AnalysisFrame>> latestFrames_;
    std::vector<sa::ChannelStats> channelStats_;

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
    ChannelTable* channelTable_ = nullptr;
    BearingPanel* bearingPanel_ = nullptr;
    FatiguePanel* fatiguePanel_ = nullptr;

    QTimer* refreshTimer_ = nullptr;
};
