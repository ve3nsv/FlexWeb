#pragma once

// Receive noise reduction for every radio other than the Flex (punch list
// 19d, 59): NNR or NR2, as the Flex runs them on flexRxDsp, applied by
// webServer to the RX audio of any other radio (USB sound card or LAN) before
// it goes to the browsers. Settings are shared with the Flex (rxNrConfig:
// QSettings "wfweb" / "nnr", or nnr.ini under WFWEB_CONFIG_DIR) and use the
// page's keys (nnr, nrKind, nnrStrength, nr2..., ...), so the same panel
// drives both. Switching on, off or between kinds goes through rxNrChain
// (no gap, no click).
//
// Pure Qt5Core.

#include <QJsonObject>
#include <QString>

#include "rxnr.h"

class nnrStage
{
public:
    nnrStage();
    ~nnrStage();

    // A page key; false if it is not one of ours.
    bool set(const QString& key, int value);

    // 16-bit mono PCM in place. `allowed`: voice mode and not transmitting.
    // Returns true when the reducer went on or off (the page is told).
    bool process(QByteArray& pcm, int rate, bool allowed);

    bool on() const { return cfg.on != 0; }
    bool active() const { return wasActive; }
    float gain() const;
    QJsonObject state() const;

private:
    rxNrConfig cfg;
    rxNrChain chain;
    bool wasActive = false;
};
