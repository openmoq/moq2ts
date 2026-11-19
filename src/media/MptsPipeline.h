#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#include "../app/PublishConfig.h"
#include "SiTables.h"

namespace moq2ts {

class IMoqOutput;
class MptsFanOut;
class SourceGapLog;

// One program of a multi-program source, published as the per-program track
// program-<programNumber> (draft "Per-Program").
struct MptsProgramConfig {
    int programNumber = 0;
    // mpeg2tsMuxRate (MSFTS 6.7). 0 works it out from the rate measured at
    // start-up (workedOutMuxRate).
    qint64 muxRateBps = 0;
    // The SI tables the track keeps beside the program. Empty keeps none.
    QSet<SiTable> siTables;
};

struct MptsConfig {
    // A live source: a FIFO or a pipe (which SRT ingest writes into).
    QString sourcePath;
    QList<MptsProgramConfig> programs;
    int targetSegmentBytes = 64 * 1024;
    // Where SRT ingest skipped ahead in the source; null outside SRT ingest.
    std::shared_ptr<SourceGapLog> sourceGaps;
    // A program that does not open at start-up fails every program. Without
    // it, the others publish, and that program joins the catalog when it
    // opens (status() reports both).
    bool strictStart = false;
};

// Publishes several programs of one live multi-program source in one MOQT
// session: each program as a per-program track with its timeline track, and
// one catalog that lists every track.
//
// MptsFanOut reads the source once and gives every program its own queue. One
// M2tsPacketizer per program reads its queue on a worker thread of its own,
// and the workers push their Objects into one small merged queue, from which
// the publisher takes them in order. A stalled program does not fill the
// merged queue, so it holds up no other, and the publisher (ObjectLookahead)
// holds back no more than its one latest Object per track. While the
// publisher does not pull (no subscriber yet), the program queues take the
// wait and drop their oldest packets. At the first pull, nothing read before
// it goes out but the catalog: every program starts at its next random access
// point after the pull, at the live edge. A program that ends, or whose
// packetizer fails to read (error() says why), ends its own track only: its
// last Object closes its Group, and the other programs go on.
class MptsPipeline final : public QObject {
    Q_OBJECT
public:
    explicit MptsPipeline(QObject* parent = nullptr);
    ~MptsPipeline() override;

    // Starts the pipeline on a thread of its own and returns. Start-up errors
    // (a seekable source, no program open, or with strictStart one program not
    // open) come as error(), and running() then turns false. The publisher (a MoqxrPublisher, or a
    // stand-in in tests) must be connected.
    void start(const PublishConfig& relay, const MptsConfig& config, IMoqOutput* publisher);
    // Stops the source, the workers, and the publish loop, and joins them. A
    // publish that waits for a subscriber ends when the caller stops the
    // publisher, as with LivePipeline.
    void stop();
    // True from start() until the pipeline ends: every program ended, a
    // start-up error, or stop().
    bool running() const;
    // The catalog last published: the first one, or the last update. Empty
    // before the programs are open.
    QByteArray catalogJson() const;

signals:
    void status(const QString& message);
    // A start-up or publish error, which ends the pipeline.
    void error(const QString& message);
    // A read error of one program, which ends that program's track only, or a
    // program that never opened by the end of its source; the session goes
    // on with the other programs.
    void programEnded(int program, const QString& reason);
    // The publisher's first pull: every program queue dropped what it held,
    // so what the source sends from now on is what the tracks start with.
    void atLiveEdge();

private:
    struct Shared;
    void run();

    PublishConfig m_relay;
    MptsConfig m_config;
    IMoqOutput* m_publisher = nullptr;

    std::atomic<bool> m_running{false};
    std::thread m_thread;

    // Guards m_fanOut and m_shared, which run() sets and stop() reaches into.
    mutable std::mutex m_mutex;
    std::shared_ptr<MptsFanOut> m_fanOut;
    std::shared_ptr<Shared> m_shared;
    QByteArray m_catalogJson;   // kept after the run, for catalogJson()
    // True while run() opens the source, which blocks on a FIFO until a writer
    // opens it.
    std::atomic<bool> m_openingSource{false};
};

} // namespace moq2ts
