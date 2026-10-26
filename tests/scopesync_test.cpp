#include "scopesync.h"
#include <cassert>
#include <cmath>
#include <vector>
int main() {
    std::vector<float> a(2000), b(2000);
    unsigned x=123456;
    float env=0;
    for(std::size_t i=0;i<a.size();++i) {
        x=x*1664525u+1013904223u;
        if (i%7==0) env=float(x%1000)/1000;
        a[i]=env;
        b[i]=i>=17?a[i-17]:0;
    }
    auto result=ScopeSync::estimate(a,b,100,100);
    assert(result && std::abs(result->lagMs-170)<.01 && result->correlation>.99);
    auto reverse=ScopeSync::estimate(b,a,100,100);
    assert(reverse && std::abs(reverse->lagMs+170)<.01);
    std::fill(a.begin(),a.end(),0);
    assert(!ScopeSync::estimate(a,b,100,100));
    for(std::size_t i=0;i<a.size();i++) a[i]=b[i]=float(std::sin(2*3.141592653589793*i/20));
    assert(!ScopeSync::estimate(a,b,100,100)); // periodic envelope has many equal peaks
}
