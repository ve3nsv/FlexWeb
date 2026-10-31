#pragma once
#include <QSet>
#include <QString>

inline bool guestCommandAllowed(const QString &command) {
    // Default deny: newly introduced commands cannot accidentally grant control.
    static const QSet<QString> readOnly{"getStatus", "enableAudio", "setAudioCodec", "setTxListen", "timePing", "getMemories", "netlogger"};
    return readOnly.contains(command);
}
