#include "launchconfig.h"
#include "../../include/radioprofile.h"   // header-only, Qt5Core (punch list 73)

#include <QProcess>
#include <QSettings>

QStringList LaunchConfig::toArguments(const QString &logFile) const
{
    QStringList args;
    // A pipe on stdin already disables the status page; the flag makes the
    // behaviour independent of how the handles were inherited.
    args << "--no-tui" << "-l" << logFile;

    if (multiRadio) {
        for (const QString &p : radioProfiles()) args << "--radio" << p;
    } else if (transport == Lan) {
        args << "--lan" << lanHost.trimmed();
        if (!lanUser.isEmpty()) args << "--lan-user" << lanUser;
        if (!lanPassword.isEmpty()) args << "--lan-pass" << lanPassword;
        if (!lanRadio.trimmed().isEmpty()) args << "--lan-radio" << lanRadio.trimmed();
        if (lanBufferMs > 0) args << "--lan-buffer" << QString::number(lanBufferMs);
        if (noAutoConnect) args << "--no-autoconnect";
    } else if (transport == FlexLan) {
        const QString host = flexHost.trimmed();
        args << "--manufacturer" << "3" << "--lan" << (host.isEmpty() ? QString("auto") : host);
    } else if (transport == SmartLink) {
        const QString radio = smartLinkRadio.trimmed();
        args << "--smartlink" << (radio.isEmpty() ? QString("auto") : radio);
    } else {
        // Nothing for the serial port when empty: the server's own "auto"
        // looks for an Icom by USB serial number.
        if (!serialPort.isEmpty()) args << "--serial-port" << serialPort;
        if (manufacturer != Icom) args << "--manufacturer" << QString::number(int(manufacturer));
        if (!civAddress.trimmed().isEmpty()) args << "--civ" << civAddress.trimmed();
        if (!audioDevice.isEmpty()) args << "--audio-device" << audioDevice;
    }

    if ((multiRadio || transport == FlexLan || transport == SmartLink) && (flexAudio == "opus" || flexAudio == "none"))
        args << "--flex-audio" << flexAudio;
    if (!multiRadio && !ampHost().trimmed().isEmpty()) args << "--rfkit" << ampHost().trimmed();
    if (isNetwork() && onDemandSeconds > 0) args << "--on-demand" << QString::number(onDemandSeconds);
    if (restLan) args << "--rest";
    if (guestView) args << "--guest-view";
    if (guestView && guestTune) args << "--guest-tune";
    if (allowRequests) args << "--allow-requests";
    if (!ntfyTopic.trimmed().isEmpty()) args << "--ntfy-topic" << ntfyTopic.trimmed();

    if (webPort > 0 && webPort != defaultWebPort) args << "-p" << QString::number(webPort);
    if (!multiRadio && !nameTag().trimmed().isEmpty()) args << "-n" << nameTag().trimmed();
    if (!settingsFile.isEmpty()) args << "-s" << settingsFile;

    args << QProcess::splitCommand(extraArguments);
    return args;
}

QStringList LaunchConfig::radioProfiles() const
{
    QStringList out;
    // The Flex: SmartLink when that is the chosen transport, else the LAN.
    {
        radioProfile f;
        f.name = nameTags[GroupFlex].trimmed();
        if (f.name.isEmpty()) f.name = QStringLiteral("FlexRadio");
        if (transport == SmartLink) {
            f.type = QStringLiteral("smartlink");
            f.host = smartLinkRadio.trimmed().isEmpty() ? QStringLiteral("auto") : smartLinkRadio.trimmed();
        } else {
            f.type = QStringLiteral("flex");
            f.host = flexHost.trimmed().isEmpty() ? QStringLiteral("auto") : flexHost.trimmed();
        }
        f.amp = ampHosts[GroupFlex].trimmed();
        out << f.toSpec();
    }
    // Each Icom LAN radio: "IC-7300, IC-9700" in the Radio field is two
    // radios on one RS-BA1 / wfview server; empty is the server's first.
    if (!lanHost.trimmed().isEmpty()) {
        QStringList names = lanRadio.split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (QString &n : names) n = n.trimmed();
        names.removeAll(QString());
        if (names.isEmpty()) names << QString();
        for (const QString &n : names) {
            radioProfile i;
            i.type = QStringLiteral("icomlan");
            i.host = lanHost.trimmed();
            i.user = lanUser;
            i.pass = lanPassword;
            i.radio = n;
            i.bufferMs = lanBufferMs;
            // No amplifier: the RF2K-S goes with the Flex only (punch list
            // 73b). In this mode the one amp field on screen is the current
            // transport's, so an Icom LAN entry would carry the Flex's amp.
            i.name = !n.isEmpty() ? n
                   : !nameTags[GroupIcomLan].trimmed().isEmpty() ? nameTags[GroupIcomLan].trimmed()
                   : QStringLiteral("Icom");
            out << i.toSpec();
        }
    }
    return out;
}

QStringList LaunchConfig::restProbeUrls() const
{
    return QStringList{
        QString("http://127.0.0.1:%1/api/v1/radio/info").arg(webPort + 1),
        QString("http://127.0.0.1:%1/api/v1/radio/info").arg(webPort),
    };
}

QString LaunchConfig::webUrl(bool https) const
{
    return QString("%1://localhost:%2/").arg(https ? "https" : "http").arg(webPort);
}

void LaunchConfig::load(const QSettings &s)
{
    const int t = s.value("transport", int(Usb)).toInt();
    transport = (t == Lan || t == FlexLan || t == SmartLink) ? Transport(t) : Usb;
    serialPort = s.value("serialPort").toString();
    int man = s.value("manufacturer", int(Icom)).toInt();
    manufacturer = (man == Kenwood) ? Kenwood : (man == Yaesu) ? Yaesu : Icom;
    civAddress = s.value("civAddress").toString();
    audioDevice = s.value("audioDevice").toString();
    lanHost = s.value("lanHost").toString();
    lanUser = s.value("lanUser").toString();
    lanPassword = s.value("lanPassword").toString();
    lanRadio = s.value("lanRadio").toString();
    lanBufferMs = qBound(0, s.value("lanBufferMs", 0).toInt(), 1000);
    multiRadio = s.value("multiRadio", false).toBool();
    noAutoConnect = s.value("noAutoConnect", false).toBool();
    flexHost = s.value("flexHost", "auto").toString();
    smartLinkRadio = s.value("smartLinkRadio", "auto").toString();
    flexAudio = s.value("flexAudio", "auto").toString();
    static const char *const groupKey[GroupCount] = {"Usb", "IcomLan", "Flex"};
    for (int g = 0; g < GroupCount; ++g) {
        ampHosts[g] = s.value(QString("ampHost%1").arg(groupKey[g])).toString();
        nameTags[g] = s.value(QString("nameTag%1").arg(groupKey[g])).toString();
    }
    // Settings from before the split had one of each. The amplifier support
    // is the Flex's (ported from AetherSDR), so an old amp goes to the Flex;
    // an old name goes to the Flex if it says so, else to the transport that
    // was in use.
    if (!s.contains("ampHostFlex") && s.contains("ampHost"))
        ampHosts[GroupFlex] = s.value("ampHost").toString();
    onDemandSeconds = qBound(0, s.value("onDemandSeconds", 0).toInt(), 86400);
    restLan = s.value("restLan", false).toBool();
    guestView = s.value("guestView", false).toBool();
    guestTune = s.value("guestTune", false).toBool();
    allowRequests = s.value("allowRequests", false).toBool();
    ntfyTopic = s.value("ntfyTopic").toString();
    webPort = s.value("webPort", defaultWebPort).toInt();
    if (webPort <= 0 || webPort > 65535) webPort = defaultWebPort;
    if (!s.contains("nameTagFlex") && !s.contains("nameTagIcomLan") && !s.contains("nameTagUsb")
        && s.contains("nameTag")) {
        const QString old = s.value("nameTag").toString();
        nameTags[old.contains("flex", Qt::CaseInsensitive) ? int(GroupFlex) : int(groupOf(transport))] = old;
    }
    settingsFile = s.value("settingsFile").toString();
    extraArguments = s.value("extraArguments").toString();
    startServerOnOpen = s.value("startServerOnOpen", false).toBool();
    openBrowserOnStart = s.value("openBrowserOnStart", true).toBool();
}

void LaunchConfig::save(QSettings &s) const
{
    s.setValue("transport", int(transport));
    s.setValue("serialPort", serialPort);
    s.setValue("manufacturer", int(manufacturer));
    s.setValue("civAddress", civAddress);
    s.setValue("audioDevice", audioDevice);
    s.setValue("lanHost", lanHost);
    s.setValue("lanUser", lanUser);
    s.setValue("lanPassword", lanPassword);
    s.setValue("lanRadio", lanRadio);
    s.setValue("lanBufferMs", lanBufferMs);
    s.setValue("multiRadio", multiRadio);
    s.setValue("noAutoConnect", noAutoConnect);
    s.setValue("flexHost", flexHost);
    s.setValue("smartLinkRadio", smartLinkRadio);
    s.setValue("flexAudio", flexAudio);
    static const char *const groupKeyOut[GroupCount] = {"Usb", "IcomLan", "Flex"};
    for (int g = 0; g < GroupCount; ++g) {
        s.setValue(QString("ampHost%1").arg(groupKeyOut[g]), ampHosts[g]);
        s.setValue(QString("nameTag%1").arg(groupKeyOut[g]), nameTags[g]);
    }
    s.remove("ampHost");
    s.setValue("onDemandSeconds", onDemandSeconds);
    s.setValue("restLan", restLan);
    s.setValue("guestView", guestView);
    s.setValue("guestTune", guestTune);
    s.setValue("allowRequests", allowRequests);
    s.setValue("ntfyTopic", ntfyTopic);
    s.setValue("webPort", webPort);
    s.remove("nameTag");
    s.setValue("settingsFile", settingsFile);
    s.setValue("extraArguments", extraArguments);
    s.setValue("startServerOnOpen", startServerOnOpen);
    s.setValue("openBrowserOnStart", openBrowserOnStart);
}
