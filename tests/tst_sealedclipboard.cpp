// Windows: the clipboard object the agent puts on the clipboard (src/clipboard/virtualfiles_win.cpp),
// called directly as a pasting application would through OLE.

#include "virtualfiles.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>

#include <ole2.h>
#include <shldisp.h>
#include <shlobj.h>

#include <atomic>

namespace {
FORMATETC format(UINT cf, DWORD tymed, LONG index = -1)
{
    return FORMATETC{CLIPFORMAT(cf), nullptr, DVASPECT_CONTENT, index, tymed};
}

QByteArray readStream(IStream *stream)
{
    QByteArray out;
    char buffer[4096];
    ULONG read = 0;
    while (SUCCEEDED(stream->Read(buffer, sizeof(buffer), &read)) && read > 0)
        out.append(buffer, qsizetype(read));
    return out;
}

QByteArray readGlobal(HGLOBAL h)
{
    const auto *data = static_cast<const char *>(GlobalLock(h));
    const QByteArray out(data, qsizetype(GlobalSize(h)));
    GlobalUnlock(h);
    return out;
}

void writeFile(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly))
        f.write(content);
}

struct Release {
    void operator()(IUnknown *p) const { p->Release(); }
};
} // namespace

class TestSealedClipboard : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { QVERIFY(SUCCEEDED(OleInitialize(nullptr))); }
    void cleanupTestCase() { OleUninitialize(); }

    // A paste of files transfers them on a worker thread while this thread keeps processing its
    // events (the agent's window, its IPC, the transfer's progress), once for the whole paste.
    void filePasteKeepsTheThreadResponsive()
    {
        QTemporaryDir received;
        writeFile(received.filePath("Folder/a.txt"), "alpha");
        writeFile(received.filePath("loose.txt"), "loose");
        QVERIFY(QDir().mkpath(received.filePath("Folder/empty")));
        std::atomic<int> fetches = 0;
        ClipboardBridge::FetchFiles fetch = [&](QString *) {
            ++fetches;
            QThread::msleep(600); // the transfer
            return QStringList{received.filePath("Folder"), received.filePath("loose.txt")};
        };
        const QList<ClipboardBridge::ListedFile> listed = {
            {QStringLiteral("Folder"), 0, true},
            {QStringLiteral("Folder/empty"), 0, true},
            {QStringLiteral("Folder/a.txt"), 5, false},
            {QStringLiteral("loose.txt"), 5, false},
        };
        std::unique_ptr<IDataObject, Release> object(createSealedData({}, {}, listed, int(listed.size()), fetch));

        // The descriptors: every folder (the empty one too), then the files.
        const UINT descriptorFormat = RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW);
        FORMATETC fd = format(descriptorFormat, TYMED_HGLOBAL);
        STGMEDIUM medium = {};
        QCOMPARE(object->GetData(&fd, &medium), S_OK);
        const auto *group = static_cast<const FILEGROUPDESCRIPTORW *>(GlobalLock(medium.hGlobal));
        QCOMPARE(int(group->cItems), 4);
        QCOMPARE(QString::fromWCharArray(group->fgd[1].cFileName), QStringLiteral("Folder\\empty"));
        QVERIFY(group->fgd[1].dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        QCOMPARE(QString::fromWCharArray(group->fgd[3].cFileName), QStringLiteral("loose.txt"));
        GlobalUnlock(medium.hGlobal);
        ReleaseStgMedium(&medium);

        int ticks = 0;
        QTimer tick;
        tick.setInterval(20);
        connect(&tick, &QTimer::timeout, this, [&] { ++ticks; });
        tick.start();
        const UINT contentsFormat = RegisterClipboardFormatW(CFSTR_FILECONTENTS);
        FORMATETC fc = format(contentsFormat, TYMED_ISTREAM, 2); // Folder/a.txt
        QCOMPARE(object->GetData(&fc, &medium), S_OK);
        QCOMPARE(readStream(medium.pstm), QByteArray("alpha"));
        ReleaseStgMedium(&medium);
        QVERIFY2(ticks >= 10, qPrintable(QString::number(ticks))); // the thread was not blocked
        fc = format(contentsFormat, TYMED_ISTREAM, 3);
        QCOMPARE(object->GetData(&fc, &medium), S_OK);
        QCOMPARE(readStream(medium.pstm), QByteArray("loose"));
        ReleaseStgMedium(&medium);
        QCOMPARE(fetches.load(), 1);
        QVERIFY(!clipboardRenderInProgress());

        // Asynchronous extraction is offered to drop targets that use it.
        IDataObjectAsyncCapability *async = nullptr;
        QCOMPARE(object->QueryInterface(__uuidof(IDataObjectAsyncCapability), reinterpret_cast<void **>(&async)), S_OK);
        BOOL mode = FALSE;
        QCOMPARE(async->GetAsyncMode(&mode), S_OK);
        QVERIFY(mode);
        async->Release();
    }

    // A trusted device's clipboard is decrypted once and served to every reader (no expiry).
    void trustedContentIsRenderedOnceAndServed()
    {
        int unseals = 0;
        ClipboardBridge::UnsealFormat unseal = [&](const QString &mime, QString *) {
            ++unseals;
            return mime == QLatin1String("text/plain") ? QByteArray("line one\nline two") : QByteArray();
        };
        std::unique_ptr<IDataObject, Release> object(
            createSealedData({{QStringLiteral("text/plain"), 17}}, unseal, {}, 0, {}, false));
        for (int i = 0; i < 3; ++i) {
            FORMATETC f = format(CF_UNICODETEXT, TYMED_HGLOBAL);
            STGMEDIUM medium = {};
            QCOMPARE(object->GetData(&f, &medium), S_OK);
            const QByteArray bytes = readGlobal(medium.hGlobal);
            QCOMPARE(QString::fromWCharArray(reinterpret_cast<const wchar_t *>(bytes.constData())),
                     QStringLiteral("line one\r\nline two"));
            ReleaseStgMedium(&medium);
        }
        QCOMPARE(unseals, 1);
        // Nothing asks clipboard history or monitors to leave it alone.
        for (const wchar_t *name : {L"ExcludeClipboardContentFromMonitorProcessing", L"CanIncludeInClipboardHistory",
                                    L"CanUploadToCloudClipboard"}) {
            FORMATETC marker = format(RegisterClipboardFormatW(name), TYMED_HGLOBAL);
            QCOMPARE(object->QueryGetData(&marker), DV_E_FORMATETC);
        }
    }

    // Any object with copied files (listed as virtual files, or a deferred CF_HDROP), gated or
    // not, asks clipboard history, the cloud clipboard and monitors to leave it alone: their data
    // moves only for a paste. Answering the markers transfers nothing.
    void fileObjectsCarryTheMarkers()
    {
        int fetches = 0;
        ClipboardBridge::FetchFiles fetch = [&](QString *) {
            ++fetches;
            return QStringList();
        };
        ClipboardBridge::UnsealFormat unseal = [](const QString &, QString *) { return QByteArray("text"); };
        const QList<ClipboardBridge::ListedFile> listed = {{QStringLiteral("a.txt"), 1, false}};
        std::unique_ptr<IDataObject, Release> virtualFiles(createSealedData({}, {}, listed, 1, fetch));
        std::unique_ptr<IDataObject, Release> deferred(
            createSealedData({{QStringLiteral("text/plain"), 4}}, unseal, {}, 3000, fetch, false));
        for (IDataObject *object : {virtualFiles.get(), deferred.get()}) {
            for (const wchar_t *name : {L"ExcludeClipboardContentFromMonitorProcessing",
                                        L"CanIncludeInClipboardHistory", L"CanUploadToCloudClipboard"}) {
                const UINT cf = RegisterClipboardFormatW(name);
                FORMATETC f = format(cf, TYMED_HGLOBAL);
                QCOMPARE(object->QueryGetData(&f), S_OK);
                STGMEDIUM medium = {};
                QCOMPARE(object->GetData(&f, &medium), S_OK);
                QCOMPARE(readGlobal(medium.hGlobal), QByteArray(sizeof(DWORD), '\0'));
                ReleaseStgMedium(&medium);
                // Listed too, for readers that enumerate.
                IEnumFORMATETC *formats = nullptr;
                QCOMPARE(object->EnumFormatEtc(DATADIR_GET, &formats), S_OK);
                bool listedMarker = false;
                FORMATETC item = {};
                while (formats->Next(1, &item, nullptr) == S_OK)
                    listedMarker = listedMarker || item.cfFormat == cf;
                formats->Release();
                QVERIFY(listedMarker);
            }
        }
        QCOMPARE(fetches, 0);
    }

    // A guest's clipboard keeps out of clipboard history and monitors, and a burst of reads after
    // a refusal (or with no user input just before) does not ask the service again and again.
    void guestContentIsKeptFromReaders()
    {
        int unseals = 0;
        ClipboardBridge::UnsealFormat unseal = [&](const QString &, QString *error) {
            ++unseals;
            *error = QStringLiteral("refused");
            return QByteArray();
        };
        std::unique_ptr<IDataObject, Release> object(
            createSealedData({{QStringLiteral("text/plain"), 5}}, unseal, {}, 0, {}, true));
        for (const wchar_t *name : {L"ExcludeClipboardContentFromMonitorProcessing", L"CanIncludeInClipboardHistory",
                                    L"CanUploadToCloudClipboard"}) {
            FORMATETC f = format(RegisterClipboardFormatW(name), TYMED_HGLOBAL);
            STGMEDIUM medium = {};
            QCOMPARE(object->GetData(&f, &medium), S_OK);
            QCOMPARE(readGlobal(medium.hGlobal), QByteArray(sizeof(DWORD), '\0'));
            ReleaseStgMedium(&medium);
        }
        for (int i = 0; i < 20; ++i) {
            FORMATETC f = format(CF_UNICODETEXT, TYMED_HGLOBAL);
            STGMEDIUM medium = {};
            QVERIFY(FAILED(object->GetData(&f, &medium)));
        }
        QVERIFY2(unseals <= 1, qPrintable(QString::number(unseals)));
    }

    // A drag's object (its own thread) keeps the plain behaviour: no asynchronous extraction.
    void dragObjectIsUnchanged()
    {
        std::unique_ptr<IDataObject, Release> object(
            createVirtualFiles({{QStringLiteral("a.txt"), 1, false}}, [](QString *) { return QStringList(); }));
        IDataObjectAsyncCapability *async = nullptr;
        QCOMPARE(object->QueryInterface(__uuidof(IDataObjectAsyncCapability), reinterpret_cast<void **>(&async)),
                 E_NOINTERFACE);
    }

    // The agent quitting (an update) while a paste waits for its files does not wait for the
    // whole transfer: the fetch is told to give up. Last: the application's quit stays in effect.
    void quittingAbandonsAPasteTransfer()
    {
        std::atomic<bool> gaveUp = false;
        ClipboardBridge::FetchFiles fetch = [&](QString *error) {
            QElapsedTimer waited;
            waited.start();
            while (waited.elapsed() < 20000) { // a long transfer
                if (ClipboardBridge::fetchesAbandoned()) {
                    gaveUp = true;
                    *error = QStringLiteral("closing");
                    return QStringList();
                }
                QThread::msleep(10);
            }
            return QStringList();
        };
        QTimer::singleShot(100, this, [] { QCoreApplication::exit(0); });
        QElapsedTimer elapsed;
        elapsed.start();
        QString error;
        QVERIFY(fetchWhilePumping(fetch, &error).isEmpty());
        QVERIFY2(elapsed.elapsed() < 5000, qPrintable(QString::number(elapsed.elapsed())));
        QVERIFY(gaveUp);
        QCOMPARE(error, QStringLiteral("closing"));
        QVERIFY(!clipboardRenderInProgress());
    }
};

QTEST_GUILESS_MAIN(TestSealedClipboard)
#include "tst_sealedclipboard.moc"
