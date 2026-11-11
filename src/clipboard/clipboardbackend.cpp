#include "clipboardbackend.h"

#include <QClipboard>
#include <QGuiApplication>

namespace {

class QtClipboardBackend final : public ClipboardBackend
{
public:
    explicit QtClipboardBackend(QObject *parent) : ClipboardBackend(parent)
    {
        connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, &ClipboardBackend::changed);
    }

    const QMimeData *mimeData() override { return QGuiApplication::clipboard()->mimeData(); }
    void setMimeData(QMimeData *data) override { QGuiApplication::clipboard()->setMimeData(data); }
    bool ownsClipboard() const override { return QGuiApplication::clipboard()->ownsClipboard(); }
};

} // namespace

ClipboardBackend *ClipboardBackend::createDefault(QObject *parent)
{
    return new QtClipboardBackend(parent);
}
