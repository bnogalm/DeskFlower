#include "stubinputbackend.h"

#include <QCoreApplication>

// Platforms without an input implementation.
InputBackend *InputBackend::create(QObject *parent)
{
    return new StubInputBackend(
        QCoreApplication::translate("InputBackend", "Keyboard and mouse sharing is not supported on this platform yet"),
        parent);
}
