// Windows: a paste served while its files arrive (src/clipboard/virtualfiles_win.cpp, streamhost_win.cpp,
// pastesession_win.cpp), called directly through IDataObject and IStream as Explorer would. No
// clipboard, shell, input or network: the list and the paste session are fakes, and the "transfer"
// is a thread writing into a temporary folder. The cross-process case re-launches this program
// (local COM only).

#include "pasterequests_win.h"
#include "pastesession_win.h"
#include "streamhost_win.h"
#include "virtualfiles.h"
#include "fsutil.h"

#include <QCborArray>
#include <QCborMap>
#include <QCborValue>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QtEndian>

#include <ole2.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

using Session = ClipboardBridge::PasteSessionBase;
using State = Session::State;

namespace {
FORMATETC format(UINT cf, DWORD tymed, LONG index = -1)
{
    return FORMATETC{CLIPFORMAT(cf), nullptr, DVASPECT_CONTENT, index, tymed};
}

const UINT kDescriptor = RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW);
const UINT kContents = RegisterClipboardFormatW(CFSTR_FILECONTENTS);

struct Release {
    void operator()(IUnknown *p) const { p->Release(); }
};
template <typename T> using ComPtr = std::unique_ptr<T, Release>;

// Recognisable bytes: a file's content at any offset can be checked.
QByteArray pattern(qsizetype size)
{
    QByteArray out(size, Qt::Uninitialized);
    for (qsizetype i = 0; i < size; ++i)
        out[i] = char((i * 31 + i / 251) & 0xFF);
    return out;
}

std::wstring native(const QString &path) { return QDir::toNativeSeparators(path).toStdWString(); }

// The transfer: writes `data` into `path` a chunk at a time (unbuffered, never preallocated, the
// way the service's receiver writes), sharing it with readers.
void writeSlowly(const QString &path, const QByteArray &data, qsizetype chunk, int delayMs, int startDelayMs = 0)
{
    Sleep(DWORD(startDelayMs));
    QDir().mkpath(QFileInfo(path).absolutePath());
    HANDLE h = CreateFileW(native(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    for (qsizetype at = 0; at < data.size(); at += chunk) {
        DWORD written = 0;
        WriteFile(h, data.constData() + at, DWORD(qMin(chunk, data.size() - at)), &written, nullptr);
        Sleep(DWORD(delayMs));
    }
    CloseHandle(h);
}

void writeNow(const QString &path, const QByteArray &data) { writeSlowly(path, data, qMax<qsizetype>(1, data.size()), 0); }

// Joins on scope exit (the test's writer threads).
struct Joiner {
    std::vector<std::thread> threads;
    ~Joiner()
    {
        for (std::thread &t : threads)
            if (t.joinable())
                t.join();
    }
};

// A paste session without a service: its state is set by the test, and it records what the
// streams tell it (reads, and the apartment each Read runs in).
class FakeSession final : public Session
{
public:
    explicit FakeSession(QString root) : m_root(std::move(root)) {}
    QString root() const override { return m_root; }
    State state() const override { return m_state; }
    void readStarted() override
    {
        ++reading;
        ++reads;
        APTTYPE type = APTTYPE_CURRENT;
        APTTYPEQUALIFIER qualifier = APTTYPEQUALIFIER_NONE;
        if (SUCCEEDED(CoGetApartmentType(&type, &qualifier)))
            readApartment = int(type);
    }
    void readEnded() override { --reading; }
    void touch() override { ++touches; }
    void consumed() override { ++consumedCalls; }
    void set(State state) { m_state = state; }

    std::atomic<int> consumedCalls{0};
    std::atomic<int> reading{0};
    std::atomic<int> reads{0};
    std::atomic<int> touches{0};
    std::atomic<int> readApartment{-1};

private:
    const QString m_root;
    std::atomic<State> m_state{State::Running};
};

ComPtr<IStream> makeStream(const std::shared_ptr<Session> &session, const QString &path, qint64 size)
{
    IStream *stream = nullptr;
    if (FAILED(df::streamhost::createProgressiveStream(session, path, size, &stream)))
        return nullptr;
    return ComPtr<IStream>(stream);
}

QStringList formatsOf(IDataObject *object)
{
    QStringList out;
    IEnumFORMATETC *formats = nullptr;
    if (FAILED(object->EnumFormatEtc(DATADIR_GET, &formats)))
        return out;
    FORMATETC item = {};
    while (formats->Next(1, &item, nullptr) == S_OK) {
        if (item.cfFormat == kDescriptor)
            out << QStringLiteral("descriptor");
        else if (item.cfFormat == kContents)
            out << QStringLiteral("contents");
        else if (item.cfFormat == CF_HDROP)
            out << QStringLiteral("hdrop");
    }
    formats->Release();
    return out;
}

// A run for a real PasteSession: the folder at once, then nothing until it is told to stop.
PasteSession::Run idleRun(const QString &root, std::shared_ptr<std::atomic<bool>> closed)
{
    return [root, closed](PasteSession::Link &link) {
        link.started(root);
        while (!link.stopRequested())
            Sleep(10);
        *closed = true; // the connection would close here
    };
}

// A fake DeskFlower service on a local server of this process (a named pipe: no network): its
// first connection is handed to `script` on a thread of its own, with blocking calls.
class FakeService
{
public:
    explicit FakeService(std::function<void(QLocalSocket &)> script)
    {
        static std::atomic<int> counter = 0;
        m_name = QStringLiteral("deskflower-tst-virtualstream-%1-%2")
                     .arg(QCoreApplication::applicationPid())
                     .arg(++counter);
        m_thread = std::thread([this, script = std::move(script)] {
            QLocalServer server;
            const bool listening = server.listen(m_name);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_ready = true;
            }
            m_changed.notify_all();
            if (!listening || !server.waitForNewConnection(10000))
                return;
            QLocalSocket *socket = server.nextPendingConnection();
            if (!socket)
                return;
            script(*socket);
            socket->abort();
            delete socket;
            done = true;
        });
        std::unique_lock<std::mutex> lock(m_mutex);
        m_changed.wait(lock, [this] { return m_ready; });
    }
    ~FakeService() { m_thread.join(); }
    QString name() const { return m_name; }

    // The request a connection sent first.
    static QCborMap readRequest(QLocalSocket &socket)
    {
        QByteArray buffer;
        QCborMap msg;
        while (!df::pasterequests::takeMessage(buffer, &msg)) {
            if (!socket.waitForReadyRead(5000))
                return {};
            buffer += socket.readAll();
        }
        return msg;
    }
    static QByteArray frame(const QCborMap &msg)
    {
        const QByteArray body = msg.toCborValue().toCbor();
        QByteArray out(4, Qt::Uninitialized);
        qToBigEndian<quint32>(quint32(body.size()), out.data());
        return out + body;
    }
    static void send(QLocalSocket &socket, const QByteArray &bytes)
    {
        socket.write(bytes);
        socket.waitForBytesWritten(3000);
    }
    static void send(QLocalSocket &socket, const QCborMap &msg) { send(socket, frame(msg)); }
    static QCborMap event(const char *ev, std::initializer_list<std::pair<const char *, QCborValue>> fields = {})
    {
        QCborMap msg;
        msg.insert(QStringLiteral("ev"), QLatin1String(ev));
        for (const auto &field : fields)
            msg.insert(QLatin1String(field.first), field.second);
        return msg;
    }

    std::atomic<bool> done{false};

private:
    QString m_name;
    std::mutex m_mutex;
    std::condition_variable m_changed;
    bool m_ready = false;
    std::thread m_thread;
};

// A streaming paste's session whose request goes to `service` (as Backend's goes to the real one).
std::shared_ptr<PasteSession> openOn(const FakeService &service, QString *error, int idleTimeoutMs = 30000)
{
    const QString name = service.name();
    return PasteSession::open(
        [name, idleTimeoutMs](PasteSession::Link &link) {
            QLocalSocket socket;
            socket.connectToServer(name);
            if (!socket.waitForConnected(3000)) {
                link.finished(State::Failed, QStringLiteral("no connection"));
                return;
            }
            df::pasterequests::runStreamingFetch(socket, QByteArray::fromHex("0102"), 1234, link, idleTimeoutMs);
        },
        error);
}

// A move paste's requests without a service (ClipboardBridge::FileStreaming::move): each open is
// a new transfer into a folder of its own under `base` ("1", "2"...), which writes `tree` there and
// then ends as `end`, or is refused with `refuse`.
struct FakeMoves {
    explicit FakeMoves(QString b) : base(std::move(b)) {}
    QString base;
    QList<QPair<QString, QByteArray>> tree = {{QStringLiteral("Tree/a.txt"), "alpha"},
                                              {QStringLiteral("Tree/sub/b.txt"), "beta"},
                                              {QStringLiteral("top.txt"), "top"}};
    std::atomic<int> opens{0};
    std::atomic<State> end{State::Succeeded};
    std::atomic<int> transferMs{50};
    QString refuse;
    std::mutex mutex;
    std::vector<std::weak_ptr<FakeSession>> sessions; // observed only: the object holds them
    Joiner writers;

    std::function<std::shared_ptr<Session>(QString *)> request()
    {
        return [this](QString *error) -> std::shared_ptr<Session> {
            const int n = ++opens;
            if (!refuse.isEmpty()) {
                *error = refuse;
                return {};
            }
            const QString root = QDir(base).filePath(QString::number(n));
            auto session = std::make_shared<FakeSession>(root);
            {
                std::lock_guard<std::mutex> lock(mutex);
                sessions.push_back(session);
                writers.threads.emplace_back([this, root, session] {
                    for (const auto &item : std::as_const(tree))
                        writeNow(QDir(root).filePath(item.first), item.second);
                    Sleep(DWORD(transferMs.load()));
                    session->set(end.load());
                });
            }
            return session;
        };
    }
    // The session of the n-th open (its transfer's writer lets go of it once done).
    std::weak_ptr<FakeSession> session(int n)
    {
        std::lock_guard<std::mutex> lock(mutex);
        return n >= 1 && n <= int(sessions.size()) ? sessions[n - 1] : std::weak_ptr<FakeSession>();
    }
};

// CF_HDROP of `object`: the paths it names (empty + hr on failure).
QStringList dropPaths(IDataObject *object, HRESULT *hr = nullptr)
{
    FORMATETC f = format(CF_HDROP, TYMED_HGLOBAL);
    STGMEDIUM medium = {};
    const HRESULT got = object->GetData(&f, &medium);
    if (hr)
        *hr = got;
    if (FAILED(got))
        return {};
    QStringList paths;
    auto drop = static_cast<HDROP>(medium.hGlobal);
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
        wchar_t path[MAX_PATH * 4] = {};
        DragQueryFileW(drop, i, path, UINT(std::size(path)));
        paths << QDir::fromNativeSeparators(QString::fromWCharArray(path));
    }
    ReleaseStgMedium(&medium);
    paths.sort();
    return paths;
}

// What the shell sets on a data object once its paste is done (SetData: a DWORD).
void reportEffect(IDataObject *object, const wchar_t *formatName, DWORD effect)
{
    FORMATETC f = format(RegisterClipboardFormatW(formatName), TYMED_HGLOBAL);
    STGMEDIUM medium = {};
    medium.tymed = TYMED_HGLOBAL;
    medium.hGlobal = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    *static_cast<DWORD *>(GlobalLock(medium.hGlobal)) = effect;
    GlobalUnlock(medium.hGlobal);
    object->SetData(&f, &medium, TRUE);
}

DWORD preferredEffect(IDataObject *object)
{
    FORMATETC f = format(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT), TYMED_HGLOBAL);
    STGMEDIUM medium = {};
    if (FAILED(object->GetData(&f, &medium)))
        return DWORD(-1);
    const DWORD effect = *static_cast<const DWORD *>(GlobalLock(medium.hGlobal));
    GlobalUnlock(medium.hGlobal);
    ReleaseStgMedium(&medium);
    return effect;
}

// Explorer moving a hand-out's items out of their folder into `to`.
bool moveItems(const QStringList &items, const QString &to)
{
    for (const QString &item : items) {
        if (!QDir().rename(item, QDir(to).filePath(QFileInfo(item).fileName())))
            return false;
    }
    return true;
}

QStringList expectedItems(const QString &root)
{
    QStringList items{root + QStringLiteral("/Tree"), root + QStringLiteral("/top.txt")};
    items.sort();
    return items;
}

// The child of crossProcessPaste: unmarshals the data object, reads the descriptor and one file
// to its end, and prints its hash. Exit code 0 when everything was as expected.
int runChild(const char *marshalledHex, const char *expectedSize)
{
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
        return 2;
    const QByteArray bytes = QByteArray::fromHex(marshalledHex);
    IStream *marshalled = SHCreateMemStream(reinterpret_cast<const BYTE *>(bytes.constData()), UINT(bytes.size()));
    IDataObject *data = nullptr;
    if (!marshalled || FAILED(CoUnmarshalInterface(marshalled, IID_IDataObject, reinterpret_cast<void **>(&data))))
        return 3;
    marshalled->Release();
    const qint64 size = QByteArray(expectedSize).toLongLong();
    FORMATETC fd = format(kDescriptor, TYMED_HGLOBAL);
    STGMEDIUM medium = {};
    if (FAILED(data->GetData(&fd, &medium)))
        return 4;
    const auto *group = static_cast<const FILEGROUPDESCRIPTORW *>(GlobalLock(medium.hGlobal));
    const bool descriptorOk = group->cItems == 2
                              && QString::fromWCharArray(group->fgd[1].cFileName) == QLatin1String("Folder\\big.bin")
                              && qint64(group->fgd[1].nFileSizeLow) == size;
    GlobalUnlock(medium.hGlobal);
    ReleaseStgMedium(&medium);
    if (!descriptorOk)
        return 5;
    FORMATETC fc = format(kContents, TYMED_ISTREAM, 1);
    if (FAILED(data->GetData(&fc, &medium)) || medium.tymed != TYMED_ISTREAM)
        return 6;
    data->Release(); // while the parent still serves its apartment
    std::printf("STREAM\n");
    std::fflush(stdout);
    // The parent's apartment is blocked from now on: the reads go to its multithreaded apartment.
    QByteArray content;
    std::vector<char> buffer(64 * 1024);
    for (;;) {
        ULONG read = 0;
        const HRESULT hr = medium.pstm->Read(buffer.data(), ULONG(buffer.size()), &read);
        if (FAILED(hr))
            return 7;
        content.append(buffer.data(), qsizetype(read));
        if (read < buffer.size())
            break; // the end (never short before it)
    }
    ReleaseStgMedium(&medium);
    if (content.size() != size)
        return 8;
    std::printf("DONE %s\n", QCryptographicHash::hash(content, QCryptographicHash::Sha1).toHex().constData());
    std::fflush(stdout);
    CoUninitialize();
    return 0;
}
} // namespace

class TestVirtualStream : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QVERIFY(SUCCEEDED(OleInitialize(nullptr)));
        setMoveLingerForTests(0); // a test that looks at it turns it on
    }
    void cleanupTestCase() { OleUninitialize(); }

    // An object announced without its list offers the descriptor and the contents from the start
    // (never a deferred CF_HDROP), and looking at it asks for nothing.
    void onDemandObjectOffersTheListBeforeAnyList()
    {
        std::atomic<int> lists = 0, opens = 0, fetches = 0;
        ClipboardBridge::FileStreaming streaming;
        streaming.list = [&](QString *) {
            ++lists;
            return QList<ClipboardBridge::ListedFile>{{QStringLiteral("a.txt"), 1, false}};
        };
        streaming.open = [&](QString *, qint64) -> std::shared_ptr<Session> {
            ++opens;
            return {};
        };
        ClipboardBridge::FetchFiles fetch = [&](QString *) {
            ++fetches;
            return QStringList();
        };
        ComPtr<IDataObject> object(createSealedData({}, {}, {}, 19300, fetch, false, DROPEFFECT_COPY, streaming));
        const QStringList offered = formatsOf(object.get());
        QVERIFY(offered.contains(QStringLiteral("descriptor")));
        QVERIFY(offered.contains(QStringLiteral("contents")));
        QVERIFY(!offered.contains(QStringLiteral("hdrop")));
        FORMATETC fd = format(kDescriptor, TYMED_HGLOBAL);
        QCOMPARE(object->QueryGetData(&fd), S_OK);
        FORMATETC fc = format(kContents, TYMED_ISTREAM);
        QCOMPARE(object->QueryGetData(&fc), S_OK);
        FORMATETC drop = format(CF_HDROP, TYMED_HGLOBAL);
        QCOMPARE(object->QueryGetData(&drop), DV_E_FORMATETC);
        QCOMPARE(lists.load(), 0);
        QCOMPARE(opens.load(), 0);
        QCOMPARE(fetches.load(), 0);
        // The clipboard monitors' markers are there too.
        FORMATETC marker = format(RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing"), TYMED_HGLOBAL);
        QCOMPARE(object->QueryGetData(&marker), S_OK);
    }

    // The descriptor of 19,300 entries, asked for at the first read only (folders first, then the
    // files with their sizes); no contents are asked for.
    void descriptorIsListedOnDemand()
    {
        constexpr int kFolders = 300;
        constexpr int kFiles = 19000;
        QList<ClipboardBridge::ListedFile> list;
        for (int k = 0; k < kFiles; ++k)
            list.append({QStringLiteral("d%1/f%2.bin").arg(k % kFolders, 3, 10, QLatin1Char('0')).arg(k, 5, 10, QLatin1Char('0')),
                         k, false});
        for (int i = 0; i < kFolders; ++i)
            list.append({QStringLiteral("d%1").arg(i, 3, 10, QLatin1Char('0')), 0, true});
        std::atomic<int> lists = 0, opens = 0, fetches = 0;
        ClipboardBridge::FileStreaming streaming;
        streaming.list = [&, list](QString *) {
            ++lists;
            QThread::msleep(100); // the list-only request
            return list;
        };
        streaming.open = [&](QString *, qint64) -> std::shared_ptr<Session> {
            ++opens;
            return {};
        };
        ClipboardBridge::FetchFiles fetch = [&](QString *) {
            ++fetches;
            return QStringList();
        };
        ComPtr<IDataObject> object(
            createSealedData({}, {}, {}, int(list.size()), fetch, false, DROPEFFECT_COPY, streaming));
        for (int round = 0; round < 2; ++round) {
            FORMATETC fd = format(kDescriptor, TYMED_HGLOBAL);
            STGMEDIUM medium = {};
            QCOMPARE(object->GetData(&fd, &medium), S_OK);
            const auto *group = static_cast<const FILEGROUPDESCRIPTORW *>(GlobalLock(medium.hGlobal));
            QCOMPARE(int(group->cItems), kFolders + kFiles);
            for (int i = 0; i < kFolders; ++i) {
                QVERIFY(group->fgd[i].dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
                QCOMPARE(group->fgd[i].dwFlags, DWORD(FD_ATTRIBUTES | FD_PROGRESSUI));
            }
            for (int k : {0, 1, 777, kFiles - 1}) {
                const FILEDESCRIPTORW &d = group->fgd[kFolders + k];
                QCOMPARE(QString::fromWCharArray(d.cFileName),
                         QStringLiteral("d%1\\f%2.bin").arg(k % kFolders, 3, 10, QLatin1Char('0')).arg(k, 5, 10, QLatin1Char('0')));
                QCOMPARE(d.dwFlags, DWORD(FD_ATTRIBUTES | FD_PROGRESSUI | FD_FILESIZE));
                QVERIFY(!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY));
                QCOMPARE(qint64(d.nFileSizeLow), qint64(k));
            }
            GlobalUnlock(medium.hGlobal);
            ReleaseStgMedium(&medium);
        }
        QCOMPARE(lists.load(), 1); // listed once
        QCOMPARE(opens.load(), 0); // no data source touched
        QCOMPARE(fetches.load(), 0);
        QVERIFY(!clipboardRenderInProgress());
    }

    // A list that fails gives the reader E_FAIL, and the next read asks again.
    void failingListFails()
    {
        std::atomic<int> lists = 0, opens = 0;
        ClipboardBridge::FileStreaming streaming;
        streaming.list = [&](QString *error) {
            ++lists;
            *error = QStringLiteral("The clipboard changed");
            return QList<ClipboardBridge::ListedFile>();
        };
        streaming.open = [&](QString *, qint64) -> std::shared_ptr<Session> {
            ++opens;
            return {};
        };
        ComPtr<IDataObject> object(createSealedData({}, {}, {}, 3, {}, false, DROPEFFECT_COPY, streaming));
        for (int i = 0; i < 2; ++i) {
            FORMATETC fd = format(kDescriptor, TYMED_HGLOBAL);
            STGMEDIUM medium = {};
            QCOMPARE(object->GetData(&fd, &medium), E_FAIL);
        }
        QCOMPARE(lists.load(), 2);
        QCOMPARE(opens.load(), 0);
    }

    // A refused paste (no paste gesture) fails the contents request quickly.
    void refusedOpenFailsQuickly()
    {
        std::atomic<int> opens = 0, fetches = 0;
        std::atomic<qint64> listedAgo = -2;
        ClipboardBridge::FileStreaming streaming;
        streaming.open = [&](QString *error, qint64 ago) -> std::shared_ptr<Session> {
            ++opens;
            listedAgo = ago;
            *error = QString::fromLatin1(ClipboardBridge::kNotAPaste);
            return {};
        };
        ClipboardBridge::FetchFiles fetch = [&](QString *) {
            ++fetches;
            return QStringList();
        };
        const QList<ClipboardBridge::ListedFile> listed = {{QStringLiteral("a.txt"), 5, false}};
        ComPtr<IDataObject> object(createSealedData({}, {}, listed, 1, fetch, false, DROPEFFECT_COPY, streaming));
        QElapsedTimer elapsed;
        elapsed.start();
        FORMATETC fc = format(kContents, TYMED_ISTREAM, 0);
        STGMEDIUM medium = {};
        QCOMPARE(object->GetData(&fc, &medium), E_FAIL);
        QVERIFY2(elapsed.elapsed() < 2000, qPrintable(QString::number(elapsed.elapsed())));
        QCOMPARE(opens.load(), 1);
        QCOMPARE(fetches.load(), 0); // never the whole transfer
        QCOMPARE(listedAgo.load(), -1); // no descriptor read: nothing to count from

        // After a descriptor read, the request says how long ago it was (the service counts a
        // press made around it: a file manager creates the folders before it reads a file).
        FORMATETC fd = format(kDescriptor, TYMED_HGLOBAL);
        QCOMPARE(object->GetData(&fd, &medium), S_OK);
        ReleaseStgMedium(&medium);
        Sleep(150);
        QCOMPARE(object->GetData(&fc, &medium), E_FAIL);
        QCOMPARE(opens.load(), 2);
        QVERIFY2(listedAgo.load() >= 100 && listedAgo.load() < 5000, qPrintable(QString::number(listedAgo.load())));
    }

    // Read waits until the bytes asked for are written and returns exactly them: never short
    // before the end, and nothing at the end.
    void readWaitsForTheBytesAsked()
    {
        QTemporaryDir root;
        const QByteArray data = pattern(1024 * 1024 + 123);
        auto session = std::make_shared<FakeSession>(root.path());
        const QString path = root.filePath(QStringLiteral("Folder/a.bin"));
        Joiner writers;
        writers.threads.emplace_back([path, data] { writeSlowly(path, data, 64 * 1024, 15, 100); });
        ComPtr<IStream> stream = makeStream(session, path, data.size());
        QVERIFY(stream);
        QByteArray got;
        std::vector<char> buffer(300000);
        for (;;) {
            ULONG read = 0;
            QCOMPARE(stream->Read(buffer.data(), ULONG(buffer.size()), &read), S_OK);
            const qsizetype expected = qMin<qsizetype>(qsizetype(buffer.size()), data.size() - got.size());
            QCOMPARE(qsizetype(read), expected);
            got.append(buffer.data(), qsizetype(read));
            if (read == 0)
                break;
        }
        QCOMPARE(got.size(), data.size());
        QVERIFY(got == data);
        QVERIFY(session->reads.load() > 0);
        QCOMPARE(session->reading.load(), 0);
    }

    // Files written as the receiver's parallel writers write them (fs::WriteFile, several threads,
    // finishing out of order: a later file before an earlier one) are read whole, in listing
    // order, each stream opening its file while a writer still has it open.
    void readWhileParallelWritersFinishOutOfOrder()
    {
        QTemporaryDir root;
        auto session = std::make_shared<FakeSession>(root.path());
        QVERIFY(QDir().mkpath(root.filePath(QStringLiteral("tree"))));
        struct Item {
            QString path;
            QByteArray data;
            int delayMs;
        };
        std::vector<Item> items;
        for (int i = 0; i < 6; ++i) {
            // The first files are the slowest: they end last.
            items.push_back({root.filePath(QStringLiteral("tree/f%1.bin").arg(i)), pattern(200 * 1024 + i * 7777), 25 - 4 * i});
        }
        Joiner writers;
        std::atomic<int> opened{0};
        for (int w = 0; w < 4; ++w) {
            writers.threads.emplace_back([&items, &opened, w] {
                for (size_t i = w; i < items.size(); i += 4) {
                    df::fs::WriteFile file;
                    if (!file.open(items[i].path))
                        return;
                    ++opened;
                    for (qsizetype at = 0; at < items[i].data.size(); at += 32 * 1024) {
                        file.write(items[i].data.constData() + at, qMin<qsizetype>(32 * 1024, items[i].data.size() - at));
                        Sleep(DWORD(items[i].delayMs));
                    }
                    file.close();
                }
            });
        }
        for (const Item &item : items) {
            ComPtr<IStream> stream = makeStream(session, item.path, item.data.size());
            QVERIFY(stream);
            QByteArray got;
            std::vector<char> buffer(100000);
            for (;;) {
                ULONG read = 0;
                QCOMPARE(stream->Read(buffer.data(), ULONG(buffer.size()), &read), S_OK);
                if (read == 0)
                    break;
                got.append(buffer.data(), qsizetype(read));
            }
            QCOMPARE(got.size(), item.data.size());
            QVERIFY(got == item.data);
        }
        QCOMPARE(opened.load(), int(items.size()));
    }

    // An empty file is at its end at once, even before it exists.
    void emptyFileIsAtItsEndAtOnce()
    {
        QTemporaryDir root;
        auto session = std::make_shared<FakeSession>(root.path());
        ComPtr<IStream> stream = makeStream(session, root.filePath(QStringLiteral("empty.txt")), 0);
        QVERIFY(stream);
        QElapsedTimer elapsed;
        elapsed.start();
        char buffer[16];
        ULONG read = 99;
        QCOMPARE(stream->Read(buffer, sizeof(buffer), &read), S_OK);
        QCOMPARE(read, ULONG(0));
        QVERIFY(elapsed.elapsed() < 1000);
    }

    // A waiting Read ends with the session: a failed transfer is a read fault, a cancel by the user
    // ERROR_CANCELLED; a transfer that succeeded with the file still short fails at once.
    void readEndsWithTheSession_data()
    {
        QTest::addColumn<int>("end");
        QTest::addColumn<int>("expected");
        QTest::newRow("failed") << int(State::Failed) << int(STG_E_READFAULT);
        QTest::newRow("abandoned") << int(State::Abandoned) << int(STG_E_READFAULT);
        QTest::newRow("cancelled") << int(State::Cancelled) << int(HRESULT_FROM_WIN32(ERROR_CANCELLED));
    }
    void readEndsWithTheSession()
    {
        QFETCH(int, end);
        QFETCH(int, expected);
        QTemporaryDir root;
        auto session = std::make_shared<FakeSession>(root.path());
        const QString path = root.filePath(QStringLiteral("a.bin"));
        writeNow(path, pattern(10)); // 10 of 100 bytes there
        ComPtr<IStream> stream = makeStream(session, path, 100);
        QVERIFY(stream);
        Joiner enders;
        enders.threads.emplace_back([session, end] {
            Sleep(300);
            session->set(State(end));
        });
        QElapsedTimer elapsed;
        elapsed.start();
        char buffer[100];
        ULONG read = 0;
        QCOMPARE(int(stream->Read(buffer, sizeof(buffer), &read)), expected);
        QVERIFY2(elapsed.elapsed() >= 250, qPrintable(QString::number(elapsed.elapsed()))); // it waited
        QVERIFY2(elapsed.elapsed() < 3000, qPrintable(QString::number(elapsed.elapsed())));
        QCOMPARE(read, ULONG(0));
    }
    void shortFileOfASucceededTransferFailsAtOnce()
    {
        QTemporaryDir root;
        auto session = std::make_shared<FakeSession>(root.path());
        session->set(State::Succeeded);
        const QString path = root.filePath(QStringLiteral("a.bin"));
        writeNow(path, pattern(50));
        ComPtr<IStream> stream = makeStream(session, path, 100);
        QVERIFY(stream);
        QElapsedTimer elapsed;
        elapsed.start();
        char buffer[100];
        ULONG read = 0;
        QCOMPARE(stream->Read(buffer, sizeof(buffer), &read), STG_E_READFAULT);
        QVERIFY2(elapsed.elapsed() < 500, qPrintable(QString::number(elapsed.elapsed())));
        // What is there can still be read.
        ComPtr<IStream> again = makeStream(session, path, 100);
        QCOMPARE(again->Read(buffer, 50, &read), S_OK);
        QCOMPARE(read, ULONG(50));
    }

    // A failed or cancelled transfer removes what it wrote at once: a stream holding a file does
    // not stop that, and lets the file go once it sees the end.
    void heldFileCanBeRemovedAfterAFailure()
    {
        QTemporaryDir root;
        auto session = std::make_shared<FakeSession>(root.path());
        const QString dir = root.filePath(QStringLiteral("Folder"));
        const QString path = dir + QStringLiteral("/a.bin");
        writeNow(path, pattern(10)); // 10 of 100 bytes there
        ComPtr<IStream> stream = makeStream(session, path, 100);
        QVERIFY(stream);
        char buffer[100];
        ULONG read = 0;
        QCOMPARE(stream->Read(buffer, 10, &read), S_OK); // the file is open now
        session->set(State::Failed);
        QVERIFY(QFile::remove(path));
        QVERIFY(QDir(dir).removeRecursively());
        QVERIFY(!QFileInfo::exists(dir));
        QCOMPARE(stream->Read(buffer, sizeof(buffer), &read), STG_E_READFAULT);
    }

    // A file that grew at its source after it was listed fails rather than end short.
    void fileLargerThanListedFails()
    {
        QTemporaryDir root;
        auto session = std::make_shared<FakeSession>(root.path());
        session->set(State::Succeeded);
        const QString path = root.filePath(QStringLiteral("a.log"));
        writeNow(path, pattern(150));
        ComPtr<IStream> stream = makeStream(session, path, 100);
        QVERIFY(stream);
        char buffer[100];
        ULONG read = 0;
        QCOMPARE(stream->Read(buffer, sizeof(buffer), &read), STG_E_READFAULT);
        QCOMPARE(read, ULONG(0));
    }

    // Quitting: stopAll stops every live session's request and joins its thread; a finished
    // transfer stays Succeeded, a running one fails.
    void stopAllStopsLiveSessions()
    {
        QTemporaryDir root;
        auto runningClosed = std::make_shared<std::atomic<bool>>(false);
        auto doneClosed = std::make_shared<std::atomic<bool>>(false);
        QString error;
        std::shared_ptr<PasteSession> running = PasteSession::open(idleRun(root.path(), runningClosed), &error);
        QVERIFY2(running, qPrintable(error));
        const QString rootPath = root.path();
        std::shared_ptr<PasteSession> done = PasteSession::open(
            [rootPath, doneClosed](PasteSession::Link &link) {
                link.started(rootPath);
                link.finished(State::Succeeded);
                while (!link.stopRequested())
                    Sleep(10);
                *doneClosed = true;
            },
            &error);
        QVERIFY2(done, qPrintable(error));
        QCOMPARE(done->state(), State::Succeeded);
        PasteSession::stopAll();
        QVERIFY(runningClosed->load()); // joined: no waiting
        QVERIFY(doneClosed->load());
        QCOMPARE(running->state(), State::Failed);
        QCOMPARE(done->state(), State::Succeeded);
        running.reset();
        done.reset();
        PasteSession::stopAll(); // nothing left
    }

    // The list arrives in clipListed chunks, between other events, split anywhere on the wire.
    void listIsGatheredFromChunks()
    {
        auto request = std::make_shared<QCborMap>();
        FakeService service([request](QLocalSocket &socket) {
            *request = FakeService::readRequest(socket);
            FakeService::send(socket, FakeService::event("state"));
            QCborArray first;
            first.append(QCborArray{QStringLiteral("a.txt"), 5});
            first.append(QCborArray{QStringLiteral("Folder"), -1});
            const QByteArray bytes = FakeService::frame(
                FakeService::event("clipListed", {{"ok", true}, {"items", first}, {"more", true}}));
            FakeService::send(socket, bytes.left(3));
            Sleep(50);
            FakeService::send(socket, bytes.mid(3));
            QCborArray last;
            last.append(QCborArray{QStringLiteral("Folder/b.bin"), 7});
            FakeService::send(socket,
                              FakeService::event("clipListed", {{"ok", true}, {"items", last}, {"more", false}}));
            socket.waitForDisconnected(3000);
        });
        QLocalSocket socket;
        socket.connectToServer(service.name());
        QVERIFY(socket.waitForConnected(3000));
        QString error;
        const QList<ClipboardBridge::ListedFile> files =
            df::pasterequests::listFiles(socket, QByteArray::fromHex("abcd"), &error, 5000);
        socket.abort();
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(request->value(QStringLiteral("cmd")).toString(), QStringLiteral("clipList"));
        QCOMPARE(request->value(QStringLiteral("kid")).toString(), QStringLiteral("abcd"));
        QCOMPARE(files.size(), 3);
        QCOMPARE(files.at(0).path, QStringLiteral("a.txt"));
        QCOMPARE(files.at(0).size, qint64(5));
        QVERIFY(!files.at(0).dir);
        QCOMPARE(files.at(1).path, QStringLiteral("Folder"));
        QVERIFY(files.at(1).dir);
        QCOMPARE(files.at(2).path, QStringLiteral("Folder/b.bin"));
        QCOMPARE(files.at(2).size, qint64(7));
    }
    void refusedListFails()
    {
        FakeService service([](QLocalSocket &socket) {
            FakeService::readRequest(socket);
            FakeService::send(socket,
                              FakeService::event("clipListed", {{"ok", false}, {"error", QStringLiteral("nope")}}));
            socket.waitForDisconnected(3000);
        });
        QLocalSocket socket;
        socket.connectToServer(service.name());
        QVERIFY(socket.waitForConnected(3000));
        QString error;
        const bool empty = df::pasterequests::listFiles(socket, QByteArray::fromHex("abcd"), &error, 5000).isEmpty();
        socket.abort();
        QVERIFY(empty);
        QCOMPARE(error, QStringLiteral("nope"));
    }

    // A streaming paste: clipStarted gives the folder, clipFetched {ok} ends it; the connection
    // stays open afterwards (the service keeps the folder), and its end then changes nothing.
    void streamingFetchSucceedsAndStaysConnected()
    {
        QTemporaryDir root;
        const QString rootPath = root.path();
        auto request = std::make_shared<QCborMap>();
        auto finish = std::make_shared<std::atomic<bool>>(false);
        auto stayed = std::make_shared<std::atomic<bool>>(false);
        FakeService service([=](QLocalSocket &socket) {
            *request = FakeService::readRequest(socket);
            FakeService::send(socket, FakeService::event("clipFetching"));
            FakeService::send(socket, FakeService::event("clipStarted", {{"root", rootPath}}));
            for (int i = 0; i < 500 && !*finish; ++i)
                Sleep(10);
            FakeService::send(socket,
                              FakeService::event("clipFetched", {{"ok", true},
                                                                 {"files", QCborArray{rootPath + QStringLiteral("/a.txt")}}}));
            *stayed = !socket.waitForDisconnected(500);
        });
        QString error;
        std::shared_ptr<PasteSession> session = openOn(service, &error);
        QVERIFY2(session, qPrintable(error));
        QCOMPARE(session->root(), rootPath);
        QCOMPARE(session->state(), State::Running);
        QCOMPARE(request->value(QStringLiteral("cmd")).toString(), QStringLiteral("clipFetch"));
        QCOMPARE(request->value(QStringLiteral("kid")).toString(), QStringLiteral("0102"));
        QVERIFY(request->value(QStringLiteral("stream")).toBool());
        QCOMPARE(request->value(QStringLiteral("listedAgo")).toInteger(), 1234); // the descriptor's read
        *finish = true;
        QTRY_COMPARE_WITH_TIMEOUT(session->state(), State::Succeeded, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(service.done.load(), 5000); // the service went after 500 ms
        QVERIFY(stayed->load());
        Sleep(300);
        QCOMPARE(session->state(), State::Succeeded);
        QCOMPARE(session->root(), rootPath);
    }

    // The service's refusals and ends: no paste, a cancel in DeskFlower, a failure.
    void streamingFetchEnds_data()
    {
        QTest::addColumn<bool>("started");
        QTest::addColumn<QString>("code");
        QTest::addColumn<int>("state");
        QTest::addColumn<QString>("error");
        QTest::newRow("noPaste") << false << QStringLiteral("noPaste") << int(State::Failed)
                                 << QString::fromLatin1(ClipboardBridge::kNotAPaste);
        QTest::newRow("refused") << false << QString() << int(State::Failed) << QStringLiteral("broken");
        QTest::newRow("cancelled") << true << QStringLiteral("cancelled") << int(State::Cancelled)
                                   << QStringLiteral("broken");
        QTest::newRow("failed") << true << QString() << int(State::Failed) << QStringLiteral("broken");
    }
    void streamingFetchEnds()
    {
        QFETCH(bool, started);
        QFETCH(QString, code);
        QFETCH(int, state);
        QFETCH(QString, error);
        QTemporaryDir root;
        const QString rootPath = root.path();
        auto finish = std::make_shared<std::atomic<bool>>(false);
        FakeService service([=](QLocalSocket &socket) {
            FakeService::readRequest(socket);
            if (started) {
                FakeService::send(socket, FakeService::event("clipStarted", {{"root", rootPath}}));
                for (int i = 0; i < 500 && !*finish; ++i)
                    Sleep(10);
            }
            QCborMap end = FakeService::event("clipFetched", {{"ok", false}, {"error", QStringLiteral("broken")}});
            if (!code.isEmpty())
                end.insert(QStringLiteral("code"), code);
            FakeService::send(socket, end);
            socket.waitForDisconnected(3000);
        });
        QString openError;
        std::shared_ptr<PasteSession> session = openOn(service, &openError);
        if (!started) {
            QVERIFY(!session);
            QCOMPARE(openError, error);
            return;
        }
        QVERIFY2(session, qPrintable(openError));
        *finish = true;
        QTRY_COMPARE_WITH_TIMEOUT(int(session->state()), state, 3000);
        QCOMPARE(session->error(), error);
    }

    // Without a word from the service the transfer fails after the idle time; clipFetching resets it.
    void silentServiceFails()
    {
        QTemporaryDir root;
        const QString rootPath = root.path();
        auto disconnected = std::make_shared<std::atomic<bool>>(false);
        FakeService service([=](QLocalSocket &socket) {
            FakeService::readRequest(socket);
            FakeService::send(socket, FakeService::event("clipStarted", {{"root", rootPath}}));
            *disconnected = socket.waitForDisconnected(5000);
        });
        QString error;
        std::shared_ptr<PasteSession> session = openOn(service, &error, 300);
        QVERIFY2(session, qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(session->state(), State::Failed, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(disconnected->load(), 3000);
    }
    void fetchingKeepsTheTransfer()
    {
        QTemporaryDir root;
        const QString rootPath = root.path();
        FakeService service([=](QLocalSocket &socket) {
            FakeService::readRequest(socket);
            FakeService::send(socket, FakeService::event("clipStarted", {{"root", rootPath}}));
            for (int i = 0; i < 9; ++i) { // three times the idle time
                Sleep(100);
                FakeService::send(socket, FakeService::event("clipFetching"));
            }
            FakeService::send(socket, FakeService::event("clipFetched", {{"ok", true}, {"files", QCborArray{}}}));
            socket.waitForDisconnected(1000);
        });
        QString error;
        std::shared_ptr<PasteSession> session = openOn(service, &error, 300);
        QVERIFY2(session, qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(session->state(), State::Succeeded, 5000);
    }

    // Files already here (an earlier paste brought them) come without clipStarted: the folder is
    // that of the top-level items.
    void filesAlreadyHereGiveTheirFolder()
    {
        QTemporaryDir root;
        const QString rootPath = root.path();
        FakeService service([=](QLocalSocket &socket) {
            FakeService::readRequest(socket);
            const QCborArray files{rootPath + QStringLiteral("/a.txt"), rootPath + QStringLiteral("/Folder")};
            FakeService::send(socket, FakeService::event("clipFetched", {{"ok", true}, {"files", files}}));
            socket.waitForDisconnected(3000);
        });
        QString error;
        std::shared_ptr<PasteSession> session = openOn(service, &error);
        QVERIFY2(session, qPrintable(error));
        QCOMPARE(session->root(), rootPath);
        QCOMPARE(session->state(), State::Succeeded);
    }

    // The session going drops the request: its connection closes (the service then cancels a
    // transfer nobody else waits for).
    void sessionGoingClosesTheConnection()
    {
        QTemporaryDir root;
        const QString rootPath = root.path();
        auto disconnected = std::make_shared<std::atomic<bool>>(false);
        FakeService service([=](QLocalSocket &socket) {
            FakeService::readRequest(socket);
            FakeService::send(socket, FakeService::event("clipStarted", {{"root", rootPath}}));
            *disconnected = socket.waitForDisconnected(5000);
        });
        QString error;
        std::shared_ptr<PasteSession> session = openOn(service, &error);
        QVERIFY2(session, qPrintable(error));
        Sleep(200);
        QVERIFY(!disconnected->load());
        session.reset();
        QTRY_VERIFY_WITH_TIMEOUT(disconnected->load(), 3000);
    }

    // CopyTo, Clone, Stat and Seek.
    void copyToCloneStatAndSeek()
    {
        QTemporaryDir root;
        auto session = std::make_shared<FakeSession>(root.path());
        const QByteArray data = pattern(200 * 1024 + 7);
        const QString path = root.filePath(QStringLiteral("dir/a.bin"));
        writeNow(path, data);
        session->set(State::Succeeded);
        ComPtr<IStream> stream = makeStream(session, path, data.size());
        QVERIFY(stream);

        STATSTG stat = {};
        QCOMPARE(stream->Stat(&stat, STATFLAG_DEFAULT), S_OK);
        QCOMPARE(qint64(stat.cbSize.QuadPart), qint64(data.size()));
        QCOMPARE(stat.type, DWORD(STGTY_STREAM));
        QVERIFY(stat.pwcsName);
        QCOMPARE(QString::fromWCharArray(stat.pwcsName), QStringLiteral("a.bin"));
        CoTaskMemFree(stat.pwcsName);
        stat = {};
        QCOMPARE(stream->Stat(&stat, STATFLAG_NONAME), S_OK);
        QVERIFY(!stat.pwcsName);
        QCOMPARE(qint64(stat.cbSize.QuadPart), qint64(data.size()));

        LARGE_INTEGER move = {};
        ULARGE_INTEGER position = {};
        QCOMPARE(stream->Seek(move, STREAM_SEEK_END, &position), S_OK);
        QCOMPARE(qint64(position.QuadPart), qint64(data.size()));
        move.QuadPart = 1000;
        QCOMPARE(stream->Seek(move, STREAM_SEEK_SET, &position), S_OK);
        char buffer[10];
        ULONG read = 0;
        QCOMPARE(stream->Read(buffer, 10, &read), S_OK);
        QCOMPARE(QByteArray(buffer, 10), data.mid(1000, 10));
        move.QuadPart = -5;
        QCOMPARE(stream->Seek(move, STREAM_SEEK_CUR, &position), S_OK);
        QCOMPARE(qint64(position.QuadPart), qint64(1005));
        move.QuadPart = -2000;
        QVERIFY(FAILED(stream->Seek(move, STREAM_SEEK_CUR, &position)));

        // A clone: same file, same position, its own position from then on.
        IStream *cloneRaw = nullptr;
        QCOMPARE(stream->Clone(&cloneRaw), S_OK);
        ComPtr<IStream> clone(cloneRaw);
        QCOMPARE(clone->Read(buffer, 10, &read), S_OK);
        QCOMPARE(QByteArray(buffer, 10), data.mid(1005, 10));
        QCOMPARE(stream->Read(buffer, 10, &read), S_OK);
        QCOMPARE(QByteArray(buffer, 10), data.mid(1005, 10));

        // CopyTo the whole file into a memory stream.
        move.QuadPart = 0;
        QCOMPARE(stream->Seek(move, STREAM_SEEK_SET, nullptr), S_OK);
        ComPtr<IStream> target(SHCreateMemStream(nullptr, 0));
        QVERIFY(target);
        ULARGE_INTEGER all = {};
        all.QuadPart = ~0ULL;
        ULARGE_INTEGER copiedIn = {}, copiedOut = {};
        QCOMPARE(stream->CopyTo(target.get(), all, &copiedIn, &copiedOut), S_OK);
        QCOMPARE(qint64(copiedIn.QuadPart), qint64(data.size()));
        QCOMPARE(qint64(copiedOut.QuadPart), qint64(data.size()));
        QCOMPARE(target->Seek(move, STREAM_SEEK_SET, nullptr), S_OK);
        QByteArray copied;
        std::vector<char> chunk(65536);
        while (SUCCEEDED(target->Read(chunk.data(), ULONG(chunk.size()), &read)) && read > 0)
            copied.append(chunk.data(), qsizetype(read));
        QVERIFY(copied == data);

        // Read only.
        ULONG written = 0;
        QVERIFY(FAILED(stream->Write("x", 1, &written)));
    }

    // A session nobody reads gives its transfer up (its connection closes).
    void sessionNobodyReadsIsAbandoned()
    {
        QTemporaryDir root;
        auto closed = std::make_shared<std::atomic<bool>>(false);
        QString error;
        std::shared_ptr<PasteSession> session = PasteSession::open(idleRun(root.path(), closed), &error, 300);
        QVERIFY2(session, qPrintable(error));
        QCOMPARE(session->root(), root.path());
        QCOMPARE(session->state(), State::Running);
        QTRY_VERIFY_WITH_TIMEOUT(closed->load(), 3000);
        QCOMPARE(session->state(), State::Abandoned);
    }

    // A Read in progress keeps the session (however long it waits); once nobody reads, it goes.
    void readInProgressKeepsTheSession()
    {
        QTemporaryDir root;
        auto closed = std::make_shared<std::atomic<bool>>(false);
        QString error;
        std::shared_ptr<PasteSession> session = PasteSession::open(idleRun(root.path(), closed), &error, 300);
        QVERIFY2(session, qPrintable(error));
        const QString path = root.filePath(QStringLiteral("late.bin"));
        const QByteArray data = pattern(4096);
        Joiner writers;
        writers.threads.emplace_back([path, data] { writeSlowly(path, data, data.size(), 0, 1200); });
        ComPtr<IStream> stream = makeStream(session, path, data.size());
        QVERIFY(stream);
        QElapsedTimer elapsed;
        elapsed.start();
        std::vector<char> buffer(data.size());
        ULONG read = 0;
        QCOMPARE(stream->Read(buffer.data(), ULONG(buffer.size()), &read), S_OK);
        QVERIFY2(elapsed.elapsed() >= 1000, qPrintable(QString::number(elapsed.elapsed())));
        QCOMPARE(qsizetype(read), data.size());
        QVERIFY(!closed->load()); // waited four times the abandon time, kept
        QCOMPARE(session->state(), State::Running);
        QTRY_VERIFY_WITH_TIMEOUT(closed->load(), 3000);
        QCOMPARE(session->state(), State::Abandoned);
        // Its streams then fail rather than wait.
        move(stream.get(), 0);
        QCOMPARE(stream->Read(buffer.data(), 10, &read), S_OK); // the bytes that are there still read
        ComPtr<IStream> other = makeStream(session, root.filePath(QStringLiteral("never.bin")), 10);
        QCOMPARE(other->Read(buffer.data(), 10, &read), STG_E_READFAULT);
    }

    // The clipboard object reuses its session while it is usable and opens a new one once it was
    // given up (a later paste).
    void givenUpSessionIsReplaced()
    {
        QTemporaryDir root;
        writeNow(root.filePath(QStringLiteral("a.txt")), "alpha");
        std::atomic<int> opens = 0;
        std::vector<std::weak_ptr<PasteSession>> sessions;
        ClipboardBridge::FileStreaming streaming;
        const QString rootPath = root.path();
        streaming.open = [&, rootPath](QString *error, qint64) -> std::shared_ptr<Session> {
            ++opens;
            auto closed = std::make_shared<std::atomic<bool>>(false);
            std::shared_ptr<PasteSession> session = PasteSession::open(idleRun(rootPath, closed), error, 300);
            sessions.push_back(session);
            return session;
        };
        const QList<ClipboardBridge::ListedFile> listed = {{QStringLiteral("a.txt"), 5, false}};
        ComPtr<IDataObject> object(createSealedData({}, {}, listed, 1, {}, false, DROPEFFECT_COPY, streaming));
        auto readAll = [&]() -> QByteArray {
            FORMATETC fc = format(kContents, TYMED_ISTREAM, 0);
            STGMEDIUM medium = {};
            if (object->GetData(&fc, &medium) != S_OK)
                return "failed";
            char buffer[16];
            ULONG read = 0;
            const HRESULT hr = medium.pstm->Read(buffer, sizeof(buffer), &read);
            ReleaseStgMedium(&medium);
            return SUCCEEDED(hr) ? QByteArray(buffer, qsizetype(read)) : QByteArray("read failed");
        };
        QCOMPARE(readAll(), QByteArray("alpha"));
        QCOMPARE(readAll(), QByteArray("alpha"));
        QCOMPARE(opens.load(), 1); // reused
        QCOMPARE(sessions.size(), size_t(1));
        // Nobody reads: given up (the object still holds it, so it is still there).
        QTRY_COMPARE_WITH_TIMEOUT(sessions.front().lock() ? int(sessions.front().lock()->state()) : -1,
                                  int(State::Abandoned), 3000);
        QCOMPARE(readAll(), QByteArray("alpha"));
        QCOMPARE(opens.load(), 2); // a new one
        // The given-up one went with the object's reference (no stream held it).
        QVERIFY(sessions.front().expired());
    }

    // Streams live in the multithreaded apartment: their Read runs there, and a proxy handed on to
    // another thread reads while the thread that made it does not pump.
    void streamsLiveInTheMultithreadedApartment()
    {
        QTemporaryDir root;
        auto session = std::make_shared<FakeSession>(root.path());
        const QByteArray data = pattern(5000);
        const QString path = root.filePath(QStringLiteral("a.bin"));
        writeNow(path, data);
        ComPtr<IStream> stream = makeStream(session, path, data.size());
        QVERIFY(stream);
        char buffer[100];
        ULONG read = 0;
        QCOMPARE(stream->Read(buffer, 100, &read), S_OK);
        QCOMPARE(session->readApartment.load(), int(APTTYPE_MTA));

        IStream *marshalled = nullptr;
        QCOMPARE(CoMarshalInterThreadInterfaceInStream(IID_IStream, stream.get(), &marshalled), S_OK);
        HANDLE finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::atomic<HRESULT> result = E_PENDING;
        std::atomic<ULONG> secondRead = 0;
        std::thread other([&] {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            IStream *proxy = nullptr;
            HRESULT hr = CoGetInterfaceAndReleaseStream(marshalled, IID_IStream, reinterpret_cast<void **>(&proxy));
            if (SUCCEEDED(hr)) {
                char inner[200];
                ULONG got = 0;
                hr = proxy->Read(inner, sizeof(inner), &got);
                secondRead = got;
                proxy->Release();
            }
            result = hr;
            SetEvent(finished);
            CoUninitialize();
        });
        // This thread does not pump meanwhile.
        const DWORD waited = WaitForSingleObject(finished, 10000);
        if (waited != WAIT_OBJECT_0) {
            other.detach();
            QFAIL("The second apartment's Read needed this thread");
        }
        other.join();
        CloseHandle(finished);
        QCOMPARE(result.load(), S_OK);
        QCOMPARE(secondRead.load(), ULONG(200));
    }

    // Explorer's side: another process gets the data object, reads the descriptor, gets a stream
    // and reads it to its end while this process's apartment is blocked (not pumping).
    void crossProcessPaste()
    {
        QTemporaryDir root;
        const QByteArray data = pattern(512 * 1024 + 11);
        const QString path = root.filePath(QStringLiteral("Folder/big.bin"));
        Joiner writers;
        auto session = std::make_shared<FakeSession>(root.path());
        std::atomic<int> opens = 0;
        ClipboardBridge::FileStreaming streaming;
        streaming.open = [&](QString *, qint64) -> std::shared_ptr<Session> {
            if (opens++ == 0) {
                // The transfer starts with the paste: written over about two seconds.
                writers.threads.emplace_back([path, data, session] {
                    writeSlowly(path, data, 32 * 1024, 120);
                    session->set(State::Succeeded);
                });
            }
            return session;
        };
        const QList<ClipboardBridge::ListedFile> listed = {{QStringLiteral("Folder"), 0, true},
                                                           {QStringLiteral("Folder/big.bin"), data.size(), false}};
        ComPtr<IDataObject> object(createSealedData({}, {}, listed, 2, {}, false, DROPEFFECT_COPY, streaming));

        IStream *marshalStream = nullptr;
        QCOMPARE(CreateStreamOnHGlobal(nullptr, TRUE, &marshalStream), S_OK);
        ComPtr<IStream> marshalHolder(marshalStream);
        QCOMPARE(CoMarshalInterface(marshalStream, IID_IDataObject, object.get(), MSHCTX_LOCAL, nullptr,
                                    MSHLFLAGS_NORMAL),
                 S_OK);
        HGLOBAL global = nullptr;
        QCOMPARE(GetHGlobalFromStream(marshalStream, &global), S_OK);
        ULARGE_INTEGER end = {};
        LARGE_INTEGER zero = {};
        marshalStream->Seek(zero, STREAM_SEEK_CUR, &end);
        const QByteArray marshalled(static_cast<const char *>(GlobalLock(global)), qsizetype(end.QuadPart));
        GlobalUnlock(global);

        QProcess child;
        child.start(QCoreApplication::applicationFilePath(),
                    {QStringLiteral("--child"), QString::fromLatin1(marshalled.toHex()), QString::number(data.size())});
        QVERIFY(child.waitForStarted(10000));
        QByteArray output;
        // Serve the child's calls (descriptor, contents) until it holds the stream...
        QTRY_VERIFY_WITH_TIMEOUT((output += child.readAllStandardOutput()).contains("STREAM")
                                     || child.state() == QProcess::NotRunning,
                                 20000);
        QVERIFY2(output.contains("STREAM"), output.constData());
        QCOMPARE(opens.load(), 1);
        // ...then block this apartment while it reads.
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, DWORD(child.processId()));
        QVERIFY(process);
        const DWORD waited = WaitForSingleObject(process, 30000);
        CloseHandle(process);
        QCOMPARE(waited, DWORD(WAIT_OBJECT_0));
        QVERIFY(child.waitForFinished(5000));
        output += child.readAllStandardOutput();
        QCOMPARE(child.exitCode(), 0);
        QVERIFY2(output.contains("DONE " + QCryptographicHash::hash(data, QCryptographicHash::Sha1).toHex()),
                 output.constData());
        QVERIFY(session->reads.load() > 0);
        QCOMPARE(session->readApartment.load(), int(APTTYPE_MTA));
    }

    // --- The move paste (large pastes: a deferred CF_HDROP moved by Explorer) -------------------

    // Large pastes only: 2,000 entries or 1 GB.
    void movePasteThreshold()
    {
        QVERIFY(!ClipboardBridge::wantsMovePaste(0, 0));
        QVERIFY(!ClipboardBridge::wantsMovePaste(ClipboardBridge::kMovePasteMinEntries - 1, 0));
        QVERIFY(ClipboardBridge::wantsMovePaste(ClipboardBridge::kMovePasteMinEntries, 0));
        QVERIFY(!ClipboardBridge::wantsMovePaste(10, ClipboardBridge::kMovePasteMinBytes - 1));
        QVERIFY(ClipboardBridge::wantsMovePaste(1, ClipboardBridge::kMovePasteMinBytes));
        QCOMPARE(ClipboardBridge::kMovePasteMinEntries, qsizetype(2000));
        QCOMPARE(ClipboardBridge::kMovePasteMinBytes, qint64(1) << 30);
    }

    // A move paste object offers a deferred CF_HDROP to be moved (Preferred DropEffect = move),
    // never the descriptor or the contents, listed or not, and looking at it asks for nothing.
    void movePasteOffersAMoveDropOnly()
    {
        QTemporaryDir root;
        FakeMoves moves(root.path());
        std::atomic<int> lists = 0, opens = 0, fetches = 0;
        ClipboardBridge::FileStreaming streaming;
        streaming.list = [&](QString *) {
            ++lists;
            return QList<ClipboardBridge::ListedFile>();
        };
        streaming.open = [&](QString *, qint64) -> std::shared_ptr<Session> {
            ++opens;
            return {};
        };
        streaming.move = moves.request();
        ClipboardBridge::FetchFiles fetch = [&](QString *) {
            ++fetches;
            return QStringList();
        };
        const QList<ClipboardBridge::ListedFile> listed = {{QStringLiteral("Tree"), 0, true},
                                                           {QStringLiteral("Tree/a.txt"), 5, false}};
        for (const bool withList : {true, false}) {
            ComPtr<IDataObject> object(createSealedData({}, {}, withList ? listed : QList<ClipboardBridge::ListedFile>(),
                                                        withList ? 2 : 19300, fetch, false, DROPEFFECT_COPY, streaming));
            QCOMPARE(formatsOf(object.get()), QStringList{QStringLiteral("hdrop")});
            FORMATETC drop = format(CF_HDROP, TYMED_HGLOBAL);
            QCOMPARE(object->QueryGetData(&drop), S_OK);
            FORMATETC fd = format(kDescriptor, TYMED_HGLOBAL);
            QCOMPARE(object->QueryGetData(&fd), DV_E_FORMATETC);
            FORMATETC fc = format(kContents, TYMED_ISTREAM);
            QCOMPARE(object->QueryGetData(&fc), DV_E_FORMATETC);
            QCOMPARE(preferredEffect(object.get()), DWORD(DROPEFFECT_MOVE));
            FORMATETC marker = format(RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing"), TYMED_HGLOBAL);
            QCOMPARE(object->QueryGetData(&marker), S_OK);
        }
        QCOMPARE(lists.load(), 0);
        QCOMPARE(opens.load(), 0);
        QCOMPARE(fetches.load(), 0);
        QCOMPARE(moves.opens.load(), 0);
    }

    // The setting off (or a small paste): no move given, so the large paste stays virtual files,
    // copied by Explorer.
    void withoutMoveALargePasteStaysVirtualFiles()
    {
        ClipboardBridge::FileStreaming streaming;
        streaming.list = [](QString *) { return QList<ClipboardBridge::ListedFile>(); };
        streaming.open = [](QString *, qint64) -> std::shared_ptr<Session> { return {}; };
        ComPtr<IDataObject> object(createSealedData({}, {}, {}, 5000, {}, false, DROPEFFECT_COPY, streaming));
        const QStringList offered = formatsOf(object.get());
        QVERIFY(offered.contains(QStringLiteral("descriptor")));
        QVERIFY(offered.contains(QStringLiteral("contents")));
        QVERIFY(!offered.contains(QStringLiteral("hdrop")));
        QCOMPARE(preferredEffect(object.get()), DWORD(DROPEFFECT_COPY));
    }

    // The paste's read transfers a copy of its own (waited for, pumping) and names its top-level
    // items; the same paste reading again soon after gets the same ones, with no new transfer.
    void movePasteHandsOverItsOwnCopy()
    {
        QTemporaryDir root;
        FakeMoves moves(root.path());
        ClipboardBridge::FileStreaming streaming;
        streaming.move = moves.request();
        ComPtr<IDataObject> object(createSealedData({}, {}, {}, 4, {}, false, DROPEFFECT_COPY, streaming));
        HRESULT hr = E_FAIL;
        const QStringList items = dropPaths(object.get(), &hr);
        QCOMPARE(hr, S_OK);
        QCOMPARE(moves.opens.load(), 1);
        const QString root1 = QDir(root.path()).filePath(QStringLiteral("1"));
        QCOMPARE(items, expectedItems(root1));
        QVERIFY(!clipboardRenderInProgress());
        auto session = moves.session(1).lock();
        QVERIFY(session);
        QVERIFY(session->reads.load() > 0); // waited for as a read: never given up meanwhile
        QCOMPARE(session->reading.load(), 0);
        QCOMPARE(dropPaths(object.get(), &hr), items);
        QCOMPARE(hr, S_OK);
        QCOMPARE(moves.opens.load(), 1);
        QCOMPARE(session->consumedCalls.load(), 0);
    }

    // Explorer moved the items and reported it: the copy is consumed (its session told, and let
    // go), and the next paste gets a new transfer. The clipboard object stays as it was.
    void movedCopyIsConsumedAndTheNextPasteTransfersAgain()
    {
        QTemporaryDir root, destination;
        FakeMoves moves(root.path());
        ClipboardBridge::FileStreaming streaming;
        streaming.move = moves.request();
        ComPtr<IDataObject> object(createSealedData({}, {}, {}, 4, {}, false, DROPEFFECT_COPY, streaming));
        const QStringList items = dropPaths(object.get());
        QCOMPARE(items.size(), 2);
        QVERIFY(moveItems(items, destination.path()));
        reportEffect(object.get(), CFSTR_PERFORMEDDROPEFFECT, DROPEFFECT_MOVE);
        auto first = moves.session(1);
        QTRY_VERIFY(first.expired());
        HRESULT hr = E_FAIL;
        const QStringList again = dropPaths(object.get(), &hr);
        QCOMPARE(hr, S_OK);
        QCOMPARE(moves.opens.load(), 2);
        QCOMPARE(again, expectedItems(QDir(root.path()).filePath(QStringLiteral("2"))));
        QCOMPARE(readFile(destination.filePath(QStringLiteral("Tree/sub/b.txt"))), QByteArray("beta"));
        QCOMPARE(formatsOf(object.get()), QStringList{QStringLiteral("hdrop")});
    }

    // The consumed session is told so (its request reports it to the service) before it goes,
    // whichever of the shell's reports says it: Explorer moving the items itself (an optimized
    // move) reports the move as the logical effect only.
    void moveIsReportedByThePerformedEffectOrPasteSucceeded_data()
    {
        QTest::addColumn<QString>("report");
        QTest::addColumn<bool>("counts");
        QTest::newRow("performed") << QString::fromWCharArray(CFSTR_PERFORMEDDROPEFFECT) << true;
        QTest::newRow("paste succeeded") << QString::fromWCharArray(CFSTR_PASTESUCCEEDED) << true;
        QTest::newRow("logical") << QString::fromWCharArray(CFSTR_LOGICALPERFORMEDDROPEFFECT) << true;
    }
    void moveIsReportedByThePerformedEffectOrPasteSucceeded()
    {
        QFETCH(QString, report);
        QFETCH(bool, counts);
        QTemporaryDir root, destination;
        FakeMoves moves(root.path());
        ClipboardBridge::FileStreaming streaming;
        streaming.move = moves.request();
        ComPtr<IDataObject> object(createSealedData({}, {}, {}, 4, {}, false, DROPEFFECT_COPY, streaming));
        const QStringList items = dropPaths(object.get());
        QCOMPARE(items.size(), 2);
        std::shared_ptr<FakeSession> session = moves.session(1).lock();
        QVERIFY(session);
        QVERIFY(moveItems(items, destination.path()));
        reportEffect(object.get(), reinterpret_cast<const wchar_t *>(report.utf16()), DROPEFFECT_MOVE);
        QCOMPARE(session->consumedCalls.load(), counts ? 1 : 0);
        session.reset();
        QCOMPARE(moves.session(1).expired(), counts);
        // Reported or not: the next read sees the items gone all the same.
        HRESULT hr = E_FAIL;
        dropPaths(object.get(), &hr);
        QCOMPARE(hr, S_OK);
        QCOMPARE(moves.opens.load(), 2);
        QVERIFY(moves.session(1).expired());
    }

    // An async paste (IDataObjectAsyncCapability) reports the move with EndOperation, maybe on its
    // own thread: handled on the object's thread.
    void asyncEndOperationReportsTheMove()
    {
        QTemporaryDir root, destination;
        FakeMoves moves(root.path());
        ClipboardBridge::FileStreaming streaming;
        streaming.move = moves.request();
        ComPtr<IDataObject> object(createSealedData({}, {}, {}, 4, {}, false, DROPEFFECT_COPY, streaming));
        IDataObjectAsyncCapability *async = nullptr;
        QCOMPARE(object->QueryInterface(IID_PPV_ARGS(&async)), S_OK);
        ComPtr<IDataObjectAsyncCapability> asyncHolder(async);
        QCOMPARE(async->StartOperation(nullptr), S_OK);
        const QStringList items = dropPaths(object.get());
        QCOMPARE(items.size(), 2);
        std::shared_ptr<FakeSession> session = moves.session(1).lock();
        QVERIFY(moveItems(items, destination.path()));
        std::thread target([async] { async->EndOperation(S_OK, nullptr, DROPEFFECT_MOVE); });
        target.join();
        QTRY_COMPARE(session->consumedCalls.load(), 1);
    }

    // A later paste (after the reuse window) gets a copy of its own: the earlier one may still be
    // moved by its paste, so it stays held (pinned) until its items are gone or the object goes.
    void laterPasteGetsANewCopyAndTheEarlierStaysHeld()
    {
        setMoveReuseWindowForTests(150);
        QTemporaryDir root, destination;
        FakeMoves moves(root.path());
        ClipboardBridge::FileStreaming streaming;
        streaming.move = moves.request();
        ComPtr<IDataObject> object(createSealedData({}, {}, {}, 4, {}, false, DROPEFFECT_COPY, streaming));
        const QStringList first = dropPaths(object.get());
        QCOMPARE(first.size(), 2);
        Sleep(300);
        const QStringList second = dropPaths(object.get());
        setMoveReuseWindowForTests(2000);
        QCOMPARE(moves.opens.load(), 2);
        QVERIFY(second != first);
        QTest::qWait(100); // the writers let go
        QVERIFY(!moves.session(1).expired());
        QVERIFY(!moves.session(2).expired());
        auto s1 = moves.session(1).lock();
        auto s2 = moves.session(2).lock();
        // The first paste's move finishes (no report): consumed when the object looks again.
        QVERIFY(moveItems(first, destination.path()));
        setMoveLingerForTests(60000);
        object.reset(); // the clipboard changed
        QCOMPARE(s1->consumedCalls.load(), 1);
        // The second is still there: kept a while (its paste may still be moving out of it,
        // Explorer holding only its paths), consumed once its items are gone.
        QCOMPARE(s2->consumedCalls.load(), 0);
        QVERIFY(QFileInfo::exists(second.first()));
        s1.reset();
        s2.reset();
        QVERIFY(moves.session(1).expired());
        QTest::qWait(100);
        QVERIFY(!moves.session(2).expired());
        QTemporaryDir destination2;
        QVERIFY(moveItems(second, destination2.path()));
        QTRY_VERIFY_WITH_TIMEOUT(moves.session(2).expired(), 10000);
        setMoveLingerForTests(0);
    }

    // A replaced object's copy that nothing moves is kept only so long, then let go untouched
    // (left to the service's prune of older copies).
    void replacedObjectsCopyIsLetGoAfterAWhile()
    {
        QTemporaryDir root;
        FakeMoves moves(root.path());
        ClipboardBridge::FileStreaming streaming;
        streaming.move = moves.request();
        ComPtr<IDataObject> object(createSealedData({}, {}, {}, 4, {}, false, DROPEFFECT_COPY, streaming));
        const QStringList items = dropPaths(object.get());
        QCOMPARE(items.size(), 2);
        QTest::qWait(100); // the writer lets go
        setMoveLingerForTests(300);
        object.reset();
        auto s = moves.session(1).lock();
        QVERIFY(s);
        s.reset();
        QVERIFY(!moves.session(1).expired());
        QTRY_VERIFY_WITH_TIMEOUT(moves.session(1).expired(), 10000);
        setMoveLingerForTests(0);
        QVERIFY(QFileInfo::exists(items.first())); // never removed from here
    }

    // Repeated pastes that move nothing (each cancelled in Explorer) do not pile up pinned copies:
    // beyond the current one and two earlier ones, the oldest is let go.
    void earlierCopiesKeptAreCapped()
    {
        setMoveReuseWindowForTests(30);
        QTemporaryDir root;
        FakeMoves moves(root.path());
        moves.transferMs = 10;
        ClipboardBridge::FileStreaming streaming;
        streaming.move = moves.request();
        ComPtr<IDataObject> object(createSealedData({}, {}, {}, 4, {}, false, DROPEFFECT_COPY, streaming));
        for (int i = 0; i < 4; ++i) {
            QCOMPARE(dropPaths(object.get()).size(), 2);
            Sleep(60);
        }
        setMoveReuseWindowForTests(2000);
        QCOMPARE(moves.opens.load(), 4);
        QTest::qWait(100); // the writers let go
        QVERIFY(moves.session(1).expired());
        for (int n = 2; n <= 4; ++n)
            QVERIFY2(!moves.session(n).expired(), qPrintable(QString::number(n)));
    }

    // What the shell sets on a move paste object is not offered back: its Preferred DropEffect
    // stays move (a next paste moves too), and one paste's results do not reach the next.
    void movePasteObjectKeepsItsOwnEffect()
    {
        QTemporaryDir root;
        FakeMoves moves(root.path());
        ClipboardBridge::FileStreaming streaming;
        streaming.move = moves.request();
        ComPtr<IDataObject> object(createSealedData({}, {}, {}, 4, {}, false, DROPEFFECT_COPY, streaming));
        reportEffect(object.get(), CFSTR_PREFERREDDROPEFFECT, DROPEFFECT_COPY);
        QCOMPARE(preferredEffect(object.get()), DWORD(DROPEFFECT_MOVE));
        for (const wchar_t *name : {CFSTR_PASTESUCCEEDED, CFSTR_PERFORMEDDROPEFFECT, CFSTR_LOGICALPERFORMEDDROPEFFECT}) {
            reportEffect(object.get(), name, DROPEFFECT_MOVE);
            FORMATETC f = format(RegisterClipboardFormatW(name), TYMED_HGLOBAL);
            QCOMPARE(object->QueryGetData(&f), DV_E_FORMATETC);
        }
        QCOMPARE(moves.opens.load(), 0);
    }

    // A report cannot say which paste it is from: one that arrives while the items are still there
    // (another paste's, or a copy) does not keep the same paste from reading its hand-out again.
    void moveReportWithTheItemsStillThereKeepsTheHandOut()
    {
        QTemporaryDir root;
        FakeMoves moves(root.path());
        ClipboardBridge::FileStreaming streaming;
        streaming.move = moves.request();
        ComPtr<IDataObject> object(createSealedData({}, {}, {}, 4, {}, false, DROPEFFECT_COPY, streaming));
        const QStringList items = dropPaths(object.get());
        QCOMPARE(items.size(), 2);
        reportEffect(object.get(), CFSTR_PASTESUCCEEDED, DROPEFFECT_MOVE);
        HRESULT hr = E_FAIL;
        QCOMPARE(dropPaths(object.get(), &hr), items);
        QCOMPARE(hr, S_OK);
        QCOMPARE(moves.opens.load(), 1);
        auto session = moves.session(1).lock();
        QVERIFY(session);
        QCOMPARE(session->consumedCalls.load(), 0);
    }

    // A copy something was taken from (a move to another volume cancelled halfway) is never handed
    // out again, even within the reuse window: the next paste gets a whole new copy.
    void partlyMovedCopyIsNotHandedOutAgain()
    {
        QTemporaryDir root;
        FakeMoves moves(root.path());
        ClipboardBridge::FileStreaming streaming;
        streaming.move = moves.request();
        ComPtr<IDataObject> object(createSealedData({}, {}, {}, 4, {}, false, DROPEFFECT_COPY, streaming));
        const QStringList first = dropPaths(object.get());
        QCOMPARE(first.size(), 2);
        QVERIFY(QFile::remove(QDir(root.path()).filePath(QStringLiteral("1/Tree/sub/b.txt"))));
        HRESULT hr = E_FAIL;
        const QStringList second = dropPaths(object.get(), &hr);
        QCOMPARE(hr, S_OK);
        QCOMPARE(moves.opens.load(), 2);
        QCOMPARE(second, expectedItems(QDir(root.path()).filePath(QStringLiteral("2"))));
        auto s1 = moves.session(1).lock();
        QVERIFY(s1); // half there: held, not consumed
        QCOMPARE(s1->consumedCalls.load(), 0);
    }

    // A transfer cancelled in DeskFlower's window, failed or given up: the paste fails, nothing is
    // kept, and the next paste asks again.
    void unfinishedMovePasteFails_data()
    {
        QTest::addColumn<int>("end");
        QTest::newRow("cancelled") << int(State::Cancelled);
        QTest::newRow("failed") << int(State::Failed);
        QTest::newRow("abandoned") << int(State::Abandoned);
    }
    void unfinishedMovePasteFails()
    {
        QFETCH(int, end);
        QTemporaryDir root;
        FakeMoves moves(root.path());
        moves.end = State(end);
        ClipboardBridge::FileStreaming streaming;
        streaming.move = moves.request();
        ComPtr<IDataObject> object(createSealedData({}, {}, {}, 4, {}, false, DROPEFFECT_COPY, streaming));
        HRESULT hr = S_OK;
        QVERIFY(dropPaths(object.get(), &hr).isEmpty());
        QCOMPARE(hr, E_FAIL);
        QTRY_VERIFY(moves.session(1).expired()); // let go: the service ends or forgets the transfer
        moves.end = State::Succeeded;
        QCOMPARE(dropPaths(object.get(), &hr).size(), 2);
        QCOMPARE(hr, S_OK);
        QCOMPARE(moves.opens.load(), 2);
    }

    // The paste rule holds on the move paste's CF_HDROP: a read that is no paste (refused by the
    // service, kNotAPaste) gets nothing, at once, and nothing of it is remembered.
    void movePasteWithoutAPasteIsRefused()
    {
        QTemporaryDir root;
        FakeMoves moves(root.path());
        moves.refuse = QString::fromLatin1(ClipboardBridge::kNotAPaste);
        ClipboardBridge::FileStreaming streaming;
        streaming.move = moves.request();
        ComPtr<IDataObject> object(createSealedData({}, {}, {}, 4, {}, false, DROPEFFECT_COPY, streaming));
        QElapsedTimer elapsed;
        elapsed.start();
        HRESULT hr = S_OK;
        QVERIFY(dropPaths(object.get(), &hr).isEmpty());
        QCOMPARE(hr, E_FAIL);
        QVERIFY2(elapsed.elapsed() < 2000, qPrintable(QString::number(elapsed.elapsed())));
        QVERIFY(dropPaths(object.get(), &hr).isEmpty());
        QCOMPARE(moves.opens.load(), 2); // asked again
        QVERIFY(!QFileInfo::exists(QDir(root.path()).filePath(QStringLiteral("1"))));
        moves.refuse.clear();
        QCOMPARE(dropPaths(object.get(), &hr).size(), 2);
        QCOMPARE(hr, S_OK);
    }

    // A second paste of the same object while the first one's transfer runs (the object's thread
    // pumps meanwhile) is refused: both would be given the same items to move.
    void secondPasteDuringTheTransferIsRefused()
    {
        QTemporaryDir root;
        FakeMoves moves(root.path());
        moves.transferMs = 800;
        ClipboardBridge::FileStreaming streaming;
        streaming.move = moves.request();
        ComPtr<IDataObject> object(createSealedData({}, {}, {}, 4, {}, false, DROPEFFECT_COPY, streaming));
        HRESULT second = S_OK;
        bool secondRan = false;
        QTimer::singleShot(200, [&] {
            dropPaths(object.get(), &second);
            secondRan = true;
        });
        HRESULT first = E_FAIL;
        const QStringList items = dropPaths(object.get(), &first);
        QCOMPARE(first, S_OK);
        QCOMPARE(items.size(), 2);
        QVERIFY(secondRan);
        QCOMPARE(second, E_FAIL);
        QCOMPARE(moves.opens.load(), 1);
    }

    // The move paste's real session: a fresh streaming request, kept out of liveRoots (DeskFlower's
    // window shows its transfer), and once consumed it tells the service (clipConsumed) before
    // closing its connection.
    void moveSessionAsksForAFreshTransferAndReportsItsConsumption()
    {
        QTemporaryDir root;
        const QString rootPath = root.path();
        std::atomic<bool> fresh = false, listedAgo = true, consumedSent = false;
        FakeService service([&](QLocalSocket &socket) {
            const QCborMap request = FakeService::readRequest(socket);
            fresh = request.value(QStringLiteral("cmd")).toString() == QLatin1String("clipFetch")
                    && request.value(QStringLiteral("stream")).toBool() && request.value(QStringLiteral("fresh")).toBool();
            listedAgo = request.contains(QStringLiteral("listedAgo"));
            FakeService::send(socket, FakeService::event("clipStarted", {{"root", QDir::toNativeSeparators(rootPath)}}));
            FakeService::send(socket, FakeService::event("clipFetched", {{"ok", true}}));
            const QCborMap next = FakeService::readRequest(socket);
            consumedSent = next.value(QStringLiteral("cmd")).toString() == QLatin1String("clipConsumed");
        });
        const QString name = service.name();
        QString error;
        std::shared_ptr<PasteSession> session = PasteSession::open(
            [name](PasteSession::Link &link) {
                QLocalSocket socket;
                socket.connectToServer(name);
                if (!socket.waitForConnected(3000)) {
                    link.finished(State::Failed, QStringLiteral("no connection"));
                    return;
                }
                df::pasterequests::runStreamingFetch(socket, QByteArray::fromHex("0102"), -1, link, 30000, true);
            },
            &error, 200, false);
        QVERIFY2(session, qPrintable(error));
        QTRY_COMPARE(session->state(), State::Succeeded);
        QVERIFY(fresh.load());
        QVERIFY(!listedAgo.load());
        QVERIFY(!PasteSession::liveRoots().contains(QDir::toNativeSeparators(rootPath)));
        QVERIFY(!PasteSession::liveRoots().contains(rootPath));
        session->consumed();
        session.reset();
        QTRY_VERIFY(service.done.load());
        QVERIFY(consumedSent.load());
    }

    // A streaming paste serves a small file that is already complete in memory (an HGLOBAL with
    // exactly its bytes) to a reader that takes one as well as a stream; anything else gets the
    // stream as before: a file not complete yet (which the stream then waits for), a large one, a
    // reader that takes only streams, a file whose size on disk is not the listed one, a missing
    // or empty file. FileContents is offered as a stream only, and a reader that takes only an
    // HGLOBAL is refused before anything opens (no transfer for a request that could fail).
    void smallCompleteFilesAreServedInMemory()
    {
        QTemporaryDir root;
        auto session = std::make_shared<FakeSession>(root.path());
        std::atomic<int> opens = 0;
        ClipboardBridge::FileStreaming streaming;
        streaming.open = [&](QString *, qint64) -> std::shared_ptr<Session> {
            ++opens;
            return session;
        };
        const QByteArray tiny = pattern(4321);
        const QByteArray limit = pattern(1024 * 1024);
        const QByteArray large = pattern(1024 * 1024 + 1);
        const QByteArray growing = pattern(3000);
        const QByteArray longer = pattern(200);
        const QList<ClipboardBridge::ListedFile> listed = {
            {QStringLiteral("Folder/tiny.bin"), tiny.size(), false},     // 1 (0 is Folder)
            {QStringLiteral("limit.bin"), limit.size(), false},            // 2
            {QStringLiteral("large.bin"), large.size(), false},            // 3
            {QStringLiteral("growing.bin"), growing.size(), false},        // 4
            {QStringLiteral("longer.bin"), longer.size() - 1, false},      // 5
            {QStringLiteral("missing.bin"), 10, false},                    // 6
            {QStringLiteral("empty.bin"), 0, false}};                      // 7
        writeNow(root.filePath(QStringLiteral("Folder/tiny.bin")), tiny);
        writeNow(root.filePath(QStringLiteral("limit.bin")), limit);
        writeNow(root.filePath(QStringLiteral("large.bin")), large);
        writeNow(root.filePath(QStringLiteral("growing.bin")), growing.left(1000));
        writeNow(root.filePath(QStringLiteral("longer.bin")), longer);
        writeNow(root.filePath(QStringLiteral("empty.bin")), QByteArray());
        ComPtr<IDataObject> object(
            createSealedData({}, {}, listed, int(listed.size()), {}, false, DROPEFFECT_COPY, streaming));

        // Offered as a stream; HGLOBAL only as an extra, as Explorer asks (HGLOBAL | ISTREAM | ISTORAGE).
        IEnumFORMATETC *formats = nullptr;
        QCOMPARE(object->EnumFormatEtc(DATADIR_GET, &formats), S_OK);
        DWORD offered = 0;
        FORMATETC item = {};
        while (formats->Next(1, &item, nullptr) == S_OK) {
            if (item.cfFormat == kContents)
                offered = item.tymed;
        }
        formats->Release();
        QCOMPARE(offered, DWORD(TYMED_ISTREAM));
        for (DWORD tymed : {DWORD(TYMED_ISTREAM), DWORD(TYMED_HGLOBAL | TYMED_ISTREAM), DWORD(TYMED_HGLOBAL | TYMED_ISTREAM | TYMED_ISTORAGE)}) {
            FORMATETC q = format(kContents, tymed, 1);
            QCOMPARE(object->QueryGetData(&q), S_OK);
        }
        for (DWORD tymed : {DWORD(TYMED_HGLOBAL), DWORD(TYMED_ISTORAGE), DWORD(TYMED_HGLOBAL | TYMED_ISTORAGE)}) {
            FORMATETC q = format(kContents, tymed, 1);
            QCOMPARE(object->QueryGetData(&q), DV_E_FORMATETC);
            // Nor served: refused before the paste session (the transfer) is opened, even for a
            // small complete file that could have been.
            STGMEDIUM medium = {};
            QCOMPARE(object->GetData(&q, &medium), DV_E_FORMATETC);
            q.lindex = 6; // missing
            QCOMPARE(object->GetData(&q, &medium), DV_E_FORMATETC);
        }
        QCOMPARE(opens.load(), 0); // looking, or asking in a medium that cannot always be served, transfers nothing

        const DWORD explorer = TYMED_HGLOBAL | TYMED_ISTREAM | TYMED_ISTORAGE;
        auto get = [&](LONG index, DWORD tymed, STGMEDIUM *medium) {
            FORMATETC fc = format(kContents, tymed, index);
            *medium = {};
            return object->GetData(&fc, medium);
        };
        auto inMemory = [&](LONG index, DWORD tymed = TYMED_HGLOBAL | TYMED_ISTREAM | TYMED_ISTORAGE) -> QByteArray {
            STGMEDIUM medium = {};
            if (get(index, tymed, &medium) != S_OK)
                return "failed";
            if (medium.tymed != TYMED_HGLOBAL) {
                ReleaseStgMedium(&medium);
                return "not in memory";
            }
            const QByteArray bytes(static_cast<const char *>(GlobalLock(medium.hGlobal)), qsizetype(GlobalSize(medium.hGlobal)));
            GlobalUnlock(medium.hGlobal);
            ReleaseStgMedium(&medium);
            return bytes;
        };
        // A stream (and its content read to the end, or "read failed").
        auto streamed = [&](LONG index, DWORD tymed = TYMED_HGLOBAL | TYMED_ISTREAM | TYMED_ISTORAGE) -> QByteArray {
            STGMEDIUM medium = {};
            if (get(index, tymed, &medium) != S_OK)
                return "failed";
            if (medium.tymed != TYMED_ISTREAM) {
                ReleaseStgMedium(&medium);
                return "not a stream";
            }
            QByteArray content;
            std::vector<char> buffer(256 * 1024);
            for (;;) {
                ULONG read = 0;
                if (FAILED(medium.pstm->Read(buffer.data(), ULONG(buffer.size()), &read))) {
                    content = "read failed";
                    break;
                }
                content.append(buffer.data(), qsizetype(read));
                if (read < buffer.size())
                    break;
            }
            ReleaseStgMedium(&medium);
            return content;
        };

        QCOMPARE(inMemory(1), tiny);
        QCOMPARE(opens.load(), 1); // the paste's session (the service's paste rule) all the same
        QCOMPARE(inMemory(1, TYMED_HGLOBAL | TYMED_ISTREAM), tiny);
        QCOMPARE(inMemory(2), limit); // 1 MiB: still in memory
        const int touches = session->touches.load();
        QCOMPARE(streamed(1, TYMED_ISTREAM), tiny); // a reader that takes only streams
        QCOMPARE(session->touches.load(), touches + 1); // a request is the session's activity either way
        QCOMPARE(streamed(3), large);                 // larger than 1 MiB
        QCOMPARE(streamed(7), QByteArray());          // empty
        // Larger on disk than listed: no memory copy cut short, the stream fails.
        QCOMPARE(streamed(5), QByteArray("read failed"));
        // Not complete yet: a stream, which waits for the rest (never this thread).
        {
            STGMEDIUM medium = {};
            QCOMPARE(get(4, explorer, &medium), S_OK);
            QCOMPARE(medium.tymed, DWORD(TYMED_ISTREAM));
            Joiner writers;
            writers.threads.emplace_back([&] {
                Sleep(100);
                writeSlowly(root.filePath(QStringLiteral("growing.bin")), growing, 500, 20);
            });
            std::vector<char> buffer(growing.size());
            ULONG read = 0;
            QCOMPARE(medium.pstm->Read(buffer.data(), ULONG(buffer.size()), &read), S_OK);
            QCOMPARE(QByteArray(buffer.data(), qsizetype(read)), growing);
            ReleaseStgMedium(&medium);
        }
        QCOMPARE(inMemory(4), growing); // complete now
        // Missing: a stream (which waits for it); a reader that takes only HGLOBAL is still refused
        // as a format (whether or not the file could be served in memory).
        {
            STGMEDIUM medium = {};
            QCOMPARE(get(6, explorer, &medium), S_OK);
            QCOMPARE(medium.tymed, DWORD(TYMED_ISTREAM));
            ReleaseStgMedium(&medium);
            QCOMPARE(get(6, TYMED_HGLOBAL, &medium), DV_E_FORMATETC);
            QCOMPARE(get(3, TYMED_HGLOBAL, &medium), DV_E_FORMATETC); // too large for memory
            QCOMPARE(get(1, TYMED_HGLOBAL, &medium), DV_E_FORMATETC); // small and complete
        }
        QCOMPARE(opens.load(), 1);
        session->set(State::Succeeded);
    }

    // A file being replaced on disk while it is read in memory is never served short: the
    // in-memory read checks its size again after reading (readCompleteFile).
    void readCompleteFileIsExact()
    {
        QTemporaryDir root;
        const QString path = root.filePath(QStringLiteral("a.bin"));
        const QByteArray data = pattern(70000);
        writeNow(path, data);
        HGLOBAL h = df::streamhost::readCompleteFile(path, data.size());
        QVERIFY(h);
        QCOMPARE(qsizetype(GlobalSize(h)), data.size());
        QCOMPARE(QByteArray(static_cast<const char *>(GlobalLock(h)), data.size()), data);
        GlobalUnlock(h);
        GlobalFree(h);
        QVERIFY(!df::streamhost::readCompleteFile(path, data.size() + 1)); // shorter than listed
        QVERIFY(!df::streamhost::readCompleteFile(path, data.size() - 1)); // longer than listed
        QVERIFY(!df::streamhost::readCompleteFile(path, 0));
        QVERIFY(!df::streamhost::readCompleteFile(root.filePath(QStringLiteral("none.bin")), 5));
        // Read while the writer holds it open for writing (the transfer's sharing), and the
        // file can be removed while read (delete sharing).
        HANDLE writer = CreateFileW(native(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(writer != INVALID_HANDLE_VALUE);
        h = df::streamhost::readCompleteFile(path, data.size());
        CloseHandle(writer);
        QVERIFY(h);
        GlobalFree(h);
    }

    // Objects that transfer everything first (drags, older services) serve streams only, and say so.
    void wholeTransferOffersStreamsOnly()
    {
        const QList<ClipboardBridge::ListedFile> listed = {{QStringLiteral("a.txt"), 5, false}};
        std::atomic<int> fetches = 0;
        ClipboardBridge::FetchFiles fetch = [&](QString *) {
            ++fetches;
            return QStringList();
        };
        for (int kind = 0; kind < 2; ++kind) {
            ComPtr<IDataObject> object(kind == 0 ? createVirtualFiles(listed, fetch)
                                                 : createSealedData({}, {}, listed, 1, fetch, false, DROPEFFECT_COPY, {}));
            IEnumFORMATETC *formats = nullptr;
            QCOMPARE(object->EnumFormatEtc(DATADIR_GET, &formats), S_OK);
            DWORD offered = 0;
            FORMATETC item = {};
            while (formats->Next(1, &item, nullptr) == S_OK) {
                if (item.cfFormat == kContents)
                    offered = item.tymed;
            }
            formats->Release();
            QCOMPARE(offered, DWORD(TYMED_ISTREAM));
            FORMATETC q = format(kContents, TYMED_HGLOBAL, 0);
            QCOMPARE(object->QueryGetData(&q), DV_E_FORMATETC);
            STGMEDIUM medium = {};
            QCOMPARE(object->GetData(&q, &medium), DV_E_FORMATETC);
            q.tymed = TYMED_ISTREAM | TYMED_HGLOBAL;
            QCOMPARE(object->QueryGetData(&q), S_OK);
        }
        QCOMPARE(fetches.load(), 0);
    }

    // The descriptor is built once per list: every read gives the same bytes; an object listed on
    // demand builds it from the list it got.
    void descriptorIsCached()
    {
        QList<ClipboardBridge::ListedFile> list;
        for (int k = 0; k < 500; ++k)
            list.append({QStringLiteral("d%1/f%2.bin").arg(k % 7).arg(k), k * 3, false});
        std::atomic<int> lists = 0;
        ClipboardBridge::FileStreaming streaming;
        streaming.list = [&, list](QString *) {
            ++lists;
            return list;
        };
        streaming.open = [&](QString *, qint64) -> std::shared_ptr<Session> { return {}; };
        auto read = [](IDataObject *object) -> QByteArray {
            FORMATETC fd = format(kDescriptor, TYMED_HGLOBAL);
            STGMEDIUM medium = {};
            if (object->GetData(&fd, &medium) != S_OK)
                return {};
            const auto *group = static_cast<const FILEGROUPDESCRIPTORW *>(GlobalLock(medium.hGlobal));
            const QByteArray bytes(reinterpret_cast<const char *>(group),
                                   qsizetype(sizeof(FILEGROUPDESCRIPTORW) + sizeof(FILEDESCRIPTORW) * (group->cItems - 1)));
            GlobalUnlock(medium.hGlobal);
            ReleaseStgMedium(&medium);
            return bytes;
        };
        ComPtr<IDataObject> onDemand(createSealedData({}, {}, {}, int(list.size()), {}, false, DROPEFFECT_COPY, streaming));
        ComPtr<IDataObject> listed(createSealedData({}, {}, list, int(list.size()), {}, false, DROPEFFECT_COPY, streaming));
        const QByteArray first = read(onDemand.get());
        QVERIFY(!first.isEmpty());
        QCOMPARE(read(onDemand.get()), first);
        QCOMPARE(read(onDemand.get()), first);
        QCOMPARE(lists.load(), 1);
        // The same list announced up front gives the same descriptor.
        QCOMPARE(read(listed.get()), first);
        QCOMPARE(read(listed.get()), first);
        const auto *group = reinterpret_cast<const FILEGROUPDESCRIPTORW *>(first.constData());
        QCOMPARE(int(group->cItems), 7 + 500); // the folders, then the files
        QCOMPARE(QString::fromWCharArray(group->fgd[7 + 499].cFileName), QStringLiteral("d2\\f499.bin"));
        QCOMPARE(qint64(group->fgd[7 + 499].nFileSizeLow), qint64(499 * 3));
    }

private:
    static QByteArray readFile(const QString &path)
    {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }
    static void move(IStream *stream, qint64 to)
    {
        LARGE_INTEGER at = {};
        at.QuadPart = to;
        stream->Seek(at, STREAM_SEEK_SET, nullptr);
    }
};

int main(int argc, char **argv)
{
    if (argc >= 4 && qstrcmp(argv[1], "--child") == 0)
        return runChild(argv[2], argv[3]);
    QCoreApplication app(argc, argv);
    TestVirtualStream test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_virtualstream.moc"
