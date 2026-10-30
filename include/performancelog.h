#pragma once
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include <QStandardPaths>
#include <atomic>
#include <functional>
#include <memory>

// Bounded diagnostics writer. All filesystem work runs off the radio/web thread.
class PerformanceLog {
    struct State {
        QString directory;
        qint64 limit;
        int archives;
        std::atomic<bool> failed{false};
        State(QString dir,qint64 bytes,int count):directory(std::move(dir)),limit(bytes),archives(count){}
        QString path(int part) const { return directory + QString("/performance-%1.jsonl").arg(part); }
        void prune() {
            const auto cutoff=QDateTime::currentDateTimeUtc().addDays(-7);
            for(int i=0;i<=archives;++i) if(QFileInfo(path(i)).exists() && QFileInfo(path(i)).lastModified()<cutoff) QFile::remove(path(i));
        }
        bool append(QJsonObject sample) {
            if (!QDir().mkpath(directory)) return false;
            prune();
            const auto data=QJsonDocument(sample).toJson(QJsonDocument::Compact)+'\n';
            if(data.size()>limit)return false;
            if(QFileInfo(path(0)).size()+data.size()>limit) {
                if(QFile::exists(path(archives)) && !QFile::remove(path(archives)))return false;
                for(int i=archives-1;i>=0;--i) if(QFile::exists(path(i)) && !QFile::rename(path(i),path(i+1)))return false;
            }
            QFile file(path(0));
            if(!file.open(QIODevice::WriteOnly|QIODevice::Append))return false;
            const bool ok=file.write(data)==data.size() && file.flush(); file.close();
            QFile::setPermissions(path(0),QFileDevice::ReadOwner|QFileDevice::WriteOwner);
            return ok;
        }
    };
    std::shared_ptr<State> state_;
    QThread thread_;
    QObject *worker_;
    std::atomic<int> pending_{0};
    std::atomic<unsigned long long> dropped_{0};
    bool enabled_=true;
    QString configPath_;
public:
    explicit PerformanceLog(QString directory={},qint64 limit=4*1024*1024,int archives=15) {
        const auto config=qEnvironmentVariable("WFWEB_CONFIG_DIR");
        const auto base=config.isEmpty()?QStandardPaths::writableLocation(QStandardPaths::AppDataLocation):config;
        configPath_=base+"/performance.ini";
        enabled_=QSettings(configPath_,QSettings::IniFormat).value("Enabled",true).toBool();
        state_=std::make_shared<State>(directory.isEmpty()?base+"/performance":directory,qMax<qint64>(128,limit),qBound(1,archives,15));
        worker_=new QObject;worker_->moveToThread(&thread_);
        QObject::connect(&thread_,&QThread::finished,worker_,&QObject::deleteLater);
        thread_.setObjectName("PerformanceLog");thread_.start();
        QMetaObject::invokeMethod(worker_,[worker=worker_,state=state_]{
            auto *timer=new QTimer(worker);
            QObject::connect(timer,&QTimer::timeout,worker,[state]{state->prune();});
            timer->start(3600000);state->prune();
        },Qt::QueuedConnection);
    }
    ~PerformanceLog(){QMetaObject::invokeMethod(worker_,[]{},Qt::BlockingQueuedConnection);thread_.quit();thread_.wait();}
    bool enabled()const{return enabled_;}
    void setEnabled(bool enabled){enabled_=enabled;QSettings(configPath_,QSettings::IniFormat).setValue("Enabled",enabled);}
    QJsonObject status()const{return {{"enabled",enabled_},{"writeError",state_->failed.load()},
        {"droppedSamples",double(dropped_.load())},{"maxBytes",double(state_->limit*(state_->archives+1))},
        {"retentionDays",7},{"parts",state_->archives+1}};}
    void record(QJsonObject sample){
        if(!enabled_)return;
        if(pending_.fetch_add(1)>=8){pending_.fetch_sub(1);++dropped_;return;}
        sample["recordedAt"]=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        sample["schema"]=1;
        QMetaObject::invokeMethod(worker_,[this,sample]{state_->failed.store(!state_->append(sample));pending_.fetch_sub(1);},Qt::QueuedConnection);
    }
    // Snapshot one bounded segment on the writer thread, then deliver on context.
    // The owning context must outlive this PerformanceLog instance.
    void download(int part,QObject *context,std::function<void(QByteArray)> done){
        if(part<0||part>state_->archives){done({});return;}
        QMetaObject::invokeMethod(worker_,[this,part,context,done]{
            QFile file(state_->path(part));QByteArray data;
            if(file.open(QIODevice::ReadOnly))data=file.read(state_->limit);
            QMetaObject::invokeMethod(context,[done,data]{done(data);},Qt::QueuedConnection);
        },Qt::QueuedConnection);
    }
};
