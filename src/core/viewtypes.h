#pragma once

// Shared types of the remote view: which screens are shown where, and
// which ones a source captures. Used by the input controller, RemoteView
// and the service.

#include <QRect>
#include <QSize>
#include <QString>
#include <QVersionNumber>

namespace df {

// A viewed screen: index (stable per entry, 0..n-1 over the device's
// screens), its rect in the remote device's native coordinates and the local
// monitor it is shown on (native coordinates).
struct ViewScreen {
    int index = 0;
    QRect remote;
    QRect monitor;
};

// A screen the source captures: index, its rect among the source's own
// screens, the largest frame the viewer wants for it and the generation of
// its capture: a new one whenever the screen starts afresh (full frame
// first), so that frames of the capture before, still on their way from the
// helper, are told apart and dropped.
struct CaptureSpec {
    int index = 0;
    QRect screen;
    QSize maxSize;
    quint32 generation = 0;
};

// Screens displayed (and streamed, and captured by a source's helper) at once: one per local
// monitor. The input controller, RemoteView, the service and the session helper all use this.
inline constexpr int kMaxViewScreens = 8;
// Entries of a device's view plan (Workspace::setViewPlan, the agent's setViewPlan command).
inline constexpr int kMaxViewPlanEntries = 32;

// First version whose remote view streams several screens at once (wire
// protocol v2). Older peers get the single-screen protocol.
inline constexpr const char *kMultiViewVersion = "0.1.20";

inline bool peerSupportsMultiView(const QString &version)
{
    return QVersionNumber::fromString(version) >= QVersionNumber::fromString(kMultiViewVersion);
}

} // namespace df
