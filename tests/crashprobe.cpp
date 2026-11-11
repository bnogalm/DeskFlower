// Helper for tst_crash: installs the crash diagnostics in <dir> and ends the way it is told.
//   crashprobe <dir> exit|fatal|terminate|segv|segv-in-loop|segv-in-timerproc|stackoverflow
// segv-in-loop crashes in a slot run by a QTimer, inside the event loop. segv-in-timerproc
// (Windows) crashes in a Win32 timer callback that the event loop dispatches: DispatchMessage
// swallows exceptions raised there unless told not to. The others crash before the loop starts.

#include "crashreport.h"

#include <QCoreApplication>
#include <QTimer>

#include <cstring>
#include <exception>
#include <stdexcept>

#ifdef Q_OS_WIN
#include <windows.h>
#include <stdlib.h>
#endif

namespace {
// Through a volatile pointer, so that the compiler cannot prove the write away.
void writeThroughNull()
{
    volatile int *volatile p = nullptr;
    *p = 42;
}

#ifdef Q_OS_WIN
void CALLBACK crashingTimerProc(HWND, UINT, UINT_PTR, DWORD) { writeThroughNull(); }
#endif

// Deep enough to overflow any thread stack; the volatile buffer and the use of the result keep
// the compiler from turning it into a loop.
int recurse(int depth)
{
    volatile char buffer[4096];
    buffer[0] = char(depth);
    if (depth < 0)
        return buffer[0];
    return recurse(depth + 1) + buffer[0];
}
} // namespace

int main(int argc, char *argv[])
{
#ifdef Q_OS_WIN
    // No Windows Error Reporting window for the crashes this probe makes on purpose.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _CALL_REPORTFAULT);
#endif
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationVersion(QStringLiteral("9.9.9"));
    if (argc < 3)
        return 2;
    df::crash::install(QString::fromLocal8Bit(argv[1]), QStringLiteral("probe"));
    const char *mode = argv[2];
    if (std::strcmp(mode, "fatal") == 0)
        qFatal("probe fatal message");
    if (std::strcmp(mode, "segv") == 0)
        writeThroughNull();
    if (std::strcmp(mode, "stackoverflow") == 0)
        return recurse(0);
    const bool timerProc = std::strcmp(mode, "segv-in-timerproc") == 0;
    if (std::strcmp(mode, "segv-in-loop") == 0 || timerProc) {
#ifdef Q_OS_WIN
        if (timerProc)
            SetTimer(nullptr, 0, 50, crashingTimerProc);
        else
#endif
            // A real timer (not a zero one, which Qt may run without a native timer).
            QTimer::singleShot(50, &app, [] { writeThroughNull(); });
        const int rc = app.exec();
        df::crash::markCleanExit(rc);
        return rc;
    }
    if (std::strcmp(mode, "terminate") == 0) {
        // As when an exception escapes: terminate while it is being handled.
        try {
            throw std::runtime_error("probe exception");
        } catch (...) {
            std::terminate();
        }
    }
    QTimer::singleShot(0, &app, [] { QCoreApplication::exit(0); });
    const int rc = app.exec();
    df::crash::markCleanExit(rc);
    return rc;
}
