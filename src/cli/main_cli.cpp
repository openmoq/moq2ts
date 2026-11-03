// Headless MSFTS publisher CLI.
//
// Reuses the exact streaming core the GUI drives (LivePipeline + MoqxrPublisher)
// but under a QCoreApplication, so it runs on a Linux server with no display. It
// accepts a seekable .ts file or a live stream (FIFO / /dev/stdin) as its source
// and publishes draft-gregoire-moq-msfts objects to a MOQ relay.

#include <atomic>
#include <csignal>
#include <cstdio>
#include <memory>
#include <thread>

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QTimer>

#ifdef MOQ2TS_HAS_SRT
#include <unistd.h>

#include "media/SourceGapLog.h"
#include "media/SrtSource.h"
#endif

#include "app/PublishConfig.h"
#include "media/LivePipeline.h"
#include "publish/MoqxrPublisher.h"

#ifdef MOQ2TS_HAS_MPTS
#include "app/PublisherSpec.h"
#include "media/MptsPipeline.h"
#endif

namespace {

// Set from the async signal handler; polled by a QTimer on the Qt thread because a
// raw signal handler cannot safely call Qt methods.
std::atomic<bool> g_interrupted{false};

void handleSignal(int) {
    g_interrupted.store(true, std::memory_order_release);
}

void logLine(std::FILE* stream, const char* tag, const QString& message) {
    std::fprintf(stream, "[%s] %s\n", tag, message.toLocal8Bit().constData());
    std::fflush(stream);
}

#ifdef MOQ2TS_HAS_SRT
// SRT ingest into a pipe: a receive thread writes the TS it receives to the
// write end, and the pipeline reads readPath(). stop() and the destructor stop
// and join the thread, then close the socket and the read end; the destructor
// also shuts the SRT library down. Each step is guarded, so running twice is
// harmless. Declare it before the publisher and the pipeline, so it outlives
// them on every exit path: srt_startup() happens inside connect()/listen(),
// and leaving it unpaired crashes in libsrt's exit-time destructors.
class SrtIngest {
public:
    SrtIngest() = default;
    SrtIngest(const SrtIngest&) = delete;
    SrtIngest& operator=(const SrtIngest&) = delete;
    ~SrtIngest() {
        stop();
        moq2ts::SrtSource::shutdown();
    }

    // Connects out (caller) or binds (listener), opens the pipe, and starts
    // the receive thread. After a drop the stream resumes at a random access
    // point on resyncPid when it is set and >= 0, and gaps records where it
    // skipped ahead. Returns an empty string on success, the error otherwise.
    QString start(const moq2ts::SrtSource::Config& cfg, std::shared_ptr<std::atomic<int>> resyncPid,
                  std::shared_ptr<moq2ts::SourceGapLog> gaps) {
        const bool listener = cfg.mode == moq2ts::SrtSource::Config::Mode::Listener;
        const QString err = listener ? moq2ts::SrtSource::listen(cfg, m_sock) : moq2ts::SrtSource::connect(cfg, m_sock);
        if (!err.isEmpty()) {
            return err;
        }
        int fds[2] = {-1, -1}; // [0]=read, [1]=write
        if (::pipe(fds) != 0) {
            return QStringLiteral("Failed to create pipe for SRT: %1").arg(strerror(errno));
        }
        m_readFd = fds[0];
        m_thread = std::make_unique<std::thread>(
            [sock = m_sock, listener, writeFd = fds[1], stop = &m_stop, resyncPid, gaps]() {
                if (listener) {
                    moq2ts::SrtSource::listenLoop(sock, writeFd, *stop, resyncPid.get(), gaps.get());
                } else {
                    moq2ts::SrtSource::receiveLoop(sock, writeFd, *stop, resyncPid.get(), gaps.get());
                }
                ::close(writeFd); // close write end to signal EOF to reader
            });
        return {};
    }

    // The read end of the pipe, as a path the pipeline opens.
    QString readPath() const { return QStringLiteral("/dev/fd/%1").arg(m_readFd); }

    void stop() {
        m_stop.store(true, std::memory_order_release);
        // The receive thread must be gone before srt_cleanup() tears down libsrt.
        if (m_thread && m_thread->joinable()) {
            m_thread->join();
        }
        if (m_sock >= 0) {
            moq2ts::SrtSource::close(m_sock);
            m_sock = -1;
        }
        if (m_readFd >= 0) {
            ::close(m_readFd);
            m_readFd = -1;
        }
    }

private:
    std::atomic<bool> m_stop{false};
    std::unique_ptr<std::thread> m_thread;
    int m_sock = -1;
    int m_readFd = -1;
};
#endif  // MOQ2TS_HAS_SRT

#ifdef MOQ2TS_HAS_MPTS
// --config: publishes the programs of one live multi-program source as
// per-program tracks (draft "Per-Program") in one session, as the JSON
// configuration at path states (see PublisherSpec.h).
int runConfig(QCoreApplication& app, const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        logLine(stderr, "error", QStringLiteral("Cannot open the configuration %1: %2").arg(path, file.errorString()));
        return 1;
    }
    moq2ts::PublisherSpec spec;
    QString specError;
    if (!moq2ts::parsePublisherSpec(file.readAll(), &spec, &specError)) {
        logLine(stderr, "error", QStringLiteral("%1: %2").arg(path, specError));
        return 2;
    }
    moq2ts::MptsConfig mpts;
    mpts.sourcePath = spec.sourcePath;
    mpts.programs = spec.programs;
    mpts.targetSegmentBytes = spec.segmentBytes;

    const bool useSrt = !spec.srt.mode.isEmpty();
#ifndef MOQ2TS_HAS_SRT
    if (useSrt) {
        logLine(stderr, "error",
                QStringLiteral("This build has no SRT support (libsrt was not found when it was "
                               "configured, or the platform is unsupported). Use an \"ingest\" "
                               "\"path\" (a FIFO) instead."));
        return 2;
    }
#else
    SrtIngest srtIngest;
    if (useSrt) {
        const bool listener = spec.srt.mode == QStringLiteral("listener");
        moq2ts::SrtSource::Config srtCfg;
        srtCfg.mode = listener ? moq2ts::SrtSource::Config::Mode::Listener : moq2ts::SrtSource::Config::Mode::Caller;
        srtCfg.host = spec.srt.host.toStdString();
        srtCfg.port = static_cast<uint16_t>(spec.srt.port);
        srtCfg.latencyMs = spec.srt.latencyMs;
        logLine(stderr, "srt", QStringLiteral("%1 srt://%2:%3 (latency=%4ms)...")
                                   .arg(listener ? QStringLiteral("Listener: waiting for a caller on")
                                                 : QStringLiteral("Caller: connecting to"),
                                        spec.srt.host)
                                   .arg(spec.srt.port)
                                   .arg(spec.srt.latencyMs));
        // Every program starts a Group where the receive thread skipped ahead.
        auto sourceGaps = std::make_shared<moq2ts::SourceGapLog>();
        // No resync PID: no one program's PID suits every program, so after a
        // drop the ring resumes at the newest random access point of any PID,
        // and each program then drops to its own next random access point.
        const QString err = srtIngest.start(srtCfg, nullptr, sourceGaps);
        if (!err.isEmpty()) {
            logLine(stderr, "error", err);
            return 1;
        }
        mpts.sourcePath = srtIngest.readPath();
        mpts.sourceGaps = sourceGaps;
    }
#endif  // MOQ2TS_HAS_SRT

    moq2ts::MoqxrPublisher publisher;
    moq2ts::MptsPipeline pipeline;
    std::atomic<bool> failed{false};

    QObject::connect(&pipeline, &moq2ts::MptsPipeline::status, &app, [](const QString& m) {
        logLine(stderr, "status", m);
    });
    QObject::connect(&pipeline, &moq2ts::MptsPipeline::error, &app, [&failed](const QString& m) {
        failed.store(true, std::memory_order_release);
        logLine(stderr, "error", m);
    });
    // A read error of one program ends that program's track only; it still
    // makes the exit code 1.
    QObject::connect(&pipeline, &moq2ts::MptsPipeline::programEnded, &app, [&failed](int, const QString& m) {
        failed.store(true, std::memory_order_release);
        logLine(stderr, "error", m);
    });
    QObject::connect(&publisher, &moq2ts::MoqxrPublisher::publishError, &app, [&failed](const QString& m) {
        failed.store(true, std::memory_order_release);
        logLine(stderr, "error", m);
    });
    QObject::connect(&publisher, &moq2ts::MoqxrPublisher::connectionStateChanged, &app,
                     [](bool, const QString& m) { logLine(stderr, "conn", m); });
    // What the publisher pulled and what moqxr sent, for the line printed at
    // exit (written from the publish thread, hence direct).
    auto publishTotals = std::make_shared<moq2ts::PublishTotals>();
    QObject::connect(&publisher, &moq2ts::MoqxrPublisher::framePublished, &app,
                     [publishTotals](const QString&, int64_t bytes, int64_t objects) {
                         publishTotals->recordPulled(objects, bytes);
                     },
                     Qt::DirectConnection);
    QObject::connect(&publisher, &moq2ts::MoqxrPublisher::sentTotals, &app,
                     [publishTotals](int64_t objects, int64_t bytes) { publishTotals->recordSent(objects, bytes); },
                     Qt::DirectConnection);

    if (!publisher.connect(spec.relay)) {
        logLine(stderr, "error", QStringLiteral("Could not open MOQ publish session."));
        return 1;
    }
    pipeline.start(spec.relay, mpts, &publisher);

    // As in main(): poll for SIGINT/SIGTERM and for the end of every program.
    // Local, so it cannot fire after the pipeline it reads has gone.
    QTimer poll;
    poll.setInterval(200);
    QObject::connect(&poll, &QTimer::timeout, &app, [&]() {
        if (g_interrupted.load(std::memory_order_acquire)) {
            logLine(stderr, "status", QStringLiteral("Interrupted; stopping."));
            app.quit();
            return;
        }
        if (!pipeline.running()) {
            app.quit();
        }
    });
    poll.start();

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, [&]() {
        // The publisher first: it ends a publish that waits for a subscriber,
        // which MptsPipeline::stop() leaves to its caller.
        publisher.stop();
        pipeline.stop();
#ifdef MOQ2TS_HAS_SRT
        srtIngest.stop();
#endif  // MOQ2TS_HAS_SRT
        std::fprintf(stdout, "[final] %s\n", qUtf8Printable(publishTotals->finalFields()));
        std::fflush(stdout);
    });

    const int rc = app.exec();
    return (rc != 0 || failed.load(std::memory_order_acquire)) ? 1 : 0;
}
#endif  // MOQ2TS_HAS_MPTS

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    // int64_t is not a built-in Qt metatype; LivePipeline::stats and
    // MoqxrPublisher::framePublished are emitted from the worker thread, so their
    // connections are queued and would be dropped without this registration.
    qRegisterMetaType<int64_t>("int64_t");
    QCoreApplication::setOrganizationName(QStringLiteral("moq2ts"));
    QCoreApplication::setApplicationName(QStringLiteral("moq2ts-cli"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Headless MSFTS (MPEG-TS over MOQ) publisher. Reads a .ts file, a\n"
                       "live stream (FIFO / /dev/stdin), or a live SRT feed, and publishes\n"
                       "to a MOQ relay."));
    parser.addHelpOption();
    parser.addVersionOption();

    const auto addValue = [&parser](const QString& name, const QString& desc,
                                    const QString& valueName, const QString& def = QString()) {
        parser.addOption(QCommandLineOption(name, desc, valueName, def));
    };

    // Session.
    addValue(QStringLiteral("endpoint"), QStringLiteral("MOQ relay endpoint (mock://... for mock builds)."),
             QStringLiteral("url"), QStringLiteral("mock://local"));
    addValue(QStringLiteral("namespace"), QStringLiteral("MOQ track namespace."),
             QStringLiteral("ns"), QStringLiteral("live/ch1"));
    // Source (one of).
    addValue(QStringLiteral("video"), QStringLiteral("TS/M2TS source: seekable file, FIFO, or /dev/stdin."),
             QStringLiteral("path"));
    addValue(QStringLiteral("audio"), QStringLiteral("Alternate TS/M2TS source path (single multiplexed stream)."),
             QStringLiteral("path"));
    addValue(QStringLiteral("camera"), QStringLiteral("Capture device id (alternative to a TS source)."),
             QStringLiteral("id"));
    addValue(QStringLiteral("mic"), QStringLiteral("Microphone device id."), QStringLiteral("id"));
    // Video / audio params (defaults mirror PublishConfig).
    addValue(QStringLiteral("width"), QStringLiteral("Capture width."), QStringLiteral("px"), QStringLiteral("1920"));
    addValue(QStringLiteral("height"), QStringLiteral("Capture height."), QStringLiteral("px"), QStringLiteral("1080"));
    addValue(QStringLiteral("fps"), QStringLiteral("Capture frame rate."), QStringLiteral("n"), QStringLiteral("30"));
    addValue(QStringLiteral("video-bitrate"), QStringLiteral("Video target bitrate (kbps)."), QStringLiteral("kbps"), QStringLiteral("2500"));
    addValue(QStringLiteral("sample-rate"), QStringLiteral("Audio sample rate."), QStringLiteral("hz"), QStringLiteral("48000"));
    addValue(QStringLiteral("channels"), QStringLiteral("Audio channels."), QStringLiteral("n"), QStringLiteral("2"));
    addValue(QStringLiteral("audio-bitrate"), QStringLiteral("Audio target bitrate (kbps)."), QStringLiteral("kbps"), QStringLiteral("160"));
    addValue(QStringLiteral("audio-codec"), QStringLiteral("Audio codec: aac or opus."), QStringLiteral("codec"), QStringLiteral("aac"));
    // Objectization.
    addValue(QStringLiteral("fragment-ms"), QStringLiteral("Fragment/group cadence (ms)."), QStringLiteral("ms"), QStringLiteral("250"));
    addValue(QStringLiteral("segment-bytes"), QStringLiteral("Target object size (bytes)."), QStringLiteral("bytes"), QStringLiteral("65536"));
    addValue(QStringLiteral("program"), QStringLiteral("MPEG program number. Per-program: the program to carry (0 = first). "
                                                        "Unmodified multiplex: the reference program that the catalog "
                                                        "names (0 = none)."),
             QStringLiteral("n"), QStringLiteral("0"));
    // MSFTS carriage-profile options (msfts#7).
    addValue(QStringLiteral("mux-rate"), QStringLiteral("Advisory source mux rate (bits/s); 0 omits the catalog hint. Dropped when an unmodified source lists several programs."),
             QStringLiteral("bps"), QStringLiteral("0"));
    // --transparent is the original name, kept so existing scripts still work.
    // --publish-multiplex names the SRT carriage choice (draft "Unmodified Multiplex").
    parser.addOption(QCommandLineOption(QStringList{QStringLiteral("unmodified"), QStringLiteral("transparent"),
                                                    QStringLiteral("publish-multiplex")},
        QStringLiteral("Unmodified carriage: forward every source packet unchanged. The track is "
                       "unmodified-program for a single-program source and unmodified-multiplex "
                       "otherwise. --transparent and --publish-multiplex are aliases; SRT ingest "
                       "uses this carriage by default.")));
    parser.addOption(QCommandLineOption(QStringLiteral("paced"),
        QStringLiteral("Pace file-source publishing at media-time rate (prevents dumping all data at wire speed).")));
    addValue(QStringLiteral("draft"), QStringLiteral("MOQT transport draft version: 14, 16, 17, 18, 21 or 22. The relay must list it."),
             QStringLiteral("version"), QStringLiteral("22"));
    parser.addOption(QCommandLineOption(QStringLiteral("insecure"),
        QStringLiteral("Do not verify the relay certificate. For a local or test relay with a self-signed "
                       "certificate; by default the certificate is verified against $SSL_CERT_FILE or the system "
                       "CA bundle.")));
    parser.addOption(QCommandLineOption(QStringLiteral("retain-si"),
        QStringLiteral("Per-program mode: also keep DVB SI PIDs (NIT/SDT/EIT/TDT-TOT), with the SDT and EIT reduced to the carried service.")));
    addValue(QStringLiteral("si"),
             QStringLiteral("Per-program mode: keep these DVB SI tables, a comma-separated list of nit, sdt, bat, "
                            "eit-pf, eit-schedule, tdt-tot, or all. Cannot be combined with --retain-si."),
             QStringLiteral("list"));
    parser.addOption(QCommandLineOption(QStringLiteral("retain-null"),
        QStringLiteral("Per-program mode: also keep null (0x1FFF) packets.")));
    // SRT ingest source (alternative to --video file).
    // Uses a JSON config file matching moqxr's format:
    //   { "srt_callers": [{ "id": "...", "srt": { "mode": "caller", "host": "...", "port": N, "latency_ms": N } }] }
    addValue(QStringLiteral("srt-config"), QStringLiteral("SRT config JSON file (moqxr-compatible format)."),
             QStringLiteral("path"));
    addValue(QStringLiteral("config"),
             QStringLiteral("Publish several programs of one live source as per-program tracks, as this JSON "
                            "configuration states (relay, ingest, programs). Takes no other option."),
             QStringLiteral("file"));

    parser.process(app);

    // The configuration states the relay, the source, and the programs, so
    // another option would be ignored or contradict it.
    if (parser.isSet(QStringLiteral("config"))) {
        for (const QString& name : parser.optionNames()) {
            if (name != QStringLiteral("config")) {
                logLine(stderr, "error", QStringLiteral("--config cannot be combined with --%1.").arg(name));
                return 2;
            }
        }
#ifdef MOQ2TS_HAS_MPTS
        return runConfig(app, parser.value(QStringLiteral("config")));
#else
        // Built without per-program publishing: the platform is unsupported.
        logLine(stderr, "error",
                QStringLiteral("This build has no per-program (--config) support (the platform is "
                               "unsupported, or MOQ2TS_ENABLE_MPTS was off when it was configured)."));
        return 2;
#endif  // MOQ2TS_HAS_MPTS
    }

    moq2ts::PublishConfig cfg;
    cfg.moqEndpoint = parser.value(QStringLiteral("endpoint"));
    cfg.namespaceName = parser.value(QStringLiteral("namespace"));
    cfg.videoSource = parser.value(QStringLiteral("video"));
    cfg.audioSource = parser.value(QStringLiteral("audio"));
    cfg.cameraDeviceId = parser.value(QStringLiteral("camera"));
    cfg.microphoneDeviceId = parser.value(QStringLiteral("mic"));
    cfg.videoWidth = parser.value(QStringLiteral("width")).toInt();
    cfg.videoHeight = parser.value(QStringLiteral("height")).toInt();
    cfg.videoFramerate = parser.value(QStringLiteral("fps")).toInt();
    cfg.videoTargetBitrateKbps = parser.value(QStringLiteral("video-bitrate")).toInt();
    cfg.audioSampleRate = parser.value(QStringLiteral("sample-rate")).toInt();
    cfg.audioChannels = parser.value(QStringLiteral("channels")).toInt();
    cfg.audioTargetBitrateKbps = parser.value(QStringLiteral("audio-bitrate")).toInt();
    cfg.audioCodec = parser.value(QStringLiteral("audio-codec")).compare(QStringLiteral("opus"), Qt::CaseInsensitive) == 0
                         ? moq2ts::AudioCodecPreset::Opus
                         : moq2ts::AudioCodecPreset::AAC;
    cfg.fragmentDurationMs = parser.value(QStringLiteral("fragment-ms")).toInt();
    cfg.targetSegmentBytes = parser.value(QStringLiteral("segment-bytes")).toInt();
    cfg.programNumber = parser.value(QStringLiteral("program")).toInt();
    cfg.mpeg2tsMuxRateBps = parser.value(QStringLiteral("mux-rate")).toInt();
    cfg.transparentMode = parser.isSet(QStringLiteral("unmodified"));
    cfg.pacedFileSource = parser.isSet(QStringLiteral("paced"));
    cfg.draftVersion = parser.value(QStringLiteral("draft")).toInt();
    cfg.insecure = parser.isSet(QStringLiteral("insecure"));
    if (!moq2ts::MoqxrPublisher::supportsDraft(cfg.draftVersion)) {
        logLine(stderr, "error", QStringLiteral("--draft %1 is not supported; use %2.")
                                     .arg(parser.value(QStringLiteral("draft")),
                                          moq2ts::MoqxrPublisher::supportedDrafts()));
        return 2;
    }
    const bool retainSi = parser.isSet(QStringLiteral("retain-si"));
    if (retainSi) {
        cfg.siTables = moq2ts::allSiTables();
    }
    if (parser.isSet(QStringLiteral("si"))) {
        if (retainSi) {
            logLine(stderr, "error", QStringLiteral("--si and --retain-si cannot be combined; --retain-si means --si all."));
            return 2;
        }
        QString siError;
        if (!moq2ts::parseSiTables(parser.value(QStringLiteral("si")), &cfg.siTables, &siError)) {
            logLine(stderr, "error", QStringLiteral("--si: %1").arg(siError));
            return 2;
        }
    }
    cfg.retainNullPackets = parser.isSet(QStringLiteral("retain-null"));

    // SRT ingest: if --srt-config is set, parse the JSON, connect to the first
    // SRT caller entry, and pipe TS data to M2tsPacketizer via a FIFO.
    const QString srtConfigPath = parser.value(QStringLiteral("srt-config"));
    const bool useSrt = !srtConfigPath.isEmpty();
#ifndef MOQ2TS_HAS_SRT
    // Built without SRT: libsrt was absent at configure time, or the platform has no
    // POSIX sockets. Say so rather than accepting the option and ingesting nothing.
    if (useSrt) {
        logLine(stderr, "error",
                QStringLiteral("This build has no SRT support (libsrt was not found when it was "
                               "configured, or the platform is unsupported). Use --video with a "
                               "file or FIFO instead."));
        return 2;
    }
#else
    // Declared before the publisher/pipeline so it is destroyed after them, on
    // EVERY exit path including the early returns below.
    SrtIngest srtIngest;

    if (useSrt) {
        // Parse the JSON config file (moqxr-compatible format)
        QFile configFile(srtConfigPath);
        if (!configFile.open(QIODevice::ReadOnly)) {
            logLine(stderr, "error", QStringLiteral("Cannot open SRT config: %1").arg(srtConfigPath));
            return 1;
        }
        const QByteArray jsonData = configFile.readAll();
        configFile.close();

        // Minimal JSON parsing for the srt_callers[0] entry
        const QJsonDocument doc = QJsonDocument::fromJson(jsonData);
        if (!doc.isObject()) {
            logLine(stderr, "error", QStringLiteral("SRT config is not valid JSON"));
            return 1;
        }
        const QJsonArray callers = doc.object().value(QStringLiteral("srt_callers")).toArray();
        if (callers.isEmpty()) {
            logLine(stderr, "error", QStringLiteral("SRT config has no srt_callers entries"));
            return 1;
        }
        const QJsonObject caller = callers[0].toObject();
        const QJsonObject srtObj = caller.value(QStringLiteral("srt")).toObject();
        const QString callerId = caller.value(QStringLiteral("id")).toString(QStringLiteral("default"));

        moq2ts::SrtSource::Config srtCfg;
        const QString modeStr = srtObj.value(QStringLiteral("mode")).toString(QStringLiteral("caller"));
        if (modeStr != QStringLiteral("caller") && modeStr != QStringLiteral("listener")) {
            logLine(stderr, "error",
                    QStringLiteral("SRT config: unknown mode '%1' (use \"caller\" or \"listener\")").arg(modeStr));
            return 1;
        }
        const bool listener = modeStr == QStringLiteral("listener");
        srtCfg.mode = listener ? moq2ts::SrtSource::Config::Mode::Listener : moq2ts::SrtSource::Config::Mode::Caller;
        // A listener binds host; by default every interface.
        srtCfg.host = srtObj.value(QStringLiteral("host"))
                          .toString(listener ? QStringLiteral("0.0.0.0") : QStringLiteral("127.0.0.1"))
                          .toStdString();
        srtCfg.port = static_cast<uint16_t>(srtObj.value(QStringLiteral("port")).toInt(9000));
        srtCfg.latencyMs = srtObj.value(QStringLiteral("latency_ms")).toInt(120);
        // Optional receive-headroom overrides; absent keys keep the built-in defaults.
        srtCfg.rcvBufBytes = srtObj.value(QStringLiteral("rcvbuf_bytes")).toInt(srtCfg.rcvBufBytes);
        srtCfg.udpRcvBufBytes =
            srtObj.value(QStringLiteral("udp_rcvbuf_bytes")).toInt(srtCfg.udpRcvBufBytes);

        if (listener) {
            logLine(stderr, "srt", QStringLiteral("Listener '%1': waiting for a caller on srt://%2:%3 (latency=%4ms)...")
                                       .arg(callerId)
                                       .arg(QString::fromStdString(srtCfg.host))
                                       .arg(srtCfg.port)
                                       .arg(srtCfg.latencyMs));
        } else {
            logLine(stderr, "srt", QStringLiteral("Caller '%1': connecting to srt://%2:%3 (latency=%4ms)...")
                                       .arg(callerId)
                                       .arg(QString::fromStdString(srtCfg.host))
                                       .arg(srtCfg.port)
                                       .arg(srtCfg.latencyMs));
        }

        // After a drop the SRT receive thread resyncs on the PID whose random
        // access points start Groups, which the pipeline sets.
        auto resyncPid = std::make_shared<std::atomic<int>>(-1);
        cfg.resyncPid = resyncPid;
        // The packetizer starts a Group where the receive thread skipped ahead.
        auto sourceGaps = std::make_shared<moq2ts::SourceGapLog>();
        cfg.sourceGaps = sourceGaps;
        const QString err = srtIngest.start(srtCfg, resyncPid, sourceGaps);
        if (!err.isEmpty()) {
            logLine(stderr, "error", err);
            return 1;
        }
        if (!listener) {
            logLine(stderr, "srt", QStringLiteral("Connected to SRT source"));
        }
        // Point the video source at the read end of the pipe
        cfg.videoSource = srtIngest.readPath();

        // SRT is a live source - no pacing needed (real-time from encoder)
        cfg.pacedFileSource = false;
        // Force unmodified carriage for SRT. An SRT contribution feed is forwarded
        // as received, so deriving one program from it is not what this path is
        // for. Say so rather than overriding in silence: the per-program options
        // otherwise appear to be accepted and then do nothing, which is
        // indistinguishable from a bug in the filter itself.
        if (!cfg.transparentMode) {
            QStringList ignored;
            if (retainSi) {
                ignored << QStringLiteral("--retain-si");
            } else if (!cfg.siTables.isEmpty()) {
                ignored << QStringLiteral("--si");
            }
            if (cfg.retainNullPackets) {
                ignored << QStringLiteral("--retain-null");
            }
            if (!ignored.isEmpty()) {
                logLine(stderr, "warn",
                        QStringLiteral("%1 %2 only meaningful in per-program mode; SRT ingest always "
                                       "forwards the source unmodified, so %3 ignored.")
                            .arg(ignored.join(QStringLiteral(", ")),
                                 ignored.size() == 1 ? QStringLiteral("is") : QStringLiteral("are"),
                                 ignored.size() == 1 ? QStringLiteral("it is") : QStringLiteral("they are")));
            }
        }
        cfg.transparentMode = true;
    }
#endif  // MOQ2TS_HAS_SRT

    // Validation mirrors the GUI guards.
    const bool hasSource = useSrt || !cfg.videoSource.isEmpty() || !cfg.audioSource.isEmpty() ||
                           !cfg.cameraDeviceId.isEmpty() || !cfg.microphoneDeviceId.isEmpty();
    if (!hasSource) {
        logLine(stderr, "error", QStringLiteral("A source is required: --video/--audio (TS path), --camera/--mic, or --srt."));
        return 2;
    }
    if (cfg.moqEndpoint.isEmpty() || cfg.namespaceName.isEmpty()) {
        logLine(stderr, "error", QStringLiteral("--endpoint and --namespace must be non-empty."));
        return 2;
    }

    moq2ts::MoqxrPublisher publisher;
    moq2ts::LivePipeline pipeline;
    std::atomic<bool> failed{false};

    QObject::connect(&pipeline, &moq2ts::LivePipeline::status, &app, [](const QString& m) {
        logLine(stderr, "status", m);
    });
    QObject::connect(&pipeline, &moq2ts::LivePipeline::error, &app, [&failed](const QString& m) {
        failed.store(true, std::memory_order_release);
        logLine(stderr, "error", m);
    });
    QObject::connect(&publisher, &moq2ts::MoqxrPublisher::publishError, &app, [&failed](const QString& m) {
        failed.store(true, std::memory_order_release);
        logLine(stderr, "error", m);
    });
    QObject::connect(&publisher, &moq2ts::MoqxrPublisher::connectionStateChanged, &app,
                     [](bool, const QString& m) { logLine(stderr, "conn", m); });

    // The latest totals, kept for the line printed at exit: the periodic line below
    // is throttled and comes before the last Group goes out. The pipeline counts
    // the media Objects it hands over; the publisher counts what it pulled and
    // handed to moqxr (those plus the catalog and timeline Objects), and what
    // moqxr sent. All are written from the worker threads, so these
    // connections are direct.
    struct Totals {
        std::atomic<int64_t> objects{0};
        std::atomic<int64_t> bytes{0};
        std::atomic<int64_t> groups{0};
        moq2ts::PublishTotals publish;
    };
    auto totals = std::make_shared<Totals>();
    QObject::connect(&publisher, &moq2ts::MoqxrPublisher::framePublished, &app,
                     [totals](const QString&, int64_t bytes, int64_t objects) {
                         totals->publish.recordPulled(objects, bytes);
                     },
                     Qt::DirectConnection);
    QObject::connect(&publisher, &moq2ts::MoqxrPublisher::sentTotals, &app,
                     [totals](int64_t objects, int64_t bytes) { totals->publish.recordSent(objects, bytes); },
                     Qt::DirectConnection);
    QObject::connect(&pipeline, &moq2ts::LivePipeline::stats, &app,
                     [totals](int64_t objects, int64_t bytes, int64_t groups) {
                         totals->objects.store(objects, std::memory_order_relaxed);
                         totals->bytes.store(bytes, std::memory_order_relaxed);
                         totals->groups.store(groups, std::memory_order_relaxed);
                     },
                     Qt::DirectConnection);

    // Throttle stats to roughly one line per second to keep server logs readable.
    auto* statsClock = new QElapsedTimer();
    statsClock->start();
    QObject::connect(&pipeline, &moq2ts::LivePipeline::stats, &app,
                     [statsClock](int64_t objects, int64_t bytes, int64_t groups) {
                         if (statsClock->elapsed() < 1000) {
                             return;
                         }
                         statsClock->restart();
                         std::fprintf(stdout, "[stats] objects=%lld bytes=%lld groups=%lld\n",
                                      static_cast<long long>(objects), static_cast<long long>(bytes),
                                      static_cast<long long>(groups));
                         std::fflush(stdout);
                     });

    if (!publisher.connect(cfg)) {
        logLine(stderr, "error", QStringLiteral("Could not open MOQ publish session."));
        return 1;
    }

    pipeline.start(cfg, &publisher);

    // Poll for SIGINT/SIGTERM and for natural completion (the worker flips
    // running() false at EOF). started only after start() so running() is true.
    // Local, so it cannot fire after the pipeline it reads has gone.
    QTimer poll;
    poll.setInterval(200);
    QObject::connect(&poll, &QTimer::timeout, &app, [&]() {
        if (g_interrupted.load(std::memory_order_acquire)) {
            logLine(stderr, "status", QStringLiteral("Interrupted; stopping."));
            pipeline.requestStop();
            app.quit();
            return;
        }
        if (!pipeline.running()) {
            app.quit();
        }
    });
    poll.start();

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, [&]() {
        publisher.stop();
        pipeline.requestStop();
#ifdef MOQ2TS_HAS_SRT
        // Stop SRT thread if running. srt_cleanup() itself is left to the
        // SrtIngest destructor, which runs on every exit path.
        srtIngest.stop();
#endif  // MOQ2TS_HAS_SRT
        pipeline.waitForStopped(); // bounded (3s) + detach fallback
        std::fprintf(stdout, "[final] objects=%lld bytes=%lld groups=%lld %s\n",
                     static_cast<long long>(totals->objects.load(std::memory_order_relaxed)),
                     static_cast<long long>(totals->bytes.load(std::memory_order_relaxed)),
                     static_cast<long long>(totals->groups.load(std::memory_order_relaxed)),
                     qUtf8Printable(totals->publish.finalFields()));
        std::fflush(stdout);
    });

    const int rc = app.exec();
    return (rc != 0 || failed.load(std::memory_order_acquire)) ? 1 : 0;
}
