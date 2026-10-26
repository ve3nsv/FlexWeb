#include "sdrspectrum.h"
#include <cassert>
#include <cmath>
#include <complex>
#include <limits>
#include <vector>
int main() {
    constexpr size_t n=2048; constexpr int bin=173;
    std::vector<std::complex<float>> iq(n);
    for(size_t i=0;i<n;i++) {
        const double a=2*3.14159265358979323846*bin*i/n;
        iq[i]={float(.5*std::cos(a)),float(.5*std::sin(a))};
    }
    const auto f=SdrSpectrum::transform(iq);
    assert(f.db.size()==n && f.overloadFraction==0);
    const auto peak=std::max_element(f.db.begin(),f.db.end())-f.db.begin();
    assert(peak==n/2+bin);
    assert(std::abs(f.db[peak]-20*std::log10(.5))<.1);
    assert(f.db[peak]>f.db[n/2-bin]+50);
    float floor=std::numeric_limits<float>::quiet_NaN();
    auto out=SdrSpectrum::display(f,floor);
    assert(out.size()==n && std::isfinite(floor) && out[peak]==160);
    iq.assign(n,{1.0f,0.0f});
    assert(SdrSpectrum::transform(iq).overloadFraction==1.0f);
    assert(SdrSpectrum::transform(std::vector<std::complex<float>>(100)).db.empty());
}
