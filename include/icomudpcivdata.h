// Class for all (pseudo) serial communications
#ifndef ICOMUDPCIVDATA_H
#define ICOMUDPCIVDATA_H

#include <QObject>
#include <QUdpSocket>
#include <QNetworkDatagram>
#include <QHostInfo>
#include <QTimer>
#include <QMutex>
#include <QDateTime>
#include <QByteArray>
#include <QVector>
#include <QMap>
#include <QUuid>

// Allow easy endian-ness conversions
#include <QtEndian>

// Needed for audio
#include <QBuffer>
#include <QThread>

#include <QDebug>

#include "packettypes.h"

#include "icomudpbase.h"

class icomUdpCivData : public icomUdpBase
{
	Q_OBJECT

public:
	icomUdpCivData(QHostAddress local, QHostAddress ip, quint16 civPort, bool splitWf, quint16 lport);
	~icomUdpCivData();
	QMutex serialmutex;

signals:
	int receive(QByteArray);

public slots:
	void send(QByteArray d);


private:
	void watchdog();
	void dataReceived();
	void sendOpenClose(bool close);

	QTimer* startCivDataTimer = Q_NULLPTR;
	bool splitWaterfall = false;
	bool alerted = false;      // watchdog warned for this stream
	// Punch list 86b: what the CI-V stream did, logged every 5 s while the
	// radio has sent no CI-V data (a radio that logs in and streams audio
	// but never answers on CI-V).
	QElapsedTimer silentFor;
	qint64 lastSilentReport = 0;
	bool gotCivData = false, gotReady = false;
	int framesSent = 0, framesDropped = 0, openRequests = 0;
	int rxControl = 0, rxPing = 0, rxOpenClose = 0, rxRetransmit = 0, rxOther = 0, rxEmptyData = 0;
	QByteArray lastOther;
	void reportSilence();
};


#endif
