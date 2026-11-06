// Unit test for the Windows launcher's pure helpers (launcher/src):
// LaunchConfig::toArguments() must produce exactly the wfweb command line a
// user would type, and LogParser must recognise the strings the server
// really logs. Built and run by tests/test_cpp_units.py (Qt5Core only).
#include "../launcher/src/launchconfig.h"
#include "../launcher/src/logparser.h"
#include "../launcher/src/portowner.h"

#include <QDebug>
#include <QSettings>
#include <QTemporaryDir>
#include <cstdlib>

namespace {
void require(bool condition, const char *message)
{
    if (!condition) { qCritical() << "FAIL:" << message; ::exit(1); }
}

int indexOfFlag(const QStringList &args, const QString &flag)
{
    return args.indexOf(flag);
}

// The value following a flag, or an empty string when the flag is absent.
QString valueOf(const QStringList &args, const QString &flag)
{
    const int i = indexOfFlag(args, flag);
    return (i >= 0 && i + 1 < args.size()) ? args.at(i + 1) : QString();
}

LogEvent classified(const char *message)
{
    return LogParser::classify(QString::fromUtf8(message));
}
}

int main()
{
    const QString log = "C:/Users/me/AppData/Local/wfweb/launcher/wfweb-server.log";

    // --- toArguments -----------------------------------------------------

    // FlexRadio on the LAN, with an amplifier and on-demand.
    {
        LaunchConfig c;
        c.transport = LaunchConfig::FlexLan;
        c.flexHost = " 10.0.0.162 ";
        c.setAmpHost("10.0.0.25");
        c.onDemandSeconds = 30;
        const QStringList args = c.toArguments(log);
        require(valueOf(args, "--manufacturer") == "3", "Flex is manufacturer 3");
        require(valueOf(args, "--lan") == "10.0.0.162", "Flex address is trimmed");
        require(valueOf(args, "--rfkit") == "10.0.0.25", "--rfkit carries the amplifier");
        require(valueOf(args, "--on-demand") == "30", "--on-demand carries the idle time");
        require(indexOfFlag(args, "--flex-audio") < 0, "auto audio adds no flag");
        require(indexOfFlag(args, "--smartlink") < 0, "no SmartLink on the LAN");
        c.flexHost.clear();
        require(valueOf(c.toArguments(log), "--lan") == "auto", "empty Flex address = discovery");
    }

    // SmartLink: radio, audio choice, REST to the LAN.
    {
        LaunchConfig c;
        c.transport = LaunchConfig::SmartLink;
        c.smartLinkRadio = "1234-5678";
        c.flexAudio = "none";
        c.restLan = true;
        const QStringList args = c.toArguments(log);
        require(valueOf(args, "--smartlink") == "1234-5678", "--smartlink carries the radio");
        require(valueOf(args, "--flex-audio") == "none", "--flex-audio when not auto");
        require(indexOfFlag(args, "--rest") >= 0, "--rest when asked");
        require(indexOfFlag(args, "--guest-view") < 0, "no --guest-view unless asked");
        require(indexOfFlag(args, "--lan") < 0, "no --lan for SmartLink");
        require(indexOfFlag(args, "--manufacturer") < 0, "--smartlink implies the Flex");
    }

    // On-demand is for network rigs only.
    {
        LaunchConfig c;
        c.onDemandSeconds = 30;
        require(indexOfFlag(c.toArguments(log), "--on-demand") < 0, "no --on-demand over USB");
    }

    // USB with everything at its default: only the launcher's own flags.
    {
        LaunchConfig c;
        const QStringList args = c.toArguments(log);
        require(args == QStringList{"--no-tui", "-l", log},
                "USB/auto with defaults adds nothing beyond --no-tui and -l");
    }

    // USB with an explicit port, manufacturer, CI-V address and audio device.
    {
        LaunchConfig c;
        c.serialPort = "COM7";
        c.manufacturer = LaunchConfig::Kenwood;
        c.civAddress = " 0x94 ";
        c.audioDevice = "USB Audio CODEC";
        const QStringList args = c.toArguments(log);
        require(valueOf(args, "--serial-port") == "COM7", "--serial-port carries the port");
        require(valueOf(args, "--manufacturer") == "1", "Kenwood is manufacturer 1");
        require(valueOf(args, "--civ") == "0x94", "--civ is trimmed");
        require(valueOf(args, "--audio-device") == "USB Audio CODEC", "device name stays one argument");
        require(indexOfFlag(args, "--lan") < 0, "no LAN flags in USB mode");
        require(indexOfFlag(args, "-p") < 0, "default web port needs no -p");
    }

    // Icom is the server default: no --manufacturer.
    {
        LaunchConfig c;
        c.manufacturer = LaunchConfig::Icom;
        require(indexOfFlag(c.toArguments(log), "--manufacturer") < 0, "Icom adds no --manufacturer");
    }

    // The amplifier and the name tag belong to the kind of radio: the Flex's
    // RF2K-S and "Flex 6400" do not follow an Icom LAN start (user,
    // 2026-11-06), and each comes back with its own radio.
    {
        LaunchConfig c;
        c.transport = LaunchConfig::FlexLan;
        c.setAmpHost("10.0.0.25");
        c.setNameTag("Flex 6400");
        c.transport = LaunchConfig::Lan;
        c.lanHost = "100.74.78.87";
        QStringList args = c.toArguments(log);
        require(indexOfFlag(args, "--rfkit") < 0, "the Flex's amplifier is not passed for an Icom LAN rig");
        require(indexOfFlag(args, "-n") < 0, "the Flex's name is not passed for an Icom LAN rig");
        c.setNameTag("IC-7300");
        require(valueOf(c.toArguments(log), "-n") == "IC-7300", "the Icom LAN rig has its own name");
        c.transport = LaunchConfig::SmartLink;
        args = c.toArguments(log);
        require(valueOf(args, "--rfkit") == "10.0.0.25", "SmartLink shares the Flex's amplifier");
        require(valueOf(args, "-n") == "Flex 6400", "SmartLink shares the Flex's name");

        QTemporaryDir dir;
        QSettings out(dir.filePath("l.ini"), QSettings::IniFormat);
        c.save(out);
        out.sync();
        QSettings in(dir.filePath("l.ini"), QSettings::IniFormat);
        LaunchConfig d;
        d.load(in);
        require(d.ampHosts[LaunchConfig::GroupFlex] == "10.0.0.25" && d.ampHosts[LaunchConfig::GroupIcomLan].isEmpty(),
                "amplifiers saved per radio kind");
        require(d.nameTags[LaunchConfig::GroupIcomLan] == "IC-7300" && d.nameTags[LaunchConfig::GroupFlex] == "Flex 6400",
                "names saved per radio kind");

        // Settings from before the split: one amp, one name.
        QSettings old(dir.filePath("old.ini"), QSettings::IniFormat);
        old.setValue("transport", int(LaunchConfig::Lan));
        old.setValue("ampHost", "10.0.0.25");
        old.setValue("nameTag", "Flex 6400");
        old.sync();
        LaunchConfig e;
        e.load(old);
        require(e.ampHosts[LaunchConfig::GroupFlex] == "10.0.0.25" && e.ampHost().isEmpty(),
                "an old amplifier moves to the Flex, not to the Icom LAN rig in use");
        require(e.nameTags[LaunchConfig::GroupFlex] == "Flex 6400" && e.nameTag().isEmpty(),
                "an old name that says Flex moves to the Flex");
    }

    // Punch list 73: one server, several radios. The Flex and each Icom LAN
    // radio become --radio profiles with their own amp and name; no single
    // transport flags, no --rfkit / -n of their own.
    {
        LaunchConfig c;
        c.multiRadio = true;
        c.transport = LaunchConfig::FlexLan;
        c.flexHost = "auto";
        c.setAmpHost("10.0.0.25");
        c.setNameTag("Flex 6400");
        c.lanHost = "100.74.78.87";
        c.lanUser = "va3nsv";
        c.lanPassword = "p;w";
        c.lanRadio = "IC-7300, IC-9700";
        c.lanBufferMs = 300;
        c.ampHosts[LaunchConfig::GroupIcomLan] = "10.0.0.25";   // typed while Icom LAN was the transport
        const QStringList args = c.toArguments(log);
        QStringList radios;
        for (int i = 0; i + 1 < args.size(); ++i) if (args[i] == "--radio") radios << args[i + 1];
        require(radios.size() == 3, "three --radio profiles (Flex, IC-7300, IC-9700)");
        require(radios.value(0).contains("name=Flex 6400") && radios.value(0).contains("type=flex")
                && radios.value(0).contains("amp=10.0.0.25"), "the Flex with its amp");
        require(radios.value(1).contains("name=IC-7300") && radios.value(1).contains("radio=IC-7300")
                && radios.value(1).contains("type=icomlan") && !radios.value(1).contains("amp="),
                "the IC-7300 without the Flex's amp");
        require(radios.value(1).contains("pass=p\\;w"), "a ; in the password escaped");
        require(radios.value(2).contains("name=IC-9700") && radios.value(2).contains("buffer=300"), "the IC-9700");
        require(indexOfFlag(args, "--lan") < 0 && indexOfFlag(args, "--manufacturer") < 0
                && indexOfFlag(args, "--rfkit") < 0 && indexOfFlag(args, "-n") < 0,
                "no single-radio flags with the RADIO menu");
    }

    // LAN: host and credentials, and the USB fields are ignored even if set.
    {
        LaunchConfig c;
        c.transport = LaunchConfig::Lan;
        c.lanHost = "192.168.1.50 ";
        c.lanUser = "alain";
        c.lanPassword = "secret";
        c.lanRadio = " 7300 ";
        c.lanBufferMs = 300;
        c.noAutoConnect = true;
        c.serialPort = "COM7";
        c.audioDevice = "USB Audio CODEC";
        const QStringList args = c.toArguments(log);
        require(valueOf(args, "--lan") == "192.168.1.50", "--lan carries the trimmed host");
        require(valueOf(args, "--lan-user") == "alain", "--lan-user");
        require(valueOf(args, "--lan-pass") == "secret", "--lan-pass");
        require(valueOf(args, "--lan-radio") == "7300", "--lan-radio carries the trimmed radio");
        require(valueOf(args, "--lan-buffer") == "300", "--lan-buffer for a WAN link");
        require(indexOfFlag(args, "--no-autoconnect") >= 0, "--no-autoconnect in LAN mode");
        require(indexOfFlag(args, "--serial-port") < 0, "no serial flag in LAN mode");
        require(indexOfFlag(args, "--audio-device") < 0, "no audio flag in LAN mode");
    }

    // --no-autoconnect is LAN only.
    {
        LaunchConfig c;
        c.noAutoConnect = true;
        require(indexOfFlag(c.toArguments(log), "--no-autoconnect") < 0, "--no-autoconnect ignored for USB");
    }

    // Server fields and extra arguments.
    {
        LaunchConfig c;
        c.webPort = 8090;
        c.setNameTag("Shack");
        c.settingsFile = "C:/Users/me/wfweb/contest.ini";
        c.extraArguments = "--rigctld-port 4532 --remote-log \"127.0.0.1:2237\"";
        const QStringList args = c.toArguments(log);
        require(valueOf(args, "-p") == "8090", "-p for a non-default port");
        require(valueOf(args, "-n") == "Shack", "-n carries the name tag");
        require(valueOf(args, "-s") == "C:/Users/me/wfweb/contest.ini", "-s carries the settings file");
        require(valueOf(args, "--rigctld-port") == "4532", "extra arguments are appended");
        require(valueOf(args, "--remote-log") == "127.0.0.1:2237", "quoted extra arguments are unquoted");
        require(args.last() == "127.0.0.1:2237", "extra arguments come last");
    }

    // Probe URLs and browser URL.
    {
        LaunchConfig c;
        require(c.restProbeUrls() == QStringList{"http://127.0.0.1:8081/api/v1/radio/info",
                                                 "http://127.0.0.1:8080/api/v1/radio/info"},
                "REST probe tries the HTTPS-companion port first, then the web port");
        require(c.webUrl(true) == "https://localhost:8080/", "https browser URL");
        require(c.webUrl(false) == "http://localhost:8080/", "http browser URL");
    }

    // --- LogParser::parse ------------------------------------------------

    {
        LogLine l;
        require(LogParser::parse("2026-10-13 12:01:03.123 INF default: Web HTTPS server listening on port 8080", l),
                "a log record parses");
        require(l.level == LogLine::Info, "INF is Info");
        require(l.category == "default", "category before the colon");
        require(l.message == "Web HTTPS server listening on port 8080", "message after the colon");
        require(l.timestamp.isValid() && l.timestamp.time().msec() == 123, "timestamp with milliseconds");

        require(LogParser::parse("2026-10-13 12:01:03.124 WRN system: Could not find an Icom serial port.", l),
                "warning record parses");
        require(l.level == LogLine::Warning && l.category == "system", "WRN system");

        require(!LogParser::parse("Operating System: Windows 11 Version 24H2", l), "a continuation line is not a record");
        require(!LogParser::parse("", l), "an empty line is not a record");
    }

    // --- LogParser::tidy / classify -------------------------------------

    require(LogParser::tidy("\"Have new rig ID: 0x94\"") == "Have new rig ID: 0x94", "tidy strips QDebug quotes");
    require(LogParser::tidy("Opened port:  \"COM4\"") == "Opened port: \"COM4\"", "tidy squeezes double spaces");

    {
        LogEvent e = classified("Web HTTPS server listening on port 8080");
        require(e.kind == LogEvent::WebListening && e.https && e.detail == "8080", "HTTPS listening");
        e = classified("Web HTTP server listening on port 8080");
        require(e.kind == LogEvent::WebListening && !e.https && e.detail == "8080", "HTTP listening");
        e = classified("Web plain HTTP REST server listening on port 8081 (use http:// on this port for scripts/microcontrollers)");
        require(e.kind == LogEvent::Plain, "the REST listening line is not the web listening line");
        e = classified("Web HTTPS server failed to listen on port 8080");
        require(e.kind == LogEvent::WebFailed && e.detail == "8080", "HTTPS bind failure");
        e = classified("Web plain HTTP REST server failed to listen on port 8081");
        require(e.kind == LogEvent::WebFailed && e.detail == "8081", "REST bind failure");
        e = classified("Web: SSL not supported on this system, using plain HTTP");
        require(e.kind == LogEvent::WebPlainHttp, "plain HTTP fallback");

        e = classified("Opened port:  \"COM4\"");
        require(e.kind == LogEvent::SerialOpened && e.detail == "COM4", "serial opened");
        e = classified("Could not open serial port  \"COM4\"  , please check Radio Access under Settings.");
        require(e.kind == LogEvent::SerialFailed && e.detail == "COM4", "serial open failure");
        e = classified("Icom Serial Port found:  \"COM4\" S/N \"IC-7300 03001234\"");
        require(e.kind == LogEvent::SerialAutoDetected && e.detail == "COM4 (IC-7300 03001234)", "serial auto-detect");
        e = classified("Could not find an Icom serial port. Falling back to OS default. Use --serial-port to specify, or modify preferences.");
        require(e.kind == LogEvent::SerialFallback, "serial fallback");

        e = classified("\"Loading Rig: IC-7300 version 2.00 from C:/wfweb/rigs/IC-7300.rig\"");
        require(e.kind == LogEvent::RigModel && e.detail == "IC-7300", "rig model from the quoted Loading Rig line");
        e = classified("\"Loading Rig: IC-9700 version 1.10 from /usr/share/wfview/rigs/IC-9700.rig\"");
        require(e.detail == "IC-9700", "rig model with a different name");

        e = classified("icomUdpHandler : Invalid Username/Password");
        require(e.kind == LogEvent::LanLoginFailed, "LAN login failure");
        e = classified("icomUdpHandler : Radio not responding.");
        require(e.kind == LogEvent::LanFailed && e.detail == "radio not responding", "LAN not responding");
        e = classified("icomUdpHandler : Connection failed, wait a few minutes or reboot the radio.");
        require(e.kind == LogEvent::LanFailed, "LAN connection failed");
        e = classified("icomUdpHandler : Got radio disconnected.");
        require(e.kind == LogEvent::LanFailed, "LAN disconnected");

        e = classified("On-demand: no browser for 30 s, releasing the rig");
        require(e.kind == LogEvent::RigReleased, "on-demand release");
        e = classified("On-demand: browser connected, connecting to the rig");
        require(e.kind == LogEvent::RigReconnecting, "on-demand reconnect");
        e = classified("On-demand: connecting to the rig when a browser opens the page, releasing it 30 s after the last one closes");
        require(e.kind == LogEvent::RigOnDemand, "on-demand at startup");
        e = classified("On-demand: last browser closed, releasing the rig in 30 s unless one comes back");
        require(e.kind == LogEvent::Plain, "the release countdown is plain");

        e = classified("Web: No rig audio device found for direct capture");
        require(e.kind == LogEvent::AudioWarning, "audio warning (web capture)");
        e = classified("No local audio devices found for \"IC-7300\" - skipping local audio setup (LAN audio unaffected)");
        require(e.kind == LogEvent::AudioWarning, "audio warning (no local devices)");

        e = classified("Serial port overridden via CLI: \"COM4\"");
        require(e.kind == LogEvent::Note, "CLI override note");
        e = classified("Loading settings from: \"C:/Users/me/wfweb/contest.ini\"");
        require(e.kind == LogEvent::Note, "settings path note");

        e = classified("Auto Information enabled for meters");
        require(e.kind == LogEvent::Plain, "an unrelated line is plain");
        e = classified("\"wfweb version: 0.9.2 (Git:7b3b6a6 on Oct 13 2026 at 10:00:00 by build@wfweb.org)\\nOperating System: Windows 11\"");
        require(e.kind == LogEvent::Plain, "the version banner is plain");
    }

    // --- PortOwner::classify / masked ------------------------------------

    {
        const QString logPath = "C:/Users/me/AppData/Local/wfweb/launcher/wfweb-server.log";
        PortOwner o;
        o.image = "wfweb.exe";
        o.commandLine = "C:\\wfweb\\wfweb.exe --no-tui -l C:\\Users\\me\\AppData\\Local\\wfweb\\launcher\\wfweb-server.log "
                        "--lan 192.168.1.5 --lan-user u --lan-pass secret";
        o.classify(logPath);
        require(o.isWfweb, "wfweb.exe is wfweb");
        require(o.isLan, "a bare --lan is a LAN instance");
        require(o.fromLauncher, "the launcher's -l path is recognised across slash styles");
        const QString masked = PortOwner::masked(o.commandLine);
        require(!masked.contains("secret") && masked.contains("--lan-pass ****"), "the LAN password is masked");
        require(PortOwner::masked("wfweb --lan-pass \"two words\" -p 8090").contains("--lan-pass **** -p"),
                "a quoted password is masked whole");

        PortOwner console;
        console.image = "WFWEB.EXE";
        console.commandLine = "wfweb.exe --debug x.log --lan-user u";
        console.classify(logPath);
        require(console.isWfweb, "image name compares case-insensitively");
        require(!console.isLan, "--lan-user alone is not --lan");
        require(!console.fromLauncher, "no -l path means not started by the launcher");

        PortOwner other;
        other.image = "node.exe";
        other.commandLine = "node server.js";
        other.classify(logPath);
        require(!other.isWfweb, "another program is not wfweb");
        require(PortOwner::masked("no password here") == "no password here", "masking leaves other lines alone");
    }

    qInfo() << "launcher_test: all checks passed";
    return 0;
}
