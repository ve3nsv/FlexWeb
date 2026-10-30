#include "SpectralNR.h"
#include <vector>
#include <cmath>
#include <cassert>
int main() {
    for (int rate : {16000,32000,48000}) {
        AetherSDR::SpectralNR nr(2048,rate,4);
        std::vector<float> in(480), out(480);
        for (int block=0; block<200; ++block) {
            for (int i=0;i<480;++i) in[i]=0.1f*std::sin(6.28318530718*700*(block*480+i)/rate);
            nr.process(in.data(),out.data(),480);
            for (float x:out) assert(std::isfinite(x));
        }
        auto n=nr.noiseEstimateResetCount();
        nr.resetTransient(); assert(nr.noiseEstimateResetCount()==n);
        nr.reset(); assert(nr.noiseEstimateResetCount()>n);
    }
}
