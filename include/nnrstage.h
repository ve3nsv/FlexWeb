#pragma once

// NNR for every radio (punch list 19d): the same neural noise reduction the
// Flex runs in flexCommander::handleRxAudio, applied by webServer to the RX
// audio of any other radio (USB sound card or LAN) before it goes to the
// browsers. Settings are shared with the Flex (QSettings "wfweb" / "nnr", or
// nnr.ini under WFWEB_CONFIG_DIR) and use the page's flexDsp keys (nnr,
// nnrStrength, ...), so the same NNR panel drives both.
//
// Pure Qt5Core.

#include <QJsonObject>
#include <QString>
#include <memory>

class nnrFilter;

class nnrStage
{
public:
    nnrStage();
    ~nnrStage();

    // A page setting (flexDsp key "nnr*"); true when it was one.
    bool set(const QString& key, int value);

    // In place on mono int16 PCM at `rate`. `allowed`: a voice mode and not
    // transmitting. Returns true when the active state changed (the page's
    // NNR status needs a refresh).
    bool process(QByteArray& pcm, int rate, bool allowed);

    bool on() const { return cfg.on != 0; }
    bool active() const { return wasActive; }
    float gain() const;
    // The flexDsp-style fields the page's NNR panel reads.
    QJsonObject state() const;

private:
    struct settings { int on = 0, strength = 38, model = 0, alpha = 100, knee = 100, tau = 200,
                      maxGain = 120, attack = 0, release = 0; } cfg;
    std::unique_ptr<nnrFilter> f;
    int modelInUse = 0;
    bool wasActive = false;
    void apply();
    void load();
    void save();
};
