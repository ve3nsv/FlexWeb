// Test double for the dynamically loaded RTL-SDR library.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>
struct rtlsdr_dev { std::atomic<bool> stop{false}; };
extern "C" {
int rtlsdr_open(rtlsdr_dev **dev,unsigned) { *dev=new rtlsdr_dev;return 0; }
int rtlsdr_close(rtlsdr_dev *dev) { delete dev;return 0; }
int rtlsdr_set_sample_rate(rtlsdr_dev *,std::uint32_t) { return 0; }
int rtlsdr_set_center_freq(rtlsdr_dev *,std::uint32_t) { return 0; }
int rtlsdr_set_tuner_gain_mode(rtlsdr_dev *,int) { return 0; }
int rtlsdr_set_tuner_gain(rtlsdr_dev *,int) { return 0; }
int rtlsdr_set_freq_correction(rtlsdr_dev *,int ppm) { return ppm == 0 ? -2 : 0; }
int rtlsdr_get_freq_correction(rtlsdr_dev *) { return 0; }
int rtlsdr_set_direct_sampling(rtlsdr_dev *,int) { return 0; }
int rtlsdr_set_bias_tee(rtlsdr_dev *,int) { return 0; }
int rtlsdr_reset_buffer(rtlsdr_dev *) { return 0; }
int rtlsdr_cancel_async(rtlsdr_dev *dev) { dev->stop=true;return 0; }
int rtlsdr_read_async(rtlsdr_dev *dev,void(*cb)(unsigned char*,std::uint32_t,void*),void *ctx,std::uint32_t,std::uint32_t) {
    dev->stop=false; std::vector<unsigned char> bytes(4096);
    for (int i=0;i<2048;i++) { double a=2*3.141592653589793*128*i/2048; bytes[2*i]=127+int(60*cos(a));bytes[2*i+1]=127+int(60*sin(a)); }
    while(!dev->stop) { cb(bytes.data(),bytes.size(),ctx);std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    return 0;
}
}
