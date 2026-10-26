#include "rtllocalsource.h"
#include <QCoreApplication>
#include <QTimer>
#include <cassert>
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);assert(argc==2);
    int frames=0; bool second=false;
    RtlLocalSource *source=nullptr;
    RtlLocalSource local(&app,[&](const SdrSpectrum::Frame &frame,std::uint32_t hz,std::uint32_t rate) {
        assert(frame.db.size()==2048 && rate==2048000);
        assert(std::max_element(frame.db.begin(),frame.db.end())-frame.db.begin()==1024+128);
        if (hz==14230000 && frames++==0) source->setCenter(7171000);
        if (hz==7171000) {second=true;app.quit();}
    },[](const QString &) {});
    source=&local;
    RtlLocalSource::Config c;c.library=QString::fromLocal8Bit(argv[1]);
    assert(local.open(c));
    QTimer::singleShot(5000,&app,&QCoreApplication::quit);
    app.exec();
    local.stop();assert(local.wait(5000));assert(second);
}
