#include "rtltcpprotocol.h"
#include <cassert>
#include <vector>
int main() {
    const auto cmd=RtlTcp::command(1, 14230000);
    assert((cmd==std::array<std::uint8_t,5>{1,0,0xd9,0x21,0xf0}));
    const std::uint8_t bytes[]={'R','T','L','0',0,0,0,5,0,0,0,29,11,22,33,44,55};
    for (unsigned chunk=1; chunk<=sizeof(bytes); ++chunk) {
        RtlTcp::Parser parser; std::vector<unsigned> iq;
        for (unsigned i=0;i<sizeof(bytes);i+=chunk) {
            const auto len=std::min(chunk,unsigned(sizeof(bytes)-i));
            parser.feed(bytes+i,len,[&](auto i,auto q){iq.push_back(i);iq.push_back(q);});
        }
        assert(!parser.failed() && parser.greeting());
        assert(parser.greeting()->tuner==5 && parser.greeting()->gainCount==29);
        assert((iq==std::vector<unsigned>{11,22,33,44}));
        std::uint8_t last=66; parser.feed(&last,1,[&](auto i,auto q){iq.push_back(i);iq.push_back(q);});
        assert((iq==std::vector<unsigned>{11,22,33,44,55,66}));
        parser.reset(); assert(!parser.greeting() && !parser.failed());
    }
    RtlTcp::Parser bad;
    std::uint8_t corrupt[]={'H','T','T','P',0,0,0,0,0,0,0,0};
    bad.feed(corrupt,sizeof(corrupt),[](auto,auto){});assert(bad.failed());
}
