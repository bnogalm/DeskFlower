#pragma once

#include <QSslCertificate>
#include <QSslKey>
#include <QStringList>

namespace df::web {

struct TlsIdentity {
    QSslCertificate certificate;
    QSslKey key;
};

// Loads <dir>/phone-cert.pem and <dir>/phone-key.pem, or creates a self-signed EC P-256 certificate
// (subjectAltName: the given IP addresses and DNS names) when they are missing or expire within
// 30 days. Phones show a warning for a self-signed certificate; keeping it stable means the user
// accepts it once.
bool loadOrCreateTlsIdentity(const QString &dir, const QStringList &ipAddresses, const QStringList &dnsNames,
                             TlsIdentity *out, QString *error);

} // namespace df::web
