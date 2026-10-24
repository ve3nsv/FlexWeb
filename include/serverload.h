#pragma once

// Sample deltas rather than lifetime averages. Percentages use total machine
// capacity; processCorePercent is also supplied for comparison with one core.
#include <QElapsedTimer>
#include <QFile>
#include <QJsonObject>
#include <QThread>
#include <cmath>
#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#ifdef Q_OS_MACOS
#include <mach/mach.h>
#endif
#endif

class ServerLoad {
public:
    QJsonObject sample() {
        double process = 0, busy = 0, total = 0;
        qint64 memory = 0;
#ifdef Q_OS_WIN
        auto seconds = [](const FILETIME &f) {
            ULARGE_INTEGER n; n.LowPart = f.dwLowDateTime; n.HighPart = f.dwHighDateTime;
            return double(n.QuadPart) / 1e7;
        };
        FILETIME created{}, exited{}, kernel{}, user{}, idle{}, sysKernel{}, sysUser{};
        if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)
            || !GetSystemTimes(&idle, &sysKernel, &sysUser)) return {};
        process = seconds(kernel) + seconds(user);
        total = seconds(sysKernel) + seconds(sysUser);
        busy = total - seconds(idle);
        // K32 is available on the supported Windows versions; dynamic lookup
        // avoids a new linker dependency on PSAPI for other platforms.
        using MemoryFn = BOOL (WINAPI *)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
        auto readMemory = reinterpret_cast<MemoryFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32GetProcessMemoryInfo"));
        PROCESS_MEMORY_COUNTERS counters{};
        if (readMemory && readMemory(GetCurrentProcess(), &counters, sizeof(counters)))
            memory = qint64(counters.WorkingSetSize);
#else
        rusage usage{};
        if (getrusage(RUSAGE_SELF, &usage) != 0) return {};
        process = usage.ru_utime.tv_sec + usage.ru_stime.tv_sec
                + (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
#ifdef Q_OS_MACOS
        host_cpu_load_info_data_t cpu{};
        mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
        const mach_port_t host = mach_host_self();
        const auto result = host_statistics(host, HOST_CPU_LOAD_INFO, reinterpret_cast<host_info_t>(&cpu), &count);
        mach_port_deallocate(mach_task_self(), host);
        if (result != KERN_SUCCESS) return {};
        busy = double(cpu.cpu_ticks[CPU_STATE_USER]) + cpu.cpu_ticks[CPU_STATE_SYSTEM] + cpu.cpu_ticks[CPU_STATE_NICE];
        total = busy + cpu.cpu_ticks[CPU_STATE_IDLE];
        mach_task_basic_info_data_t task{};
        count = MACH_TASK_BASIC_INFO_COUNT;
        if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&task), &count) == KERN_SUCCESS)
            memory = qint64(task.resident_size);
#else
        QFile stat(QStringLiteral("/proc/stat"));
        if (!stat.open(QIODevice::ReadOnly)) return {};
        const auto fields = stat.readLine().simplified().split(' ');
        if (fields.size() < 9 || fields[0] != "cpu") return {};
        // guest and guest_nice already count in user/nice. Do not double count.
        for (int i = 1; i <= 8; ++i) total += fields[i].toDouble();
        busy = total - fields[4].toDouble() - fields[5].toDouble();
        QFile mem(QStringLiteral("/proc/self/statm"));
        if (mem.open(QIODevice::ReadOnly)) {
            const auto pages = mem.readAll().simplified().split(' ');
            if (pages.size() > 1) memory = pages[1].toLongLong() * sysconf(_SC_PAGESIZE);
        }
#endif
#endif
        const double now = clock_.isValid() ? clock_.nsecsElapsed() / 1e9 : 0;
        const int cores = qMax(1, QThread::idealThreadCount());
        QJsonObject out;
        if (ready_ && now > lastTime_ && total > lastTotal_ && process >= lastProcess_) {
            const double core = qBound(0.0, (process - lastProcess_) / (now - lastTime_) * 100, 100.0 * cores);
            out["type"] = "serverLoad";
            out["processCorePercent"] = core;
            out["processPercent"] = core / cores;
            out["systemPercent"] = qBound(0.0, (busy - lastBusy_) / (total - lastTotal_) * 100, 100.0);
            out["cores"] = cores;
            if (memory > 0) out["processMemoryBytes"] = double(memory);
        }
        if (!clock_.isValid()) clock_.start();
        lastTime_ = now; lastTotal_ = total; lastBusy_ = busy; lastProcess_ = process; ready_ = true;
        return out;
    }
private:
    QElapsedTimer clock_;
    bool ready_ = false;
    double lastTime_ = 0, lastTotal_ = 0, lastBusy_ = 0, lastProcess_ = 0;
};
