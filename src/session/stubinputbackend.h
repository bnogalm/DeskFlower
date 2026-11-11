#pragma once

#include "inputbackend.h"

#include <QLoggingCategory>

// Used where no input implementation is available (unsupported platform or display server):
// the helper still reports screens and thumbnails and handles the clipboard.
class StubInputBackend : public InputBackend
{
public:
    explicit StubInputBackend(const QString &reason, QObject *parent = nullptr) : InputBackend(parent), m_reason(reason) {}

    bool start() override
    {
        qWarning("Keyboard and mouse sharing is not available: %s", qUtf8Printable(m_reason));
        return false;
    }
    QString problem() const override { return m_reason; } // the agent says what it is about
    bool isCapturing() const override { return false; }
    void setReportEdges(bool) override {}
    void setCapture(bool, QPoint, int) override {}
    void moveTo(QPoint) override {}
    void button(int, bool, QPoint) override {}
    void wheel(int, int) override {}
    void key(quint32, bool) override {}
    void releaseAll() override {}

private:
    const QString m_reason;
};
