#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace bench {

struct LatencyStats {
    double p50, p90, p99, p999, max, mean;
    std::size_t n;
};

// Percentiles, not means, are the point of this whole file. A mean can
// look great while hiding a heavy tail (a handful of operations that hit a
// cache miss, a hash collision chain, or -- in the naive book's case -- an
// unlucky tree rebalance), and tail latency is exactly what a low-latency
// system lives or dies on. p99.9 in particular is the number that would
// actually matter in production: it's "1 in 1000 operations," which at
// real trading volumes is a lot of operations per session.
inline LatencyStats compute_stats(std::vector<double> durations_ns) {
    std::sort(durations_ns.begin(), durations_ns.end());
    std::size_t n = durations_ns.size();
    auto pct = [&](double p) {
        std::size_t idx = static_cast<std::size_t>(std::min<double>(n - 1, p * static_cast<double>(n)));
        return durations_ns[idx];
    };
    double sum = 0;
    for (double d : durations_ns) sum += d;
    return LatencyStats{pct(0.50), pct(0.90), pct(0.99), pct(0.999), durations_ns.back(), sum / static_cast<double>(n), n};
}

}  // namespace bench
