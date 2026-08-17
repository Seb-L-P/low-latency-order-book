#include <algorithm>
#include <random>
#include <unordered_map>
#include <vector>

#include "lob/naive_order_book.hpp"
#include "lob/order_book.hpp"
#include "test_framework.hpp"

using namespace lob;

// Differential testing: feed two *independently implemented* order books
// the exact same randomized sequence of adds/cancels/market orders, and
// assert every externally observable thing -- trades generated, best bid/
// ask, order count -- matches at every single step. This is a much
// stronger correctness signal than either book's own unit tests alone:
// the unit tests check specific scenarios I thought to write down, this
// checks that two independent implementations agree across thousands of
// combinations I didn't specifically think of. If OrderBook's array
// indexing or best-price refresh logic had a subtle bug, it would have to
// coincidentally match NaiveOrderBook's completely different (tree-based)
// logic to slip through this test -- vanishingly unlikely for a real bug.

namespace {

bool trades_equal(const std::vector<Trade>& a, const std::vector<Trade>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].aggressor_id != b[i].aggressor_id || a[i].resting_id != b[i].resting_id ||
            a[i].price != b[i].price || a[i].quantity != b[i].quantity) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE(differential_fast_book_matches_naive_book_on_randomized_flow) {
    constexpr Price kMin = 90, kMax = 110;
    constexpr int kNumOps = 20000;

    OrderBook fast(kMin, kMax, static_cast<std::size_t>(kNumOps));
    NaiveOrderBook naive;

    std::mt19937_64 rng(42);
    std::uniform_int_distribution<Price> price_dist(kMin, kMax);
    std::uniform_int_distribution<Qty> qty_dist(1, 20);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int> op_dist(0, 99);

    std::vector<OrderId> live_ids;
    std::unordered_map<OrderId, Qty> remaining_qty;
    OrderId next_id = 1;

    auto apply_fills = [&](const std::vector<Trade>& trades) {
        for (const auto& tr : trades) {
            auto it = remaining_qty.find(tr.resting_id);
            if (it == remaining_qty.end()) continue;
            it->second -= tr.quantity;
            if (it->second <= 0) {
                remaining_qty.erase(it);
                live_ids.erase(std::remove(live_ids.begin(), live_ids.end(), tr.resting_id), live_ids.end());
            }
        }
    };

    for (int i = 0; i < kNumOps; ++i) {
        int op = op_dist(rng);

        if (op < 15 && !live_ids.empty()) {
            std::uniform_int_distribution<std::size_t> pick(0, live_ids.size() - 1);
            std::size_t idx = pick(rng);
            OrderId id = live_ids[idx];
            bool r1 = fast.cancel_order(id);
            bool r2 = naive.cancel_order(id);
            CHECK_EQ(r1, r2);
            remaining_qty.erase(id);
            live_ids[idx] = live_ids.back();
            live_ids.pop_back();
        } else if (op < 20 && !live_ids.empty()) {
            Side side = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
            Qty qty = qty_dist(rng);
            OrderId id = next_id++;
            auto t1 = fast.add_market_order(id, side, qty);
            auto t2 = naive.add_market_order(id, side, qty);
            CHECK(trades_equal(t1, t2));
            apply_fills(t1);
        } else if (op < 25 && !live_ids.empty()) {
            Side side = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
            Price price = price_dist(rng);
            Qty qty = qty_dist(rng);
            OrderId id = next_id++;
            auto t1 = fast.add_ioc_order(id, side, price, qty);
            auto t2 = naive.add_ioc_order(id, side, price, qty);
            CHECK(trades_equal(t1, t2));
            apply_fills(t1);
        } else {
            Side side = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
            Price price = price_dist(rng);
            Qty qty = qty_dist(rng);
            OrderId id = next_id++;
            auto t1 = fast.add_limit_order(id, side, price, qty);
            auto t2 = naive.add_limit_order(id, side, price, qty);
            CHECK(trades_equal(t1, t2));
            apply_fills(t1);

            Qty filled = 0;
            for (const auto& tr : t1) filled += tr.quantity;
            if (filled < qty) {
                live_ids.push_back(id);
                remaining_qty[id] = qty - filled;
            }
        }

        CHECK_EQ(fast.has_bid(), naive.has_bid());
        CHECK_EQ(fast.has_ask(), naive.has_ask());
        if (fast.has_bid()) CHECK_EQ(fast.best_bid(), naive.best_bid());
        if (fast.has_ask()) CHECK_EQ(fast.best_ask(), naive.best_ask());
        CHECK_EQ(fast.order_count(), naive.order_count());

        // Full L2 depth comparison every few hundred ops (it's O(book) per
        // call, so doing it every op would dominate the test's runtime
        // without adding much: depth errors don't self-repair, they linger).
        if (i % 250 == 0) {
            for (Side side : {Side::Buy, Side::Sell}) {
                auto l1 = fast.top_levels(side, 10);
                auto l2 = naive.top_levels(side, 10);
                CHECK_EQ(l1.size(), l2.size());
                for (std::size_t k = 0; k < l1.size(); ++k) {
                    CHECK_EQ(l1[k].price, l2[k].price);
                    CHECK_EQ(l1[k].quantity, l2[k].quantity);
                    CHECK_EQ(l1[k].order_count, l2[k].order_count);
                }
            }
        }
    }
}
