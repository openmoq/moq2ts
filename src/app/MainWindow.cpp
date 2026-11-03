#include "MainWindow.h"

#include <QCloseEvent>
#include <QDateTime>
#include <QHBoxLayout>
#include <QSettings>
#include <QTabWidget>

#include <algorithm>

#include "../media/LibavCaptureSource.h"

#ifdef MOQ2TS_HAVE_QT_MULTIMEDIA
#if !defined(Q_OS_DARWIN)
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QAudioDevice>
#include <QCameraDevice>
#include <QMediaDevices>
#else
#include <QAudio>
#include <QAudioDeviceInfo>
#include <QCameraInfo>
#endif
#endif
#endif

namespace moq2ts {

namespace {

QString formatByteCount(int64_t bytes) {
    constexpr double kib = 1024.0;
    constexpr double mib = kib * 1024.0;
    constexpr double gib = mib * 1024.0;
    const double value = static_cast<double>(bytes);
    if (value >= gib) {
        return QStringLiteral("%1 GiB").arg(value / gib, 0, 'f', 2);
    }
    if (value >= mib) {
        return QStringLiteral("%1 MiB").arg(value / mib, 0, 'f', 2);
    }
    if (value >= kib) {
        return QStringLiteral("%1 KiB").arg(value / kib, 0, 'f', 2);
    }
    return QStringLiteral("%1 B").arg(bytes);
}

} // namespace

MainWindow::MainWindow(QWidget* parent) : QWidget(parent) {
    setWindowTitle("MOQ M2TS Live Publisher");
    setMinimumSize(720, 560);

    auto* root = new QVBoxLayout(this);
    tabs = new QTabWidget();
    auto* configTab = new QWidget();
    auto* configLayout = new QVBoxLayout(configTab);
    auto* logsTab = new QWidget();
    auto* logsLayout = new QVBoxLayout(logsTab);
    auto* statsTab = new QWidget();
    auto* statsLayout = new QFormLayout(statsTab);
    previewPanel = new PreviewPanel();

    auto* form = new QFormLayout();

    endpointEdit = new QLineEdit("mock://local");
    insecureCheck = new QCheckBox("Do not verify the relay certificate (self-signed test relays only)");
    insecureCheck->setChecked(false);
    namespaceEdit = new QLineEdit("live/paul1");

    videoSourceEdit = new QLineEdit();
    auto* videoBrowse = new QPushButton("Browse");
    QObject::connect(videoBrowse, &QPushButton::clicked, this, &MainWindow::selectVideoSource);
    auto* videoRow = new QWidget();
    auto* videoRowLayout = new QHBoxLayout(videoRow);
    videoRowLayout->setContentsMargins(0, 0, 0, 0);
    videoRowLayout->addWidget(videoSourceEdit);
    videoRowLayout->addWidget(videoBrowse);

    audioSourceEdit = new QLineEdit();
    auto* audioBrowse = new QPushButton("Browse");
    QObject::connect(audioBrowse, &QPushButton::clicked, this, &MainWindow::selectAudioSource);
    auto* audioRow = new QWidget();
    auto* audioRowLayout = new QHBoxLayout(audioRow);
    audioRowLayout->setContentsMargins(0, 0, 0, 0);
    audioRowLayout->addWidget(audioSourceEdit);
    audioRowLayout->addWidget(audioBrowse);

    cameraSourceCombo = new QComboBox();
    microphoneSourceCombo = new QComboBox();
    auto* refreshDevicesButton = new QPushButton("Refresh capture devices");
    QObject::connect(refreshDevicesButton, &QPushButton::clicked, this, &MainWindow::refreshCaptureDevices);

    widthSpin = new QSpinBox();
    widthSpin->setRange(160, 7680);
    widthSpin->setValue(1920);

    heightSpin = new QSpinBox();
    heightSpin->setRange(120, 4320);
    heightSpin->setValue(1080);

    fpsSpin = new QSpinBox();
    fpsSpin->setRange(1, 120);
    fpsSpin->setValue(30);

    videoBitrateSpin = new QSpinBox();
    videoBitrateSpin->setRange(200, 20000);
    videoBitrateSpin->setValue(2500);

    sampleRateSpin = new QSpinBox();
    sampleRateSpin->setRange(8000, 192000);
    sampleRateSpin->setValue(48000);

    channelSpin = new QSpinBox();
    channelSpin->setRange(1, 8);
    channelSpin->setValue(2);

    audioBitrateSpin = new QSpinBox();
    audioBitrateSpin->setRange(32, 512);
    audioBitrateSpin->setValue(160);

    fragmentMsSpin = new QSpinBox();
    fragmentMsSpin->setRange(50, 4000);
    fragmentMsSpin->setValue(250);

    fragmentBytesSpin = new QSpinBox();
    fragmentBytesSpin->setRange(4 * 1024, 1024 * 1024);
    fragmentBytesSpin->setValue(64 * 1024);

    programNumberSpin = new QSpinBox();
    programNumberSpin->setRange(0, 65535);
    programNumberSpin->setValue(0);
    programNumberSpin->setSpecialValueText("First program");

    audioCodecCombo = new QComboBox();
    audioCodecCombo->addItem("AAC (libav encoder)", static_cast<int>(AudioCodecPreset::AAC));
    audioCodecCombo->addItem("Opus (libopus)", static_cast<int>(AudioCodecPreset::Opus));

    openh264Check = new QCheckBox("Use OpenH264 H.264 encoder path");
    openh264Check->setChecked(true);
    useLibAvCheck = new QCheckBox("Use direct libav media integration");
    useLibAvCheck->setChecked(true);
    useOpusFallbackCheck = new QCheckBox("Enable libopus fallback");
    useOpusFallbackCheck->setChecked(false);

    form->addRow("MOQ endpoint", endpointEdit);
    form->addRow(QString(), insecureCheck);
    form->addRow("MOQ namespace (tuple, /-separated)", namespaceEdit);
    form->addRow("M2TS source", videoRow);
    form->addRow("Alternate M2TS source", audioRow);
    form->addRow("Camera source", cameraSourceCombo);
    form->addRow("Microphone source", microphoneSourceCombo);
    form->addRow("", refreshDevicesButton);
    form->addRow("Video width", widthSpin);
    form->addRow("Video height", heightSpin);
    form->addRow("Video frame rate", fpsSpin);
    form->addRow("Video bitrate (kbps)", videoBitrateSpin);
    form->addRow("Audio sample rate", sampleRateSpin);
    form->addRow("Audio channels", channelSpin);
    form->addRow("Audio bitrate (kbps)", audioBitrateSpin);
    form->addRow("Preferred audio codec", audioCodecCombo);
    form->addRow("Fragment duration (ms)", fragmentMsSpin);
    form->addRow("Target segment bytes", fragmentBytesSpin);
    form->addRow("M2TS program", programNumberSpin);
    form->addRow("", openh264Check);
    form->addRow("", useLibAvCheck);
    form->addRow("", useOpusFallbackCheck);

    configLayout->addLayout(form);

    auto* buttonRow = new QHBoxLayout();
    startButton = new QPushButton("Start publishing");
    stopButton = new QPushButton("Stop");
    stopButton->setEnabled(false);
    buttonRow->addWidget(startButton);
    buttonRow->addWidget(stopButton);

    statusLabel = new QLabel("Idle");
    statusLabel->setWordWrap(true);
    statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    logBrowser = new QTextBrowser();
    logBrowser->setOpenExternalLinks(false);
    logBrowser->setPlaceholderText("Logs and pipeline events appear here.");

    pipelineObjectsValue = new QLabel(QStringLiteral("0"));
    pipelineBytesValue = new QLabel(QStringLiteral("0 B"));
    publisherTrackValue = new QLabel(QStringLiteral("-"));
    publisherObjectsValue = new QLabel(QStringLiteral("0"));
    publisherBytesValue = new QLabel(QStringLiteral("0 B"));
    publisherUpdatedValue = new QLabel(QStringLiteral("-"));
    for (QLabel* label : {pipelineObjectsValue,
                          pipelineBytesValue,
                          publisherTrackValue,
                          publisherObjectsValue,
                          publisherBytesValue,
                          publisherUpdatedValue}) {
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    }
    statsLayout->addRow(QStringLiteral("Pipeline objects"), pipelineObjectsValue);
    statsLayout->addRow(QStringLiteral("Pipeline bytes"), pipelineBytesValue);
    statsLayout->addRow(QStringLiteral("Publisher track"), publisherTrackValue);
    statsLayout->addRow(QStringLiteral("Publisher objects"), publisherObjectsValue);
    statsLayout->addRow(QStringLiteral("Publisher bytes"), publisherBytesValue);
    statsLayout->addRow(QStringLiteral("Last publisher update"), publisherUpdatedValue);

    configLayout->addLayout(buttonRow);
    configLayout->addWidget(statusLabel);
    configLayout->addStretch(1);
    logsLayout->addWidget(logBrowser);
    tabs->addTab(configTab, QStringLiteral("Config"));
    tabs->addTab(previewPanel, QStringLiteral("Preview"));
    tabs->addTab(statsTab, QStringLiteral("Stats"));
    tabs->addTab(logsTab, QStringLiteral("Logs"));
    root->addWidget(tabs);

    QObject::connect(startButton, &QPushButton::clicked, this, &MainWindow::handleStart);
    QObject::connect(stopButton, &QPushButton::clicked, this, &MainWindow::handleStop);
    QObject::connect(cameraSourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::updatePreviewConfig);
    QObject::connect(microphoneSourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::updatePreviewConfig);
    QObject::connect(cameraSourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        const bool hasDevice = !cameraSourceCombo->currentData().toString().isEmpty();
        logSourceSelection(QStringLiteral("Camera"), hasDevice ? cameraSourceCombo->currentText() : QStringLiteral("none"));
    });
    QObject::connect(microphoneSourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        const bool hasDevice = !microphoneSourceCombo->currentData().toString().isEmpty();
        logSourceSelection(QStringLiteral("Microphone"), hasDevice ? microphoneSourceCombo->currentText() : QStringLiteral("none"));
    });
    QObject::connect(widthSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::updatePreviewConfig);
    QObject::connect(heightSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::updatePreviewConfig);
    QObject::connect(fpsSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::updatePreviewConfig);
    QObject::connect(sampleRateSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::updatePreviewConfig);
    QObject::connect(channelSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::updatePreviewConfig);

    loadPreferences();
    refreshCaptureDevices();
    updatePreviewConfig();
    logBrowser->append("Ready to publish.");

    setUiEnabled(true);
}

PreviewPanel* MainWindow::previewPanelWidget() const {
    return previewPanel;
}

void MainWindow::closeEvent(QCloseEvent* event) {
    savePreferences();
    QWidget::closeEvent(event);
}

void MainWindow::setUiEnabled(bool enabled) {
    endpointEdit->setEnabled(enabled);
    insecureCheck->setEnabled(enabled);
    namespaceEdit->setEnabled(enabled);
    videoSourceEdit->setEnabled(enabled);
    audioSourceEdit->setEnabled(enabled);
    cameraSourceCombo->setEnabled(enabled);
    microphoneSourceCombo->setEnabled(enabled);
    widthSpin->setEnabled(enabled);
    heightSpin->setEnabled(enabled);
    fpsSpin->setEnabled(enabled);
    videoBitrateSpin->setEnabled(enabled);
    sampleRateSpin->setEnabled(enabled);
    channelSpin->setEnabled(enabled);
    audioBitrateSpin->setEnabled(enabled);
    audioCodecCombo->setEnabled(enabled);
    fragmentMsSpin->setEnabled(enabled);
    fragmentBytesSpin->setEnabled(enabled);
    programNumberSpin->setEnabled(enabled);
    openh264Check->setEnabled(enabled);
    useLibAvCheck->setEnabled(enabled);
    useOpusFallbackCheck->setEnabled(enabled);
    previewPanel->setControlsEnabled(enabled);

    startButton->setEnabled(enabled);
    stopButton->setEnabled(!enabled);
}

void MainWindow::selectVideoSource() {
    const QString filePath = QFileDialog::getOpenFileName(this, "Select video M2TS source", QString(), "M2TS files (*.ts *.m2ts);;All files (*.*)");
    if (!filePath.isEmpty()) {
        videoSourceEdit->setText(filePath);
        logSourceSelection(QStringLiteral("Video file"), filePath);
    }
}

void MainWindow::selectAudioSource() {
    const QString filePath = QFileDialog::getOpenFileName(this, "Select audio M2TS source", QString(), "M2TS files (*.ts *.m2ts);;All files (*.*)");
    if (!filePath.isEmpty()) {
        audioSourceEdit->setText(filePath);
        logSourceSelection(QStringLiteral("Audio file"), filePath);
    }
}

void MainWindow::logSourceSelection(const QString& label, const QString& value) {
    logBrowser->append(QString("%1 | %2 selected: %3")
        .arg(QDateTime::currentDateTime().toString(Qt::ISODate), label, value));
}

void MainWindow::refreshCaptureDevices() {
    cameraSourceCombo->clear();
    microphoneSourceCombo->clear();

    cameraSourceCombo->addItem("No camera selected", QString());
    microphoneSourceCombo->addItem("No microphone selected", QString());

#if defined(Q_OS_DARWIN) || defined(MOQ2TS_HAVE_LIBAV_CAPTURE)
    for (const CaptureDevice& device : LibavCaptureSource::enumerateVideoInputs()) {
        cameraSourceCombo->addItem(device.description, device.id);
    }
    for (const CaptureDevice& device : LibavCaptureSource::enumerateAudioInputs()) {
        microphoneSourceCombo->addItem(device.description, device.id);
    }
#elif defined(MOQ2TS_HAVE_QT_MULTIMEDIA)
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const auto videoInputs = QMediaDevices::videoInputs();
    for (const QCameraDevice& device : videoInputs) {
        cameraSourceCombo->addItem(device.description(), QString::fromUtf8(device.id()));
    }

    const auto audioInputs = QMediaDevices::audioInputs();
    for (const QAudioDevice& device : audioInputs) {
        microphoneSourceCombo->addItem(device.description(), QString::fromUtf8(device.id()));
    }
#else
    const auto cameras = QCameraInfo::availableCameras();
    for (const QCameraInfo& camera : cameras) {
        cameraSourceCombo->addItem(camera.description(), camera.deviceName());
    }

    const auto microphones = QAudioDeviceInfo::availableDevices(QAudio::AudioInput);
    for (const QAudioDeviceInfo& microphone : microphones) {
        microphoneSourceCombo->addItem(microphone.deviceName(), microphone.deviceName());
    }
#endif
#else
    cameraSourceCombo->setItemText(0, "Qt Multimedia unavailable");
    microphoneSourceCombo->setItemText(0, "Qt Multimedia unavailable");
#endif

    if (cameraSourceCombo->count() == 1) {
        cameraSourceCombo->setItemText(0, "No camera detected");
    }
    if (microphoneSourceCombo->count() == 1) {
        microphoneSourceCombo->setItemText(0, "No microphone detected");
    }
    updatePreviewConfig();
}

PublishConfig MainWindow::currentConfig() const {
    PublishConfig cfg;
    cfg.moqEndpoint = endpointEdit->text().trimmed();
    cfg.insecure = insecureCheck->isChecked();
    cfg.namespaceName = namespaceEdit->text().trimmed();
    cfg.videoSource = videoSourceEdit->text().trimmed();
    cfg.audioSource = audioSourceEdit->text().trimmed();
    cfg.cameraDeviceId = cameraSourceCombo->currentData().toString();
    cfg.microphoneDeviceId = microphoneSourceCombo->currentData().toString();

    cfg.videoWidth = widthSpin->value();
    cfg.videoHeight = heightSpin->value();
    cfg.videoFramerate = fpsSpin->value();
    cfg.videoTargetBitrateKbps = videoBitrateSpin->value();

    cfg.audioSampleRate = sampleRateSpin->value();
    cfg.audioChannels = channelSpin->value();
    cfg.audioTargetBitrateKbps = audioBitrateSpin->value();
    cfg.audioCodec = static_cast<AudioCodecPreset>(audioCodecCombo->currentData().toInt());

    cfg.fragmentDurationMs = fragmentMsSpin->value();
    cfg.targetSegmentBytes = fragmentBytesSpin->value();
    cfg.programNumber = programNumberSpin->value();

    cfg.useOpenh264 = openh264Check->isChecked();
    cfg.useLibAvTranscode = useLibAvCheck->isChecked();
    cfg.useLibOpusFallback = useOpusFallbackCheck->isChecked();

    return cfg;
}

void MainWindow::loadPreferences() {
    QSettings settings;
    endpointEdit->setText(settings.value(QStringLiteral("publish/endpoint"), endpointEdit->text()).toString());
    insecureCheck->setChecked(settings.value(QStringLiteral("publish/insecure"), insecureCheck->isChecked()).toBool());
    namespaceEdit->setText(settings.value(QStringLiteral("publish/namespace"), namespaceEdit->text()).toString());

    widthSpin->setValue(settings.value(QStringLiteral("video/width"), widthSpin->value()).toInt());
    heightSpin->setValue(settings.value(QStringLiteral("video/height"), heightSpin->value()).toInt());
    fpsSpin->setValue(settings.value(QStringLiteral("video/framerate"), fpsSpin->value()).toInt());
    videoBitrateSpin->setValue(settings.value(QStringLiteral("video/bitrate"), videoBitrateSpin->value()).toInt());

    sampleRateSpin->setValue(settings.value(QStringLiteral("audio/sampleRate"), sampleRateSpin->value()).toInt());
    channelSpin->setValue(settings.value(QStringLiteral("audio/channels"), channelSpin->value()).toInt());
    audioBitrateSpin->setValue(settings.value(QStringLiteral("audio/bitrate"), audioBitrateSpin->value()).toInt());
    audioCodecCombo->setCurrentIndex(std::max(0, audioCodecCombo->findData(settings.value(QStringLiteral("audio/codec"), audioCodecCombo->currentData()).toInt())));

    fragmentMsSpin->setValue(settings.value(QStringLiteral("fragment/durationMs"), fragmentMsSpin->value()).toInt());
    fragmentBytesSpin->setValue(settings.value(QStringLiteral("fragment/targetBytes"), fragmentBytesSpin->value()).toInt());
    programNumberSpin->setValue(settings.value(QStringLiteral("m2ts/program"), programNumberSpin->value()).toInt());

    openh264Check->setChecked(settings.value(QStringLiteral("use/openh264"), openh264Check->isChecked()).toBool());
    useLibAvCheck->setChecked(settings.value(QStringLiteral("use/libav"), useLibAvCheck->isChecked()).toBool());
    useOpusFallbackCheck->setChecked(settings.value(QStringLiteral("use/libopusFallback"), useOpusFallbackCheck->isChecked()).toBool());
}

void MainWindow::savePreferences() const {
    QSettings settings;
    settings.setValue(QStringLiteral("publish/endpoint"), endpointEdit->text().trimmed());
    settings.setValue(QStringLiteral("publish/insecure"), insecureCheck->isChecked());
    settings.setValue(QStringLiteral("publish/namespace"), namespaceEdit->text().trimmed());

    settings.setValue(QStringLiteral("video/width"), widthSpin->value());
    settings.setValue(QStringLiteral("video/height"), heightSpin->value());
    settings.setValue(QStringLiteral("video/framerate"), fpsSpin->value());
    settings.setValue(QStringLiteral("video/bitrate"), videoBitrateSpin->value());

    settings.setValue(QStringLiteral("audio/sampleRate"), sampleRateSpin->value());
    settings.setValue(QStringLiteral("audio/channels"), channelSpin->value());
    settings.setValue(QStringLiteral("audio/bitrate"), audioBitrateSpin->value());
    settings.setValue(QStringLiteral("audio/codec"), audioCodecCombo->currentData().toInt());

    settings.setValue(QStringLiteral("fragment/durationMs"), fragmentMsSpin->value());
    settings.setValue(QStringLiteral("fragment/targetBytes"), fragmentBytesSpin->value());
    settings.setValue(QStringLiteral("m2ts/program"), programNumberSpin->value());

    settings.setValue(QStringLiteral("use/openh264"), openh264Check->isChecked());
    settings.setValue(QStringLiteral("use/libav"), useLibAvCheck->isChecked());
    settings.setValue(QStringLiteral("use/libopusFallback"), useOpusFallbackCheck->isChecked());
}

void MainWindow::handleStart() {
    const auto cfg = currentConfig();
    savePreferences();
    previewPanel->setConfig(cfg);
    previewPanel->stopPreview();

    if (cfg.videoSource.isEmpty() && cfg.audioSource.isEmpty() &&
        cfg.cameraDeviceId.isEmpty() && cfg.microphoneDeviceId.isEmpty()) {
        QMessageBox::warning(this, "Missing source", "Select an M2TS file/pipe or a camera/microphone capture device.");
        return;
    }

    if (cfg.moqEndpoint.isEmpty() || cfg.namespaceName.isEmpty()) {
        QMessageBox::warning(this, "Missing config", "MOQ endpoint and namespace are required.");
        return;
    }

    setUiEnabled(false);
    resetStats();
    statusLabel->setText("Connecting...");
    logBrowser->append(QString("%1 | Publishing started for namespace %2").arg(QDateTime::currentDateTime().toString(Qt::ISODate), cfg.namespaceName));

    const QString cameraLabel = cfg.cameraDeviceId.isEmpty() ? QStringLiteral("none") : cameraSourceCombo->currentText();
    const QString micLabel = cfg.microphoneDeviceId.isEmpty() ? QStringLiteral("none") : microphoneSourceCombo->currentText();
    const QString videoFileLabel = cfg.videoSource.isEmpty() ? QStringLiteral("none") : cfg.videoSource;
    const QString audioFileLabel = cfg.audioSource.isEmpty() ? QStringLiteral("none") : cfg.audioSource;
    logBrowser->append(QString("%1 | Sources - camera: %2 | microphone: %3 | video file: %4 | audio file: %5")
        .arg(QDateTime::currentDateTime().toString(Qt::ISODate), cameraLabel, micLabel, videoFileLabel, audioFileLabel));

    emit startRequested(cfg);
}

void MainWindow::handleStop() {
    emit stopRequested();
    statusLabel->setText("Stopping...");
    logBrowser->append(QString("%1 | Stop requested by user").arg(QDateTime::currentDateTime().toString(Qt::ISODate)));
}

void MainWindow::updatePreviewConfig() {
    if (previewPanel) {
        previewPanel->setConfig(currentConfig());
    }
}

void MainWindow::resetStats() {
    pipelineObjectsValue->setText(QStringLiteral("0"));
    pipelineBytesValue->setText(QStringLiteral("0 B"));
    publisherTrackValue->setText(QStringLiteral("-"));
    publisherObjectsValue->setText(QStringLiteral("0"));
    publisherBytesValue->setText(QStringLiteral("0 B"));
    publisherUpdatedValue->setText(QStringLiteral("-"));
}

void MainWindow::onPublishStatus(const QString& message) {
    statusLabel->setText(message);
    logBrowser->append(QString("%1 | [status] %2").arg(QDateTime::currentDateTime().toString(Qt::ISODate), message));
    if (message.contains("ready", Qt::CaseInsensitive) || message.contains("stopped", Qt::CaseInsensitive)) {
        setUiEnabled(true);
    }
}

void MainWindow::onPublishError(const QString& message) {
    statusLabel->setText("Error");
    logBrowser->append(QString("%1 | [error] %2").arg(QDateTime::currentDateTime().toString(Qt::ISODate), message));
    QMessageBox::critical(this, "Publish error", message);
    setUiEnabled(true);
}

void MainWindow::onPublishStats(int64_t packets, int64_t bytes) {
    statusLabel->setText(QString("Packets sent: %1, bytes sent: %2").arg(packets).arg(bytes));
    pipelineObjectsValue->setText(QString::number(packets));
    pipelineBytesValue->setText(formatByteCount(bytes));
}

void MainWindow::onPublisherSentTotals(int64_t objects, int64_t bytes) {
    publisherObjectsValue->setText(QString::number(objects));
    publisherBytesValue->setText(formatByteCount(bytes));
}

void MainWindow::onPublisherFramePublished(const QString& track, int64_t bytes, int64_t objects) {
    publisherTrackValue->setText(track);
    publisherObjectsValue->setText(QString::number(objects));
    publisherBytesValue->setText(formatByteCount(bytes));
    publisherUpdatedValue->setText(QDateTime::currentDateTime().toString(Qt::ISODate));
}

} // namespace moq2ts
