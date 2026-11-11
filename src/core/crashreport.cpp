#include "crashreport.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <exception>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dbghelp.h>
#include <crtdbg.h>
#include <stdlib.h>
#else
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#endif

// Everything a crash handler touches is prepared at install: paths are fixed-size buffers, the
// dump function is resolved beforehand, and the handlers only format into stack buffers and call
// the OS directly (no Qt, no allocation), since the heap or Qt itself may be what failed.

namespace df::crash {

namespace {
constexpr int kPathChars = 1024;
constexpr int kLineChars = 1024;

std::atomic<bool> g_installed{false};
std::atomic<bool> g_reported{false}; // a crash was written down (abort after terminate: one dump)
QtMessageHandler g_previousHandler = nullptr;

#ifdef Q_OS_WIN
wchar_t g_logPath[kPathChars];
wchar_t g_dumpPrefix[kPathChars]; // <dir>\<name>-
using MiniDumpWriteDumpFn = BOOL(WINAPI *)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                           PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
MiniDumpWriteDumpFn g_miniDumpWriteDump = nullptr;
#else
char g_logPath[kPathChars];
#endif

// Appends "<local time> <text>\n" to problems.log.
void writeLine(const char *text)
{
    char line[kLineChars];
#ifdef Q_OS_WIN
    SYSTEMTIME t;
    GetLocalTime(&t);
    const int n = _snprintf_s(line, sizeof line, _TRUNCATE, "%04u-%02u-%02u %02u:%02u:%02u.%03u %s\r\n", t.wYear,
                              t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, text);
    const HANDLE f = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(f, line, n > 0 ? DWORD(n) : DWORD(std::strlen(line)), &written, nullptr);
    CloseHandle(f);
#else
    // UTC: localtime_r may take locks; the start line says the offset.
    struct tm t;
    const time_t now = time(nullptr);
    gmtime_r(&now, &t);
    int n = std::snprintf(line, sizeof line, "%04d-%02d-%02d %02d:%02d:%02dZ %s\n", t.tm_year + 1900, t.tm_mon + 1,
                          t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, text);
    if (n < 0)
        return;
    if (n >= int(sizeof line))
        n = int(sizeof line) - 1;
    const int f = open(g_logPath, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
    if (f < 0)
        return;
    ssize_t ignored = write(f, line, size_t(n));
    (void)ignored;
    close(f);
#endif
}

#ifdef Q_OS_WIN
// Not in older SDKs' winuser.h (Windows 10 1607 and later honour it).
#ifndef UOI_TIMERPROC_EXCEPTION_SUPPRESSION
#define UOI_TIMERPROC_EXCEPTION_SUPPRESSION 7
#endif

struct DumpRequest {
    EXCEPTION_POINTERS *exception;
    DWORD threadId;
    bool describe; // write the CRASH line for `exception` first
};

// "<module>+0x<offset>" for a code address.
void describeAddress(const void *address, char *out, size_t size)
{
    HMODULE module = nullptr;
    wchar_t path[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(address), &module))
        GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t *base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    _snprintf_s(out, size, _TRUNCATE, "%ls+0x%llx", base,
                static_cast<unsigned long long>(reinterpret_cast<const char *>(address)
                                                - reinterpret_cast<const char *>(module)));
}

void describeException(const DumpRequest &request)
{
    char where[MAX_PATH + 32];
    describeAddress(request.exception->ExceptionRecord->ExceptionAddress, where, sizeof where);
    char line[kLineChars];
    _snprintf_s(line, sizeof line, _TRUNCATE, "CRASH: unhandled exception 0x%08lX at %s (thread %lu)",
                request.exception->ExceptionRecord->ExceptionCode, where, request.threadId);
    writeLine(line);
}

DWORD WINAPI dumpThread(LPVOID param)
{
    const auto *request = static_cast<const DumpRequest *>(param);
    if (request->describe)
        describeException(*request);
    if (!g_miniDumpWriteDump)
        return 1;
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t path[kPathChars];
    _snwprintf_s(path, kPathChars, _TRUNCATE, L"%s%04u%02u%02u-%02u%02u%02u.dmp", g_dumpPrefix, t.wYear, t.wMonth,
                 t.wDay, t.wHour, t.wMinute, t.wSecond);
    const HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return 1;
    MINIDUMP_EXCEPTION_INFORMATION info{request->threadId, request->exception, FALSE};
    // Stacks, threads and modules: enough for a stack trace, without the heap (which may hold
    // clipboard contents or typed text).
    const auto type = MINIDUMP_TYPE(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
    const BOOL ok = g_miniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, type,
                                        request->exception ? &info : nullptr, nullptr, nullptr);
    CloseHandle(f);
    if (!ok)
        DeleteFileW(path);
    return ok ? 0 : 1;
}

// Written from a thread of its own, as is the CRASH line of an exception (describe): the
// crashing one may have no stack left (stack overflow), and formatting takes a few KB.
void writeDump(EXCEPTION_POINTERS *exception, bool describe = false)
{
    DumpRequest request{exception, GetCurrentThreadId(), describe};
    const HANDLE thread = CreateThread(nullptr, 256 * 1024, dumpThread, &request, 0, nullptr);
    if (!thread) {
        if (describe)
            describeException(request); // on this stack then, as the last resort
        return;
    }
    WaitForSingleObject(thread, 60000);
    CloseHandle(thread);
}

LONG WINAPI onUnhandledException(EXCEPTION_POINTERS *exception)
{
    // Nothing on this stack but starting the thread: see writeDump.
    if (!g_reported.exchange(true))
        writeDump(exception, true);
    return EXCEPTION_CONTINUE_SEARCH; // Windows Error Reporting still records it
}

// The CRT ends the process on these without passing through the exception filter (fast fail).
void onInvalidParameter(const wchar_t *, const wchar_t *, const wchar_t *, unsigned, uintptr_t)
{
    if (!g_reported.exchange(true)) {
        writeLine("CRASH: invalid parameter passed to a C runtime function");
        writeDump(nullptr);
    }
    std::abort();
}

void onPureCall()
{
    if (!g_reported.exchange(true)) {
        writeLine("CRASH: pure virtual function call");
        writeDump(nullptr);
    }
    std::abort();
}
#endif

extern "C" void onAbort(int)
{
    if (!g_reported.exchange(true)) {
        writeLine("CRASH: abort");
#ifdef Q_OS_WIN
        writeDump(nullptr);
#endif
    }
#ifndef Q_OS_WIN
    std::signal(SIGABRT, SIG_DFL);
    std::raise(SIGABRT);
#endif
    // Windows: the CRT goes on ending the process (exit code 3).
}

#ifndef Q_OS_WIN
extern "C" void onFatalSignal(int sig)
{
    if (!g_reported.exchange(true)) {
        char line[64];
        std::snprintf(line, sizeof line, "CRASH: signal %d", sig);
        writeLine(line);
    }
    // SA_RESETHAND restored the default action: raising again ends the process as it would have.
    std::raise(sig);
}
#endif

void onTerminate()
{
    if (!g_reported.exchange(true)) {
        char line[kLineChars];
        const char *what = "no exception";
        std::exception_ptr current = std::current_exception();
        // what() of the exception being handled, if any (the usual cause: one escaped a slot).
        if (current) {
            what = "unknown exception";
            try {
                std::rethrow_exception(current);
            } catch (const std::exception &e) {
                std::snprintf(line, sizeof line, "CRASH: std::terminate: %s", e.what());
                what = nullptr;
            } catch (...) {
            }
        }
        if (what)
            std::snprintf(line, sizeof line, "CRASH: std::terminate (%s)", what);
        writeLine(line);
#ifdef Q_OS_WIN
        writeDump(nullptr);
#endif
    }
    std::abort();
}

void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (type == QtFatalMsg && !g_reported.exchange(true)) {
        writeLine(("FATAL: " + message.left(600)).toUtf8().constData());
#ifdef Q_OS_WIN
        writeDump(nullptr);
#endif
    }
    if (g_previousHandler)
        g_previousHandler(type, context, message);
}

#ifdef Q_OS_WIN
void pruneDumps(const QString &dir, const QString &name)
{
    // The newest kMaxDumps - 1 stay, so that the next crash makes kMaxDumps.
    const QFileInfoList dumps = QDir(dir).entryInfoList({name + QStringLiteral("-*.dmp")}, QDir::Files, QDir::Time);
    for (qsizetype i = kMaxDumps - 1; i < dumps.size(); ++i)
        QFile::remove(dumps.at(i).filePath());
}
#endif
} // namespace

QString problemsLogPath(const QString &dir) { return QDir(dir).filePath(QStringLiteral("problems.log")); }

void install(const QString &dir, const QString &name)
{
    if (g_installed.exchange(true))
        return;
    QDir().mkpath(dir);
    const QString log = problemsLogPath(dir);
    if (QFileInfo(log).size() > kMaxLogBytes) {
        QFile::remove(log + QStringLiteral(".1"));
        QFile::rename(log, log + QStringLiteral(".1"));
    }
    const QString nativeLog = QDir::toNativeSeparators(log);
#ifdef Q_OS_WIN
    nativeLog.left(kPathChars - 1).toWCharArray(g_logPath);
    const QString prefix = QDir::toNativeSeparators(QDir(dir).filePath(name + QLatin1Char('-')));
    prefix.left(kPathChars - 32).toWCharArray(g_dumpPrefix);
    pruneDumps(dir, name);
    // Loaded now: a crash is no time to load a library.
    if (HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
        g_miniDumpWriteDump = reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
    SetUnhandledExceptionFilter(onUnhandledException);
    // DispatchMessage wraps timer callbacks (SetTimer with a TIMERPROC, as Windows components and
    // libraries loaded in the process use) in an exception handler that swallows everything: a
    // crash there would leave the process going on with corrupt state, and nothing recorded.
    // Turned off, such an exception reaches the filter above like any other. (A crash in a slot
    // run by a QTimer reaches it either way: tst_crash checks both.)
    BOOL suppress = FALSE;
    SetUserObjectInformationW(GetCurrentProcess(), UOI_TIMERPROC_EXCEPTION_SUPPRESSION, &suppress,
                              sizeof suppress);
    // Room for the filter on this thread (the event loop's) once it has overflowed its stack.
    ULONG guarantee = 32 * 1024;
    SetThreadStackGuarantee(&guarantee);
    _set_invalid_parameter_handler(onInvalidParameter);
    _set_purecall_handler(onPureCall);
    // No message box on abort (a service has nobody to click it; the process would hang).
    _set_abort_behavior(0, _WRITE_ABORT_MSG);
#ifdef _DEBUG
    // Debug runtimes report qFatal and asserts in a dialog the same way.
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_DEBUG);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG);
#endif
#else
    const QByteArray encoded = QFile::encodeName(nativeLog).left(kPathChars - 1);
    std::memcpy(g_logPath, encoded.constData(), size_t(encoded.size()));
    g_logPath[encoded.size()] = '\0';
    // Their own stack, so that a stack overflow can still be written down.
    static char altStack[64 * 1024];
    stack_t ss{};
    ss.ss_sp = altStack;
    ss.ss_size = sizeof altStack;
    sigaltstack(&ss, nullptr);
    struct sigaction sa{};
    sa.sa_handler = onFatalSignal;
    sa.sa_flags = SA_RESETHAND | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE})
        sigaction(sig, &sa, nullptr);
#endif
    std::signal(SIGABRT, onAbort);
    std::set_terminate(onTerminate);
    g_previousHandler = qInstallMessageHandler(messageHandler);

    const QByteArray start = QStringLiteral("start %1 %2 (pid %3, local time is UTC%4%5 min)")
                                 .arg(name, QCoreApplication::applicationVersion())
                                 .arg(QCoreApplication::applicationPid())
                                 .arg(QDateTime::currentDateTime().offsetFromUtc() < 0 ? QString() : QStringLiteral("+"))
                                 .arg(QDateTime::currentDateTime().offsetFromUtc() / 60)
                                 .toUtf8();
    writeLine(start.constData());
}

void markCleanExit(int code)
{
    if (!g_installed)
        return;
    char line[64];
    std::snprintf(line, sizeof line, "exit %d", code);
    writeLine(line);
}

} // namespace df::crash
