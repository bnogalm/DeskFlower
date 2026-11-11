#pragma once

// HTTP client helpers shared by the phone-server tests (tst_web.cpp, tst_phoneuser.cpp).

#include <QBuffer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <utility>

namespace df::test {

struct Reply {
    int status = 0;
    QByteArray body;
    QNetworkReply *raw = nullptr;
    QJsonObject json() const { return QJsonDocument::fromJson(body).object(); }
};

// One request, waited for. Self-signed certificate: the client accepts it, like a phone after
// the warning. A null body sends none; headers are added raw.
inline Reply request(QNetworkAccessManager &nam, const QByteArray &verb, const QUrl &url, const QByteArray &body = {},
                     const QList<std::pair<QByteArray, QByteArray>> &headers = {})
{
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    for (const auto &[name, value] : headers)
        req.setRawHeader(name, value);
    QBuffer *buffer = nullptr;
    if (!body.isNull()) {
        buffer = new QBuffer;
        buffer->setData(body);
        buffer->open(QIODevice::ReadOnly);
    }
    QNetworkReply *reply = nam.sendCustomRequest(req, verb, buffer);
    if (buffer)
        buffer->setParent(reply);
    QObject::connect(reply, &QNetworkReply::sslErrors, reply, [reply] { reply->ignoreSslErrors(); });
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    Reply r{reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), reply->readAll(), reply};
    reply->deleteLater();
    return r;
}

// Deterministic non-repeating bytes, for uploads and range checks.
inline QByteArray pattern(int size)
{
    QByteArray b(size, Qt::Uninitialized);
    for (int i = 0; i < size; ++i)
        b[i] = char((i * 7 + i / 977) & 0xFF);
    return b;
}

} // namespace df::test
