#include "keydiagnostics.h"

#include "node.h"

#include <QDateTime>
#include <QLoggingCategory>

#include <algorithm>
#include <utility>

Q_LOGGING_CATEGORY(lcKeyDiag, "deskflower.keydiag")

namespace df {

namespace {
const QString kT = QStringLiteral("t");
const QString kOn = QStringLiteral("on");
const QString kMs = QStringLiteral("ms");
const QString kL = QStringLiteral("l");
const QString kRequest = QStringLiteral("diag.key");
const QString kLine = QStringLiteral("diag.line");

QString hex(quint32 scancode)
{
    return QStringLiteral("0x") + QString::number(scancode, 16).toUpper().rightJustified(scancode > 0xFF ? 4 : 2, QLatin1Char('0'));
}

// A line as kept: one line, bounded.
QString clean(const QString &line)
{
    QString l = line.left(KeyDiagnostics::kMaxLineLength);
    l.replace(QLatin1Char('\n'), QLatin1Char(' ')).replace(QLatin1Char('\r'), QLatin1Char(' '));
    return l;
}

bool isModifier(quint32 sc)
{
    switch (sc) {
    case 0x2A: case 0x36: case 0x1D: case 0xE01D: case 0x38: case 0xE038: case 0xE05B: case 0xE05C:
        return true;
    default:
        return false;
    }
}
} // namespace

KeyDiagnostics::KeyDiagnostics(Node *node, QObject *parent) : QObject(parent), m_node(node)
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, [this] { end(QStringLiteral("time is up")); });
    connect(m_node, &Node::messageReceived, this, &KeyDiagnostics::onMessage);
}

QCborMap KeyDiagnostics::requestMessage(bool on, int ms)
{
    QCborMap m;
    m.insert(kT, kRequest);
    m.insert(kOn, on);
    if (on)
        m.insert(kMs, ms);
    return m;
}

QString KeyDiagnostics::keyName(quint32 sc)
{
    switch (sc) {
    case 0x01: return QStringLiteral("Esc");
    case 0x0F: return QStringLiteral("Tab");
    case 0x1C: return QStringLiteral("Enter");
    case 0x2A: return QStringLiteral("LShift");
    case 0x36: case 0xE036: return QStringLiteral("RShift");
    case 0x1D: return QStringLiteral("LCtrl");
    case 0xE01D: return QStringLiteral("RCtrl");
    case 0x38: return QStringLiteral("LAlt");
    case 0xE038: return QStringLiteral("RAlt/AltGr");
    case 0xE05B: return QStringLiteral("LWin");
    case 0xE05C: return QStringLiteral("RWin");
    case 0xE05D: return QStringLiteral("Menu");
    case 0x20: return QStringLiteral("D");
    case 0x12: return QStringLiteral("E");
    case 0x13: return QStringLiteral("R");
    case 0x26: return QStringLiteral("L");
    case 0x0E: return QStringLiteral("Backspace");
    case 0xE053: return QStringLiteral("Delete");
    default: return {};
    }
}

qint64 KeyDiagnostics::remainingMs() const
{
    return m_active ? std::max<qint64>(0, m_windowMs - m_since.elapsed()) : 0;
}

bool KeyDiagnostics::mayAsk(const QByteArray &peerId) const
{
    const auto declared = m_node->declaredTrust(peerId);
    return m_node->isTrusted(peerId) && !m_node->isGuest(peerId) && !(declared && *declared == TrustLevel::Guest);
}

QString KeyDiagnostics::peerName(const QByteArray &id) const
{
    for (const PeerView &p : m_node->peers()) {
        if (p.id == id)
            return p.name.isEmpty() ? QString::fromLatin1(id.toHex().left(8)) : p.name;
    }
    return QString::fromLatin1(id.toHex().left(8));
}

void KeyDiagnostics::start()
{
    if (m_active) {
        const QSet<QByteArray> asked = std::exchange(m_asked, {});
        for (const QByteArray &peer : asked)
            m_node->sendMessage(peer, requestMessage(false, 0));
        m_active = false;
        m_timer.stop();
        emit activeChanged(false);
    }
    begin({}, m_durationMs);
    QStringList names;
    for (const PeerView &p : m_node->peers()) {
        if (!p.connected || !p.trusted || p.otherUser || !mayAsk(p.id))
            continue;
        if (m_node->sendMessage(p.id, requestMessage(true, m_durationMs))) {
            m_asked.insert(p.id);
            names << QStringLiteral("%1 (%2)").arg(peerName(p.id), p.version.isEmpty() ? QStringLiteral("?") : p.version);
        }
    }
    append(names.isEmpty() ? QStringLiteral("no other device connected to ask")
                           : QStringLiteral("asked to record too: %1 (versions without diagnostics ignore it)")
                                 .arg(names.join(QStringLiteral(", "))),
           false);
}

void KeyDiagnostics::stop()
{
    if (!m_active)
        return;
    end(QStringLiteral("stopped by the user"));
}

void KeyDiagnostics::begin(const QByteArray &requestedBy, int ms)
{
    m_active = true;
    m_requestedBy = requestedBy;
    m_asked.clear();
    m_held.clear();
    m_header.clear();
    m_lines.clear();
    m_dropped = 0;
    m_endedAt.invalidate();
    m_windowMs = ms;
    m_since.start();
    m_timer.start(ms);
    emit activeChanged(true);
    append(QStringLiteral("started on %1 for %2 s%3")
               .arg(m_node->name())
               .arg(ms / 1000)
               .arg(requestedBy.isEmpty() ? QString() : QStringLiteral(", asked by ") + peerName(requestedBy)),
           true);
}

void KeyDiagnostics::end(const QString &why)
{
    if (!m_active)
        return;
    append(QStringLiteral("ended on %1: %2").arg(m_node->name(), why), true);
    if (m_requestedBy.isEmpty()) {
        for (const QByteArray &peer : std::as_const(m_asked))
            m_node->sendMessage(peer, requestMessage(false, 0));
    }
    m_active = false;
    m_timer.stop();
    m_held.clear();
    // The lines stay for the agents to show and copy (until the next start); the devices asked
    // are heard a little longer, for their last lines on the way (their own end among them).
    m_endedAt.start();
    emit activeChanged(false);
}

void KeyDiagnostics::addLine(const QString &line)
{
    if (m_active)
        append(clean(line), true);
}

void KeyDiagnostics::noteKey(const QByteArray &peer, quint32 sc, bool down, bool sent)
{
    if (!m_active)
        return;
    const QByteArray key = (peer.isEmpty() ? QByteArray("n") : QByteArray(sent ? "s" : "r")) + peer;
    QHash<quint32, int> &held = m_held[key];
    // Autorepeat: one line when the key goes down and one when it comes up, with the count.
    int repeats = 0;
    if (down) {
        const auto it = held.find(sc);
        if (it != held.end()) {
            ++*it;
            return;
        }
    } else {
        repeats = held.value(sc);
    }
    const QString state = down ? QStringLiteral("down") : QStringLiteral("up");
    const QString repeated = repeats ? QStringLiteral(" (after %1 repeats)").arg(repeats) : QString();
    if (peer.isEmpty()) {
        if (down)
            held.insert(sc, 0);
        else
            held.remove(sc);
        append(QStringLiteral("core: key sc=%1 %2%3 from the helper not forwarded (the input is on this device, or a view awaits approval)")
                   .arg(hex(sc), state, repeated),
               true);
        return;
    }
    QStringList mods;
    for (const quint32 m : {0x1Du, 0xE01Du, 0x2Au, 0x36u, 0x38u, 0xE038u, 0xE05Bu, 0xE05Cu}) {
        if (held.contains(m) && m != sc)
            mods << keyName(m);
    }
    if (down)
        held.insert(sc, 0);
    else
        held.remove(sc);
    const QString name = keyName(sc);
    append(QStringLiteral("core: %1 in.key sc=%2%3 %4%5%6 %7 %8")
               .arg(sent ? QStringLiteral("sent") : QStringLiteral("received"), hex(sc),
                    name.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(name), state, repeated,
                    mods.isEmpty() || isModifier(sc) ? QString() : QStringLiteral(" with ") + mods.join(QLatin1Char('+')),
                    sent ? QStringLiteral("to") : QStringLiteral("from"), peerName(peer)),
           true);
}

void KeyDiagnostics::noteIgnoredKey(const QByteArray &peer, quint32 sc, bool down, const QString &why)
{
    if (!m_active)
        return;
    const QString name = keyName(sc);
    append(QStringLiteral("core: ignored in.key sc=%1%2 %3 from %4: %5")
               .arg(hex(sc), name.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(name),
                    down ? QStringLiteral("down") : QStringLiteral("up"), peerName(peer), why),
           true);
}

bool KeyDiagnostics::isHeaderLine(const QString &stamped)
{
    // "HH:mm:ss.zzz " then, from a device asked, "[name] " and its own stamp.
    QStringView l = QStringView(stamped).mid(13);
    if (l.startsWith(QLatin1Char('['))) {
        const qsizetype close = l.indexOf(QLatin1String("] "));
        if (close < 0)
            return false;
        l = l.mid(close + 2 + 13);
    }
    return l.startsWith(QLatin1String("started on ")) || l.startsWith(QLatin1String("helper: where: "))
           || l.startsWith(QLatin1String("asked to record too: ")) || l.startsWith(QLatin1String("no other device connected"));
}

QStringList KeyDiagnostics::lines() const
{
    QStringList all = m_header;
    if (m_dropped)
        all << QStringLiteral("... %1 older lines dropped (in the log) ...").arg(m_dropped);
    all << m_lines;
    return all;
}

void KeyDiagnostics::append(const QString &line, bool forward)
{
    const QString stamped = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz ")) + line;
    qCInfo(lcKeyDiag).noquote() << "keydiag:" << stamped;
    const bool header = isHeaderLine(stamped) && m_header.size() < kMaxHeaderLines;
    if (header) {
        m_header.append(stamped);
    } else {
        m_lines.append(stamped);
        // The header, the "dropped" line and these: at most kMaxLines.
        while (m_lines.size() > kMaxLines - 1 - m_header.size()) {
            m_lines.removeFirst();
            ++m_dropped;
        }
    }
    emit lineAdded(stamped, header);
    if (forward && !m_requestedBy.isEmpty()) {
        QCborMap m;
        m.insert(kT, kLine);
        m.insert(kL, stamped);
        m_node->sendMessage(m_requestedBy, m);
    }
}

void KeyDiagnostics::onMessage(const QByteArray &peerId, const QCborMap &msg)
{
    const QString type = msg.value(kT).toString();
    if (!type.startsWith(QLatin1String("diag.")))
        return;
    if (type == kRequest) {
        const bool on = msg.value(kOn).toBool();
        if (!on) {
            if (m_active && m_requestedBy == peerId)
                end(QStringLiteral("stopped by ") + peerName(peerId));
            return;
        }
        if (m_active || !mayAsk(peerId)) {
            qCInfo(lcKeyDiag) << "Ignoring a keyboard diagnostics request from" << peerId.toHex()
                              << (m_active ? "(one runs already)" : "(not trusted)");
            return;
        }
        const qint64 ms = msg.value(kMs).toInteger();
        begin(peerId, int(ms > 0 ? std::min<qint64>(ms, m_durationMs) : m_durationMs)); // never longer than here
    } else if (type == kLine) {
        // Only from a device asked for this run, while it runs (and a little after: its last lines
        // were on the way).
        const bool open = m_active || (m_endedAt.isValid() && m_endedAt.elapsed() < kLateLinesMs);
        if (!open || !m_requestedBy.isEmpty() || !m_asked.contains(peerId))
            return;
        append(QStringLiteral("[%1] %2").arg(peerName(peerId), clean(msg.value(kL).toString())), false);
    }
}

} // namespace df
