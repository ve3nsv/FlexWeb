#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

// Correlate uniformly sampled audio/spectrum envelopes. The caller supplies
// a common, monotonic sampling clock (e.g. 100 Hz) and must reset when the
// radio, source, rate, or passband changes. Positive lag means the second
// envelope arrives later than the first.
namespace ScopeSync {
struct Estimate { double lagMs, correlation; };
inline std::optional<Estimate> estimate(const std::vector<float> &first,
                                        const std::vector<float> &second,
                                        double sampleRateHz,
                                        int maxLagSamples) {
    if (first.size() != second.size() || first.size() < 500 || !std::isfinite(sampleRateHz)
        || sampleRateHz <= 0 || maxLagSamples < 1 || maxLagSamples > 500
        || first.size() < std::size_t(2 * maxLagSamples + 500)) return std::nullopt;
    for (std::size_t i = 0; i < first.size(); ++i)
        if (!std::isfinite(first[i]) || !std::isfinite(second[i])) return std::nullopt;
    std::vector<double> scores(2 * maxLagSamples + 1, -2.0);
    for (int lag = -maxLagSamples; lag <= maxLagSamples; ++lag) {
        const std::size_t begin = std::size_t(maxLagSamples);
        const std::size_t end = first.size() - std::size_t(maxLagSamples);
        double sx=0,sy=0,sxx=0,syy=0,sxy=0;
        for (std::size_t i=begin; i<end; ++i) {
            const double x=first[i], y=second[std::size_t(int(i)+lag)];
            sx+=x; sy+=y; sxx+=x*x; syy+=y*y; sxy+=x*y;
        }
        const double count=double(end-begin), vx=sxx-sx*sx/count, vy=syy-sy*sy/count;
        if (vx > 1e-8 && vy > 1e-8)
            scores[lag+maxLagSamples]=(sxy-sx*sy/count)/std::sqrt(vx*vy);
    }
    const auto best=std::max_element(scores.begin(),scores.end());
    const int index=int(best-scores.begin()), lag=index-maxLagSamples;
    // A peak touching the search boundary cannot establish the real lag.
    if (index==0 || index==int(scores.size())-1 || *best<0.55) return std::nullopt;
    double competitor=-2;
    for(int i=0;i<int(scores.size());++i)
        if (std::abs(i-index)>10) competitor=(std::max)(competitor,scores[i]);
    // Repeated or flat envelopes yield ambiguous peaks; keep the last good
    // offset in the caller instead of tracking a random match.
    if (*best-competitor<0.08) return std::nullopt;
    return Estimate{1000.0*lag/sampleRateHz,*best};
}
} // namespace ScopeSync
