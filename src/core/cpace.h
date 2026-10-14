#pragma once

// CPace-style balanced PAKE over ristretto255 (draft-irtf-cfrg-cpace), used once per pairing.
//
// Both sides derive a secret generator from the PIN, the session id and a channel identifier that
// binds both devices' long-term public keys. A man-in-the-middle who swaps keys ends up with a
// different generator and cannot complete key confirmation. An attacker gets exactly one online
// guess per PIN (the responder discards the PIN after one attempt).

#include <QByteArray>
#include <QString>
#include <optional>

namespace df {

class CPace
{
public:
    CPace(const QString &pin, const QByteArray &sessionId, const QByteArray &channelId);
    ~CPace();

    bool isValid() const { return !m_Y.isEmpty(); }
    // Public share to send to the peer (32 bytes).
    QByteArray share() const { return m_Y; }

    // Derives the intermediate session key (64 bytes). The transcript is ordered initiator-first.
    std::optional<QByteArray> finish(const QByteArray &peerShare, bool isInitiator);

    // Key confirmation tag for a role ('A' = initiator, 'B' = responder).
    static QByteArray confirmationTag(const QByteArray &isk, char role);

private:
    QByteArray m_sid;
    QByteArray m_y; // secret scalar
    QByteArray m_Y; // public share
};

} // namespace df
