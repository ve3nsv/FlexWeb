#ifndef SERVERLOAD_H
#define SERVERLOAD_H

// CPU and memory use of the FlexWeb process and of the whole machine, for the
// CPU chip in the page's top bar (punch list 17). Pure Qt5Core, no QObject:
// webServer samples it on a timer and sends {type:'serverLoad'}.

#include <QJsonObject>
#include <QtGlobal>

class ServerLoad {
public:
    ServerLoad();
    // Takes a reading and returns the use since the previous call (the first
    // call has nothing to compare with and leaves the CPU fields out).
    QJsonObject sample();

private:
    struct Times {
        bool ok = false;
        double procSec = 0;     // this process, user + kernel, seconds
        double sysBusy = 0;     // machine, busy time (any unit, same as total)
        double sysTotal = 0;    // machine, busy + idle
        qint64 wallMs = 0;
    };
    static Times readTimes();
    static void readMemory(QJsonObject& o);
    Times last;
    qint64 startMs = 0;
};

#endif
