#include "performancelog.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <cassert>

QByteArray snapshot(PerformanceLog& log,int part,QObject* context) {
    bool done=false;QByteArray result;
    log.download(part,context,[&](QByteArray bytes){result=bytes;done=true;});
    QElapsedTimer timer;timer.start();
    while(!done && timer.elapsed()<3000){QCoreApplication::processEvents();QThread::msleep(1);}
    assert(done);return result;
}
int main(int argc,char**argv) {
    QCoreApplication app(argc,argv);QTemporaryDir temp;assert(temp.isValid());
    qputenv("WFWEB_CONFIG_DIR",temp.path().toUtf8());
    const auto dir=temp.path()+"/logs";
    {
        PerformanceLog log(dir,512,2);log.setEnabled(true);
        for(int i=0;i<20;++i){
            log.record({{"type","serverLoad"},{"sequence",i},{"padding",QString(100,'x')}});
            const auto bytes=snapshot(log,0,&app);assert(!bytes.isEmpty());
            for(const auto&line:bytes.split('\n'))if(!line.isEmpty())assert(QJsonDocument::fromJson(line).isObject());
        }
        qint64 total=0;
        for(int i=0;i<3;++i){const auto bytes=snapshot(log,i,&app);assert(bytes.size()<=512);total+=bytes.size();}
        assert(total<=1536);assert(snapshot(log,3,&app).isEmpty());
        const auto before=snapshot(log,0,&app);
        log.setEnabled(false);log.record({{"shouldNotAppear",true}});
        assert(snapshot(log,0,&app)==before);
        // Expired archive is removed on the next record.
        QFile old(dir+"/performance-2.jsonl");assert(old.open(QIODevice::ReadWrite));
        old.resize(0);old.write("expiredMarker\n");
        assert(old.setFileTime(QDateTime::currentDateTimeUtc().addDays(-8),QFileDevice::FileModificationTime));old.close();
        log.setEnabled(true);log.record({{"type","serverLoad"}});snapshot(log,0,&app);
        assert(!snapshot(log,2,&app).contains("expiredMarker"));
    }
    QFile blocked(temp.path()+"/blocked");assert(blocked.open(QIODevice::WriteOnly));blocked.close();
    PerformanceLog broken(blocked.fileName(),512,2);broken.setEnabled(true);
    broken.record({{"type","test"}});snapshot(broken,0,&app);
    assert(broken.status()["writeError"].toBool());
}
