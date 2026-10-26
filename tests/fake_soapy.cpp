#include <chrono>
#include <cmath>
#include <cstddef>
#include <thread>
struct Device { double rate=0; };
extern "C" {
const char *SoapySDR_getABIVersion(){return "0.8-3";}
void *SoapySDRDevice_makeStrArgs(const char *){return new Device;}
int SoapySDRDevice_unmake(void *d){delete static_cast<Device*>(d);return 0;}
int SoapySDRDevice_setSampleRate(void *d,int,size_t,double rate){static_cast<Device*>(d)->rate=rate;return 0;}
double SoapySDRDevice_getSampleRate(const void *d,int,size_t){return static_cast<const Device*>(d)->rate;}
int SoapySDRDevice_setFrequency(void *,int,size_t,double,const void*){return 0;}
void *SoapySDRDevice_setupStream(void *,int,const char *,const size_t *,size_t,const void*){return reinterpret_cast<void*>(1);}
int SoapySDRDevice_activateStream(void *,void *,int,long long,size_t){return 0;}
int SoapySDRDevice_deactivateStream(void *,void *,int,long long){return 0;}
int SoapySDRDevice_closeStream(void *,void *){return 0;}
int SoapySDRDevice_readStream(void *,void *,void *const *buffers,size_t n,int*,long long*,long){
 float *p=static_cast<float*>(buffers[0]);
 for(size_t i=0;i<n;i++){double a=2*3.141592653589793*128*i/2048;p[2*i]=float(.5*cos(a));p[2*i+1]=float(.5*sin(a));}
 std::this_thread::sleep_for(std::chrono::milliseconds(10));return int(n);
}
bool SoapySDRDevice_hasGainMode(const void*,int,size_t){return true;}
int SoapySDRDevice_setGainMode(void*,int,size_t,bool){return 0;}
int SoapySDRDevice_setGain(void*,int,size_t,double){return 0;}
}
