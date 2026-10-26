#pragma once
#include "sdrspectrum.h"
#include <QElapsedTimer>
#include <QLibrary>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QObject>
#include <QThread>
#include <QString>
#include <atomic>
#include <complex>
#include <cstdint>
#include <functional>
#include <vector>

struct rtlsdr_dev;
// librtlsdr is loaded at runtime, so FlexWeb starts normally without a dongle
// or vendor driver. Blocking USB operations live on the worker thread.
class RtlLocalSource : public QThread {
public:
    struct Config {
        QString library; // optional absolute path, otherwise platform default
        unsigned deviceIndex = 0;
        std::uint32_t rate = 2048000, center = 14230000;
        int ppm = 0, gainTenthsDb = 0;
        bool autoGain = true, directQ = false, biasT = false;
    };
    using FrameCallback = std::function<void(const SdrSpectrum::Frame &, std::uint32_t, std::uint32_t)>;
    using StatusCallback = std::function<void(const QString &)>;
    RtlLocalSource(QObject *receiver, FrameCallback frame, StatusCallback status)
        : QThread(receiver), receiver_(receiver), frame_(std::move(frame)), status_(std::move(status)) {}
    ~RtlLocalSource() override { stop(); wait(); }
    bool open(const Config &config) {
        if (isRunning() || config.rate < 225001 || config.rate > 3200000 || !config.center || config.ppm < -1000 || config.ppm > 1000) return false;
        config_ = config; requestedCenter_.store(config.center); stopping_.store(false); start(); return true;
    }
    void setCenter(std::uint32_t hz) {
        if (!hz || hz == requestedCenter_.exchange(hz)) return;
        cancelRead();
    }
    void stop() { stopping_.store(true); cancelRead(); }
protected:
    void run() override {
        QLibrary lib(config_.library.isEmpty() ? QStringLiteral("rtlsdr") : config_.library);
        if (!lib.load()) {
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
            if (config_.library.isEmpty()) lib.setFileNameAndVersion(QStringLiteral("rtlsdr"), 0);
#endif
            if (!lib.load()) { status(QStringLiteral("RTL-SDR library unavailable: ") + lib.errorString()); return; }
        }
        auto open = sym<Open>(lib,"rtlsdr_open"); auto close = sym<Close>(lib,"rtlsdr_close");
        auto sampleRate = sym<SetU32>(lib,"rtlsdr_set_sample_rate"); auto frequency = sym<SetU32>(lib,"rtlsdr_set_center_freq");
        auto gainMode = sym<SetInt>(lib,"rtlsdr_set_tuner_gain_mode"); auto gain = sym<SetInt>(lib,"rtlsdr_set_tuner_gain");
        auto ppm = sym<SetInt>(lib,"rtlsdr_set_freq_correction"); auto getPpm = sym<Reset>(lib,"rtlsdr_get_freq_correction"); auto direct = sym<SetInt>(lib,"rtlsdr_set_direct_sampling");
        auto bias = sym<SetInt>(lib,"rtlsdr_set_bias_tee"); auto reset = sym<Reset>(lib,"rtlsdr_reset_buffer");
        auto read = sym<ReadAsync>(lib,"rtlsdr_read_async"); auto cancel = sym<Cancel>(lib,"rtlsdr_cancel_async");
        if (!open || !close || !sampleRate || !frequency || !gainMode || !gain || !ppm || !direct || !reset || !read || !cancel) {
            status(QStringLiteral("RTL-SDR library is missing required functions")); return;
        }
        rtlsdr_dev *device = nullptr;
        if (open(&device,config_.deviceIndex) != 0 || !device) { status(QStringLiteral("Could not open RTL-SDR device")); return; }
        { QMutexLocker lock(&deviceMutex_); device_=device; cancel_=cancel; }
        auto fail = [&](int r) { return r != 0; };
        bool configured = !fail(sampleRate(device,config_.rate)) && !fail(gainMode(device,config_.autoGain ? 0 : 1))
            && (config_.autoGain || !fail(gain(device,config_.gainTenthsDb)))
            && !fail(direct(device,config_.directQ ? 2 : 0));
        if (configured && (!getPpm || getPpm(device) != config_.ppm)) configured = !fail(ppm(device,config_.ppm));
        if (config_.biasT && (!bias || fail(bias(device,1)))) configured=false;
        if (!configured) status(QStringLiteral("RTL-SDR configuration failed"));
        while (configured && !stopping_.load()) {
            const auto center=requestedCenter_.load();
            if (fail(frequency(device,center)) || fail(reset(device))) { status(QStringLiteral("RTL-SDR tuning failed")); break; }
            activeCenter_=center; samples_.clear(); clock_.invalidate(); settle_.restart();
            status(QStringLiteral("receiving"));
            const int result=read(device,&RtlLocalSource::onSamples,this,0,32768);
            if (stopping_.load()) break;
            if (center == requestedCenter_.load()) { status(QStringLiteral("RTL-SDR stream stopped (%1)").arg(result)); break; }
        }
        { QMutexLocker lock(&deviceMutex_); device_=nullptr; cancel_=nullptr; close(device); }
    }
private:
    using Open=int(*)(rtlsdr_dev **,unsigned); using Close=int(*)(rtlsdr_dev *);
    using SetU32=int(*)(rtlsdr_dev *,std::uint32_t); using SetInt=int(*)(rtlsdr_dev *,int);
    using Reset=int(*)(rtlsdr_dev *); using Cancel=int(*)(rtlsdr_dev *);
    using Callback=void(*)(unsigned char *,std::uint32_t,void *);
    using ReadAsync=int(*)(rtlsdr_dev *,Callback,void *,std::uint32_t,std::uint32_t);
    template<class T> static T sym(QLibrary &lib,const char *name) { return reinterpret_cast<T>(lib.resolve(name)); }
    void status(const QString &text) {
        if (receiver_ && status_) { const auto cb=status_; QMetaObject::invokeMethod(receiver_,[cb,text]{cb(text);},Qt::QueuedConnection); }
    }
    void cancelRead() {
        QMutexLocker lock(&deviceMutex_);
        if (device_ && cancel_) cancel_(device_);
    }
    static void onSamples(unsigned char *bytes,std::uint32_t len,void *context) {
        auto *self=static_cast<RtlLocalSource *>(context);
        if (self->stopping_.load() || self->requestedCenter_.load()!=self->activeCenter_) {
            // Cancellation can race with the call that begins async reads.
            // Retry from the first callback if that happens.
            self->cancelRead(); return;
        }
        if (self->settle_.elapsed()<250 || (self->clock_.isValid() && self->clock_.elapsed()<50)) return;
        for (std::uint32_t i=0;i+1<len && self->samples_.size()<2048;i+=2)
            self->samples_.emplace_back((float(bytes[i])-127.5f)/127.5f,(float(bytes[i+1])-127.5f)/127.5f);
        if (self->samples_.size()==2048) {
            auto result=SdrSpectrum::transform(self->samples_); self->samples_.clear(); self->clock_.restart();
            const auto cb=self->frame_; const auto center=self->activeCenter_, rate=self->config_.rate;
            if (self->receiver_ && cb) QMetaObject::invokeMethod(self->receiver_,[cb,result=std::move(result),center,rate]{cb(result,center,rate);},Qt::QueuedConnection);
        }
    }
    Config config_;
    QObject *receiver_;
    FrameCallback frame_;
    StatusCallback status_;
    QMutex deviceMutex_;
    rtlsdr_dev *device_ = nullptr;
    Cancel cancel_ = nullptr;
    std::atomic<bool> stopping_{false};
    std::atomic<std::uint32_t> requestedCenter_{0};
    std::uint32_t activeCenter_ = 0;
    std::vector<std::complex<float>> samples_;
    QElapsedTimer clock_, settle_;
};
