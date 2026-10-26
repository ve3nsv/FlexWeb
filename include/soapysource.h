#pragma once
#include "sdrspectrum.h"
#include <QElapsedTimer>
#include <QLibrary>
#include <QMetaObject>
#include <QObject>
#include <QThread>
#include <QString>
#include <atomic>
#include <complex>
#include <cstdint>
#include <functional>
#include <vector>

// SoapySDR 0.8 C ABI, loaded at runtime. No headers or closed-source SDRplay
// libraries are bundled. SoapyRemote is selected by its normal device args.
class SoapySource : public QThread {
public:
    struct Config {
        QString library, args;
        std::uint32_t rate=2048000, center=14230000;
        bool autoGain=true;
        double gain=0;
    };
    using FrameCallback=std::function<void(const SdrSpectrum::Frame &,std::uint32_t,std::uint32_t)>;
    using StatusCallback=std::function<void(const QString &)>;
    SoapySource(QObject *receiver,FrameCallback frame,StatusCallback status)
        : QThread(receiver),receiver_(receiver),frame_(std::move(frame)),status_(std::move(status)) {}
    ~SoapySource() override { stop();wait(); }
    bool open(const Config &c) {
        if (isRunning() || c.rate<100000 || c.rate>10000000 || !c.center || c.args.size()>1024 || !std::isfinite(c.gain)) return false;
        config_=c;center_.store(c.center);stopping_.store(false);start();return true;
    }
    void setCenter(std::uint32_t hz) {if(hz) center_.store(hz);}
    void stop() {stopping_.store(true);}
protected:
    void run() override {
        QLibrary lib(config_.library.isEmpty()?QStringLiteral("SoapySDR"):config_.library);
        if(!lib.load()) {
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
            if(config_.library.isEmpty()) lib.setFileNameAndVersion(QStringLiteral("SoapySDR"),"0.8");
#endif
            if(!lib.load()){status("SoapySDR library unavailable: "+lib.errorString());return;}
        }
        auto version=sym<Version>(lib,"SoapySDR_getABIVersion");
        if(!version || !version() || !QByteArray(version()).startsWith("0.8")){status("SoapySDR 0.8 is required");return;}
        auto make=sym<Make>(lib,"SoapySDRDevice_makeStrArgs"); auto unmake=sym<Unmake>(lib,"SoapySDRDevice_unmake");
        auto rate=sym<SetRate>(lib,"SoapySDRDevice_setSampleRate"); auto getRate=sym<GetRate>(lib,"SoapySDRDevice_getSampleRate");
        auto tune=sym<Tune>(lib,"SoapySDRDevice_setFrequency"); auto setup=sym<Setup>(lib,"SoapySDRDevice_setupStream");
        auto activate=sym<Activate>(lib,"SoapySDRDevice_activateStream");auto deactivate=sym<Deactivate>(lib,"SoapySDRDevice_deactivateStream");
        auto read=sym<Read>(lib,"SoapySDRDevice_readStream");auto close=sym<Close>(lib,"SoapySDRDevice_closeStream");
        auto hasAgc=sym<HasAgc>(lib,"SoapySDRDevice_hasGainMode");auto agc=sym<SetAgc>(lib,"SoapySDRDevice_setGainMode");auto gain=sym<SetGain>(lib,"SoapySDRDevice_setGain");
        if(!make||!unmake||!rate||!getRate||!tune||!setup||!activate||!deactivate||!read||!close){status("SoapySDR library is missing required functions");return;}
        const QByteArray args=config_.args.toUtf8();
        void *dev=make(args.constData()); if(!dev){status("SoapySDR device unavailable; check driver and device arguments");return;}
        constexpr int rx=1; constexpr std::size_t channel=0;
        if(rate(dev,rx,channel,double(config_.rate))!=0 || tune(dev,rx,channel,double(center_.load()),nullptr)!=0){status("SoapySDR sample rate or frequency is unsupported");unmake(dev);return;}
        if(config_.autoGain && hasAgc && agc && hasAgc(dev,rx,channel)) agc(dev,rx,channel,true);
        else if(!config_.autoGain && gain && gain(dev,rx,channel,config_.gain)!=0){status("SoapySDR gain unsupported");unmake(dev);return;}
        const double actualRate=getRate(dev,rx,channel);
        if(!std::isfinite(actualRate)||actualRate<100000||actualRate>10000000){status("SoapySDR returned an invalid sample rate");unmake(dev);return;}
        void *stream=setup(dev,rx,"CF32",&channel,1,nullptr);
        if(!stream){status("SoapySDR could not open an RX stream");unmake(dev);return;}
        if(activate(dev,stream,0,0,0)!=0){status("SoapySDR could not start RX");close(dev,stream);unmake(dev);return;}
        status("receiving");std::uint32_t current=center_.load();QElapsedTimer settle,clock;settle.start();
        std::vector<std::complex<float>> iq;iq.reserve(2048);
        std::vector<float> buffer(8192);void *buffers[]={buffer.data()};
        while(!stopping_.load()){
            auto requested=center_.load();
            if(requested!=current){
                if(tune(dev,rx,channel,double(requested),nullptr)!=0){status("SoapySDR tuning failed");break;}
                current=requested;iq.clear();settle.restart();
            }
            int flags=0;long long time=0;
            int n=read(dev,stream,buffers,4096,&flags,&time,100000);
            if(n==-1)continue; // timeout, allowing a prompt stop
            if(n==-4){iq.clear();status("SoapySDR overflow");continue;}
            if(n<0 || n>4096){status("SoapySDR RX stream failed");break;}
            if(settle.elapsed()<250 || (clock.isValid() && clock.elapsed()<50))continue;
            for(int i=0;i<n && iq.size()<2048;i++)iq.emplace_back(buffer[2*i],buffer[2*i+1]);
            if(iq.size()==2048){
                auto f=SdrSpectrum::transform(iq);iq.clear();clock.restart();
                const auto cb=frame_;const auto hz=current,actual=std::uint32_t(actualRate);
                if(receiver_&&cb)QMetaObject::invokeMethod(receiver_,[cb,f=std::move(f),hz,actual]{cb(f,hz,actual);},Qt::QueuedConnection);
            }
        }
        deactivate(dev,stream,0,0);close(dev,stream);unmake(dev);
    }
private:
    using Version=const char*(*)();using Make=void*(*)(const char*);using Unmake=int(*)(void*);
    using SetRate=int(*)(void*,int,std::size_t,double);using GetRate=double(*)(const void*,int,std::size_t);
    using Tune=int(*)(void*,int,std::size_t,double,const void*);using Setup=void*(*)(void*,int,const char*,const std::size_t*,std::size_t,const void*);
    using Activate=int(*)(void*,void*,int,long long,std::size_t);using Deactivate=int(*)(void*,void*,int,long long);
    using Read=int(*)(void*,void*,void*const*,std::size_t,int*,long long*,long);using Close=int(*)(void*,void*);
    using HasAgc=bool(*)(const void*,int,std::size_t);using SetAgc=int(*)(void*,int,std::size_t,bool);using SetGain=int(*)(void*,int,std::size_t,double);
    template<class T>static T sym(QLibrary &l,const char *n){return reinterpret_cast<T>(l.resolve(n));}
    void status(const QString &text){if(receiver_&&status_){const auto cb=status_;QMetaObject::invokeMethod(receiver_,[cb,text]{cb(text);},Qt::QueuedConnection);}}
    Config config_;QObject *receiver_;FrameCallback frame_;StatusCallback status_;std::atomic<bool> stopping_{false};std::atomic<std::uint32_t> center_{0};
};
