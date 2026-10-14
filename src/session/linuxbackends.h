#pragma once

class InputBackend;
class QObject;

// Linux display-server specific backends (selected in inputbackend_linux.cpp).
InputBackend *createX11InputBackend(QObject *parent);
#ifdef DESKFLOWER_HAVE_LIBEI
InputBackend *createWaylandInputBackend(QObject *parent);
#endif
