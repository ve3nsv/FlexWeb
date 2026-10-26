#pragma once
// rtl_tcp wire protocol. The 12-byte greeting and sample stream can be split
// across arbitrary TCP reads; no network or device operations happen here.
#include <array>
#include <cstdint>
#include <optional>
#include <vector>
namespace RtlTcp {
inline std::array<std::uint8_t,5> command(std::uint8_t type, std::uint32_t value) {
    return {type, std::uint8_t(value >> 24), std::uint8_t(value >> 16), std::uint8_t(value >> 8), std::uint8_t(value)};
}
struct Greeting { std::uint32_t tuner, gainCount; };
class Parser {
public:
    void reset() { headerSize_ = 0; greeting_.reset(); failed_ = false; hasI_ = false; }
    bool failed() const { return failed_; }
    std::optional<Greeting> greeting() const { return greeting_; }
    // Emits complete interleaved I/Q pairs. One trailing byte is retained.
    template<class Sink> void feed(const std::uint8_t *data, std::size_t len, Sink sink) {
        if (failed_) return;
        while (!greeting_ && len) {
            header_[headerSize_++] = *data++; --len;
            if (headerSize_ == header_.size()) {
                if (header_[0] != 'R' || header_[1] != 'T' || header_[2] != 'L' || header_[3] != '0') { failed_ = true; return; }
                auto u32 = [&](int i) { return (std::uint32_t(header_[i]) << 24) | (std::uint32_t(header_[i+1]) << 16) | (std::uint32_t(header_[i+2]) << 8) | header_[i+3]; };
                greeting_ = Greeting{u32(4), u32(8)};
                // A corrupt greeting must not become an allocation size.
                if (greeting_->gainCount > 4096) { greeting_.reset(); failed_ = true; return; }
            }
        }
        while (len) {
            const auto x = *data++; --len;
            if (!hasI_) { pendingI_ = x; hasI_ = true; }
            else { sink(pendingI_, x); hasI_ = false; }
        }
    }
private:
    std::array<std::uint8_t,12> header_{};
    std::size_t headerSize_ = 0;
    std::optional<Greeting> greeting_;
    bool failed_ = false, hasI_ = false;
    std::uint8_t pendingI_ = 0;
};
} // namespace RtlTcp
