#include "stubinputbackend.h"

// Platforms without an input implementation.
InputBackend *InputBackend::create(QObject *parent)
{
    return new StubInputBackend("this platform is not supported yet", parent);
}
