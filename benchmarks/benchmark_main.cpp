#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "lob/naive_order_book.hpp"
#include "lob/order_book.hpp"
#include "stats.hpp"

using namespace lob;
using namespace bench;
using Clock = std::chrono::steady_clock;

namespace {

constexpr Price kMinPrice = 0;
constexpr Price kMaxPrice = 20000;
constexpr Price kMid = (kMinPrice + kMaxPrice) / 2;
constexpr Price kHalfGap = 500;             // keeps non-crossing benchmarks genuinely non-crossing
constexpr Price kBidHigh = kMid - kHalfGap;  // bids rest in [kMinPrice, kBidHigh]
constexpr Price kAskLow = kMid + kHalfGap;   // asks rest in [kAskLow, kMaxPrice]

double to_ns(Clock::duration d) { return std::chrono::duration<double, std::nano>(d).count(); }

template <typename Book>
std::vector<OrderId> prefill(Book& book, std::mt19937_64& rng, OrderId& next_id, int n) {
    std::uniform_int_distribution<Price> bid_price(kMinPrice, kBidHigh);
    std::uniform_int_distribution<Price> ask_price(kAskLow, kMaxPrice);
    std::uniform_int_distribution<Qty> qty(1, 100);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::vector<OrderId> ids;
    ids.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        Side side = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
        Price price = (side == Side::Buy) ? bid_price(rng) : ask_price(rng);
        OrderId id = next_id++;
        book.add_limit_order(id, side, price, qty(rng));
        ids.push_back(id);
    }
    return ids;
}

// Pure insertion cost: every order here rests, none of them cross, so
// matching logic never runs -- this isolates the array+pool vs. tree+heap
// data-structure cost specifically.
template <typename Book>
LatencyStats benchmark_add(Book& book, std::mt19937_64& rng, OrderId& next_id, int n) {
    std::uniform_int_distribution<Price> bid_price(kMinPrice, kBidHigh);
    std::uniform_int_distribution<Price> ask_price(kAskLow, kMaxPrice);
    std::uniform_int_distribution<Qty> qty(1, 100);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::vector<double> durations;
    durations.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        Side side = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
        Price price = (side == Side::Buy) ? bid_price(rng) : ask_price(rng);
        Qty q = qty(rng);
        OrderId id = next_id++;
        auto t0 = Clock::now();
        book.add_limit_order(id, side, price, q);
        auto t1 = Clock::now();
        durations.push_back(to_ns(t1 - t0));
    }
    return compute_stats(std::move(durations));
}

template <typename Book>
LatencyStats benchmark_cancel(Book& book, std::mt19937_64& rng, std::vector<OrderId> ids) {
    std::shuffle(ids.begin(), ids.end(), rng);
    std::vector<double> durations;
    durations.reserve(ids.size());
    for (OrderId id : ids) {
        auto t0 = Clock::now();
        book.cancel_order(id);
        auto t1 = Clock::now();
        durations.push_back(to_ns(t1 - t0));
    }
    return compute_stats(std::move(durations));
}

// Marketable orders that actually walk the book and generate fills.
// Liquidity is replenished (untimed) whenever a side runs dry, so the
// benchmark never stalls waiting for a book that's run out of depth.
template <typename Book>
LatencyStats benchmark_cross(Book& book, std::mt19937_64& rng, OrderId& next_id, int n) {
    std::uniform_int_distribution<Qty> qty(1, 50);
    std::uniform_int_distribution<int> side_dist(0, 1);

    auto replenish = [&](Side liquidity_side) {
        std::uniform_int_distribution<Price> price(liquidity_side == Side::Buy ? kMinPrice : kAskLow,
                                                     liquidity_side == Side::Buy ? kBidHigh : kMaxPrice);
        std::uniform_int_distribution<Qty> replenish_qty(1, 100);
        for (int j = 0; j < 200; ++j) {
            book.add_limit_order(next_id++, liquidity_side, price(rng), replenish_qty(rng));
        }
    };
    replenish(Side::Buy);
    replenish(Side::Sell);

    std::vector<double> durations;
    durations.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        Side aggressor_side = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
        if (aggressor_side == Side::Buy && !book.has_ask()) replenish(Side::Sell);
        if (aggressor_side == Side::Sell && !book.has_bid()) replenish(Side::Buy);

        Price marketable_price = (aggressor_side == Side::Buy) ? kMaxPrice : kMinPrice;
        OrderId id = next_id++;

        auto t0 = Clock::now();
        book.add_limit_order(id, aggressor_side, marketable_price, qty(rng));
        auto t1 = Clock::now();
        durations.push_back(to_ns(t1 - t0));
    }
    return compute_stats(std::move(durations));
}

// A realistic mixed workload -- mostly new resting orders, a meaningful
// share of cancels (real markets see far more cancels than fills), and a
// small share of marketable orders that actually cross. Timed as one
// block (wall clock / n), since throughput -- not any single operation's
// latency -- is the question here.
template <typename Book>
double benchmark_throughput(Book& book, std::mt19937_64& rng, OrderId& next_id, int n) {
    std::uniform_int_distribution<Price> bid_price(kMinPrice, kBidHigh);
    std::uniform_int_distribution<Price> ask_price(kAskLow, kMaxPrice);
    std::uniform_int_distribution<Qty> qty(1, 100);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int> op_dist(0, 99);  // 15% cancel, 5% market, 80% new limit

    std::vector<OrderId> live_ids;
    live_ids.reserve(static_cast<std::size_t>(n));

    auto t0 = Clock::now();
    for (int i = 0; i < n; ++i) {
        int op = op_dist(rng);
        if (op < 15 && !live_ids.empty()) {
            std::uniform_int_distribution<std::size_t> pick(0, live_ids.size() - 1);
            std::size_t idx = pick(rng);
            book.cancel_order(live_ids[idx]);
            live_ids[idx] = live_ids.back();
            live_ids.pop_back();
        } else if (op < 20) {
            Side side = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
            book.add_market_order(next_id++, side, qty(rng));
        } else {
            Side side = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
            Price price = (side == Side::Buy) ? bid_price(rng) : ask_price(rng);
            OrderId id = next_id++;
            book.add_limit_order(id, side, price, qty(rng));
            live_ids.push_back(id);
        }
    }
    auto t1 = Clock::now();
    return static_cast<double>(n) / std::chrono::duration<double>(t1 - t0).count();
}

void print_row(const std::string& label, const LatencyStats& s) {
    std::printf("  %-26s p50=%7.0f  p90=%7.0f  p99=%8.0f  p99.9=%9.0f  max=%10.0f  mean=%8.0f\n", label.c_str(), s.p50,
                s.p90, s.p99, s.p999, s.max, s.mean);
}

void write_latency_csv(const std::string& path, const std::vector<std::pair<std::string, LatencyStats>>& rows) {
    std::ofstream out(path);
    out << "benchmark,p50_ns,p90_ns,p99_ns,p999_ns,max_ns,mean_ns,n\n";
    for (const auto& [name, s] : rows) {
        out << name << "," << s.p50 << "," << s.p90 << "," << s.p99 << "," << s.p999 << "," << s.max << "," << s.mean
            << "," << s.n << "\n";
    }
}

}  // namespace

int main() {
    constexpr int kPrefillCount = 50'000;
    constexpr int kLatencyOps = 100'000;
    constexpr int kCrossOps = 10'000;
    constexpr int kThroughputOps = 500'000;
    constexpr std::size_t kPoolCapacity = 2'000'000;

    std::vector<std::pair<std::string, LatencyStats>> results;

    std::cout << "=== Latency (nanoseconds; all figures are percentiles across " << kLatencyOps << " timed ops) ===\n\n";

    {
        std::cout << "-- Add (non-crossing limit order) --\n";
        std::mt19937_64 rng_fast(1), rng_naive(1);
        OrderBook fast(kMinPrice, kMaxPrice, kPoolCapacity);
        NaiveOrderBook naive;
        OrderId id_fast = 1, id_naive = 1;
        prefill(fast, rng_fast, id_fast, kPrefillCount);
        prefill(naive, rng_naive, id_naive, kPrefillCount);
        auto s_fast = benchmark_add(fast, rng_fast, id_fast, kLatencyOps);
        auto s_naive = benchmark_add(naive, rng_naive, id_naive, kLatencyOps);
        print_row("OrderBook (array+pool)", s_fast);
        print_row("NaiveOrderBook (map+heap)", s_naive);
        results.emplace_back("add_fast", s_fast);
        results.emplace_back("add_naive", s_naive);
    }

    {
        std::cout << "\n-- Cancel --\n";
        std::mt19937_64 rng_fast(2), rng_naive(2);
        OrderBook fast(kMinPrice, kMaxPrice, kPoolCapacity);
        NaiveOrderBook naive;
        OrderId id_fast = 1, id_naive = 1;
        auto ids_fast = prefill(fast, rng_fast, id_fast, kPrefillCount);
        auto ids_naive = prefill(naive, rng_naive, id_naive, kPrefillCount);
        auto s_fast = benchmark_cancel(fast, rng_fast, ids_fast);
        auto s_naive = benchmark_cancel(naive, rng_naive, ids_naive);
        print_row("OrderBook (array+pool)", s_fast);
        print_row("NaiveOrderBook (map+heap)", s_naive);
        results.emplace_back("cancel_fast", s_fast);
        results.emplace_back("cancel_naive", s_naive);
    }

    {
        std::cout << "\n-- Crossing (marketable) order --\n";
        std::mt19937_64 rng_fast(3), rng_naive(3);
        OrderBook fast(kMinPrice, kMaxPrice, kPoolCapacity);
        NaiveOrderBook naive;
        OrderId id_fast = 1, id_naive = 1;
        auto s_fast = benchmark_cross(fast, rng_fast, id_fast, kCrossOps);
        auto s_naive = benchmark_cross(naive, rng_naive, id_naive, kCrossOps);
        print_row("OrderBook (array+pool)", s_fast);
        print_row("NaiveOrderBook (map+heap)", s_naive);
        results.emplace_back("cross_fast", s_fast);
        results.emplace_back("cross_naive", s_naive);
    }

    write_latency_csv("results/latency_benchmarks.csv", results);

    std::cout << "\n=== Throughput (mixed realistic workload, " << kThroughputOps << " ops) ===\n\n";
    {
        std::mt19937_64 rng_fast(4), rng_naive(4);
        OrderBook fast(kMinPrice, kMaxPrice, kPoolCapacity);
        NaiveOrderBook naive;
        OrderId id_fast = 1, id_naive = 1;
        prefill(fast, rng_fast, id_fast, kPrefillCount);
        prefill(naive, rng_naive, id_naive, kPrefillCount);
        double tp_fast = benchmark_throughput(fast, rng_fast, id_fast, kThroughputOps);
        double tp_naive = benchmark_throughput(naive, rng_naive, id_naive, kThroughputOps);
        std::printf("  %-26s %14.0f ops/sec\n", "OrderBook (array+pool)", tp_fast);
        std::printf("  %-26s %14.0f ops/sec\n", "NaiveOrderBook (map+heap)", tp_naive);
        std::printf("  Speedup: %.2fx\n", tp_fast / tp_naive);

        std::ofstream out("results/throughput.csv");
        out << "book,ops_per_sec\n";
        out << "fast," << tp_fast << "\n";
        out << "naive," << tp_naive << "\n";
    }

    std::cout << "\nWrote results/latency_benchmarks.csv and results/throughput.csv\n";
    return 0;
}
