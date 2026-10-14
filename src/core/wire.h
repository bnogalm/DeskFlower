#pragma once

// On-the-wire constants and small encoding helpers.
//
// Every UDP packet starts with [version u8][type u8].
//   Beacon / Pair*  : header + CBOR map (plaintext; pairing is protected by CPace)
//   HandshakeInit   : header + [sender index u32] + Noise IK message 1
//   HandshakeResp   : header + [sender index u32][receiver index u32] + Noise IK message 2
//   Data            : header + [receiver index u32][counter u64] + AEAD(inner packet), AD = 14-byte header
// Integers are little-endian.

#include <QByteArray>
#include <QCborMap>
#include <QHostAddress>
#include <QtEndian>
#include <optional>

namespace df::wire {

constexpr quint8 kVersion = 1;
constexpr quint16 kDiscoveryPort = 24800;
constexpr quint16 kDefaultDataPort = 24801;
constexpr int kMaxPacket = 1400;
inline QHostAddress multicastGroup() { return QHostAddress(QStringLiteral("239.255.77.100")); }

enum class Type : quint8 {
    Beacon = 0x01,
    PairRequest = 0x10,
    PairChallenge = 0x11,
    PairMsg1 = 0x12,
    PairMsg2 = 0x13,
    PairMsg3 = 0x14,
    PairDone = 0x15,
    PairAbort = 0x16,
    HandshakeInit = 0x20,
    HandshakeResp = 0x21,
    Data = 0x30,
};

// First byte of a decrypted Data payload.
enum class Inner : quint8 {
    Keepalive = 0,
    Reliable = 1,
    Ack = 2,
    Datagram = 3,
};

constexpr int kHeaderSize = 2;
constexpr int kDataHeaderSize = kHeaderSize + 4 + 8;

inline QByteArray header(Type t)
{
    QByteArray h(2, Qt::Uninitialized);
    h[0] = char(kVersion);
    h[1] = char(t);
    return h;
}

inline std::optional<Type> parseType(const QByteArray &packet)
{
    if (packet.size() < kHeaderSize || quint8(packet[0]) != kVersion)
        return std::nullopt;
    return Type(quint8(packet[1]));
}

inline void appendU32(QByteArray &b, quint32 v)
{
    char tmp[4];
    qToLittleEndian<quint32>(v, tmp);
    b.append(tmp, 4);
}

inline void appendU64(QByteArray &b, quint64 v)
{
    char tmp[8];
    qToLittleEndian<quint64>(v, tmp);
    b.append(tmp, 8);
}

inline quint32 readU32(const QByteArray &b, int offset) { return qFromLittleEndian<quint32>(b.constData() + offset); }
inline quint64 readU64(const QByteArray &b, int offset) { return qFromLittleEndian<quint64>(b.constData() + offset); }

inline QByteArray cborPacket(Type t, const QCborMap &map) { return header(t) + map.toCborValue().toCbor(); }

inline std::optional<QCborMap> parseCborPacket(const QByteArray &packet)
{
    const QCborValue v = QCborValue::fromCbor(packet.mid(kHeaderSize));
    if (!v.isMap())
        return std::nullopt;
    return v.toMap();
}

} // namespace df::wire
