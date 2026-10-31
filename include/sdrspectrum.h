#pragma once

// Shared IQ-to-spectrum processing for external receive-only scope sources.
// No device-specific assumptions are made here: sample rate and centre are
// properties of the source; input samples are normalized complex values.
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

namespace SdrSpectrum {
struct Frame {
    std::vector<float> db;
    float overloadFraction = 0;
};

inline bool powerOfTwo(std::size_t n) { return n >= 64 && n <= 32768 && !(n & (n - 1)); }

// Hann-windowed radix-2 FFT. DC is placed at the middle; an uncalibrated
// magnitude is expressed in dB relative to full scale, not in dBm.
inline Frame transform(const std::vector<std::complex<float>> &samples) {
    const std::size_t n = samples.size();
    if (!powerOfTwo(n)) return {};
    constexpr double pi = 3.14159265358979323846;
    std::vector<std::complex<float>> v(n);
    std::size_t clipped = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const float re = std::isfinite(samples[i].real()) ? samples[i].real() : 0;
        const float im = std::isfinite(samples[i].imag()) ? samples[i].imag() : 0;
        if (std::abs(re) >= 0.995f || std::abs(im) >= 0.995f) ++clipped;
        const float w = float(0.5 - 0.5 * std::cos(2 * pi * double(i) / double(n - 1)));
        v[i] = std::complex<float>(re, im) * w;
    }
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(v[i], v[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const float angle = float(-2 * pi / double(len));
        const std::complex<float> root(std::cos(angle), std::sin(angle));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<float> w(1, 0);
            for (std::size_t j = 0; j < len / 2; ++j) {
                const auto a = v[i + j], b = v[i + j + len / 2] * w;
                v[i + j] = a + b; v[i + j + len / 2] = a - b; w *= root;
            }
        }
    }
    Frame out; out.db.resize(n); out.overloadFraction = float(clipped) / float(n);
    const float gain = 2.0f / float(n - 1);
    for (std::size_t i = 0; i < n; ++i) {
        const float magnitude = std::abs(v[(i + n / 2) % n]) * gain;
        out.db[i] = 20.0f * std::log10((std::max)(magnitude, 1.0e-12f));
    }
    return out;
}

inline std::vector<unsigned char> display(const Frame &frame, float &smoothedFloor, int ampMax = 160) {
    if (frame.db.empty() || ampMax < 1 || ampMax > 255) return {};
    auto sorted = frame.db;
    auto middle = sorted.begin() + sorted.size() / 2;
    std::nth_element(sorted.begin(), middle, sorted.end());
    const float median = *middle;
    if (!std::isfinite(smoothedFloor)) smoothedFloor = median;
    else smoothedFloor += 0.1f * (median - smoothedFloor);
    const float base = smoothedFloor - 6.0f;
    std::vector<unsigned char> out(frame.db.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        const float x = (frame.db[i] - base) * ampMax / 45.0f;
        out[i] = static_cast<unsigned char>(std::lround((std::max)(0.0f, (std::min)(float(ampMax), x))));
    }
    return out;
}
} // namespace SdrSpectrum
