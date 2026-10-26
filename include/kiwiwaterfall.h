#pragma once
#include "sdrspectrum.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>
namespace KiwiWaterfall {
struct View { int zoom; std::uint32_t start; double lowHz, highHz; };
// Kiwi's W/F start field uses 1024 << zoom_max fixed-point units across
// the whole receiver bandwidth. The server may advertise a different max.
inline std::optional<View> view(double centerHz, double requestedSpanHz,
                                double fullHz=30000000, int zoomMax=14) {
    if(!std::isfinite(centerHz)||!std::isfinite(requestedSpanHz)||!std::isfinite(fullHz)
       || fullHz<=0||requestedSpanHz<=0||centerHz<0||centerHz>fullHz||zoomMax<0||zoomMax>20)return std::nullopt;
    int zoom=0;
    while(zoom<zoomMax && fullHz/std::pow(2.0,zoom+1)>=requestedSpanHz)++zoom;
    const double span=fullHz/std::pow(2.0,zoom),scale=std::ldexp(1024.0,zoomMax);
    const double low=std::clamp(centerHz-span/2,0.0,fullHz-span);
    const auto start=std::uint32_t(std::round(low/fullHz*scale));
    const double actualLow=start/scale*fullHz;
    return View{zoom,start,actualLow,actualLow+span};
}
struct Row { SdrSpectrum::Frame spectrum; std::uint32_t start=0; int zoom=0; bool extended=false; };
inline std::optional<Row> decode(const std::uint8_t *data,std::size_t len,int zoomCap=20) {
    if(!data||len<4||data[0]!='W'||data[1]!='/'||data[2]!='F')return std::nullopt;
    std::size_t offset;
    Row row;
    if(len==4+1024)offset=4;
    else if(len==16+1024){
        auto u32=[&](int i){return std::uint32_t(data[i]) | (std::uint32_t(data[i+1])<<8) | (std::uint32_t(data[i+2])<<16) | (std::uint32_t(data[i+3])<<24);};
        row.start=u32(4);const auto flags=u32(8);row.zoom=int(flags & 0xffffu);
        if((flags & 0x10000u) || row.zoom>zoomCap)return std::nullopt;
        row.extended=true;offset=16;
    } else return std::nullopt;
    row.spectrum.db.reserve(1024);
    for(std::size_t i=offset;i<len;i++)row.spectrum.db.push_back(float(data[i])-255.0f);
    return row;
}
} // namespace KiwiWaterfall
