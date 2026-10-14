#include "pairing.h"

#include "clock.h"

#include <QSysInfo>
#include <utility>

namespace df {

namespace {
constexpr int kRetransmitMs = 700;
constexpr int kMaxRetries = 12;

const QString kI = QStringLiteral("i");
const QString kN = QStringLiteral("n");
const QString kK = QStringLiteral("k");
const QString kO = QStringLiteral("o");
const QString kS = QStringLiteral("s");
const QString kY = QStringLiteral("y");
const QString kC = QStringLiteral("c");
const QString kR = QStringLiteral("r");
} // namespace

struct PairingManager::Outgoing {
    enum class Stage { Requesting, WaitingPin, SentMsg1, SentMsg3 };
    Stage stage = Stage::Requesting;
    QByteArray peerId;
    QHostAddress addr;
    quint16 port = 0;
    QByteArray peerPub;
    QString peerName;
    QByteArray sid;
    std::unique_ptr<CPace> cpace;
    QByteArray isk;
    QByteArray lastPacket;
    qint64 lastSent = 0;
    qint64 startedAt = 0;
    int retries = 0;
};

struct PairingManager::Incoming {
    enum class Stage { ChallengeSent, Msg2Sent, Done };
    Stage stage = Stage::ChallengeSent;
    QByteArray peerId;
    QHostAddress addr;
    quint16 port = 0;
    QByteArray peerPub;
    QString peerName;
    QByteArray sid;
    QString pin;
    QByteArray peerShare;
    QByteArray isk;
    QByteArray cachedReply;
    qint64 replyAt = 0; // PairMsg2 is not sent (nor repeated) before then
    qint64 expiresAt = 0;
};

PairingManager::PairingManager(const Identity &self, SendFn send, QObject *parent)
    : QObject(parent), m_self(self), m_send(std::move(send))
{
    m_timer.setInterval(200);
    connect(&m_timer, &QTimer::timeout, this, &PairingManager::onTimer);
    m_timer.start();
}

PairingManager::~PairingManager() = default;

QCborMap PairingManager::selfInfo() const
{
    QCborMap m;
    m.insert(kI, m_self.id());
    m.insert(kN, m_self.name());
    m.insert(kK, m_self.keys().publicKey);
    m.insert(kO, QSysInfo::productType());
    return m;
}

QByteArray PairingManager::channelId(const QByteArray &idA, const QByteArray &pubA, const QByteArray &idB,
                                     const QByteArray &pubB) const
{
    // All fields have fixed sizes, so plain concatenation is unambiguous.
    return idA + pubA + idB + pubB;
}

void PairingManager::sendOut(const QByteArray &packet)
{
    m_out->lastPacket = packet;
    m_out->lastSent = nowMs();
    m_out->retries = 0;
    m_send(packet, m_out->addr, m_out->port);
}

void PairingManager::abortTo(const QByteArray &sid, const QString &reason, const QHostAddress &addr, quint16 port)
{
    QCborMap m;
    m.insert(kI, m_self.id());
    m.insert(kS, sid);
    m.insert(kR, reason);
    m_send(wire::cborPacket(wire::Type::PairAbort, m), addr, port);
}

void PairingManager::failOutgoing(const QString &reason, bool notifyPeer)
{
    if (!m_out)
        return;
    auto out = std::move(m_out);
    if (notifyPeer)
        abortTo(out->sid, reason, out->addr, out->port);
    emit failed(out->peerId, reason);
}

void PairingManager::failIncoming(const QString &reason, bool notifyPeer, bool attempt)
{
    if (!m_in)
        return;
    auto in = std::move(m_in);
    if (notifyPeer)
        abortTo(in->sid, reason, in->addr, in->port);
    QString shown = reason;
    if (attempt && ++m_failedAttempts >= kMaxFailedAttempts) {
        m_failedAttempts = 0;
        m_lockedUntil = nowMs() + kLockoutMs;
        shown = tr("%1. Too many failed attempts: pairing is blocked for %n minute(s)", nullptr,
                   int(kLockoutMs / 60000))
                    .arg(reason);
    }
    emit failed(in->peerId, shown);
}

bool PairingManager::start(const QByteArray &peerId, const QHostAddress &addr, quint16 port)
{
    if (m_out)
        failOutgoing(tr("Cancelled"), true);
    m_out = std::make_unique<Outgoing>();
    m_out->peerId = peerId;
    m_out->addr = addr;
    m_out->port = port;
    m_out->startedAt = nowMs();
    sendOut(wire::cborPacket(wire::Type::PairRequest, selfInfo()));
    return true;
}

bool PairingManager::submitPin(const QByteArray &peerId, const QString &pin)
{
    if (!m_out || m_out->peerId != peerId || m_out->stage != Outgoing::Stage::WaitingPin)
        return false;
    const QString digits = QString(pin).remove(QLatin1Char(' ')).remove(QLatin1Char('-'));
    m_out->cpace = std::make_unique<CPace>(
        digits, m_out->sid, channelId(m_self.id(), m_self.keys().publicKey, m_out->peerId, m_out->peerPub));
    if (!m_out->cpace->isValid()) {
        failOutgoing(tr("Cryptographic error"), true);
        return false;
    }
    QCborMap m;
    m.insert(kI, m_self.id());
    m.insert(kS, m_out->sid);
    m.insert(kY, m_out->cpace->share());
    m_out->stage = Outgoing::Stage::SentMsg1;
    sendOut(wire::cborPacket(wire::Type::PairMsg1, m));
    return true;
}

void PairingManager::cancel(const QByteArray &peerId)
{
    if (m_out && m_out->peerId == peerId)
        failOutgoing(tr("Cancelled by the user"), true);
    if (m_in && m_in->peerId == peerId)
        failIncoming(tr("Cancelled by the user"), true, false);
}

void PairingManager::onTimer()
{
    const qint64 now = nowMs();
    if (m_out) {
        if (m_out->stage == Outgoing::Stage::WaitingPin) {
            if (now - m_out->startedAt > kPinTimeoutMs)
                failOutgoing(tr("Timed out"), true);
        } else if (now - m_out->lastSent > kRetransmitMs) {
            if (++m_out->retries > kMaxRetries) {
                failOutgoing(tr("The other device is not responding"), false);
            } else {
                m_out->lastSent = now;
                m_send(m_out->lastPacket, m_out->addr, m_out->port);
            }
        }
    }
    if (m_in && now > m_in->expiresAt) {
        if (m_in->stage == Incoming::Stage::Done)
            m_in.reset();
        else
            failIncoming(tr("Timed out"), true);
    }
}

void PairingManager::handlePacket(wire::Type type, const QCborMap &msg, const QHostAddress &from, quint16 port)
{
    switch (type) {
    case wire::Type::PairRequest:
    case wire::Type::PairMsg1:
    case wire::Type::PairMsg3:
        handleAsResponder(type, msg, from, port);
        break;
    case wire::Type::PairChallenge:
    case wire::Type::PairMsg2:
    case wire::Type::PairDone:
        handleAsInitiator(type, msg, from, port);
        break;
    case wire::Type::PairAbort: {
        const QByteArray sid = msg.value(kS).toByteArray();
        const QString reason = msg.value(kR).toString();
        if (m_out && !sid.isEmpty() && m_out->sid == sid)
            failOutgoing(reason, false);
        else if (m_out && m_out->stage == Outgoing::Stage::Requesting && msg.value(kI).toByteArray() == m_out->peerId)
            failOutgoing(reason, false);
        if (m_in && !sid.isEmpty() && m_in->sid == sid && m_in->stage != Incoming::Stage::Done)
            failIncoming(reason, false);
        break;
    }
    default:
        break;
    }
}

void PairingManager::handleAsResponder(wire::Type type, const QCborMap &msg, const QHostAddress &from, quint16 port)
{
    const QByteArray peerId = msg.value(kI).toByteArray();

    if (type == wire::Type::PairRequest) {
        const QByteArray pub = msg.value(kK).toByteArray();
        if (pub.size() != crypto::kKeySize || crypto::deviceIdFromPublicKey(pub) != peerId || peerId == m_self.id())
            return;
        if (m_in && m_in->peerId == peerId && m_in->stage == Incoming::Stage::ChallengeSent) {
            m_send(m_in->cachedReply, from, port); // duplicate request
            return;
        }
        if (!m_accepting) {
            abortTo({}, tr("The other device is not in discovery mode"), from, port);
            return;
        }
        if (const qint64 left = m_lockedUntil - nowMs(); left > 0) {
            abortTo({},
                    tr("Too many failed pairing attempts: try again in %n minute(s)", nullptr,
                       int((left + 59999) / 60000)),
                    from, port);
            return;
        }
        if (m_in && m_in->stage != Incoming::Stage::Done) {
            abortTo({}, tr("The other device is pairing with another device"), from, port);
            return;
        }
        m_in = std::make_unique<Incoming>();
        m_in->peerId = peerId;
        m_in->peerPub = pub;
        m_in->peerName = msg.value(kN).toString().left(64);
        m_in->addr = from;
        m_in->port = port;
        m_in->sid = crypto::randomBytes(16);
        m_in->pin = QStringLiteral("%1").arg(crypto::randomUniform(1000000), 6, 10, QLatin1Char('0'));
        m_in->expiresAt = nowMs() + kPinTimeoutMs;
        QCborMap reply = selfInfo();
        reply.insert(kS, m_in->sid);
        m_in->cachedReply = wire::cborPacket(wire::Type::PairChallenge, reply);
        m_send(m_in->cachedReply, from, port);
        emit pinGenerated(m_in->peerId, m_in->peerName, m_in->pin);
        return;
    }

    if (!m_in || m_in->peerId != peerId || msg.value(kS).toByteArray() != m_in->sid)
        return;

    if (type == wire::Type::PairMsg1) {
        const QByteArray share = msg.value(kY).toByteArray();
        if (m_in->stage == Incoming::Stage::Msg2Sent && share == m_in->peerShare) {
            if (nowMs() >= m_in->replyAt)
                m_send(m_in->cachedReply, from, port); // duplicate
            return;
        }
        if (m_in->stage != Incoming::Stage::ChallengeSent)
            return; // the PIN is single use
        CPace cpace(m_in->pin,
                    m_in->sid,
                    channelId(m_in->peerId, m_in->peerPub, m_self.id(), m_self.keys().publicKey));
        auto isk = cpace.finish(share, false);
        if (!isk) {
            failIncoming(tr("Invalid pairing message"), true);
            return;
        }
        m_in->isk = *isk;
        m_in->peerShare = share;
        m_in->pin.clear();
        m_in->stage = Incoming::Stage::Msg2Sent;
        QCborMap reply;
        reply.insert(kI, m_self.id());
        reply.insert(kS, m_in->sid);
        reply.insert(kY, cpace.share());
        reply.insert(kC, CPace::confirmationTag(m_in->isk, 'B'));
        m_in->cachedReply = wire::cborPacket(wire::Type::PairMsg2, reply);
        if (m_failedAttempts == 0) {
            m_send(m_in->cachedReply, from, port);
            return;
        }
        // An attempt already failed: the answer to this guess waits.
        m_in->replyAt = nowMs() + kRetryDelayMs;
        QTimer::singleShot(kRetryDelayMs, this, [this, sid = m_in->sid, from, port] {
            if (m_in && m_in->sid == sid && m_in->stage == Incoming::Stage::Msg2Sent)
                m_send(m_in->cachedReply, from, port);
        });
        return;
    }

    if (type == wire::Type::PairMsg3) {
        if (m_in->stage == Incoming::Stage::Done) {
            m_send(m_in->cachedReply, from, port);
            return;
        }
        if (m_in->stage != Incoming::Stage::Msg2Sent)
            return;
        if (!crypto::constantTimeEqual(msg.value(kC).toByteArray(), CPace::confirmationTag(m_in->isk, 'A'))) {
            failIncoming(tr("Wrong PIN"), true);
            return;
        }
        QCborMap reply;
        reply.insert(kI, m_self.id());
        reply.insert(kS, m_in->sid);
        m_in->cachedReply = wire::cborPacket(wire::Type::PairDone, reply);
        m_in->stage = Incoming::Stage::Done;
        m_in->expiresAt = nowMs() + 10000; // keep answering duplicates for a while
        m_failedAttempts = 0;
        crypto::wipe(m_in->isk);
        m_send(m_in->cachedReply, from, port);
        emit succeeded(TrustedPeer{m_in->peerId, m_in->peerPub, m_in->peerName, QDateTime::currentDateTimeUtc()},
                       from, port);
    }
}

void PairingManager::handleAsInitiator(wire::Type type, const QCborMap &msg, const QHostAddress &from, quint16 port)
{
    if (!m_out || msg.value(kI).toByteArray() != m_out->peerId)
        return;

    if (type == wire::Type::PairChallenge) {
        if (m_out->stage != Outgoing::Stage::Requesting)
            return;
        const QByteArray pub = msg.value(kK).toByteArray();
        const QByteArray sid = msg.value(kS).toByteArray();
        if (pub.size() != crypto::kKeySize || crypto::deviceIdFromPublicKey(pub) != m_out->peerId || sid.size() != 16)
            return;
        m_out->peerPub = pub;
        m_out->peerName = msg.value(kN).toString().left(64);
        m_out->sid = sid;
        m_out->addr = from;
        m_out->port = port;
        m_out->stage = Outgoing::Stage::WaitingPin;
        m_out->startedAt = nowMs();
        emit pinRequired(m_out->peerId, m_out->peerName);
        return;
    }

    if (msg.value(kS).toByteArray() != m_out->sid)
        return;

    if (type == wire::Type::PairMsg2 && m_out->stage == Outgoing::Stage::SentMsg1) {
        auto isk = m_out->cpace->finish(msg.value(kY).toByteArray(), true);
        if (!isk || !crypto::constantTimeEqual(msg.value(kC).toByteArray(), CPace::confirmationTag(*isk, 'B'))) {
            failOutgoing(tr("Wrong PIN"), true);
            return;
        }
        m_out->isk = *isk;
        m_out->cpace.reset();
        QCborMap reply;
        reply.insert(kI, m_self.id());
        reply.insert(kS, m_out->sid);
        reply.insert(kC, CPace::confirmationTag(m_out->isk, 'A'));
        m_out->stage = Outgoing::Stage::SentMsg3;
        sendOut(wire::cborPacket(wire::Type::PairMsg3, reply));
        return;
    }

    if (type == wire::Type::PairDone && m_out->stage == Outgoing::Stage::SentMsg3) {
        auto out = std::move(m_out);
        crypto::wipe(out->isk);
        emit succeeded(TrustedPeer{out->peerId, out->peerPub, out->peerName, QDateTime::currentDateTimeUtc()},
                       from, port);
    }
}

} // namespace df
