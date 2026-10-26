#pragma once

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>
#include <cmath>
#include <optional>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace CpuTemperature {
struct Reading { double celsius; QString sensor; QString provider; int priority; };

// Only CPU sensors are admitted. Prefer a package/die sensor, falling back
// to the hottest core. Never substitute a motherboard, GPU or ACPI zone.
inline std::optional<Reading> select(const QJsonArray &sensors, const QString &provider)
{
    std::optional<Reading> best;
    const QRegularExpression cpuId(QStringLiteral("^/(intelcpu|amdcpu|cpu)/"));
    for (const auto &entry : sensors) {
        const auto s = entry.toObject();
        if (!s["Value"].isDouble()) continue;
        const double value = s["Value"].toDouble();
        if (!std::isfinite(value) || value <= 0 || value > 150) continue;
        if (!cpuId.match(s["Identifier"].toString()).hasMatch()) continue;
        const QString name = s["Name"].toString().trimmed().left(128);
        int priority = 0;
        if (name.contains("package", Qt::CaseInsensitive)) priority = 3;
        else if (name.contains("tdie", Qt::CaseInsensitive) || name.contains("tccd", Qt::CaseInsensitive)) priority = 2;
        else if (name.contains("core", Qt::CaseInsensitive) || name.contains("tctl", Qt::CaseInsensitive)) priority = 1;
        if (!priority) continue;
        if (!best || priority > best->priority || (priority == best->priority && value > best->celsius))
            best = Reading{value, name, provider, priority};
    }
    return best;
}

inline QJsonArray linuxSensors(const QString &root = QStringLiteral("/sys/class/hwmon"))
{
    auto read = [](const QString &path) {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.read(256)).trimmed() : QString();
    };
    QJsonArray out;
    QDir dir(root);
    for (const QString &entry : dir.entryList({"hwmon*"}, QDir::Dirs | QDir::NoDotAndDotDot).mid(0, 64)) {
        const QString base = dir.filePath(entry);
        const QString driver = read(base + "/name");
        if (driver != "coretemp" && driver != "k10temp" && driver != "zenpower") continue;
        for (const auto &input : QDir(base).entryList({"temp*_input"}, QDir::Files).mid(0, 128)) {
            const QString stem = input.left(input.size() - 6);
            if (read(base + '/' + stem + "_fault") == "1") continue;
            bool ok = false;
            const double c = read(base + '/' + input).toDouble(&ok) / 1000.0;
            if (!ok) continue;
            out.append(QJsonObject{{"Name", read(base + '/' + stem + "_label")},
                {"Identifier", "/cpu/" + entry + '/' + stem}, {"Value", c}});
        }
    }
    return out;
}

// A persistent, read-only helper avoids launching PowerShell every two seconds.
// No driver, privilege escalation, network listener or sensor software is installed.
inline QString windowsScript()
{
    return QString::fromLatin1(R"PS(
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
function Read-CpuNodes($node) {
  if ($node.Type -eq 'Temperature' -and $node.SensorId -match '^/(intelcpu|amdcpu|cpu)/' -and $null -ne $node.RawValue) {
    @{Name=$node.Text;Identifier=$node.SensorId;Value=$node.RawValue}
  }
  foreach ($child in $node.Children) { Read-CpuNodes $child }
}
function Read-LocalMonitor {
  $request = [System.Net.HttpWebRequest]::Create('http://127.0.0.1:8085/data.json')
  $request.Proxy = $null; $request.AllowAutoRedirect = $false
  $request.Timeout = 2000; $request.ReadWriteTimeout = 2000
  $response = $null; $stream = $null
  try {
    $response = $request.GetResponse(); $stream = $response.GetResponseStream()
    $bytes = New-Object byte[] 262145; $offset = 0
    while ($offset -lt $bytes.Length) {
      $n = $stream.Read($bytes, $offset, $bytes.Length - $offset)
      if ($n -eq 0) { break }; $offset += $n
    }
    if ($offset -ge $bytes.Length) { throw 'Sensor response too large' }
    $tree = [System.Text.Encoding]::UTF8.GetString($bytes, 0, $offset) | ConvertFrom-Json
    Read-CpuNodes $tree
  } finally {
    if ($stream) { $stream.Dispose() }; if ($response) { $response.Dispose() }
  }
}
while ($true) {
  $sensors = @(); $provider = ''
  foreach ($ns in @('root\LibreHardwareMonitor','root\OpenHardwareMonitor')) {
    try {
      $sensors = @(Get-CimInstance -Namespace $ns -ClassName Sensor -Filter "SensorType='Temperature'" -OperationTimeoutSec 2 -ErrorAction Stop |
        Where-Object { $_.Identifier -match '^/(intelcpu|amdcpu|cpu)/' } |
        Select-Object Name,Identifier,Value)
      if ($sensors.Count -gt 0) { $provider = $ns; break }
    } catch { $sensors = @() }
  }
  if ($sensors.Count -eq 0) {
    try { $sensors = @(Read-LocalMonitor); if ($sensors.Count -gt 0) { $provider = 'LibreHardwareMonitor local JSON' } } catch { $sensors = @() }
  }
  @{Provider=$provider;Sensors=$sensors} | ConvertTo-Json -Compress -Depth 4
  Start-Sleep -Seconds 2
}
)PS");
}

class Monitor : public QObject {
public:
    explicit Monitor(QObject *parent = nullptr) : QObject(parent) {
        timer_.setInterval(2000);
        connect(&timer_, &QTimer::timeout, this, [this]() { poll(); });
        connect(&process_, &QProcess::readyReadStandardOutput, this, [this]() {
            buffer_ += process_.readAllStandardOutput();
            if (buffer_.size() > 65536) { process_.kill(); invalidate("Invalid sensor response"); buffer_.clear(); return; }
            int end;
            while ((end = buffer_.indexOf('\n')) >= 0) {
                const auto line = buffer_.left(end).trimmed(); buffer_.remove(0, end + 1);
                accept(line);
            }
        });
        connect(&process_, &QProcess::readyReadStandardError, this, [this]() { process_.readAllStandardError(); });
        connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
            invalidate("Temperature provider unavailable");
        });
#ifdef Q_OS_WIN
        process_.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW; });
#endif
    }
    ~Monitor() override {
        timer_.stop();
        if (process_.state() != QProcess::NotRunning) { process_.kill(); process_.waitForFinished(1000); }
    }
    void start() { if (!timer_.isActive()) { poll(); timer_.start(); } }
    void accept(const QByteArray &line) {
        QJsonParseError error;
        const auto doc = QJsonDocument::fromJson(line, &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject() || !doc.object()["Sensors"].isArray()) {
            invalidate("Invalid sensor response"); return;
        }
        responseAge_.restart();
        reading_ = select(doc.object()["Sensors"].toArray(), doc.object()["Provider"].toString().left(128));
        if (reading_) { age_.restart(); detail_.clear(); }
        else invalidate("No CPU temperature sensor available");
    }
    QJsonObject snapshot(qint64 maxAgeMs = 15000) const {
        const bool stale = reading_ && (!age_.isValid() || age_.elapsed() > maxAgeMs);
        const bool available = reading_ && !stale;
        QJsonObject out{{"cpuTemperatureStatus", available ? "ok" : stale ? "stale" : "unavailable"},
            {"cpuTemperatureC", available ? QJsonValue(reading_->celsius) : QJsonValue(QJsonValue::Null)},
            {"cpuTemperatureDetail", stale ? "Sensor readings stopped updating" : detail_}};
        if (available) {
            out["cpuTemperatureSensor"] = reading_->sensor;
            out["cpuTemperatureProvider"] = reading_->provider;
            out["cpuTemperatureAgeMs"] = double(age_.elapsed());
        }
        return out;
    }
private:
    void invalidate(const QString &detail) { reading_.reset(); detail_ = detail; }
    void poll() {
#if defined(Q_OS_WIN)
        if (process_.state() != QProcess::NotRunning) {
            if (responseAge_.isValid() && responseAge_.elapsed() > 20000) {
                process_.kill(); invalidate("Temperature provider timed out");
            }
            return;
        }
        if (retry_.isValid() && retry_.elapsed() < 30000) return;
        retry_.restart(); responseAge_.restart(); buffer_.clear();
        const QString windows = qEnvironmentVariable("SystemRoot", QStringLiteral("C:/Windows"));
        process_.start(windows + "/System32/WindowsPowerShell/v1.0/powershell.exe",
            {"-NoLogo", "-NoProfile", "-NonInteractive", "-Command", windowsScript()});
#elif defined(Q_OS_LINUX)
        reading_ = select(linuxSensors(), QStringLiteral("Linux hwmon"));
        if (reading_) { age_.restart(); detail_.clear(); }
        else invalidate("No supported CPU temperature sensor available");
#else
        invalidate("CPU temperature provider is not available on this platform");
#endif
    }
    QTimer timer_;
    QProcess process_;
    QByteArray buffer_;
    QElapsedTimer age_, responseAge_, retry_;
    std::optional<Reading> reading_;
    QString detail_ = QStringLiteral("Waiting for CPU temperature sensor");
};
} // namespace CpuTemperature
