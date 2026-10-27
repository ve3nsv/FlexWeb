#include "serverload.h"

#include <QDateTime>
#include <QFile>
#include <QThread>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#include <sys/time.h>
#endif
#if defined(Q_OS_MACOS)
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <sys/sysctl.h>
#endif

namespace {
#if defined(Q_OS_WIN)
double fileTimeSec(const FILETIME& ft)
{
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return double(u.QuadPart) / 1e7;   // 100 ns units
}
#endif

#if defined(Q_OS_LINUX)
// kB value of "Key:   1234 kB" in a /proc file, -1 if missing
qint64 procKb(const QString& path, const QByteArray& key)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return -1;
    const QList<QByteArray> lines = f.readAll().split('\n');
    for (const QByteArray& l : lines) {
        if (l.startsWith(key)) {
            const QList<QByteArray> parts = l.mid(key.size()).simplified().split(' ');
            if (!parts.isEmpty())
                return parts.first().toLongLong();
        }
    }
    return -1;
}
#endif
}

ServerLoad::ServerLoad()
{
    startMs = QDateTime::currentMSecsSinceEpoch();
}

ServerLoad::Times ServerLoad::readTimes()
{
    Times t;
    t.wallMs = QDateTime::currentMSecsSinceEpoch();
#if defined(Q_OS_WIN)
    FILETIME c, e, k, u;
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u))
        t.procSec = fileTimeSec(k) + fileTimeSec(u);
    FILETIME idle, kern, user;
    if (GetSystemTimes(&idle, &kern, &user)) {
        // kernel time includes the idle time
        t.sysTotal = fileTimeSec(kern) + fileTimeSec(user);
        t.sysBusy = t.sysTotal - fileTimeSec(idle);
        t.ok = true;
    }
#else
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        t.procSec = double(ru.ru_utime.tv_sec) + double(ru.ru_utime.tv_usec) / 1e6
                  + double(ru.ru_stime.tv_sec) + double(ru.ru_stime.tv_usec) / 1e6;
    }
#if defined(Q_OS_LINUX)
    QFile f(QStringLiteral("/proc/stat"));
    if (f.open(QIODevice::ReadOnly)) {
        // cpu  user nice system idle iowait irq softirq steal ...
        const QList<QByteArray> v = f.readLine().simplified().split(' ');
        if (v.size() >= 5 && v[0] == "cpu") {
            double total = 0;
            for (int i = 1; i < v.size() && i <= 8; ++i)
                total += v[i].toDouble();
            const double idle = v[4].toDouble() + (v.size() > 5 ? v[5].toDouble() : 0.0);
            t.sysTotal = total;
            t.sysBusy = total - idle;
            t.ok = true;
        }
    }
#elif defined(Q_OS_MACOS)
    host_cpu_load_info_data_t info;
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, reinterpret_cast<host_info_t>(&info), &count)
        == KERN_SUCCESS) {
        const double user = info.cpu_ticks[CPU_STATE_USER], sys = info.cpu_ticks[CPU_STATE_SYSTEM];
        const double nice = info.cpu_ticks[CPU_STATE_NICE], idle = info.cpu_ticks[CPU_STATE_IDLE];
        t.sysBusy = user + sys + nice;
        t.sysTotal = t.sysBusy + idle;
        t.ok = true;
    }
#endif
#endif
    return t;
}

void ServerLoad::readMemory(QJsonObject& o)
{
    double procMb = -1, usedMb = -1, totalMb = -1;
#if defined(Q_OS_WIN)
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        procMb = double(pmc.WorkingSetSize) / 1048576.0;
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        totalMb = double(ms.ullTotalPhys) / 1048576.0;
        usedMb = double(ms.ullTotalPhys - ms.ullAvailPhys) / 1048576.0;
    }
#elif defined(Q_OS_LINUX)
    const qint64 rss = procKb(QStringLiteral("/proc/self/status"), "VmRSS:");
    if (rss >= 0)
        procMb = double(rss) / 1024.0;
    const qint64 total = procKb(QStringLiteral("/proc/meminfo"), "MemTotal:");
    const qint64 avail = procKb(QStringLiteral("/proc/meminfo"), "MemAvailable:");
    if (total > 0 && avail >= 0) {
        totalMb = double(total) / 1024.0;
        usedMb = double(total - avail) / 1024.0;
    }
#elif defined(Q_OS_MACOS)
    mach_task_basic_info_data_t ti;
    mach_msg_type_number_t tc = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&ti), &tc) == KERN_SUCCESS)
        procMb = double(ti.resident_size) / 1048576.0;
    uint64_t mem = 0;
    size_t len = sizeof(mem);
    if (sysctlbyname("hw.memsize", &mem, &len, nullptr, 0) == 0)
        totalMb = double(mem) / 1048576.0;
    vm_statistics64_data_t vm;
    mach_msg_type_number_t vc = HOST_VM_INFO64_COUNT;
    vm_size_t page = 0;
    if (host_page_size(mach_host_self(), &page) == KERN_SUCCESS
        && host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &vc)
               == KERN_SUCCESS) {
        usedMb = double(uint64_t(vm.active_count) + vm.wire_count + vm.compressor_page_count) * double(page)
                 / 1048576.0;
    }
#endif
    if (procMb >= 0)
        o[QStringLiteral("procMemMb")] = qRound(procMb);
    if (totalMb > 0) {
        o[QStringLiteral("memTotalMb")] = qRound(totalMb);
        if (usedMb >= 0)
            o[QStringLiteral("memUsedMb")] = qRound(usedMb);
    }
}

QJsonObject ServerLoad::sample()
{
    QJsonObject o;
    o[QStringLiteral("type")] = QStringLiteral("serverLoad");
    const int cores = qMax(1, QThread::idealThreadCount());
    o[QStringLiteral("cores")] = cores;
    const Times now = readTimes();
    o[QStringLiteral("uptimeS")] = int((now.wallMs - startMs) / 1000);
    if (last.wallMs > 0 && now.wallMs > last.wallMs) {
        const double wall = double(now.wallMs - last.wallMs) / 1000.0;
        const double proc = qMax(0.0, now.procSec - last.procSec);
        // % of one core, and % of the whole machine
        o[QStringLiteral("procCorePct")] = qRound(1000.0 * proc / wall) / 10.0;
        o[QStringLiteral("procPct")] = qRound(1000.0 * proc / wall / cores) / 10.0;
        if (now.ok && last.ok && now.sysTotal > last.sysTotal) {
            const double busy = (now.sysBusy - last.sysBusy) / (now.sysTotal - last.sysTotal);
            o[QStringLiteral("sysPct")] = qRound(1000.0 * qBound(0.0, busy, 1.0)) / 10.0;
        }
    }
    last = now;
    readMemory(o);
    return o;
}
