#include "MainWindow.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "BearingPanel.hpp"
#include "ChannelTable.hpp"
#include "FatiguePanel.hpp"
#include "PlotWidget.hpp"
#include "SpectrogramWidget.hpp"
#include "sa/Bearing.hpp"
#include "sa/Dsp.hpp"

namespace {

// Shared by the spectrum plot (in dB mode) and the spectrogram colour scale.
constexpr double kDbFloor = -100.0;
constexpr double kDbCeiling = 10.0;

QDoubleSpinBox* makeHzSpin(double value) {
    auto* spin = new QDoubleSpinBox;
    spin->setRange(0.0, 1'000'000.0);
    spin->setDecimals(0);
    spin->setSingleStep(100.0);
    spin->setSuffix(QStringLiteral(" Hz"));
    spin->setValue(value);
    return spin;
}

bool hasClearLine(const sa::AnalysisFrame& frame) {
    return frame.envelopePeakProminenceDb >= sa::kClearLineDb;
}

QString envelopePeakText(const sa::AnalysisFrame& frame) {
    const QString prominence = QStringLiteral("+%1 dB").arg(frame.envelopePeakProminenceDb, 0, 'f', 0);
    return hasClearLine(frame)
               ? QStringLiteral("%1 Hz (%2)").arg(frame.envelopePeakHz, 0, 'f', 2).arg(prominence)
               : MainWindow::tr("none (%1)").arg(prominence);
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

    // Recording (live only) and replay of capture files (.sacap).
    recordButton_ = new QPushButton(tr("Record…"));
    recordButton_->setCheckable(true);
    recordButton_->setEnabled(false);
    recordButton_->setToolTip(tr("Save every received packet to a capture file (.sacap)"));
    connect(recordButton_, &QPushButton::toggled, this, &MainWindow::toggleRecording);

    openButton_ = new QPushButton(tr("Open recording…"));
    openButton_->setToolTip(tr("Replay a capture file through the same analysis as live data"));
    connect(openButton_, &QPushButton::clicked, this, &MainWindow::openRecording);
    speedCombo_ = new QComboBox;
    for (const auto& [label, speed] : {std::pair{"1×", 1.0}, {"2×", 2.0}, {"5×", 5.0}, {"10×", 10.0},
                                       {"Max", 0.0}}) {
        speedCombo_->addItem(QString::fromUtf8(label), speed);
    }
    speedCombo_->setToolTip(tr("Replay speed; Max analyses the file as fast as possible"));

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
    controls->addWidget(openButton_);
    controls->addWidget(speedCombo_);
    controls->addSpacing(12);
    controls->addWidget(recordButton_);
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

    // --- Channels: one row each, the selected row is shown in the plots --------
    channelTable_ = new ChannelTable({tr("Sample rate"), tr("RMS"), tr("Peak"), tr("Crest"),
                                      tr("Dominant f"), tr("Envelope peak"), tr("Bearing"),
                                      tr("Fatigue life"), tr("Link")});
    connect(channelTable_, &ChannelTable::channelSelected, this, &MainWindow::selectChannel);
    auto* channelsBox = new QGroupBox(tr("Channels (select a row to show it in the plots)"));
    auto* channelsLayout = new QVBoxLayout(channelsBox);
    channelsLayout->addWidget(channelTable_);

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
    layout->addWidget(channelsBox);
    setCentralWidget(central);
    statusBar()->showMessage(tr("Stopped. Start the Python simulator, then press Start."));

    // --- Side panels: bearing defect frequencies and fatigue, as tabs ----------
    const auto redraw = [this] {
        if (shownFrame_) {
            showFrame(*shownFrame_);
        }
        updateChannelTable();
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

void MainWindow::startPipeline(const QString& replayFile) {
    sa::PipelineConfig cfg;
    cfg.port = static_cast<std::uint16_t>(portSpin_->value());
    cfg.fftSize = static_cast<std::size_t>(fftCombo_->currentData().toInt());
    cfg.hopSize = cfg.fftSize / 4;  // 75 % overlap
    cfg.envelopeBandLoHz = static_cast<float>(envLoSpin_->value());
    cfg.envelopeBandHiHz = static_cast<float>(envHiSpin_->value());
    cfg.replayFile = replayFile.toStdString();
    cfg.replaySpeed = speedCombo_->currentData().toDouble();
    replayFile_ = replayFile;

    try {
        auto pipeline = std::make_unique<sa::Pipeline>(cfg);
        pipeline->start();
        pipeline_ = std::move(pipeline);
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Cannot start"), QString::fromStdString(e.what()));
        return;
    }

    lastFrameIndex_ = 0;
    latestFrames_.clear();
    channelStats_.clear();
    updateChannelTable();
    spectrogram_->clear();
    setControlsRunning(true);
    refreshTimer_->start();
    statusBar()->showMessage(replayFile.isEmpty()
                                 ? tr("Listening on UDP port %1...").arg(pipeline_->port())
                                 : tr("Replaying %1...").arg(replayFile));
}

void MainWindow::stopPipeline() {
    if (refreshTimer_) {
        refreshTimer_->stop();
    }
    QString message = tr("Stopped.");
    if (pipeline_) {
        pipeline_->stop();  // also ends a recording; its totals stay readable
        const auto recording = pipeline_->recordingStatus();
        if (!recordPath_.isEmpty() && recording.packets > 0) {
            message = tr("Stopped. Recorded %1 packets (%2 kB) to %3.")
                          .arg(qulonglong(recording.packets))
                          .arg(double(recording.bytes) / 1024.0, 0, 'f', 0)
                          .arg(recordPath_);
        }
        pipeline_.reset();
    }
    recordPath_.clear();
    if (startButton_) {
        setControlsRunning(false);
        statusBar()->showMessage(message);
    }
}

void MainWindow::setControlsRunning(bool running) {
    startButton_->setText(running ? tr("Stop") : tr("Start"));
    portSpin_->setEnabled(!running);
    fftCombo_->setEnabled(!running);
    openButton_->setEnabled(!running);
    speedCombo_->setEnabled(!running);
    const QSignalBlocker blocker(recordButton_);  // reflect state, do not start/stop anything
    recordButton_->setChecked(running && !recordPath_.isEmpty());
    recordButton_->setEnabled(running && replayFile_.isEmpty());
}

void MainWindow::toggleRecording(bool on) {
    if (!pipeline_) {
        return;
    }
    if (!on) {
        pipeline_->stopRecording();
        const auto recording = pipeline_->recordingStatus();
        statusBar()->showMessage(tr("Recorded %1 packets (%2 kB) to %3.")
                                     .arg(qulonglong(recording.packets))
                                     .arg(double(recording.bytes) / 1024.0, 0, 'f', 0)
                                     .arg(recordPath_),
                                 5000);
        recordPath_.clear();
        return;
    }
    const QString suggested =
        QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
            .filePath(QStringLiteral("recording-%1.sacap")
                          .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"))));
    const QString path =
        QFileDialog::getSaveFileName(this, tr("Record to"), suggested, tr("Captures (*.sacap)"));
    const auto undoToggle = [this] {
        const QSignalBlocker blocker(recordButton_);
        recordButton_->setChecked(false);
    };
    if (path.isEmpty()) {
        undoToggle();  // dialog cancelled
        return;
    }
    try {
        pipeline_->startRecording(path.toStdString());
        recordPath_ = path;
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Cannot record"), QString::fromStdString(e.what()));
        undoToggle();
    }
}

void MainWindow::openRecording() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open recording"), QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        tr("Captures (*.sacap);;All files (*)"));
    if (!path.isEmpty()) {
        startPipeline(path);
    }
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

void MainWindow::selectChannel(std::uint16_t channel) {
    if (channel == channel_) {
        return;
    }
    channel_ = channel;
    channelTable_->selectChannel(channel);
    // Start the plots over; the next refresh refills the spectrogram from this
    // channel's recent frames.
    lastFrameIndex_ = 0;
    spectrogram_->clear();
    const auto cached = latestFrames_.find(channel);
    shownFrame_ = cached != latestFrames_.end() ? cached->second : nullptr;
    if (shownFrame_) {
        showFrame(*shownFrame_);
    }
}

void MainWindow::refresh() {
    if (!pipeline_) {
        return;
    }

    // Read before collecting the frames: if the pipeline had already stopped (a
    // replay that ended), the frames collected below are its final ones.
    const bool running = pipeline_->running();
    const auto stats = pipeline_->stats();
    channelStats_ = stats.channels;
    for (const sa::ChannelStats& c : channelStats_) {
        if (auto frame = pipeline_->latestFrame(c.channel)) {
            latestFrames_[c.channel] = std::move(frame);
        }
    }

    QString source = pipeline_->replaying()
                         ? tr("Replay %1 %").arg(pipeline_->replayProgress() * 100.0, 0, 'f', 0)
                         : tr("Port %1").arg(pipeline_->port());
    const auto recording = pipeline_->recordingStatus();
    if (recording.active) {
        source += tr(" | REC %1 kB").arg(double(recording.bytes) / 1024.0, 0, 'f', 0);
    } else if (!recording.error.empty() && !recordPath_.isEmpty()) {
        recordPath_.clear();
        setControlsRunning(true);  // un-check Record
        QMessageBox::warning(this, tr("Recording stopped"), QString::fromStdString(recording.error));
    }
    statusBar()->showMessage(
        tr("%1 | channels %2 | frames %3 | malformed %4 | dropped samples %5 | buffer %6")
            .arg(source)
            .arg(qulonglong(stats.channels.size()))
            .arg(qulonglong(stats.framesComputed))
            .arg(qulonglong(stats.packetsMalformed))
            .arg(qulonglong(stats.samplesDropped))
            .arg(qulonglong(stats.bufferFill)));

    // Follow the data if the selected channel has none, e.g. a sender on channel 3 only.
    const bool selectedPresent = std::any_of(channelStats_.begin(), channelStats_.end(),
                                             [this](const auto& c) { return c.channel == channel_; });
    if (!selectedPresent && !channelStats_.empty()) {
        selectChannel(channelStats_.front().channel);
    }
    updateChannelTable();

    const auto frames = pipeline_->framesSince(channel_, lastFrameIndex_);
    if (!frames.empty()) {
        // Every frame becomes a spectrogram row; the other views only need the newest.
        const double hop = static_cast<double>(pipeline_->config().hopSize);
        for (const auto& frame : frames) {
            spectrogram_->addSpectrum(frame->spectrum, frame->binHz, hop / frame->sampleRate);
        }
        lastFrameIndex_ = frames.back()->index;
        shownFrame_ = frames.back();
        showFrame(*shownFrame_);
    }

    if (!running) {
        // A replay that has ended (no error), or a failure.
        const QString err = QString::fromStdString(pipeline_->lastError());
        const QString finishedReplay = err.isEmpty() ? replayFile_ : QString();
        stopPipeline();
        if (!finishedReplay.isEmpty()) {
            statusBar()->showMessage(tr("Replay finished: %1").arg(finishedReplay));
        }
        if (!err.isEmpty()) {
            QMessageBox::warning(this, tr("Acquisition stopped"), err);
        }
    }
}

void MainWindow::updateChannelTable() {
    const QString none = QStringLiteral("—");
    std::vector<ChannelTable::Row> rows;
    for (const sa::ChannelStats& c : channelStats_) {
        ChannelTable::Row row;
        row.channel = c.channel;
        const auto cached = latestFrames_.find(c.channel);
        if (cached != latestFrames_.end()) {
            const sa::AnalysisFrame& f = *cached->second;
            row.cells = {QStringLiteral("%1 Hz").arg(f.sampleRate),
                         QString::number(f.rms, 'f', 4),
                         QString::number(f.peak, 'f', 4),
                         f.rms > 0 ? QString::number(f.peak / f.rms, 'f', 2) : none,
                         QStringLiteral("%1 Hz").arg(f.dominantHz, 0, 'f', 2),
                         envelopePeakText(f),
                         bearingText(f),
                         fatiguePanel_->lifeText(f)};
        } else {
            row.cells = {QStringLiteral("%1 Hz").arg(c.sampleRate), none, none, none, none, none, none, none};
        }
        row.cells << tr("%1 pkts, %2 lost").arg(qulonglong(c.packetsReceived)).arg(qulonglong(c.packetsLost));
        rows.push_back(std::move(row));
    }
    channelTable_->setRows(rows);
    channelTable_->selectChannel(channel_);
}

QString MainWindow::bearingText(const sa::AnalysisFrame& frame) const {
    const auto& bearing = bearingPanel_->frequencies();
    if (!hasClearLine(frame) || !bearing) {
        return QStringLiteral("—");
    }
    const auto match = sa::matchBearingFault(*bearing, frame.envelopePeakHz);
    if (!match) {
        return tr("no defect frequency matches");
    }
    return tr("%1 %2× (%3), %4 %")
        .arg(QString::fromUtf8(sa::shortName(match->fault)))
        .arg(match->harmonic)
        .arg(QString::fromUtf8(sa::description(match->fault)))
        .arg(QString::asprintf("%+.1f", match->errorPercent));
}

void MainWindow::showFrame(const sa::AnalysisFrame& frame) {
    const double dtMs = 1000.0 / frame.sampleRate;
    waveformPlot_->setTitle(tr("Waveform, channel %1").arg(frame.channel));
    waveformPlot_->setData(frame.waveform, 0.0, dtMs);

    std::vector<float> spectrum = frame.spectrum;
    if (dbCheck_->isChecked()) {
        std::transform(spectrum.begin(), spectrum.end(), spectrum.begin(), sa::amplitudeToDb);
    }
    spectrumPlot_->setTitle(tr("Amplitude spectrum (Hann window), channel %1").arg(frame.channel));
    spectrumPlot_->setData(std::move(spectrum), 0.0, frame.binHz);

    envelopePlot_->setTitle(tr("Envelope spectrum, band %1–%2 Hz")
                                .arg(frame.envelopeBandLoHz, 0, 'f', 0)
                                .arg(frame.envelopeBandHiHz, 0, 'f', 0));
    envelopePlot_->setData(frame.envelopeSpectrum, 0.0, frame.binHz);
    // Harmonics of a clear envelope line (a fault shows a whole series), and the
    // expected bearing defect frequencies on a second label row.
    const auto& bearing = bearingPanel_->frequencies();
    std::vector<PlotWidget::Marker> markers;
    if (hasClearLine(frame)) {
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

    fatiguePanel_->showFrame(frame);
}
