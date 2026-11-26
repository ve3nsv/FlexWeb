#ifndef RADIOREACH_H
#define RADIOREACH_H

// Punch list 95: is each radio of the RADIO menu answering on the network?
// Every 30 s, one probe per radio, in the radio's own protocol (no ICMP,
// which needs privileges on Windows and says nothing about the radio's
// control service):
//   icomlan  - an "Are you there" (0x03, 16 bytes) to the control port, sent
//              three times over 1.2 s; any "I am here" (0x04) within 3 s is
//              an answer. It opens no session: no login follows.
//   flex     - a TCP connect to the API port (4992) within 3 s, closed at once.
//   smartlink, flex "auto" - not checked (no address to try).
// Only while a radio page is open (setActive).
// The radio connected right now is not probed while its link is up
// (linkUp says so): the link is the better check.
//
// No Q_OBJECT: lambdas only, Qt Core + Network.

#include <QObject>
#include <QString>
#include <QVector>
#include <QTimer>
#include <functional>

class RadioReach : public QObject {
public:
    struct Target {
        QString name;
        QString type;      // flex | smartlink | icomlan
        QString host;
        quint16 port = 0;
    };
    explicit RadioReach(QObject *parent = nullptr);

    // Keeps the results when the list is the same (a radio switch resends it).
    void setTargets(const QVector<Target> &targets);
    void setInterval(int ms) { timer_.setInterval(ms); }
    // Checks run only while a radio page is open (as the amp is polled):
    // an idle server sends nothing to the radios. Off forgets the results.
    void setActive(bool on);

    // "up", "down" or "" (not checked yet / cannot be checked); why: "no answer",
    // "refused", "no address" with "down".
    QString state(int i) const { return i >= 0 && i < state_.size() ? state_[i] : QString(); }
    QString why(int i) const { return i >= 0 && i < why_.size() ? why_[i] : QString(); }

    std::function<bool(int)> linkUp;      // index -> its link is up now
    std::function<void()> changed;        // a state changed

    void probeAll();

private:
    void probe(int i);
    void probeIcom(int i, quint64 gen);
    void probeFlex(int i, quint64 gen);
    void withAddress(int i, quint64 gen, std::function<void(const class QHostAddress &)> fn);
    void result(int i, quint64 gen, const QString &state, const QString &why);

    QVector<Target> targets_;
    QVector<QString> state_, why_;
    QVector<QString> logged_;            // the last state put in the log (kept while inactive)
    QVector<quint64> gen_;               // per radio: a late answer to an old probe is ignored
    quint64 nextGen_ = 1;
    bool active_ = false;
    QTimer timer_;
};

#endif
