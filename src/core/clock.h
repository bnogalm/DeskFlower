#pragma once

#include <QElapsedTimer>

namespace df {

// Monotonic milliseconds since process start.
inline qint64 nowMs()
{
    static const QElapsedTimer timer = [] {
        QElapsedTimer t;
        t.start();
        return t;
    }();
    return timer.elapsed();
}

} // namespace df
