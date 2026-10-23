#include "MainWindow.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

#include "BearingPanel.hpp"
#include "FatiguePanel.hpp"
#include "PlotWidget.hpp"
#include "SpectrogramWidget.hpp"
#include "sa/Bearing.hpp"
#include "sa/Dsp.hpp"

namespace {

// Shared by the spectrum plot (in dB mode) and the spectrogram colour scale.
constexpr double kDbFloor = -100.0;
constexpr double kDbCeiling = 10.0;

QLabel* makeValueLabel() {
    auto* label = new QLabel(QStringLiteral("—"));
    label->setMinimumWidth(110);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont f = label->font();
    f.setFamily(QStringLiteral("monospace"));
    f.setStyleHint(QFont::Monospace);
    label->setFont(f);
    return label;
}

QDoubleSpinBox* makeHzSpin(double value) {
    auto* spin = new QDoubleSpinBox;
    spin->setRange(0.0, 1'000'000.0);
    spin->setDecimals(0);
    spin->setSingleStep(100.0);
    spin->setSuffix(QStringLiteral(" Hz"));
    spin->setValue(value);
    return spin;
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(tr("Signal Analyzer"));

    // --- Acquisition controls -------------------------------------------------
    portSpin_ = new QSpinBox;
    portSpin_->setRange(1, 65535);
    portSpin_->setValue(5005);

    fftCombo_ = new QComboBox;
    for (int n : {1024, 2048, 4096, 8192, 16384}) {
        fftCombo_->addItem(QString::number(n), n);
    }
    fftCombo_->setCurrentIndex(2);

    startButton_ = new QPushButton(tr("Start"));
    startButton_->setDefault(true);
    connect(startButton_, &QPushButton::clicked, this, &MainWindow::toggleAcquisition);

    dbCheck_ = new QCheckBox(tr("Spectrum in dB"));
    dbCheck_->setChecked(true);
    connect(dbCheck_, &QCheckBox::toggled, this, &MainWindow::updateSpectrumScale);

    maxFreqSpin_ = new QDoubleSpinBox;
    maxFreqSpin_->setRange(0.0, 1'000'000.0);
    maxFreqSpin_->setDecimals(0);
    maxFreqSpin_->setSingleStep(100.0);
    maxFreqSpin_->setSuffix(tr(" Hz"));
    maxFreqSpin_->setSpecialValueText(tr("Nyquist"));
    maxFreqSpin_->setValue(1000.0);
    connect(maxFreqSpin_, &QDoubleSpinBox::valueChanged, this,
            [this](double v) {
                spectrumPlot_->setXMax(v);
                spectrogram_->setXMax(v);
            });

    auto* controls = new QHBoxLayout;
    controls->addWidget(new QLabel(tr("UDP port:")));
    controls->addWidget(portSpin_);
    controls->addSpacing(12);
    controls->addWidget(new QLabel(tr("FFT size:")));
    controls->addWidget(fftCombo_);
    controls->addSpacing(12);
    controls->addWidget(new QLabel(tr("Max frequency:")));
    controls->addWidget(maxFreqSpin_);
    controls->addSpacing(12);
    controls->addWidget(dbCheck_);
    controls->addStretch();
    controls->addWidget(startButton_);

    // --- Envelope analysis controls (the band can be tuned while running) ------
    const sa::PipelineConfig defaults;
    envLoSpin_ = makeHzSpin(defaults.envelopeBandLoHz);
    envHiSpin_ = makeHzSpin(defaults.envelopeBandHiHz);
    connect(envLoSpin_, &QDoubleSpinBox::valueChanged, this, &MainWindow::applyEnvelopeBand);
    connect(envHiSpin_, &QDoubleSpinBox::valueChanged, this, &MainWindow::applyEnvelopeBand);

    envMaxFreqSpin_ = makeHzSpin(500.0);
    envMaxFreqSpin_->setSpecialValueText(tr("Nyquist"));
    connect(envMaxFreqSpin_, &QDoubleSpinBox::valueChanged, this,
            [this](double v) { envelopePlot_->setXMax(v); });

    auto* envControls = new QHBoxLayout;
    envControls->addWidget(new QLabel(tr("Envelope band:")));
    envControls->addWidget(envLoSpin_);
    envControls->addWidget(new QLabel(QStringLiteral("–")));
    envControls->addWidget(envHiSpin_);
    envControls->addSpacing(12);
    envControls->addWidget(new QLabel(tr("Envelope max frequency:")));
    envControls->addWidget(envMaxFreqSpin_);
    envControls->addStretch();

    // --- Plots -----------------------------------------------------------------
    waveformPlot_ = new PlotWidget(tr("Waveform"), tr("ms"), tr("amplitude"));
    waveformPlot_->setAutoScale(/*symmetric=*/true);
    spectrumPlot_ = new PlotWidget(tr("Amplitude spectrum (Hann window)"), tr("Hz"), tr("dB"));
    spectrumPlot_->setXMax(maxFreqSpin_->value());
    spectrogram_ = new SpectrogramWidget;
    spectrogram_->setXMax(maxFreqSpin_->value());
    spectrogram_->setDbRange(kDbFloor, kDbCeiling);
    envelopePlot_ = new PlotWidget(tr("Envelope spectrum"), tr("Hz"), tr("amplitude"));
    envelopePlot_->setAutoScale(/*symmetric=*/false);
    envelopePlot_->setXMax(envMaxFreqSpin_->value());

    // --- Metrics ---------------------------------------------------------------
    rmsLabel_ = makeValueLabel();
    peakLabel_ = makeValueLabel();
    crestLabel_ = makeValueLabel();
    domFreqLabel_ = makeValueLabel();
    envPeakLabel_ = makeValueLabel();
    bearingLabel_ = makeValueLabel();
    rateLabel_ = makeValueLabel();
    linkLabel_ = makeValueLabel();

    auto* metricsBox = new QGroupBox(tr("Metrics"));
    auto* metrics = new QGridLayout(metricsBox);
    metrics->setHorizontalSpacing(12);
    constexpr int kMetricsPerRow = 4;
    int metricCount = 0;
    auto addMetric = [metrics, &metricCount](const QString& name, QLabel* value) {
        const int row = metricCount / kMetricsPerRow;
        const int col = (metricCount % kMetricsPerRow) * 2;
        metrics->addWidget(new QLabel(name), row, col);
        metrics->addWidget(value, row, col + 1);
        ++metricCount;
    };
    addMetric(tr("RMS"), rmsLabel_);
    addMetric(tr("Peak"), peakLabel_);
    addMetric(tr("Crest factor"), crestLabel_);
    addMetric(tr("Dominant f"), domFreqLabel_);
    addMetric(tr("Envelope peak"), envPeakLabel_);
    addMetric(tr("Sample rate"), rateLabel_);
    addMetric(tr("Link"), linkLabel_);
    addMetric(tr("Bearing"), bearingLabel_);
    metrics->setColumnStretch(kMetricsPerRow * 2, 1);

    auto* central = new QWidget;
    auto* layout = new QVBoxLayout(central);
    layout->addLayout(controls);
    layout->addLayout(envControls);
    auto* topPlots = new QHBoxLayout;
    topPlots->addWidget(waveformPlot_, 1);
    topPlots->addWidget(envelopePlot_, 1);
    layout->addLayout(topPlots, 1);
    layout->addWidget(spectrumPlot_, 1);
    layout->addWidget(spectrogram_, 1);
    layout->addWidget(metricsBox);
    setCentralWidget(central);
    statusBar()->showMessage(tr("Stopped. Start the Python simulator, then press Start."));

    // --- Side panels: bearing defect frequencies and fatigue, as tabs ----------
    const auto redraw = [this] {
        if (shownFrame_) {
            showFrame(*shownFrame_);
        }
    };
    auto addPanel = [this](const QString& title, QWidget* panel) {
        auto* dock = new QDockWidget(title, this);
        dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
        dock->setWidget(panel);
        addDockWidget(Qt::RightDockWidgetArea, dock);
        return dock;
    };

    bearingPanel_ = new BearingPanel;
    connect(bearingPanel_, &BearingPanel::changed, this, redraw);
    fatiguePanel_ = new FatiguePanel;
    connect(fatiguePanel_, &FatiguePanel::changed, this, redraw);
    connect(fatiguePanel_, &FatiguePanel::resetRequested, this, [this] {
        if (pipeline_) {
            pipeline_->resetFatigue();
        }
    });
    QDockWidget* bearingDock = addPanel(tr("Bearing"), bearingPanel_);
    tabifyDockWidget(bearingDock, addPanel(tr("Fatigue"), fatiguePanel_));
    bearingDock->raise();

    updateSpectrumScale();

    // ~30 FPS UI refresh, decoupled from the acquisition rate.
    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(33);
    connect(refreshTimer_, &QTimer::timeout, this, &MainWindow::refresh);

    resize(1380, 940);
}

MainWindow::~MainWindow() { stopPipeline(); }

void MainWindow::toggleAcquisition() {
    if (pipeline_) {
        stopPipeline();
    } else {
        startPipeline();
    }
}

void MainWindow::startPipeline() {
    sa::PipelineConfig cfg;
    cfg.port = static_cast<std::uint16_t>(portSpin_->value());
    cfg.fftSize = static_cast<std::size_t>(fftCombo_->currentData().toInt());
    cfg.hopSize = cfg.fftSize / 4;  // 75 % overlap
    cfg.envelopeBandLoHz = static_cast<float>(envLoSpin_->value());
    cfg.envelopeBandHiHz = static_cast<float>(envHiSpin_->value());

    try {
        auto pipeline = std::make_unique<sa::Pipeline>(cfg);
        pipeline->start();
        pipeline_ = std::move(pipeline);
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Cannot start"), QString::fromStdString(e.what()));
        return;
    }

    lastFrameIndex_ = 0;
    spectrogram_->clear();
    setControlsRunning(true);
    refreshTimer_->start();
    statusBar()->showMessage(tr("Listening on UDP port %1...").arg(pipeline_->port()));
}

void MainWindow::stopPipeline() {
    if (refreshTimer_) {
        refreshTimer_->stop();
    }
    if (pipeline_) {
        pipeline_->stop();
        pipeline_.reset();
    }
    if (startButton_) {
        setControlsRunning(false);
        statusBar()->showMessage(tr("Stopped."));
    }
}

void MainWindow::setControlsRunning(bool running) {
    startButton_->setText(running ? tr("Stop") : tr("Start"));
    portSpin_->setEnabled(!running);
    fftCombo_->setEnabled(!running);
}

void MainWindow::updateSpectrumScale() {
    if (dbCheck_->isChecked()) {
        spectrumPlot_->setYUnit(tr("dB (re 1)"));
        spectrumPlot_->setFixedYRange(kDbFloor, kDbCeiling);
    } else {
        spectrumPlot_->setYUnit(tr("amplitude"));
        spectrumPlot_->setAutoScale(/*symmetric=*/false);
    }
    if (shownFrame_) {
        showFrame(*shownFrame_);  // redraw with the new scale, also while stopped
    }
}

void MainWindow::applyEnvelopeBand() {
    const double lo = envLoSpin_->value();
    const double hi = envHiSpin_->value();
    // Flag an inverted band instead of fighting the user while they type.
    const bool valid = lo < hi;
    for (QDoubleSpinBox* spin : {envLoSpin_, envHiSpin_}) {
        spin->setStyleSheet(valid ? QString() : QStringLiteral("color: #d33;"));
    }
    if (valid && pipeline_) {
        pipeline_->setEnvelopeBand(static_cast<float>(lo), static_cast<float>(hi));
    }
}

void MainWindow::refresh() {
    if (!pipeline_) {
        return;
    }

    if (!pipeline_->running()) {
        const QString err = QString::fromStdString(pipeline_->lastError());
        stopPipeline();
        if (!err.isEmpty()) {
            QMessageBox::warning(this, tr("Acquisition stopped"), err);
        }
        return;
    }

    const auto stats = pipeline_->stats();
    linkLabel_->setText(tr("%1 pkts, %2 lost").arg(qulonglong(stats.packetsReceived)).arg(qulonglong(stats.packetsLost)));
    statusBar()->showMessage(
        tr("Port %1 | frames %2 | malformed %3 | dropped samples %4 | buffer %5")
            .arg(pipeline_->port())
            .arg(qulonglong(stats.framesComputed))
            .arg(qulonglong(stats.packetsMalformed))
            .arg(qulonglong(stats.samplesDropped))
            .arg(qulonglong(stats.bufferFill)));

    const auto frames = pipeline_->framesSince(lastFrameIndex_);
    if (frames.empty()) {
        return;
    }
    // Every frame becomes a spectrogram row; the other views only need the newest.
    const double hop = static_cast<double>(pipeline_->config().hopSize);
    for (const auto& frame : frames) {
        spectrogram_->addSpectrum(frame->spectrum, frame->binHz, hop / frame->sampleRate);
    }
    lastFrameIndex_ = frames.back()->index;
    shownFrame_ = frames.back();
    showFrame(*shownFrame_);
}

void MainWindow::showFrame(const sa::AnalysisFrame& frame) {
    const double dtMs = 1000.0 / frame.sampleRate;
    waveformPlot_->setData(frame.waveform, 0.0, dtMs);

    std::vector<float> spectrum = frame.spectrum;
    if (dbCheck_->isChecked()) {
        std::transform(spectrum.begin(), spectrum.end(), spectrum.begin(), sa::amplitudeToDb);
    }
    spectrumPlot_->setData(std::move(spectrum), 0.0, frame.binHz);

    envelopePlot_->setTitle(tr("Envelope spectrum, band %1–%2 Hz")
                                .arg(frame.envelopeBandLoHz, 0, 'f', 0)
                                .arg(frame.envelopeBandHiHz, 0, 'f', 0));
    envelopePlot_->setData(frame.envelopeSpectrum, 0.0, frame.binHz);
    // Harmonics of a clear envelope line (a fault shows a whole series), and the
    // expected bearing defect frequencies on a second label row.
    const bool clearLine = frame.envelopePeakProminenceDb >= sa::kClearLineDb;
    const auto& bearing = bearingPanel_->frequencies();
    std::vector<PlotWidget::Marker> markers;
    if (clearLine) {
        for (int h = 1; h <= 5; ++h) {
            markers.push_back({h * double(frame.envelopePeakHz), QStringLiteral("%1×").arg(h)});
        }
    }
    if (bearing && bearingPanel_->markOnEnvelope()) {
        for (const sa::BearingFault fault : sa::kBearingFaults) {
            markers.push_back({sa::frequencyOf(*bearing, fault),
                               QString::fromUtf8(sa::shortName(fault)), faultColor(fault),
                               Qt::DotLine, 1});
        }
    }
    envelopePlot_->setMarkers(std::move(markers));

    rmsLabel_->setText(QString::number(frame.rms, 'f', 4));
    peakLabel_->setText(QString::number(frame.peak, 'f', 4));
    crestLabel_->setText(frame.rms > 0 ? QString::number(frame.peak / frame.rms, 'f', 2)
                                       : QStringLiteral("—"));
    domFreqLabel_->setText(QStringLiteral("%1 Hz").arg(frame.dominantHz, 0, 'f', 2));
    const QString prominence = QStringLiteral("+%1 dB").arg(frame.envelopePeakProminenceDb, 0, 'f', 0);
    envPeakLabel_->setText(clearLine ? QStringLiteral("%1 Hz (%2)")
                                           .arg(frame.envelopePeakHz, 0, 'f', 2)
                                           .arg(prominence)
                                     : tr("none (%1)").arg(prominence));

    QString diagnosis = QStringLiteral("—");
    if (clearLine && bearing) {
        if (const auto match = sa::matchBearingFault(*bearing, frame.envelopePeakHz)) {
            diagnosis = tr("%1 %2× (%3), %4 %")
                            .arg(QString::fromUtf8(sa::shortName(match->fault)))
                            .arg(match->harmonic)
                            .arg(QString::fromUtf8(sa::description(match->fault)))
                            .arg(QString::asprintf("%+.1f", match->errorPercent));
        } else {
            diagnosis = tr("no defect frequency matches");
        }
    }
    bearingLabel_->setText(diagnosis);

    fatiguePanel_->showFrame(frame);
    rateLabel_->setText(QStringLiteral("%1 Hz").arg(frame.sampleRate));
}
