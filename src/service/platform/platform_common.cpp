#include "platform.h"

namespace df::platform {

SessionEvents *SessionEvents::instance()
{
    static SessionEvents events;
    return &events;
}

} // namespace df::platform
