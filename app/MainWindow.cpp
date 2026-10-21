#include "MainWindow.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
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
#include <cmath>

#include "PlotWidget.hpp"

namespace {

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
            [this](double v) { spectrumPlot_->setXMax(v); });

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

    // --- Plots -----------------------------------------------------------------
    waveformPlot_ = new PlotWidget(tr("Waveform"), tr("ms"), tr("amplitude"));
    waveformPlot_->setAutoScale(/*symmetric=*/true);
    spectrumPlot_ = new PlotWidget(tr("Amplitude spectrum (Hann window)"), tr("Hz"), tr("dB"));
    spectrumPlot_->setXMax(maxFreqSpin_->value());

    // --- Metrics ---------------------------------------------------------------
    rmsLabel_ = makeValueLabel();
    peakLabel_ = makeValueLabel();
    crestLabel_ = makeValueLabel();
    domFreqLabel_ = makeValueLabel();
    rateLabel_ = makeValueLabel();
    linkLabel_ = makeValueLabel();

    auto* metricsBox = new QGroupBox(tr("Metrics"));
    auto* metrics = new QHBoxLayout(metricsBox);
    auto addMetric = [metrics](const QString& name, QLabel* value) {
        auto* form = new QFormLayout;
        form->addRow(name, value);
        metrics->addLayout(form);
    };
    addMetric(tr("RMS"), rmsLabel_);
    addMetric(tr("Peak"), peakLabel_);
    addMetric(tr("Crest factor"), crestLabel_);
    addMetric(tr("Dominant f"), domFreqLabel_);
    addMetric(tr("Sample rate"), rateLabel_);
    addMetric(tr("Link"), linkLabel_);
    metrics->addStretch();

    auto* central = new QWidget;
    auto* layout = new QVBoxLayout(central);
    layout->addLayout(controls);
    layout->addWidget(waveformPlot_, 1);
    layout->addWidget(spectrumPlot_, 1);
    layout->addWidget(metricsBox);
    setCentralWidget(central);
    statusBar()->showMessage(tr("Stopped. Start the Python simulator, then press Start."));

    updateSpectrumScale();

    // ~30 FPS UI refresh, decoupled from the acquisition rate.
    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(33);
    connect(refreshTimer_, &QTimer::timeout, this, &MainWindow::refresh);

    resize(1100, 760);
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

    try {
        auto pipeline = std::make_unique<sa::Pipeline>(cfg);
        pipeline->start();
        pipeline_ = std::move(pipeline);
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Cannot start"), QString::fromStdString(e.what()));
        return;
    }

    lastFrameIndex_ = 0;
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
        spectrumPlot_->setFixedYRange(-100.0, 10.0);
    } else {
        spectrumPlot_->setYUnit(tr("amplitude"));
        spectrumPlot_->setAutoScale(/*symmetric=*/false);
    }
    lastFrameIndex_ = 0;  // force redraw with the new scale
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

    const auto frame = pipeline_->latestFrame();
    if (!frame || frame->index == lastFrameIndex_) {
        return;
    }
    lastFrameIndex_ = frame->index;

    const double dtMs = 1000.0 / frame->sampleRate;
    waveformPlot_->setData(frame->waveform, 0.0, dtMs);

    std::vector<float> spectrum = frame->spectrum;
    if (dbCheck_->isChecked()) {
        for (float& a : spectrum) {
            a = 20.0f * std::log10(std::max(a, 1e-6f));
        }
    }
    spectrumPlot_->setData(std::move(spectrum), 0.0, frame->binHz);

    rmsLabel_->setText(QString::number(frame->rms, 'f', 4));
    peakLabel_->setText(QString::number(frame->peak, 'f', 4));
    crestLabel_->setText(frame->rms > 0 ? QString::number(frame->peak / frame->rms, 'f', 2)
                                        : QStringLiteral("—"));
    domFreqLabel_->setText(QStringLiteral("%1 Hz").arg(frame->dominantHz, 0, 'f', 2));
    rateLabel_->setText(QStringLiteral("%1 Hz").arg(frame->sampleRate));
}
