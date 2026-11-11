// Windows: the agent's clipboard objects as Explorer sees them. A sealed IDataObject
// (createSealedData, ClipboardBridge::applySealed) is pasted into a temporary folder through that
// folder's shell IDropTarget, which is what Ctrl+V in Explorer (and a drop) does: once handed over
// directly, and once through the real clipboard, read back with OleGetClipboard (the OLE clipboard
// wrapper, as any other application gets it). No window is shown; the test pumps messages while the
// shell copies.
//
// It touches the user's desktop session, so it only runs with DESKFLOWER_DESKTOP_TESTS=1 (ctest
// label "desktop"); otherwise it skips. The clipboard is shared by the whole session: the clipboard
// rows replace its content (and leave it empty), and concurrent runs take turns through a named
// mutex. Where the process may not use the clipboard (a service, a sandbox, a runner without a
// desktop) those rows are skipped.

#include "clipboardbridge.h"
#include "virtualfiles.h"

#include <QClipboard>
#include <QCryptographicHash>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QMimeData>
#include <QSet>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windows.h>

namespace {

using Listed = QList<ClipboardBridge::ListedFile>;

// relative path -> "dir" or "<size>:<sha256>", for the items under `root` (or the given items).
QMap<QString, QString> tree(const QString &root)
{
    QMap<QString, QString> out;
    QDirIterator it(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QString rel = QDir(root).relativeFilePath(it.filePath());
        if (it.fileInfo().isDir()) {
            out.insert(rel, QStringLiteral("dir"));
            continue;
        }
        QFile f(it.filePath());
        f.open(QIODevice::ReadOnly);
        out.insert(rel, QStringLiteral("%1:%2").arg(f.size()).arg(QString::fromLatin1(
                                                                    QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256).toHex())));
    }
    return out;
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

void pump(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        QCoreApplication::processEvents();
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
}

bool pumpUntil(const std::function<bool()> &done, int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        if (done())
            return true;
        pump(50);
    }
    return done();
}

// Drops `data` on the folder as a paste does: the folder's drop target, copy effect.
HRESULT dropInto(IDataObject *data, const QString &folder)
{
    PIDLIST_ABSOLUTE pidl = nullptr;
    HRESULT hr = SHParseDisplayName(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(folder).utf16()), nullptr,
                                    &pidl, 0, nullptr);
    if (FAILED(hr))
        return hr;
    IShellFolder *parent = nullptr;
    PCUITEMID_CHILD child = nullptr;
    hr = SHBindToParent(pidl, IID_IShellFolder, reinterpret_cast<void **>(&parent), &child);
    IDropTarget *target = nullptr;
    if (SUCCEEDED(hr))
        hr = parent->GetUIObjectOf(nullptr, 1, &child, IID_IDropTarget, nullptr, reinterpret_cast<void **>(&target));
    if (SUCCEEDED(hr)) {
        DWORD effect = DROPEFFECT_COPY;
        const POINTL at = {0, 0};
        hr = target->DragEnter(data, MK_LBUTTON, at, &effect);
        if (SUCCEEDED(hr)) {
            effect = DROPEFFECT_COPY;
            hr = target->DragOver(MK_LBUTTON, at, &effect);
        }
        if (SUCCEEDED(hr)) {
            effect = DROPEFFECT_COPY;
            hr = target->Drop(data, 0, at, &effect);
        }
        target->Release();
    }
    if (parent)
        parent->Release();
    CoTaskMemFree(pidl);
    return hr;
}

HGLOBAL getGlobal(IDataObject *data, UINT format, LONG lindex = -1)
{
    FORMATETC f = {CLIPFORMAT(format), nullptr, DVASPECT_CONTENT, lindex, TYMED_HGLOBAL};
    STGMEDIUM m = {};
    if (FAILED(data->GetData(&f, &m)) || m.tymed != TYMED_HGLOBAL)
        return nullptr;
    return m.hGlobal; // owned by the caller (GlobalFree)
}

QString unicodeText(IDataObject *data)
{
    HGLOBAL h = getGlobal(data, CF_UNICODETEXT);
    if (!h)
        return QString();
    const QString text = QString::fromWCharArray(static_cast<const wchar_t *>(GlobalLock(h)));
    GlobalUnlock(h);
    GlobalFree(h);
    return text;
}

QByteArray bytesOf(IDataObject *data, UINT format)
{
    HGLOBAL h = getGlobal(data, format);
    if (!h)
        return QByteArray();
    const QByteArray bytes(static_cast<const char *>(GlobalLock(h)), qsizetype(GlobalSize(h)));
    GlobalUnlock(h);
    GlobalFree(h);
    return bytes;
}

QStringList hdropPaths(IDataObject *data)
{
    HGLOBAL h = getGlobal(data, CF_HDROP);
    if (!h)
        return {};
    QStringList paths;
    auto drop = static_cast<HDROP>(h);
    const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < n; ++i) {
        wchar_t path[32768];
        DragQueryFileW(drop, i, path, 32768);
        paths << QDir::fromNativeSeparators(QString::fromWCharArray(path));
    }
    GlobalFree(h);
    return paths;
}

bool offers(IDataObject *data, UINT format, DWORD tymed = TYMED_HGLOBAL)
{
    FORMATETC f = {CLIPFORMAT(format), nullptr, DVASPECT_CONTENT, -1, tymed};
    return data->QueryGetData(&f) == S_OK;
}

// Clipboard managers and remote-desktop tools open the clipboard briefly after every change
// (CLIPBRD_E_CANT_OPEN meanwhile): try again for a moment, as a user would.
template <typename F>
HRESULT retryingClipboard(F call)
{
    HRESULT hr = E_FAIL;
    for (int attempt = 0; attempt < 60; ++attempt) {
        hr = call();
        if (hr != CLIPBRD_E_CANT_OPEN)
            return hr;
        pump(50);
    }
    return hr;
}

const UINT kDescriptor = RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW);
const UINT kHtml = RegisterClipboardFormatW(L"HTML Format");

} // namespace

class TestClipboardBridge : public QObject
{
    Q_OBJECT

    HANDLE m_turn = nullptr;
    QString m_noClipboard; // why the clipboard rows are skipped (empty: they run)
    QTemporaryDir m_dir;

    // The object a paste gets: `object` itself (row "direct"), or (row "clipboard") the clipboard's
    // own object after `object` was put there, as another application reads it. Takes over one
    // reference and returns one.
    IDataObject *throughClipboard(IDataObject *object)
    {
        QFETCH_GLOBAL(bool, viaClipboard);
        if (!viaClipboard)
            return object;
        const HRESULT set = retryingClipboard([object] { return OleSetClipboard(object); });
        // Another program may take the clipboard over right away: then this read is not ours.
        const bool current = SUCCEEDED(set) && OleIsCurrentClipboard(object) == S_OK;
        object->Release();
        if (!current)
            return nullptr;
        IDataObject *read = nullptr;
        if (FAILED(retryingClipboard([&read] { return OleGetClipboard(&read); })))
            return nullptr;
        return read;
    }

    // A staging folder with the given tree, as FileTransfer leaves a transfer: returns its
    // top-level items.
    QStringList stage(const QString &name, const Listed &items, const QHash<QString, QByteArray> &content)
    {
        const QString root = m_dir.filePath(name);
        QSet<QString> top;
        for (const ClipboardBridge::ListedFile &f : items) {
            const QString path = QDir(root).filePath(f.path);
            if (f.dir)
                QDir().mkpath(path);
            else
                writeFile(path, content.value(f.path));
            top.insert(QDir(root).filePath(f.path.section(QLatin1Char('/'), 0, 0)));
        }
        QStringList out = top.values();
        std::sort(out.begin(), out.end());
        return out;
    }

private slots:
    void initTestCase()
    {
        // It replaces the user's clipboard and drives the shell's drop targets: opt-in only.
        if (qgetenv("DESKFLOWER_DESKTOP_TESTS") != "1")
            QSKIP("uses the real clipboard and the shell: set DESKFLOWER_DESKTOP_TESTS=1 (docs/TESTING.md)");
        QVERIFY(m_dir.isValid());
        m_turn = CreateMutexW(nullptr, FALSE, L"Local\\deskflower-tst-clipboardbridge");
        QVERIFY(m_turn);
        const DWORD waited = WaitForSingleObject(m_turn, 180000);
        QVERIFY2(waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED, "another run kept the clipboard");
        // The agent's thread is an OLE STA (Qt's Windows platform initializes it for drag and
        // drop); this one may not be yet.
        const HRESULT ole = OleInitialize(nullptr);
        QVERIFY2(SUCCEEDED(ole), qPrintable(QString::number(quint32(ole), 16)));
        IDataObject *probe = createSealedData({}, {}, {}, 0, {});
        if (const HRESULT hr = retryingClipboard([probe] { return OleSetClipboard(probe); }); FAILED(hr))
            m_noClipboard = QStringLiteral("this process may not use the clipboard (service, sandbox or runner "
                                           "without a desktop): 0x%1").arg(quint32(hr), 0, 16);
        probe->Release();
        retryingClipboard([] { return OleSetClipboard(nullptr); });
    }

    void initTestCase_data()
    {
        QTest::addColumn<bool>("viaClipboard");
        QTest::newRow("direct") << false;
        QTest::newRow("clipboard") << true;
    }

    void init()
    {
        QFETCH_GLOBAL(bool, viaClipboard);
        if (viaClipboard && !m_noClipboard.isEmpty())
            QSKIP(qPrintable(m_noClipboard));
    }

    void cleanup()
    {
        // Never leave our objects (and their callbacks) on the clipboard.
        if (m_noClipboard.isEmpty())
            retryingClipboard([] { return OleSetClipboard(nullptr); });
        pump(50);
    }

    void cleanupTestCase()
    {
        if (m_turn) {
            ReleaseMutex(m_turn);
            CloseHandle(m_turn);
        }
    }

    void sealedTextAndHtmlRenderOnPaste()
    {
        const QString text = QStringLiteral("hola — ñ 日本");
        const QByteArray html = "<b>hola</b>";
        // The callbacks own their state: the object may outlive this function on the clipboard.
        struct Unsealing {
            int calls = 0;
            bool refuseNext = true;
        };
        const auto state = std::make_shared<Unsealing>();
        auto unseal = [state, text, html](const QString &mime, QString *error) -> QByteArray {
            ++state->calls;
            if (state->refuseNext) {
                state->refuseNext = false;
                *error = QStringLiteral("Not allowed");
                return {};
            }
            return mime == QLatin1String("text/plain") ? text.toUtf8() : html;
        };
        IDataObject *data = throughClipboard(createSealedData(
            {{QStringLiteral("text/plain"), text.toUtf8().size()}, {QStringLiteral("text/html"), html.size()}}, unseal, {}, 0,
            [](QString *error) {
                *error = QStringLiteral("no files");
                return QStringList();
            }));
        if (!data)
            QSKIP("another program took the clipboard over (a clipboard manager or remote-desktop tool)");
        const auto release = qScopeGuard([data] { data->Release(); });
        QCOMPARE(state->calls, 0); // nothing is decrypted until an application asks
        QVERIFY(offers(data, CF_UNICODETEXT) && offers(data, kHtml));
        QVERIFY(!offers(data, CF_HDROP) && !offers(data, kDescriptor));
        // A refused render fails and is not remembered: the next paste asks again.
        QVERIFY(unicodeText(data).isEmpty());
        QCOMPARE(state->calls, 1);
        QCOMPARE(unicodeText(data), text);
        QCOMPARE(state->calls, 2);
        const QByteArray cfHtml = bytesOf(data, kHtml);
        QVERIFY2(cfHtml.contains("StartFragment:") && cfHtml.contains(html), cfHtml.constData());
    }

    void virtualFilesWithFoldersPasteIntoAFolder()
    {
        // Nested and empty folders, a folder holding only empty folders, Unicode names, a file.
        const Listed items = {{QStringLiteral("Carpeta ñ"), 0, true},
                              {QStringLiteral("Carpeta ñ/sub"), 0, true},
                              {QStringLiteral("Carpeta ñ/vacía"), 0, true},
                              {QStringLiteral("Carpeta ñ/sub/hoja €.txt"), 4, false},
                              {QStringLiteral("Carpeta ñ/datos.bin"), 70000, false},
                              {QStringLiteral("Solo carpetas"), 0, true},
                              {QStringLiteral("Solo carpetas/a"), 0, true},
                              {QStringLiteral("Solo carpetas/a/b"), 0, true},
                              {QStringLiteral("suelto ü.txt"), 6, false}};
        QHash<QString, QByteArray> content{{QStringLiteral("Carpeta ñ/sub/hoja €.txt"), "hoja"},
                                           {QStringLiteral("Carpeta ñ/datos.bin"), QByteArray(70000, 'x')},
                                           {QStringLiteral("suelto ü.txt"), "suelto"}};
        const QStringList staged = stage(QStringLiteral("staged"), items, content);
        const auto fetches = std::make_shared<int>(0);
        IDataObject *data = throughClipboard(createSealedData({}, {}, items, int(items.size()), [fetches, staged](QString *) {
            ++*fetches;
            return staged;
        }));
        if (!data)
            QSKIP("another program took the clipboard over (a clipboard manager or remote-desktop tool)");
        const auto release = qScopeGuard([data] { data->Release(); });
        QVERIFY(offers(data, kDescriptor));
        // The descriptor lists every folder (parents first) with the directory attribute.
        HGLOBAL h = getGlobal(data, kDescriptor);
        QVERIFY(h);
        {
            const auto *group = static_cast<const FILEGROUPDESCRIPTORW *>(GlobalLock(h));
            QStringList dirs, files;
            for (UINT i = 0; i < group->cItems; ++i) {
                const FILEDESCRIPTORW &d = group->fgd[i];
                const QString name = QString::fromWCharArray(d.cFileName).replace(QLatin1Char('\\'), QLatin1Char('/'));
                ((d.dwFlags & FD_ATTRIBUTES) && (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? dirs : files) << name;
            }
            GlobalUnlock(h);
            GlobalFree(h);
            QCOMPARE(dirs, (QStringList{QStringLiteral("Carpeta ñ"), QStringLiteral("Carpeta ñ/sub"), QStringLiteral("Carpeta ñ/vacía"),
                                        QStringLiteral("Solo carpetas"), QStringLiteral("Solo carpetas/a"),
                                        QStringLiteral("Solo carpetas/a/b")}));
            QCOMPARE(files.size(), 3);
        }
        QCOMPARE(*fetches, 0); // looking at the list transfers nothing

        const QString dest = m_dir.filePath(QStringLiteral("dest-virtual-") + QLatin1String(QTest::currentDataTag()));
        QDir().mkpath(dest);
        QCOMPARE(dropInto(data, dest), S_OK);
        const QMap<QString, QString> want = tree(m_dir.filePath(QStringLiteral("staged")));
        QVERIFY2(pumpUntil([&] { return tree(dest) == want; }, 30000),
                 qPrintable(QStringList(tree(dest).keys()).join(QLatin1Char('\n'))));
        QCOMPARE(*fetches, 1);
    }

    void deferredFilesWithoutAListArriveAsHdrop()
    {
        // Too many files to list: a deferred CF_HDROP that transfers them at the first request.
        const Listed items = {{QStringLiteral("Grande"), 0, true},
                              {QStringLiteral("Grande/vacía"), 0, true},
                              {QStringLiteral("Grande/x.txt"), 1, false},
                              {QStringLiteral("y.txt"), 1, false}};
        const QStringList staged = stage(QStringLiteral("staged-deferred"), items,
                                         {{QStringLiteral("Grande/x.txt"), "x"}, {QStringLiteral("y.txt"), "y"}});
        const auto fetches = std::make_shared<int>(0);
        IDataObject *data = throughClipboard(createSealedData({}, {}, {}, 2, [fetches, staged](QString *) {
            ++*fetches;
            return staged;
        }));
        if (!data)
            QSKIP("another program took the clipboard over (a clipboard manager or remote-desktop tool)");
        const auto release = qScopeGuard([data] { data->Release(); });
        QVERIFY(offers(data, CF_HDROP));
        QVERIFY(!offers(data, kDescriptor));
        QCOMPARE(*fetches, 0);
        const QString dest = m_dir.filePath(QStringLiteral("dest-deferred-") + QLatin1String(QTest::currentDataTag()));
        QDir().mkpath(dest);
        QCOMPARE(dropInto(data, dest), S_OK);
        const QMap<QString, QString> want = tree(m_dir.filePath(QStringLiteral("staged-deferred")));
        QVERIFY2(pumpUntil([&] { return tree(dest) == want; }, 30000),
                 qPrintable(QStringList(tree(dest).keys()).join(QLatin1Char('\n'))));
        QCOMPARE(*fetches, 1);
    }

    void pathsTooLongForDescriptorsFallBackToHdrop()
    {
        // A descriptor holds at most MAX_PATH - 1 characters: a longer relative path makes the
        // whole set a deferred CF_HDROP of the transferred copies (never truncated names).
        QString deep = QStringLiteral("largo");
        while (deep.size() < MAX_PATH + 20)
            deep += QStringLiteral("/carpeta-con-nombre-largo");
        const Listed items = {{QStringLiteral("largo"), 0, true}, {deep + QStringLiteral("/f.txt"), 1, false}};
        const QStringList staged = {QDir::fromNativeSeparators(m_dir.filePath(QStringLiteral("largo-staged")))};
        QDir().mkpath(staged.first());
        const auto fetches = std::make_shared<int>(0);
        IDataObject *data = throughClipboard(createSealedData({}, {}, items, int(items.size()), [fetches, staged](QString *) {
            ++*fetches;
            return staged;
        }));
        if (!data)
            QSKIP("another program took the clipboard over (a clipboard manager or remote-desktop tool)");
        const auto release = qScopeGuard([data] { data->Release(); });
        QVERIFY(offers(data, CF_HDROP));
        QVERIFY(!offers(data, kDescriptor));
        QCOMPARE(hdropPaths(data), staged);
        QCOMPARE(*fetches, 1);
    }

    void bridgeOffersSealedFilesAndIgnoresItsOwnEcho()
    {
        QFETCH_GLOBAL(bool, viaClipboard);
        if (!viaClipboard)
            QSKIP("goes through the clipboard only");
        ClipboardBridge bridge;
        bridge.setEnabled(true);
        QSignalSpy changed(&bridge, &ClipboardBridge::changed);
        const QStringList staged = stage(QStringLiteral("staged-bridge"),
                                         {{QStringLiteral("uno.txt"), 3, false}, {QStringLiteral("dos"), 0, true}},
                                         {{QStringLiteral("uno.txt"), "uno"}});
        const auto fetches = std::make_shared<int>(0);
        IDataObject *data = nullptr;
        // (Applied again if a clipboard manager held the clipboard at that moment.)
        for (int attempt = 0; attempt < 30 && !data; ++attempt) {
            bridge.applySealed({{QStringLiteral("text/plain"), 4}}, {}, 2,
                               [](const QString &, QString *) { return QByteArray("text"); },
                               [fetches, staged](QString *) {
                                   ++*fetches;
                                   return staged;
                               });
            QCOMPARE(retryingClipboard([&data] { return OleGetClipboard(&data); }), S_OK);
            if (!offers(data, CF_HDROP)) {
                data->Release();
                data = nullptr;
                pump(100);
            }
        }
        if (!data)
            QSKIP("another program took the clipboard over (a clipboard manager or remote-desktop tool)");
        const auto release = qScopeGuard([data] { data->Release(); });
        QCOMPARE(unicodeText(data), QStringLiteral("text"));
        QCOMPARE(*fetches, 0);
        QStringList got = hdropPaths(data);
        std::sort(got.begin(), got.end());
        QCOMPARE(got, staged);
        QCOMPARE(*fetches, 1);
        pump(500);
        QCOMPARE(changed.size(), 0); // what it put there itself is not a local copy
    }

    void bridgeReportsCopiedFolders()
    {
        QFETCH_GLOBAL(bool, viaClipboard);
        if (!viaClipboard)
            QSKIP("goes through the clipboard only");
        // Explorer copies a folder and a file: CF_HDROP with both, reported with the folder.
        ClipboardBridge bridge;
        bridge.setEnabled(true);
        QSignalSpy changed(&bridge, &ClipboardBridge::changed);
        const QString folder = m_dir.filePath(QStringLiteral("copied/carpeta vacía"));
        const QString file = m_dir.filePath(QStringLiteral("copied/archivo.txt"));
        QDir().mkpath(folder);
        writeFile(file, "a");
        auto *mime = new QMimeData;
        mime->setUrls({QUrl::fromLocalFile(folder), QUrl::fromLocalFile(file)});
        QGuiApplication::clipboard()->setMimeData(mime);
        QVERIFY(pumpUntil([&] { return changed.size() > 0; }, 5000));
        QStringList files = changed.last().at(1).toStringList();
        for (QString &f : files)
            f = QDir::fromNativeSeparators(f);
        files.sort();
        QStringList want{QDir::fromNativeSeparators(file), QDir::fromNativeSeparators(folder)};
        want.sort();
        QCOMPARE(files, want);
    }
};

QTEST_MAIN(TestClipboardBridge)
#include "tst_clipboardbridge.moc"
