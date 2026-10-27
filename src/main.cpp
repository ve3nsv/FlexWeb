#ifdef BUILD_WFSERVER
#include <QtCore/QCoreApplication>
#include <QPointer>
#include <QSocketNotifier>
#include "keyboard.h"
#include "consolestatus.h"
#else
#include <QApplication>
#include <QTranslator>
#endif

#include "wfweb_version.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <atomic>
#include <csignal>
#else
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

#include <cstdio>
#include <iostream>

#if (QT_VERSION < QT_VERSION_CHECK(6,0,0))
#include <QAudioDeviceInfo>
#else
#include <QMediaDevices>
#include <QAudioDevice>
#endif

#ifdef BUILD_WFSERVER
#include "servermain.h"
#else
#include "wfmain.h"
#endif

#include "logcategories.h"

#include "direwolfprocessor.h"
#include "smartlinkclient.h"
#include "webauth.h"

#include <QEventLoop>
#include <QTimer>
#include <string>
#ifndef Q_OS_WIN
#include <termios.h>
#else
#include <io.h>
#endif

bool debugMode=false;

#ifdef BUILD_WFSERVER
// Smart pointer to log file
QScopedPointer<QFile>   m_logFile;
QMutex logMutex;
servermain* w=Q_NULLPTR;
keyboard* kb=Q_NULLPTR;
// Terminal status page; null when the plain log goes to stdout instead.
ConsoleStatus* console=Q_NULLPTR;

#ifdef Q_OS_WIN
// Set true by main() once a.exec() has returned and Qt teardown is done.
// The console-control handler runs on its own OS thread; we block it on
// this flag so Windows does not terminate the process before
// ~servermain() has had a chance to send the LAN 0x05 token-removal
// packet, flush settings, and close handles.
static std::atomic<bool> g_mainExited{false};

bool __stdcall cleanup(DWORD sig)
{
    int budgetMs = 0;
    switch (sig) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
        budgetMs = 4000;
        break;
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        budgetMs = 1500;
        break;
    default:
        return FALSE;
    }

    ConsoleStatus::restoreTerminal();
    qInfo() << "terminate signal caught (Windows)" << sig;
    if (kb != Q_NULLPTR) kb->terminate();
    if (w != Q_NULLPTR)
        QMetaObject::invokeMethod(w, "deleteLater", Qt::QueuedConnection);
    QMetaObject::invokeMethod(QCoreApplication::instance(), "quit",
                              Qt::QueuedConnection);

    // Block the handler thread until main() finishes Qt teardown, or we hit
    // the platform deadline. Without this, Windows will ExitProcess us
    // before ~servermain() runs.
    const int step = 50;
    for (int waited = 0; waited < budgetMs && !g_mainExited.load(); waited += step) {
        Sleep(step);
    }
    return TRUE;
}
#else
// Write end is poked by the signal handler, read end wakes the event loop.
static int quitPipe[2] = {-1, -1};

static void cleanup(int sig)
{
    // Async-signal-safe calls only. The signal may have interrupted the main
    // thread inside malloc or holding the log mutex, where logging or queueing
    // a Qt call from here deadlocks; the shutdown itself runs in the event
    // loop (see the notifier in main()).
    ConsoleStatus::restoreTerminal();
    const char byte = static_cast<char>(sig);
    ssize_t ignored = write(quitPipe[1], &byte, 1);
    (void)ignored;
}
#endif


 #ifndef Q_OS_WIN
// A crash must not leave the shell on the status page with echo off.
static void restoreTerminalAndReraise(int sig)
{
    ConsoleStatus::restoreTerminal();
    signal(sig, SIG_DFL);
    raise(sig);
}

void initDaemon()
{
    if(getppid()==1)
        return; /* already a daemon */
    pid_t pid=fork();
    if (pid<0)
        exit(1); /* fork error */
    if (pid>0)
        exit(0); /* parent exits */

    setsid(); /* obtain a new process group */

    // Detach the standard streams by pointing them at /dev/null. Do NOT close
    // every descriptor: QCoreApplication has already been constructed by the
    // time "-b" is parsed, and it owns the event-loop wakeup eventfd (plus
    // other internal fds). Blindly closing them with getdtablesize()/close()
    // corrupts the Qt event dispatcher — the main loop then spins a CPU core at
    // 100%, and its wakeup writes (uint64 = 1) leak into whatever low fd gets
    // reused next (typically the -l log file, filling it with NUL garbage).
    // Redirecting only stdin/stdout/stderr leaves Qt's descriptors untouched.
    int fd=open("/dev/null",O_RDWR);
    if (fd>=0) {
        dup2(fd,STDIN_FILENO);
        dup2(fd,STDOUT_FILENO);
        dup2(fd,STDERR_FILENO);
        if (fd>STDERR_FILENO)
            close(fd);
    }

    signal(SIGCHLD,SIG_IGN);
    signal(SIGTSTP,SIG_IGN);
    signal(SIGTTOU,SIG_IGN);
    signal(SIGTTIN,SIG_IGN);
}

 #else

void initDaemon() {
    std::cout << "Background mode does not currently work in Windows\n";
    exit(1);
}

 #endif

void messageHandler(QtMsgType type, const QMessageLogContext& context, const QString& msg);
#endif

// Read a line from the console without echoing it (the SmartLink password).
static std::string readHidden()
{
    std::string line;
#ifdef Q_OS_WIN
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    const bool console = GetConsoleMode(h, &mode);
    if (console) SetConsoleMode(h, mode & ~ENABLE_ECHO_INPUT);
    std::getline(std::cin, line);
    if (console) SetConsoleMode(h, mode);
#else
    termios old {};
    const bool tty = tcgetattr(STDIN_FILENO, &old) == 0;
    if (tty) {
        termios quiet = old;
        quiet.c_lflag &= ~tcflag_t(ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &quiet);
    }
    std::getline(std::cin, line);
    if (tty) tcsetattr(STDIN_FILENO, TCSANOW, &old);
#endif
    std::cout << "\n";
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

static bool stdinIsTerminal()
{
#ifdef Q_OS_WIN
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(STDIN_FILENO) != 0;
#endif
}

// wfweb --add-user / --passwd <name>: on a terminal ask twice without echo;
// from a pipe (the Windows launcher) read one line.
static int webUserPassword(const QString& name, bool mustBeNew)
{
    if (!webAuth::validUserName(name)) {
        std::cout << "User names are 1-32 letters, digits, '.', '_' or '-'.\n";
        return 1;
    }
    if (mustBeNew && webAuth::userExists(name)) {
        std::cout << "User " << name.toStdString() << " already exists; use --passwd to change the password.\n";
        return 1;
    }
    if (!mustBeNew && !webAuth::userExists(name)) {
        std::cout << "No user " << name.toStdString() << ".\n";
        return 1;
    }
    QString pw;
    if (stdinIsTerminal()) {
        std::cout << "Password for " << name.toStdString() << ": " << std::flush;
        pw = QString::fromStdString(readHidden());
        std::cout << "Again: " << std::flush;
        if (QString::fromStdString(readHidden()) != pw) {
            std::cout << "The passwords do not match.\n";
            return 1;
        }
    } else {
        std::string line;
        std::getline(std::cin, line);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        pw = QString::fromStdString(line);
    }
    if (pw.size() < 8) {
        std::cout << "Use at least 8 characters.\n";
        return 1;
    }
    if (!webAuth::setPassword(name, pw)) {
        std::cout << "Could not save the user.\n";
        return 1;
    }
    std::cout << (mustBeNew ? "Added " : "Changed the password of ") << name.toStdString()
              << ". Web login is on (" << webAuth::users().size() << " user(s)).\n";
    return 0;
}

static int smartLinkRun(const QString& email, const QString& password);

// wfweb --smartlink-login [email]: sign in to the FlexRadio account once,
// keep the refresh token (not the password), list the account's radios.
static int smartLinkLogin(QString email)
{
    if (email.isEmpty()) {
        std::cout << "FlexRadio SmartLink account e-mail: " << std::flush;
        std::string e;
        std::getline(std::cin, e);
        email = QString::fromStdString(e).trimmed();
    }
    if (email.isEmpty()) {
        std::cout << "No e-mail given.\n";
        return 1;
    }
    std::cout << "Password (not shown, not stored): " << std::flush;
    const QString password = QString::fromStdString(readHidden());
    smartLinkClient::forgetCertPins();
    return smartLinkRun(email, password);
}

// wfweb --smartlink-radios: the account's radios with the stored sign-in
// (the Windows launcher's radio list). Exit code 2 = not signed in.
static int smartLinkRadios()
{
    if (!smartLinkClient::haveStoredToken()) {
        std::cout << "Not signed in to SmartLink.\n";
        return 2;
    }
    return smartLinkRun(QString(), QString());
}

// One SmartLink session: sign in (password, or the stored token when the
// password is empty), print the radio list, sign out of the broker.
static int smartLinkRun(const QString& email, const QString& password)
{
    smartLinkClient client;
    QEventLoop loop;
    int rc = 1;
    QObject::connect(&client, &smartLinkClient::authFailed, &loop, [&](const QString& err) {
        std::cout << "Sign-in failed: " << err.toStdString() << "\n";
        loop.quit();
    });
    QObject::connect(&client, &smartLinkClient::authenticated, &loop, [&]() {
        std::cout << "Signed in. Asking SmartLink for your radios...\n";
    });
    QObject::connect(&client, &smartLinkClient::radioList, &loop, [&](const QList<smartLinkRadio>& radios) {
        rc = 0;
        if (radios.isEmpty())
            std::cout << "No radios are online on this account right now (the sign-in is saved).\n";
        for (const smartLinkRadio& r : radios) {
            std::cout << QString("  %1  \"%2\"  serial %3  %4  %5")
                .arg(r.model, r.nickname, r.serial, r.status,
                     r.needsHolePunch ? "(no forwarded/UPnP ports: FlexWeb cannot reach it yet)"
                                      : QString("(%1 TLS %2 UDP %3)").arg(r.publicIp).arg(r.publicTlsPort).arg(r.publicUdpPort))
                .toStdString() << "\n";
        }
        std::cout << "\nThe sign-in is saved. Start FlexWeb with\n"
                     "  wfweb --smartlink <serial or radio name>\n"
                     "(--smartlink auto picks the only radio on the account).\n";
        loop.quit();
    });
    QTimer::singleShot(30000, &loop, [&]() {
        std::cout << "SmartLink did not answer within 30 s.\n";
        loop.quit();
    });
    if (password.isEmpty())
        client.loginWithStoredToken();
    else
        client.login(email, password);
    loop.exec();
    client.close();
    return rc;
}

int main(int argc, char *argv[])
{
#ifndef Q_OS_WIN
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
#endif

#ifdef BUILD_WFSERVER
#ifdef Q_OS_WIN
    // QCoreApplication doesn't load the Windows platform plugin, so COM is
    // never initialised.  WASAPI audio capture requires COM on every thread
    // that touches audio objects.  Initialise it here for the main thread
    // (device enumeration) — the web-server thread does its own init.
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
#endif
    QCoreApplication a(argc, argv);
    a.setOrganizationName("wfweb");
    a.setOrganizationDomain("wfweb.org");
    a.setApplicationName("wfweb");
    kb = new keyboard();
#else
#if (QT_VERSION < QT_VERSION_CHECK(6,0,0))
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif
    QApplication a(argc, argv);
    a.setOrganizationName("wfweb");
    a.setOrganizationDomain("wfweb.org");
    a.setApplicationName("wfweb");
#endif

#ifdef QT_DEBUG
    //debugMode = true;
#endif

    QDateTime date = QDateTime::currentDateTime();
    QString formattedTime = date.toString("dd.MM.yyyy hh:mm:ss");
    QString logFilename = (QString("%1/%2-%3.log").arg(QStandardPaths::standardLocations(QStandardPaths::TempLocation)[0]).arg(a.applicationName()).arg(date.toString("yyyyMMddhhmmss")));

    QString settingsFile = NULL;
    QString currentArg;
    cmdLineOverrides overrides;
    bool noTui = false;


    const QString helpText = QString(
        "\nUsage: wfweb [options]\n"
        "  -s --settings <file>    Settings .ini file\n"
        "  -p --port <port>        Web server port\n"
        "  -S --no-web             Disable web server, enable rig server\n"
        "  --lan <ip>              Connect via LAN to IP (enables LAN mode). FlexRadio:\n"
        "                          '--lan auto' finds the radio by its discovery broadcast\n"
        "  --lan-control <port>    LAN control port (default 50001)\n"
        "  --lan-serial <port>     LAN serial port (default 50002)\n"
        "  --lan-audio <port>      LAN audio port (default 50003)\n"
        "  --lan-user <user>       LAN username\n"
        "  --lan-pass <pass>       LAN password\n"
        "  --civ <addr>            CI-V address, hex or decimal (e.g. 0x94 or 148 for IC-7300)\n"
        "  --serial-port <path>    Serial port device (e.g. /dev/ttyUSB0)\n"
        "  --audio-system <id>     Audio system (0=Qt, 1=PortAudio, 2=RtAudio)\n"
        "  --audio-device <name>   Audio device name; matches a name prefix, or a\n"
        "                          case-insensitive substring (e.g. 'USB Audio CODEC').\n"
        "                          With multiple matches, the Windows/system default wins.\n"
        "                          Use '--audio-device list' to show available devices\n"
        "  --manufacturer <id>     Manufacturer ID (0=Icom, 1=Kenwood, 2=Yaesu, 3=FlexRadio)\n"
        "  --rigctld-port <port>   Enable Hamlib rigctld TCP on the given port (default 4532)\n"
        "  --no-rigctld            Disable rigctld even if enabled in settings\n"
        "  --rigctld-bind-all      Bind rigctld to 0.0.0.0 (default: 127.0.0.1 only)\n"
        "  -n --name <tag>         Name shown in the web UI top bar and browser tab\n"
        "                          (default: the rig model, e.g. IC-7300)\n"
        "  --no-autoconnect        Start without connecting to the rig (LAN only; connect\n"
        "                          later with the web UI Reconnect button). Also enabled\n"
        "                          by setting WFWEB_NO_AUTOCONNECT=1 in the environment\n"
        "  --on-demand [seconds]   Connect to the rig only while a browser has the page\n"
        "                          open, and release it this many seconds (default 30)\n"
        "                          after the last one closes, so other programs can use\n"
        "                          the radio meanwhile (LAN only). Also enabled by\n"
        "                          setting WFWEB_ON_DEMAND=<seconds> in the environment\n"
        "  --no-tui                Print the plain log on an interactive terminal instead\n"
        "                          of the status page. Also enabled by setting\n"
        "                          WFWEB_NO_TUI=1 in the environment\n"
        "  --logbook <file>        QSO logbook (ADIF; default: <data dir>/logbook.adi)\n"
        "  --remote-log <host[:port]>  Send every logged QSO to an external logging\n"
        "                          program (GridTracker, JTAlert, Log4OM, ...) which\n"
        "                          listens for the WSJT-X UDP protocol; port 2237 if omitted\n"
        "  --no-remote-log         Disable remote logging even if enabled in settings\n"
        "  --smartlink <radio>     FlexRadio over the internet through SmartLink: the\n"
        "                          radio's serial or name, or 'auto' (implies\n"
        "                          --manufacturer 3). Sign in once with --smartlink-login\n"
        "  --smartlink-login [email]  Sign in to SmartLink, list the account's radios, exit.\n"
        "                          Keeps a sign-in token, never the password\n"
        "  --smartlink-radios      List the SmartLink account's radios (stored sign-in)\n"
        "  --smartlink-logout      Forget the SmartLink sign-in\n"
        "  --flex-audio <mode>     FlexRadio receive audio: opus (~50 kbit/s), none\n"
        "                          (uncompressed, ~1.5 Mbit/s) or auto (default: Opus\n"
        "                          over SmartLink, uncompressed on the LAN)\n"
        "  --add-user <name>       Add a web page user (asks for the password). With at\n"
        "                          least one user, the page asks to sign in\n"
        "  --passwd <name>         Change a web user's password\n"
        "  --remove-user <name>    Remove a web user (none left = login off)\n"
        "  --list-users            List the web users\n"
        "  --rest                  Open the plain-HTTP REST port (web port + 1) to the\n"
        "                          LAN; default is 127.0.0.1 only. With web users, LAN\n"
        "                          requests need HTTP Basic auth\n"
        "  --rfkit <host[:port]>   RF-KIT (RF2K-S) amplifier on the LAN (REST API, port\n"
        "                          8080 if omitted); shown under FUNC > AMP\n"
        "  --guest-view            Add a \"View only (no password)\" button to the login\n"
        "                          page: guests see and hear everything but cannot\n"
        "                          change or key the radio (also [Web] GuestView=true)\n"
        "  --remote-log-decodes    Also forward FT8/FT4 decodes to the remote logger\n"
        "  -l --logfile <file>     Log file\n"
        "  -b --background         Run as daemon (not Windows)\n"
        "  -d --debug [file]       Enable verbose debug logging (optionally to file)\n"
        "  -c --clearconfig CONFIRM  Clear all settings\n"
        "  --packet-self-test      Run Dire Wolf encode/decode loopback and exit\n"
        "  -v --version            Show version\n"
        "  -? --help               Show this help\n");
#ifdef BUILD_WFSERVER
    const QString version = QString("FlexWeb version: %1 (Git:%2 on %3 at %4 by %5@%6)\nOperating System: %7 (%8)\nBuild Qt Version %9. Current Qt Version: %10\n")
        .arg(QString(FLEXWEB_VERSION) + " (based on wfweb " WFWEB_VERSION ")")
        .arg(GITSHORT).arg(__DATE__).arg(__TIME__).arg(UNAME).arg(HOST)
        .arg(QSysInfo::prettyProductName()).arg(QSysInfo::buildCpuArchitecture())
        .arg(QT_VERSION_STR).arg(qVersion());
#else
    const QString version = QString("FlexWeb version: %1 (Git:%2 on %3 at %4 by %5@%6)\nOperating System: %7 (%8)\nBuild Qt Version %9. Current Qt Version: %10\n")
        .arg(QString(FLEXWEB_VERSION) + " (based on wfweb " WFWEB_VERSION ")")
        .arg(GITSHORT).arg(__DATE__).arg(__TIME__).arg(UNAME).arg(HOST)
        .arg(QSysInfo::prettyProductName()).arg(QSysInfo::buildCpuArchitecture())
        .arg(QT_VERSION_STR).arg(qVersion());

    // Translator doesn't really make sense for wfweb right now.
    QTranslator myappTranslator;
    qDebug() << "Current translation language: " << myappTranslator.language();

    bool trResult = myappTranslator.load(QLocale(), QLatin1String("wfweb"), QLatin1String("_"), QLatin1String(":/translations"));
    if(trResult) {
        qDebug() << "Recognized requested language and loaded the translations (or at least found the /translations resource folder). Installing translator.";
        a.installTranslator(&myappTranslator);
    } else {
        qDebug() << "Could not load translation.";
    }

    qDebug() << "Changed to translation language: " << myappTranslator.language();
#endif

    // Early CLI short-circuit: run the packet self-test and exit, without
    // starting the web server or touching radio state.  Used by
    // tests/test_packet.py.
    for (int c = 1; c < argc; c++) {
        if (QString(argv[c]) == "--packet-self-test") {
            return DireWolfProcessor::runSelfTest();
        }
    }

    for(int c=1; c<argc; c++)
    {
        //qInfo() << "Argc: " << c << " argument: " << argv[c];
        currentArg = QString(argv[c]);

        if ((currentArg == "-d") || (currentArg == "--debug"))
        {
            debugMode = true;
            // Optional filename argument: --debug [file]
            if (argc > c + 1 && argv[c + 1][0] != '-')
            {
                logFilename = argv[c + 1];
                c += 1;
            }
        }
        else if ((currentArg == "-l") || (currentArg == "--logfile"))
        {
            if (argc > c)
            {
                logFilename = argv[c + 1];
                c += 1;
            }
        }
        else if ((currentArg == "-s") || (currentArg == "--settings"))
        {
            if (argc > c)
            {
                settingsFile = argv[c + 1];
                c += 1;
                if (settingsFile.endsWith(".rig", Qt::CaseInsensitive))
                {
                    std::cout <<
                        "Error: '" << settingsFile.toStdString() << "' is a radio\n"
                        "definition file (.rig), not a wfweb settings file. --settings\n"
                        "does not take .rig files.\n"
                        "\n"
                        ".rig files are CI-V command dictionaries for specific radio\n"
                        "models. wfweb already loads them automatically from its install's\n"
                        "rigs/ directory based on the radio it detects on the CI-V bus.\n"
                        "You should never pass one on the command line.\n"
                        "\n"
                        "What --settings is actually for:\n"
                        "\n"
                        "wfweb normally stores YOUR preferences (which serial port, which\n"
                        "audio device, web-server port, LAN address and credentials, per-\n"
                        "radio config, etc.) in the OS default location under your user\n"
                        "profile. --settings lets you point it somewhere else instead.\n"
                        "The file itself is created and managed by wfweb — you don't\n"
                        "hand-write it; you edit settings through the web UI and wfweb\n"
                        "persists them to whichever file you named.\n"
                        "\n"
                        "Common reasons to use --settings:\n"
                        "\n"
                        "  * Running multiple wfweb instances in parallel, one per radio.\n"
                        "    Give each its own --settings file so they don't overwrite\n"
                        "    each other's config.\n"
                        "\n"
                        "  * Docker or systemd deployments where the default per-user\n"
                        "    location is inconvenient. Mount or install a config at a\n"
                        "    fixed, predictable path (e.g. /etc/wfweb/station.ini).\n"
                        "\n"
                        "  * Named profiles you can switch between or back up:\n"
                        "    home.ini, contest.ini, portable.ini, etc.\n"
                        "\n"
                        "To create a fresh settings file, just run:\n"
                        "\n"
                        "    wfweb -s ./my-profile.ini\n"
                        "\n"
                        "wfweb writes a file with sensible defaults on first run. After\n"
                        "that, open the web UI and configure as usual — your changes are\n"
                        "saved back to that file.\n"
                        "\n"
                        "If all you need is to talk to a rig on a non-default CI-V address\n"
                        "or a different manufacturer, you don't need --settings at all —\n"
                        "use --civ <addr> and --manufacturer <id> directly.\n";
                    return -1;
                }
            }
        }
        else if ((currentArg == "-c") || (currentArg == "--clearconfig"))
        {
            if (argc > c)
            {
                QString confirm = argv[c + 1];
                c += 1;
                if (confirm == "CONFIRM") {
                    QSettings* settings;
                    // Clear config
                    if (settingsFile.isEmpty()) {
                        settings = new QSettings();
                    }
                    else
                    {
                        QString file = settingsFile;
                        QFile info(settingsFile);
                        QString path="";
                        if (!QFileInfo(info).isAbsolute())
                        {
                            path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
                            if (path.isEmpty())
                            {
                                path = QDir::homePath();
                            }
                            path = path + "/";
                            file = info.fileName();
                        }
                        settings = new QSettings(path + file, QSettings::Format::IniFormat);
                    }
                    settings->clear();
                    delete settings;
                    std::cout << QString("All FlexWeb settings cleared.\n").toStdString();
                    exit(0);
                }
            }
            std::cout << QString("Error: Clear config not confirmed (please add the word CONFIRM), aborting\n").toStdString();
            std::cout << helpText.toStdString();
            exit(-1);
        }
#ifdef BUILD_WFSERVER
        else if ((currentArg == "-b") || (currentArg == "--background"))
        {
            initDaemon();
        }
#endif
        else if ((currentArg == "-p") || (currentArg == "--port"))
        {
            if (argc > c + 1)
            {
                bool ok;
                int port = QString(argv[c + 1]).toInt(&ok);
                if (ok && port > 0 && port <= 65535)
                {
                    overrides.webPort = static_cast<quint16>(port);
                    c += 1;
                }
                else
                {
                    std::cout << "Error: Invalid port number (must be 1-65535)\n";
                    std::cout << helpText.toStdString();
                    return -1;
                }
            }
            else
            {
                std::cout << "Error: Port number required\n";
                std::cout << helpText.toStdString();
                return -1;
            }
        }
        else if ((currentArg == "-S") || (currentArg == "--no-web"))
        {
            overrides.noWeb = true;
        }
        else if (currentArg == "--lan")
        {
            if (argc > c + 1) { overrides.lanIP = argv[++c]; }
            else { std::cout << "Error: --lan requires IP address\n"; return -1; }
        }
        else if (currentArg == "--logbook")
        {
            if (argc > c + 1) overrides.logbook = argv[++c];
            else { std::cout << "Error: --logbook requires a file\n"; return -1; }
        }
        else if (currentArg == "--remote-log")
        {
            if (argc > c + 1) overrides.remoteLogTarget = argv[++c];
            else { std::cout << "Error: --remote-log requires host[:port]\n"; return -1; }
        }
        else if (currentArg == "--smartlink")
        {
            if (argc > c + 1) overrides.lanIP = QString("smartlink:") + argv[++c];
            else { std::cout << "Error: --smartlink requires a radio serial or name (or auto)\n"; return -1; }
        }
        else if (currentArg == "--smartlink-login")
        {
            QString email;
            if (argc > c + 1 && argv[c + 1][0] != '-') email = argv[++c];
            return smartLinkLogin(email);
        }
        else if (currentArg == "--add-user" || currentArg == "--passwd")
        {
            if (argc <= c + 1) { std::cout << "Error: " << currentArg.toStdString() << " requires a user name\n"; return -1; }
            return webUserPassword(QString(argv[++c]), currentArg == "--add-user");
        }
        else if (currentArg == "--remove-user")
        {
            if (argc <= c + 1) { std::cout << "Error: --remove-user requires a user name\n"; return -1; }
            const QString name = argv[++c];
            if (!webAuth::removeUser(name)) { std::cout << "No user " << name.toStdString() << ".\n"; return 1; }
            std::cout << "Removed " << name.toStdString() << ".";
            std::cout << (webAuth::hasUsers() ? "\n" : " No users left: web login is off.\n");
            return 0;
        }
        else if (currentArg == "--list-users")
        {
            const QStringList u = webAuth::users();
            if (u.isEmpty()) std::cout << "No web users: login is off.\n";
            for (const QString& n : u) std::cout << n.toStdString() << "\n";
            return 0;
        }
        else if (currentArg == "--flex-audio")
        {
            const QString v = argc > c + 1 ? QString(argv[++c]).toLower() : QString();
            if (v != "opus" && v != "none" && v != "auto") { std::cout << "Error: --flex-audio takes opus, none or auto\n"; return -1; }
            qputenv("WFWEB_FLEX_AUDIO", v.toLatin1());
        }
        else if (currentArg == "--rest")
        {
            overrides.restLan = true;
        }
        else if (currentArg == "--smartlink-radios")
        {
            return smartLinkRadios();
        }
        else if (currentArg == "--smartlink-logout")
        {
            smartLinkClient::forgetCredentials();
            smartLinkClient::forgetCertPins();
            std::cout << "SmartLink sign-in removed.\n";
            return 0;
        }
        else if (currentArg == "--rfkit")
        {
            if (argc > c + 1) overrides.rfkit = argv[++c];
            else { std::cout << "Error: --rfkit requires host[:port]\n"; return -1; }
        }
        else if (currentArg == "--guest-view") overrides.guestView = true;
        else if (currentArg == "--no-remote-log") overrides.noRemoteLog = true;
        else if (currentArg == "--remote-log-decodes") overrides.remoteLogDecodes = true;
        else if (currentArg == "--lan-control")
        {
            if (argc > c + 1) { overrides.controlPort = QString(argv[++c]).toInt(); }
            else { std::cout << "Error: --lan-control requires port\n"; return -1; }
        }
        else if (currentArg == "--lan-serial")
        {
            if (argc > c + 1) { overrides.serialPort = QString(argv[++c]).toInt(); }
            else { std::cout << "Error: --lan-serial requires port\n"; return -1; }
        }
        else if (currentArg == "--lan-audio")
        {
            if (argc > c + 1) { overrides.audioPort = QString(argv[++c]).toInt(); }
            else { std::cout << "Error: --lan-audio requires port\n"; return -1; }
        }
        else if (currentArg == "--lan-user")
        {
            if (argc > c + 1) { overrides.lanUser = argv[++c]; }
            else { std::cout << "Error: --lan-user requires username\n"; return -1; }
        }
        else if (currentArg == "--lan-pass")
        {
            if (argc > c + 1) { overrides.lanPass = argv[++c]; }
            else { std::cout << "Error: --lan-pass requires password\n"; return -1; }
        }
        else if (currentArg == "--civ")
        {
            if (argc > c + 1) {
                QString civStr = QString(argv[++c]).trimmed();
                bool ok = false;
                int civ = 0;
                if (civStr.startsWith("0x", Qt::CaseInsensitive))
                    civ = civStr.mid(2).toInt(&ok, 16);
                else
                    civ = civStr.toInt(&ok, 10);
                if (!ok || civ < 0 || civ > 0xFF) {
                    std::cout << "Error: --civ value must be 0x00-0xFF or 0-255 (e.g. 0x94 or 148)\n";
                    return -1;
                }
                overrides.civAddr = civ;
            }
            else { std::cout << "Error: --civ requires address\n"; return -1; }
        }
        else if (currentArg == "--serial-port")
        {
            if (argc > c + 1) { overrides.usbPort = argv[++c]; }
            else { std::cout << "Error: --serial-port requires device path\n"; return -1; }
        }
        else if (currentArg == "--audio-system")
        {
            if (argc > c + 1) { overrides.audioSystem = QString(argv[++c]).toInt(); }
            else { std::cout << "Error: --audio-system requires ID (0=Qt, 1=PortAudio, 2=RtAudio)\n"; return -1; }
        }
        else if (currentArg == "--audio-device")
        {
            if (argc > c + 1) {
                QString devArg = argv[++c];
                if (devArg.compare("list", Qt::CaseInsensitive) == 0) {
                    std::cout << "Available audio input devices:\n";
#if (QT_VERSION < QT_VERSION_CHECK(6,0,0))
                    for (const auto &dev : QAudioDeviceInfo::availableDevices(QAudio::AudioInput)) {
                        std::cout << "  " << dev.deviceName().toStdString() << "\n";
                    }
#else
                    for (const QAudioDevice &dev : QMediaDevices::audioInputs()) {
                        std::cout << "  " << dev.description().toStdString() << "\n";
                    }
#endif
                    std::cout << "\nAvailable audio output devices:\n";
#if (QT_VERSION < QT_VERSION_CHECK(6,0,0))
                    for (const auto &dev : QAudioDeviceInfo::availableDevices(QAudio::AudioOutput)) {
                        std::cout << "  " << dev.deviceName().toStdString() << "\n";
                    }
#else
                    for (const QAudioDevice &dev : QMediaDevices::audioOutputs()) {
                        std::cout << "  " << dev.description().toStdString() << "\n";
                    }
#endif
                    return 0;
                }
                overrides.audioDevice = devArg;
            }
            else { std::cout << "Error: --audio-device requires device name (e.g. 'USB Audio CODEC')\n"; return -1; }
        }
        else if (currentArg == "--manufacturer")
        {
            if (argc > c + 1) { overrides.manufacturer = QString(argv[++c]).toInt(); }
            else { std::cout << "Error: --manufacturer requires ID\n"; return -1; }
        }
        else if (currentArg == "--rigctld-port")
        {
            if (argc > c + 1) {
                bool ok;
                int port = QString(argv[++c]).toInt(&ok);
                if (ok && port > 0 && port <= 65535) {
                    overrides.rigCtlPort = port;
                } else {
                    std::cout << "Error: --rigctld-port requires 1-65535\n";
                    return -1;
                }
            }
            else { std::cout << "Error: --rigctld-port requires a port\n"; return -1; }
        }
        else if (currentArg == "--no-rigctld")
        {
            overrides.noRigCtl = true;
        }
        else if (currentArg == "--rigctld-bind-all")
        {
            overrides.rigCtlBindAll = true;
        }
        else if ((currentArg == "-n") || (currentArg == "--name"))
        {
            if (argc > c + 1) { overrides.name = QString(argv[++c]).trimmed(); }
            else { std::cout << "Error: --name requires a tag\n"; return -1; }
        }
        else if (currentArg == "--no-autoconnect")
        {
            overrides.noAutoConnect = true;
        }
        else if (currentArg == "--on-demand")
        {
            // Optional idle time: only a plain number is taken as one.
            overrides.onDemandIdle = 30;
            if (argc > c + 1) {
                bool ok = false;
                int secs = QString(argv[c + 1]).toInt(&ok);
                if (ok) {
                    if (secs < 1) { std::cout << "Error: --on-demand needs at least 1 second\n"; return -1; }
                    overrides.onDemandIdle = secs;
                    ++c;
                }
            }
        }
        else if (currentArg == "--no-tui")
        {
            noTui = true;
        }
        else if ((currentArg == "-?") || (currentArg == "--help"))
        {
            std::cout << helpText.toStdString();
            return 0;
        }
        else if ((currentArg == "-v") || (currentArg == "--version"))
        {
            std::cout << version.toStdString();
            return 0;
	}
        else {
            std::cout << "Unrecognized option: " << currentArg.toStdString();
            std::cout << helpText.toStdString();
            return -1;
        }

    }

    // --smartlink is FlexRadio only.
    if (overrides.lanIP.startsWith("smartlink:") && overrides.manufacturer < 0)
        overrides.manufacturer = 3;

    // Container-friendly alternative to --no-autoconnect (issue #70)
    if (qEnvironmentVariableIntValue("WFWEB_NO_AUTOCONNECT") > 0)
        overrides.noAutoConnect = true;
    if (overrides.onDemandIdle == 0 && qEnvironmentVariableIntValue("WFWEB_ON_DEMAND") > 0)
        overrides.onDemandIdle = qEnvironmentVariableIntValue("WFWEB_ON_DEMAND");
    if (qEnvironmentVariableIntValue("WFWEB_REST") > 0)
        overrides.restLan = true;
    if (qEnvironmentVariableIntValue("WFWEB_NO_TUI") > 0)
        noTui = true;

#ifdef BUILD_WFSERVER

    // Set the logging file before doing anything else.
    m_logFile.reset(new QFile(logFilename));
    // Open the file logging
    m_logFile.data()->open(QFile::WriteOnly | QFile::Truncate | QFile::Text);
    // On an interactive terminal, show a status page (URLs, rig, ports) and
    // keep the log one keypress away. Daemons, pipes and containers without a
    // TTY get the plain log on stdout as before.
    if (!noTui && ConsoleStatus::wanted()) {
        console = new ConsoleStatus(logFilename);
        console->setProvider([]() { return w != Q_NULLPTR ? w->consoleSnapshot() : ConsoleSnapshot(); });
        QObject::connect(kb, &keyboard::keyPressed, console, [](char key) { console->onKey(key); });
        console->enter(debugMode);
 #ifndef Q_OS_WIN
        signal(SIGSEGV, restoreTerminalAndReraise);
        signal(SIGABRT, restoreTerminalAndReraise);
        signal(SIGQUIT, restoreTerminalAndReraise);
        signal(SIGHUP, restoreTerminalAndReraise);
 #endif
    }
    // Set handler
    qInstallMessageHandler(messageHandler);

    qInfo(logSystem()) << version;

#endif

#ifdef BUILD_WFSERVER
 #ifdef Q_OS_WIN
    SetConsoleCtrlHandler((PHANDLER_ROUTINE)cleanup, TRUE);
 #else
    // The pipe is read once the event loop runs, so a signal that arrives
    // while the server is still starting is not lost.
    if (pipe(quitPipe) == 0) {
        QSocketNotifier* quitNotifier = new QSocketNotifier(quitPipe[0], QSocketNotifier::Read, &a);
        QObject::connect(quitNotifier, &QSocketNotifier::activated, [quitNotifier]() {
            quitNotifier->setEnabled(false);
            qInfo() << "terminate signal caught";
            if (kb != Q_NULLPTR) kb->terminate();
            if (w != Q_NULLPTR) w->deleteLater();
            QCoreApplication::quit();
        });
        signal(SIGINT, cleanup);
        signal(SIGTERM, cleanup);
    }
 #endif
    kb->start();
    w = new servermain(settingsFile, overrides);
    QPointer<servermain> server(w);
#else
    a.setWheelScrollLines(1); // one line per wheel click
    wfmain w(settingsFile, logFilename, debugMode, webPort);
    w.show();

#endif
    int rc = a.exec();
#ifdef BUILD_WFSERVER
    // The 'q' key ends the event loop without the signal handlers'
    // deleteLater(): stop the server and its threads here, or the process
    // crashes while tearing down underneath them. The status page must not
    // query a server that is being destroyed, hence the null first.
    w = Q_NULLPTR;
    delete server.data();
    if (console != Q_NULLPTR) {
        ConsoleStatus::restoreTerminal();
        std::cout << "FlexWeb stopped. Log: " << logFilename.toLocal8Bit().toStdString() << std::endl;
    }
#endif
#if defined(BUILD_WFSERVER) && defined(Q_OS_WIN)
    // Release the console-control handler thread (if any) so Windows can
    // proceed to terminate the process now that Qt teardown has completed.
    g_mainExited.store(true);
#endif
    return rc;

}

#ifdef BUILD_WFSERVER

void messageHandler(QtMsgType type, const QMessageLogContext& context, const QString& msg)
{
    // Open stream file writes
    if (type == QtDebugMsg && !debugMode)
    {
        return;
    }
#ifdef Q_OS_WIN
    // Qt 5.15 + OpenSSL 3.x: suppress noise about removed 1.1 symbols
    if (msg.contains("cannot call unresolved function"))
        return;
#endif
    QMutexLocker locker(&logMutex);
    QTextStream out(m_logFile.data());
    QString text;

    // Write the date of recording
    out << QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz ");
    // By type determine to what level belongs message
    
    switch (type)
    {
        case QtDebugMsg:
            out << "DBG ";
            break;
        case QtInfoMsg:
            out << "INF ";
            break;
        case QtWarningMsg:
            out << "WRN ";
            break;
        case QtCriticalMsg:
            out << "CRT ";
            break;
        case QtFatalMsg:
            out << "FTL ";
            break;
    } 
    // Write to the output category of the message and the message itself
    out << context.category << ": " << msg << "\n";
    if (console != Q_NULLPTR)
        console->logLine(type, msg);
    else
        std::cout << QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz ").toLocal8Bit().toStdString() << msg.toLocal8Bit().toStdString() << "\n";
    out.flush();    // Clear the buffered data
}
#endif
