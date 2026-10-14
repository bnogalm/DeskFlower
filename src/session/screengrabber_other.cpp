#include "screengrabber.h"

// No worker-thread capture outside Windows: the streamer uses QScreen::grabWindow (X11, macOS with
// the Screen Recording permission). Wayland would need the ScreenCast portal (not implemented).
std::unique_ptr<ScreenGrabber> ScreenGrabber::create() { return nullptr; }
