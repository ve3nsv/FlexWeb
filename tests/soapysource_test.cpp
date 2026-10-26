#include "soapysource.h"
#include <QCoreApplication>
#include <QTimer>
#include <cassert>
int main(int argc,char **argv){
 QCoreApplication app(argc,argv);assert(argc==2);bool retuned=false;SoapySource *worker=nullptr;
 SoapySource source(&app,[&](const SdrSpectrum::Frame &f,std::uint32_t hz,std::uint32_t rate){
   assert(rate==2048000 && f.db.size()==2048);
   assert(std::max_element(f.db.begin(),f.db.end())-f.db.begin()==1024+128);
   if(hz==14230000)worker->setCenter(7171000);
   if(hz==7171000){retuned=true;app.quit();}
 },[](const QString&){});worker=&source;
 SoapySource::Config c;c.library=QString::fromLocal8Bit(argv[1]);c.args="driver=sdrplay";assert(source.open(c));
 QTimer::singleShot(5000,&app,&QCoreApplication::quit);app.exec();source.stop();assert(source.wait(5000));assert(retuned);
}
