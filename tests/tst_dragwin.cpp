// Windows only: the virtual files of a drag (src/clipboard/virtualfiles_win.cpp) dropped on the
// shell's own drop target of a folder, as Explorer and folder views do, without touching the mouse:
// files and folders (nested and empty) arrive, the target extracts in async mode where it
// supports it (the drag's thread keeps serving it until it ends the operation), and what the
// target reports back (performed drop effect, end of the operation) is the only evidence that it
// kept the files: a failed transfer is never reported as a move.
//
// The drops go through the real shell (its drop target, its copy engine, its dialogs) of the
// desktop session running the test: they run only with DESKFLOWER_DESKTOP_TESTS=1 (ctest label
// "desktop"); the tests of the data object alone always run.

#include "virtualfiles.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include <windows.h>
#include <ole2.h>
#include <shlobj.h>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

namespace {

void writeFile(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly))
        f.write(content);
}

QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

struct DropOutcome {
    HRESULT dropResult = E_FAIL;
    DropReport report;
    bool waited = false;
    QStringList dismissed; // dialogs the shell showed (and the test answered "Cancel")
};

// The shell tells a user about a failed copy with a dialog (in this process, on its copy thread),
// and waits: answers "Cancel", as a user would, and notes its title.
class DialogDismisser
{
public:
    DialogDismisser()
        : m_thread([this] {
              while (!m_stop) {
                  EnumWindows(&DialogDismisser::visit, reinterpret_cast<LPARAM>(this));
                  Sleep(200);
              }
          })
    {
    }
    ~DialogDismisser() { finish(); }
    // Stops watching; the titles of the dialogs answered.
    QStringList finish()
    {
        m_stop = true;
        if (m_thread.joinable())
            m_thread.join();
        return m_titles;
    }

private:
    static BOOL CALLBACK visit(HWND w, LPARAM self)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        wchar_t cls[64] = {};
        GetClassNameW(w, cls, 64);
        if (pid == GetCurrentProcessId() && IsWindowVisible(w) && wcscmp(cls, L"#32770") == 0) {
            wchar_t title[256] = {};
            GetWindowTextW(w, title, 256);
            auto *d = reinterpret_cast<DialogDismisser *>(self);
            const QString t = QString::fromWCharArray(title);
            if (!d->m_titles.contains(t))
                d->m_titles.append(t);
            PostMessageW(w, WM_COMMAND, IDCANCEL, 0);
        }
        return TRUE;
    }
    std::atomic_bool m_stop = false;
    QStringList m_titles;
    std::thread m_thread;
};

// On a thread of its own with an OLE apartment, as the agent's drag thread: the shell drop target
// of `folder` gets the object through DragEnter, DragOver and Drop; then the thread serves the
// target until it ends an async extraction, and reads what it reported.
DropOutcome dropOnFolder(const QString &folder, const std::function<IDataObject *()> &make, bool expectDialogs = false)
{
    DropOutcome outcome;
    std::unique_ptr<DialogDismisser> dismisser;
    if (expectDialogs)
        dismisser = std::make_unique<DialogDismisser>();
    std::thread([&] {
        OleInitialize(nullptr);
        IDataObject *data = make();
        PIDLIST_ABSOLUTE pidl = nullptr;
        IShellFolder *parent = nullptr;
        PCUITEMID_CHILD child = nullptr;
        IDropTarget *target = nullptr;
        if (SUCCEEDED(SHParseDisplayName(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(folder).utf16()),
                                         nullptr, &pidl, 0, nullptr))
            && SUCCEEDED(SHBindToParent(pidl, IID_PPV_ARGS(&parent), &child))
            && SUCCEEDED(parent->GetUIObjectOf(nullptr, 1, &child, IID_IDropTarget, nullptr,
                                               reinterpret_cast<void **>(&target)))) {
            POINTL pt = {10, 10};
            DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
            target->DragEnter(data, MK_LBUTTON, pt, &effect);
            effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
            target->DragOver(MK_LBUTTON, pt, &effect);
            effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
            outcome.dropResult = target->Drop(data, 0, pt, &effect);
            outcome.waited = waitForAsyncDrop(data, 60000);
            outcome.report = dropReport(data);
            target->Release();
        }
        if (parent)
            parent->Release();
        CoTaskMemFree(pidl);
        data->Release();
        OleUninitialize();
    }).join();
    if (dismisser)
        outcome.dismissed = dismisser->finish();
    return outcome;
}

// The files a drag announces (folders first, empty ones included) and the transfer that the
// first read of their contents triggers: it writes them under `staging` and returns the top-level
// items, as the service's dragFetch does.
QList<ClipboardBridge::ListedFile> listing()
{
    return {{"Folder", 0, true},
            {"Folder/sub", 0, true},
            {"Folder/empty", 0, true},
            {"Folder/sub/a.txt", 5, false},
            {"big.bin", 3 * 1024 * 1024, false}};
}

ClipboardBridge::FetchFiles fetchInto(const QString &staging, int *calls, bool fail = false)
{
    return [staging, calls, fail](QString *error) -> QStringList {
        ++*calls;
        if (fail) {
            *error = QStringLiteral("Peer not reachable");
            return {};
        }
        writeFile(staging + "/Folder/sub/a.txt", "hello");
        QDir().mkpath(staging + "/Folder/empty");
        writeFile(staging + "/big.bin", QByteArray(3 * 1024 * 1024, 'x'));
        return {staging + "/Folder", staging + "/big.bin"};
    };
}

void checkTree(const QString &dest)
{
    QCOMPARE(readFile(dest + "/Folder/sub/a.txt"), QByteArray("hello"));
    QVERIFY(QFileInfo(dest + "/Folder/empty").isDir());
    QCOMPARE(QFileInfo(dest + "/big.bin").size(), qint64(3 * 1024 * 1024));
}

// The shell's drop target and copy engine belong to the user's desktop session.
bool desktopTestsAllowed()
{
    return qEnvironmentVariableIntValue("DESKFLOWER_DESKTOP_TESTS") == 1;
}

#define REQUIRE_DESKTOP()                                                                              if (!desktopTestsAllowed())                                                                        QSKIP("Drives the real shell: set DESKFLOWER_DESKTOP_TESTS=1 to run it")

} // namespace

class TestDragWin : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        // The shell copies through its own threads and may show its progress window: fine, as
        // long as no question is asked (the destination is always empty).
        QVERIFY(SUCCEEDED(OleInitialize(nullptr))); // for this thread's helpers; drops run apart
    }
    void cleanupTestCase() { OleUninitialize(); }

    // The drag's object (async capable, move preferred) on a folder: the whole tree arrives, the
    // transfer ran once, and the target reported a move, which is what lets the originals go.
    void dragObjectIsMovedIntoAFolder()
    {
        REQUIRE_DESKTOP();
        QTemporaryDir staging, dest;
        int calls = 0;
        const DropOutcome outcome = dropOnFolder(dest.path(), [&] {
            return createVirtualFiles(listing(), fetchInto(staging.path(), &calls), DROPEFFECT_MOVE, true);
        });
        QVERIFY2(SUCCEEDED(outcome.dropResult), qPrintable(QString::number(quint32(outcome.dropResult), 16)));
        QVERIFY(outcome.waited);
        QVERIFY(!outcome.report.asyncPending);
        QVERIFY(!outcome.report.asyncFailed);
        QCOMPARE(calls, 1);
        checkTree(dest.path());
        QVERIFY2(outcome.report.moved(), qPrintable(QString::number(outcome.report.effect)));
    }

    // The clipboard's object (no async capability, copy preferred): the same tree, reported as a
    // copy, never as a move.
    void clipboardObjectIsCopied()
    {
        REQUIRE_DESKTOP();
        QTemporaryDir staging, dest;
        int calls = 0;
        const DropOutcome outcome = dropOnFolder(dest.path(), [&] {
            return createVirtualFiles(listing(), fetchInto(staging.path(), &calls));
        });
        QVERIFY(SUCCEEDED(outcome.dropResult));
        QCOMPARE(calls, 1);
        checkTree(dest.path());
        QVERIFY2(outcome.report.copied(), qPrintable(QString::number(outcome.report.effect)));
        QVERIFY(!outcome.report.moved());
    }

    // The transfer fails: no file arrives and nothing is reported as moved (the originals stay).
    void failedTransferIsNeverAMove()
    {
        REQUIRE_DESKTOP();
        QTemporaryDir staging, dest;
        int calls = 0;
        const DropOutcome outcome = dropOnFolder(dest.path(), [&] {
            return createVirtualFiles(listing(), fetchInto(staging.path(), &calls, true), DROPEFFECT_MOVE, true);
        }, true);
        qInfo() << "The shell showed:" << outcome.dismissed;
        QVERIFY(!outcome.dismissed.isEmpty()); // the user is told, by the target itself
        QVERIFY(outcome.waited);
        QVERIFY(outcome.report.asyncFailed);
        QVERIFY(calls >= 1);
        QVERIFY(!QFile::exists(dest.path() + "/big.bin"));
        QVERIFY(!QFile::exists(dest.path() + "/Folder/sub/a.txt"));
        QVERIFY2(!outcome.report.moved(), qPrintable(QString::number(outcome.report.effect)));
    }

    // The async protocol as a target drives it: only the drag's object offers it; while the
    // target works the drag's thread keeps waiting (and can give up); its end decides.
    void asyncOperationDecidesTheOutcome()
    {
        IDataObject *clip = createVirtualFiles(listing(), [](QString *) { return QStringList(); });
        IDataObjectAsyncCapability *none = nullptr;
        QVERIFY(FAILED(clip->QueryInterface(IID_PPV_ARGS(&none))));
        QVERIFY(waitForAsyncDrop(clip, 10)); // nothing to wait for
        clip->Release();

        for (const bool succeed : {true, false}) {
            IDataObject *data = createVirtualFiles(listing(), [](QString *) { return QStringList(); }, DROPEFFECT_MOVE, true);
            IDataObjectAsyncCapability *async = nullptr;
            QVERIFY(SUCCEEDED(data->QueryInterface(IID_PPV_ARGS(&async))));
            BOOL mode = FALSE;
            QVERIFY(SUCCEEDED(async->GetAsyncMode(&mode)));
            QVERIFY(mode);
            QVERIFY(SUCCEEDED(async->StartOperation(nullptr)));
            QVERIFY(dropReport(data).asyncPending);
            QVERIFY(!waitForAsyncDrop(data, 50));                           // timed out
            QVERIFY(!waitForAsyncDrop(data, 60000, [] { return true; })); // given up
            QVERIFY(!dropReport(data).moved());
            async->EndOperation(succeed ? S_OK : E_FAIL, nullptr, DROPEFFECT_MOVE);
            QVERIFY(waitForAsyncDrop(data, 10));
            const DropReport report = dropReport(data);
            QVERIFY(!report.asyncPending);
            QCOMPARE(report.asyncFailed, !succeed);
            QCOMPARE(report.moved(), succeed);
            async->Release();
            data->Release();
        }
    }

    // A target that reports through SetData (Performed DropEffect, Paste Succeeded) once done; the
    // logical effect, which the shell sets before reading anything, is no evidence.
    void reportedEffectsAreRead()
    {
        auto set = [](IDataObject *data, const wchar_t *format, DWORD value) {
            FORMATETC f = {CLIPFORMAT(RegisterClipboardFormatW(format)), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            STGMEDIUM m = {};
            m.tymed = TYMED_HGLOBAL;
            m.hGlobal = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
            *static_cast<DWORD *>(GlobalLock(m.hGlobal)) = value;
            GlobalUnlock(m.hGlobal);
            data->SetData(&f, &m, TRUE);
        };
        IDataObject *data = createVirtualFiles(listing(), [](QString *) { return QStringList(); }, DROPEFFECT_MOVE, true);
        QCOMPARE(dropReport(data).effect, 0UL);
        set(data, CFSTR_LOGICALPERFORMEDDROPEFFECT, DROPEFFECT_MOVE);
        QVERIFY(!dropReport(data).moved());
        set(data, CFSTR_PERFORMEDDROPEFFECT, DROPEFFECT_NONE);
        QVERIFY(!dropReport(data).moved());
        set(data, CFSTR_PERFORMEDDROPEFFECT, DROPEFFECT_MOVE);
        QVERIFY(dropReport(data).moved());
        data->Release();

        data = createVirtualFiles(listing(), [](QString *) { return QStringList(); });
        set(data, CFSTR_PASTESUCCEEDED, DROPEFFECT_COPY);
        QVERIFY(dropReport(data).copied());
        QVERIFY(!dropReport(data).moved());
        data->Release();
    }
};

QTEST_GUILESS_MAIN(TestDragWin)
#include "tst_dragwin.moc"
