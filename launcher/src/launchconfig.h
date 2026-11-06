#ifndef LAUNCHCONFIG_H
#define LAUNCHCONFIG_H

// What the launcher form collects, and the one place the wfweb command line
// is composed from it. The launcher never edits the server's settings: every
// field becomes a CLI flag on each start (the server does not persist CLI
// overrides, see applyCLIOverrides in src/servermain.cpp), so an existing
// user's server settings are left exactly as they were.
//
// Qt5Core only, no Q_OBJECT: tests/launcher_test.cpp links this file through
// tests/test_cpp_units.py with nothing but Qt5Core.

#include <QString>
#include <QStringList>

class QSettings;

struct LaunchConfig {
    // FlexLan / SmartLink: a FlexRadio on the LAN (--manufacturer 3 --lan)
    // or through FlexRadio's SmartLink service (--smartlink).
    enum Transport { Usb = 0, Lan = 1, FlexLan = 2, SmartLink = 3 };
    enum Manufacturer { Icom = 0, Kenwood = 1, Yaesu = 2 };

    static const int defaultWebPort = 8080;

    Transport transport = Usb;

    // USB
    QString serialPort;          // "COM5"; empty = server default ("auto")
    Manufacturer manufacturer = Icom;
    QString civAddress;          // "0x94" or "148"; empty = rig default
    QString audioDevice;         // Qt Multimedia device name; empty = server default

    // LAN (Icom only)
    QString lanHost;
    QString lanUser;
    QString lanPassword;
    QString lanRadio;      // a server with several radios: part of a name or its number
    int lanBufferMs = 0;   // --lan-buffer; 0 = the server's 150 ms
    bool noAutoConnect = false;  // --no-autoconnect (LAN only)

    // FlexRadio
    QString flexHost = "auto";   // IP, hostname or "auto" (discovery)
    QString smartLinkRadio = "auto";  // serial, radio name or "auto"
    QString flexAudio = "auto";  // --flex-audio auto|opus|none

    // The amplifier and the top-bar name belong to the radio, not to the
    // launcher: one of each per kind of connection - USB, Icom LAN, Flex
    // (LAN and SmartLink) - so starting the IC-7300 does not bring the
    // Flex's RF2K-S and "Flex 6400" name along (user, 2026-11-06).
    enum Group { GroupUsb = 0, GroupIcomLan = 1, GroupFlex = 2, GroupCount = 3 };
    static Group groupOf(Transport t)
    { return t == Usb ? GroupUsb : t == Lan ? GroupIcomLan : GroupFlex; }
    QString ampHosts[GroupCount];   // --rfkit host[:port]; empty = no amplifier
    QString nameTags[GroupCount];   // -n
    QString ampHost() const { return ampHosts[groupOf(transport)]; }
    QString nameTag() const { return nameTags[groupOf(transport)]; }
    void setAmpHost(const QString &h) { ampHosts[groupOf(transport)] = h; }
    void setNameTag(const QString &n) { nameTags[groupOf(transport)] = n; }

    // Station (any network rig)
    int onDemandSeconds = 0;     // --on-demand N; 0 = stay connected
    bool restLan = false;        // --rest
    bool guestView = false;      // --guest-view: "View only" on the login page
    bool guestTune = false;      // --guest-tune: guests may also tune (receive only)
    bool allowRequests = false;  // --allow-requests: "Request access" on the sign-in page
    QString ntfyTopic;           // --ntfy-topic: push new requests to ntfy.sh/<topic>

    bool isNetwork() const { return multiRadio || transport != Usb; }

    // Punch list 73: one server, several radios, chosen on the page. The
    // Flex (LAN or SmartLink, whichever is picked; LAN otherwise) and each
    // Icom LAN radio listed in lanRadio ("IC-7300, IC-9700") become --radio
    // profiles; the transport choice is then only the Flex's LAN /
    // SmartLink. The amplifier and the names are each radio kind's own.
    bool multiRadio = false;
    QStringList radioProfiles() const;

    // Server
    int webPort = defaultWebPort;
    QString settingsFile;        // -s, absolute; empty = server default location
    QString extraArguments;      // appended verbatim, split like a shell would

    // Launcher behaviour
    bool startServerOnOpen = false;
    bool openBrowserOnStart = true;

    // The wfweb command line for this configuration. logFile is the absolute
    // path the launcher tails, passed as -l on every start.
    QStringList toArguments(const QString &logFile) const;

    // Plain-HTTP REST URLs to probe for readiness, most likely first: the
    // REST port next to HTTPS (webPort+1), then the web port itself, which is
    // where REST lives when the server fell back to plain HTTP.
    QStringList restProbeUrls() const;

    // The address to open in the browser once the server listens.
    QString webUrl(bool https) const;

    void load(const QSettings &s);
    void save(QSettings &s) const;
};

#endif // LAUNCHCONFIG_H
