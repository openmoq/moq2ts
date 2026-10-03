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

#include "media/SrtSource.h"
#endif

#include "app/PublishConfig.h"
#include "media/LivePipeline.h"
#include "publish/MoqxrPublisher.h"

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
// Stops the SRT receive thread and shuts the SRT library down on every exit path,
// including early returns. Each step is guarded so running twice is harmless.
struct SrtRuntimeGuard {
    std::atomic<bool>* stopFlag = nullptr;
    std::unique_ptr<std::thread>* thread = nullptr;
    int* sock = nullptr;
    int* readFd = nullptr;

    ~SrtRuntimeGuard() {
        if (stopFlag) {
            stopFlag->store(true, std::memory_order_release);
        }
        // The receive thread must be gone before srt_cleanup() tears down libsrt.
        if (thread && *thread && (*thread)->joinable()) {
            (*thread)->join();
        }
        if (sock && *sock >= 0) {
            moq2ts::SrtSource::close(*sock);
            *sock = -1;
        }
        if (readFd && *readFd >= 0) {
            ::close(*readFd);
            *readFd = -1;
        }
        moq2ts::SrtSource::shutdown();
    }
};
#endif  // MOQ2TS_HAS_SRT

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
    addValue(QStringLiteral("program"), QStringLiteral("MPEG program number (0 = first)."), QStringLiteral("n"), QStringLiteral("0"));
    // MSFTS carriage-profile options (msfts#7).
    addValue(QStringLiteral("mux-rate"), QStringLiteral("Advisory source mux rate (bits/s); 0 omits the catalog hint. Dropped when an unmodified source lists several programs."),
             QStringLiteral("bps"), QStringLiteral("0"));
    // --transparent is the original name, kept so existing scripts still work.
    parser.addOption(QCommandLineOption(QStringList{QStringLiteral("unmodified"), QStringLiteral("transparent")},
        QStringLiteral("Unmodified carriage: forward every source packet unchanged. The track is "
                       "unmodified-program for a single-program source and unmodified-multiplex "
                       "otherwise. --transparent is an alias.")));
    parser.addOption(QCommandLineOption(QStringLiteral("paced"),
        QStringLiteral("Pace file-source publishing at media-time rate (prevents dumping all data at wire speed).")));
    addValue(QStringLiteral("draft"), QStringLiteral("MOQ draft version (14 or 16)."),
             QStringLiteral("version"), QStringLiteral("16"));
    parser.addOption(QCommandLineOption(QStringLiteral("retain-si"),
        QStringLiteral("Per-program mode: also keep DVB SI PIDs (NIT/SDT/EIT/TDT-TOT), with the SDT and EIT reduced to the carried service.")));
    parser.addOption(QCommandLineOption(QStringLiteral("retain-null"),
        QStringLiteral("Per-program mode: also keep null (0x1FFF) packets.")));
    // SRT ingest source (alternative to --video file).
    // Uses a JSON config file matching moqxr's format:
    //   { "srt_callers": [{ "id": "...", "srt": { "mode": "caller", "host": "...", "port": N, "latency_ms": N } }] }
    addValue(QStringLiteral("srt-config"), QStringLiteral("SRT config JSON file (moqxr-compatible format)."),
             QStringLiteral("path"));

    parser.process(app);

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
    cfg.retainSiTables = parser.isSet(QStringLiteral("retain-si"));
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
    int srtPipeFds[2] = {-1, -1}; // [0]=read, [1]=write
    int srtSock = -1;
    std::atomic<bool> srtStop{false};
    std::unique_ptr<std::thread> srtThread;
    // Declared before the publisher/pipeline so it is destroyed after them, and so
    // it runs on EVERY exit path including the early returns below. srt_startup()
    // happens inside SrtSource::connect(); leaving it unpaired crashes in libsrt's
    // exit-time destructors. Every step is idempotent, so the normal shutdown in
    // aboutToQuit doing the same work first is harmless.
    SrtRuntimeGuard srtGuard{&srtStop, &srtThread, &srtSock, &srtPipeFds[0]};

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
        srtCfg.host = srtObj.value(QStringLiteral("host")).toString(QStringLiteral("127.0.0.1")).toStdString();
        srtCfg.port = static_cast<uint16_t>(srtObj.value(QStringLiteral("port")).toInt(9000));
        srtCfg.latencyMs = srtObj.value(QStringLiteral("latency_ms")).toInt(120);
        // Optional receive-headroom overrides; absent keys keep the built-in defaults.
        srtCfg.rcvBufBytes = srtObj.value(QStringLiteral("rcvbuf_bytes")).toInt(srtCfg.rcvBufBytes);
        srtCfg.udpRcvBufBytes =
            srtObj.value(QStringLiteral("udp_rcvbuf_bytes")).toInt(srtCfg.udpRcvBufBytes);

        logLine(stderr, "srt", QStringLiteral("Caller '%1': connecting to srt://%2:%3 (latency=%4ms)...")
                                   .arg(callerId)
                                   .arg(QString::fromStdString(srtCfg.host))
                                   .arg(srtCfg.port)
                                   .arg(srtCfg.latencyMs));

        const QString err = moq2ts::SrtSource::connect(srtCfg, srtSock);
        if (!err.isEmpty()) {
            logLine(stderr, "error", err);
            return 1;
        }
        logLine(stderr, "srt", QStringLiteral("Connected to SRT source"));

        // Create a pipe: SRT thread writes TS to pipe[1], M2tsPacketizer reads from pipe[0]
        if (::pipe(srtPipeFds) != 0) {
            logLine(stderr, "error", QStringLiteral("Failed to create pipe for SRT: %1").arg(strerror(errno)));
            moq2ts::SrtSource::close(srtSock);
            return 1;
        }

        // Point the video source at the read end of the pipe
        cfg.videoSource = QStringLiteral("/dev/fd/%1").arg(srtPipeFds[0]);

        // Start the SRT receive thread
        srtThread = std::make_unique<std::thread>([srtSock, writeFd = srtPipeFds[1], &srtStop]() {
            moq2ts::SrtSource::receiveLoop(srtSock, writeFd, srtStop);
            ::close(writeFd); // close write end to signal EOF to reader
        });

        // SRT is a live source - no pacing needed (real-time from encoder)
        cfg.pacedFileSource = false;
        // Force unmodified carriage for SRT. An SRT contribution feed is forwarded
        // as received, so deriving one program from it is not what this path is
        // for. Say so rather than overriding in silence: the per-program options
        // otherwise appear to be accepted and then do nothing, which is
        // indistinguishable from a bug in the filter itself.
        if (!cfg.transparentMode) {
            QStringList ignored;
            if (cfg.retainSiTables) {
                ignored << QStringLiteral("--retain-si");
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

    // The draft forbids mpeg2tsMuxRate in unmodified-multiplex mode, so the
    // catalog drops it; say so rather than accepting a value that does nothing.
    // Whether an unmodified source is a multiplex is known only once its PAT
    // is read, so the warning names the condition.
    if (cfg.transparentMode && cfg.mpeg2tsMuxRateBps > 0) {
        logLine(stderr, "warn",
                QStringLiteral("--mux-rate is ignored in unmodified publishing when the source "
                               "PAT lists several programs (unmodified-multiplex)."));
    }

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
    auto* poll = new QTimer(&app);
    poll->setInterval(200);
    QObject::connect(poll, &QTimer::timeout, &app, [&]() {
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
    poll->start();

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, [&]() {
        publisher.stop();
        pipeline.requestStop();
#ifdef MOQ2TS_HAS_SRT
        // Stop SRT thread if running
        srtStop.store(true, std::memory_order_release);
        if (srtThread && srtThread->joinable()) {
            srtThread->join();
        }
        if (srtSock >= 0) {
            moq2ts::SrtSource::close(srtSock);
            srtSock = -1;
        }
        if (srtPipeFds[0] >= 0) {
            ::close(srtPipeFds[0]);
            srtPipeFds[0] = -1;
        }
        // srt_cleanup() itself is left to srtGuard, which runs on every exit path.
#endif  // MOQ2TS_HAS_SRT
        pipeline.waitForStopped(); // bounded (3s) + detach fallback
    });

    const int rc = app.exec();
    return (rc != 0 || failed.load(std::memory_order_acquire)) ? 1 : 0;
}
