#pragma once

// Keyboard diagnostics: for a limited time (kDurationMs, 2 minutes), the keys that go between this
// device and the others are recorded, step by step, so that a key combination that does nothing on
// the device being controlled (Win+D on Windows...) can be followed from the hook that caught it to
// the injection that should have typed it. Started by the user from the agent (Advanced settings,
// "Keyboard diagnostics"), stopped by them or when the time is up.
//
// Each line goes to the log with the "keydiag:" prefix and to the agents (lineAdded), which show
// them and copy them. Lines come from:
//  - the service: each key forwarded to another device and each key received from one
//    (noteKey: scan code, name, down/up, the modifiers held in that direction, which device);
//  - the session helper (addLine, its "diag" messages): what its keyboard hook sees, whether a key
//    was swallowed and forwarded, each injection with the exact fields given to the system and
//    its result, and once at the start where it runs (user, integrity, session, desktop, the
//    foreground program);
//  - the other devices asked (below), prefixed with their name.
// Only keys that go to or come from another device are recorded (and the events the helper's own
// injection causes): what is typed for this device itself is not, and nothing at all outside the
// window. What is typed on the other device while it records is, by scan code (passwords too), in
// the log of both devices: the agent says not to type anything secret meanwhile.
//
// The volume stays modest: a key held down (autorepeat) is one line when pressed and one when
// released, saying how many repeats went between; the lines that say where each side runs (the
// start, the helper's "where:", the devices asked) are kept apart from the others (header()) and
// never dropped, while the rest keeps the newest kMaxLines.
//
// Starting it here also asks the connected devices that trust this one (not guests, either way)
// to record for the same time and send their lines back: "diag.key {on, ms}" turns it on (ms at
// most kDurationMs) or off, "diag.line {l}" carries one line back to the device that asked.
// A device runs one diagnostic at a time (a request while one runs is ignored) and only the
// device that asked can stop it remotely. Versions without diagnostics ignore both messages (the
// higher layers only take their own message types).

#include <QByteArray>
#include <QCborMap>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>

namespace df {

class Node;

class KeyDiagnostics : public QObject
{
    Q_OBJECT
public:
    static constexpr int kDurationMs = 2 * 60 * 1000;
    static constexpr int kMaxLines = 2000;      // kept for the agents (the oldest go first)
    static constexpr int kMaxHeaderLines = 40;  // of those, the header (never dropped)
    static constexpr int kLateLinesMs = 2000;   // a device asked is still heard after the end
    static constexpr int kMaxLineLength = 400;  // characters, lines from the helper or a peer

    explicit KeyDiagnostics(Node *node, QObject *parent = nullptr);

    // Tests: a shorter window (also the most a peer may ask for).
    void setDurationMs(int ms) { m_durationMs = ms; }

    // The local user starts or stops it. Starting while it runs (also on behalf of another
    // device) starts afresh, here and on the devices asked.
    void start();
    void stop();

    bool isActive() const { return m_active; }
    qint64 remainingMs() const;
    // The device whose user started it (empty: this one).
    QByteArray requestedBy() const { return m_requestedBy; }
    QList<QByteArray> asked() const { return m_asked.values(); }
    // The header (where each side runs), then the newest lines, with a line saying how many were
    // dropped in between, if any: at most kMaxLines in all.
    QStringList lines() const;
    QStringList header() const { return m_header; }
    // Whether a line (as kept, with its time stamp, from here or a device asked) belongs to the
    // header.
    static bool isHeaderLine(const QString &stamped);

    // A line from this device (the session helper): kept, logged and, when another device asked,
    // sent to it. Ignored while inactive.
    void addLine(const QString &line);
    // A key forwarded to (`sent`) or received from `peer` (InputController::setKeyTrace).
    void noteKey(const QByteArray &peer, quint32 scancode, bool down, bool sent);
    // A key from `peer` the input controller did not take (InputController::setIgnoredKeyTrace).
    void noteIgnoredKey(const QByteArray &peer, quint32 scancode, bool down, const QString &why);

    // A readable name for the keys that matter in combinations (modifiers and a few others), else
    // empty.
    static QString keyName(quint32 scancode);
    static QCborMap requestMessage(bool on, int ms);

signals:
    // Turned on or off (the session helper follows: "keyDiag {on}").
    void activeChanged(bool on);
    // `header`: one of the lines that are never dropped (isHeaderLine).
    void lineAdded(const QString &line, bool header);

private:
    void onMessage(const QByteArray &peerId, const QCborMap &msg);
    void begin(const QByteArray &requestedBy, int ms);
    void end(const QString &why);
    void append(const QString &line, bool forward);
    QString peerName(const QByteArray &id) const;
    bool mayAsk(const QByteArray &peerId) const;

    Node *m_node;
    int m_durationMs = kDurationMs;
    bool m_active = false;
    QByteArray m_requestedBy;
    QSet<QByteArray> m_asked; // devices asked to record for us (their lines are taken)
    QElapsedTimer m_endedAt;  // their lines still come for kLateLinesMs after the end
    QStringList m_header;
    QStringList m_lines;
    int m_dropped = 0;
    QTimer m_timer;
    QElapsedTimer m_since;
    int m_windowMs = 0;
    // Keys held per direction and device (noteKey), to show the modifiers of a combination, with
    // the autorepeats seen since each went down.
    QHash<QByteArray, QHash<quint32, int>> m_held;
};

} // namespace df
