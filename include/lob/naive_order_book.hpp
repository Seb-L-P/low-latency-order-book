#pragma once

#include <algorithm>
#include <limits>
#include <list>
#include <map>
#include <unordered_map>
#include <vector>

#include "lob/types.hpp"

namespace lob {

// A deliberately "obvious first pass" order book -- the data structures
// most people reach for before thinking about cache behavior or
// allocation cost. std::map<Price, ...> for price levels (a red-black
// tree: O(log n) find, but pointer-chasing through tree nodes scattered
// across the heap, which is exactly what defeats CPU cache prefetching).
// std::list<Order> per level, so every order is its own individually
// heap-allocated node (std::list's default allocator calls `new`/`delete`
// per node), rather than pooled.
//
// This exists purely as an honest baseline for benchmarks/order_book_
// bench.cpp and the differential test in tests/test_differential.cpp --
// same external behavior as OrderBook, verified identical on randomized
// order flow, so the benchmark comparison is actually measuring the
// data-structure/allocation difference and nothing else.
class NaiveOrderBook {
public:
    std::vector<Trade> add_limit_order(OrderId id, Side side, Price price, Qty quantity) {
        std::vector<Trade> trades;
        Qty remaining = match_against_book(id, side, price, quantity, trades);
        if (remaining > 0) {
            rest_order(id, side, price, remaining);
        }
        return trades;
    }

    std::vector<Trade> add_market_order(OrderId id, Side side, Qty quantity) {
        std::vector<Trade> trades;
        Price unbounded = (side == Side::Buy) ? std::numeric_limits<Price>::max() : std::numeric_limits<Price>::min();
        match_against_book(id, side, unbounded, quantity, trades);
        return trades;
    }

    bool cancel_order(OrderId id) {
        auto it = order_lookup_.find(id);
        if (it == order_lookup_.end()) return false;
        Location loc = it->second;
        auto& book_side = (loc.side == Side::Buy) ? bids_ : asks_;
        auto level_it = book_side.find(loc.price);
        level_it->second.erase(loc.it);
        if (level_it->second.empty()) book_side.erase(level_it);
        order_lookup_.erase(it);
        return true;
    }

    bool reduce_quantity(OrderId id, Qty new_quantity) {
        auto it = order_lookup_.find(id);
        if (it == order_lookup_.end()) return false;
        Order& order = *it->second.it;
        if (new_quantity <= 0 || new_quantity >= order.quantity) return false;
        order.quantity = new_quantity;
        return true;
    }

    bool has_bid() const { return !bids_.empty(); }
    bool has_ask() const { return !asks_.empty(); }
    Price best_bid() const { return bids_.rbegin()->first; }
    Price best_ask() const { return asks_.begin()->first; }

    Qty depth_at(Side side, Price price) const {
        const auto& book_side = (side == Side::Buy) ? bids_ : asks_;
        auto it = book_side.find(price);
        if (it == book_side.end()) return 0;
        Qty total = 0;
        for (const Order& o : it->second) total += o.quantity;
        return total;
    }

    std::size_t order_count() const { return order_lookup_.size(); }

private:
    struct Location {
        Price price;
        Side side;
        std::list<Order>::iterator it;
    };

    Qty match_against_book(OrderId aggressor_id, Side side, Price price, Qty quantity, std::vector<Trade>& trades) {
        Qty remaining = quantity;
        if (side == Side::Buy) {
            while (remaining > 0 && !asks_.empty() && asks_.begin()->first <= price) {
                remaining = drain_level(aggressor_id, asks_.begin(), asks_, remaining, trades);
            }
        } else {
            while (remaining > 0 && !bids_.empty() && bids_.rbegin()->first >= price) {
                remaining = drain_level(aggressor_id, std::prev(bids_.end()), bids_, remaining, trades);
            }
        }
        return remaining;
    }

    Qty drain_level(OrderId aggressor_id, std::map<Price, std::list<Order>>::iterator level_it,
                     std::map<Price, std::list<Order>>& book_side, Qty remaining, std::vector<Trade>& trades) {
        std::list<Order>& orders = level_it->second;
        while (remaining > 0 && !orders.empty()) {
            Order& resting = orders.front();
            Qty trade_qty = std::min(remaining, resting.quantity);
            trades.push_back(Trade{aggressor_id, resting.id, level_it->first, trade_qty});
            remaining -= trade_qty;
            resting.quantity -= trade_qty;
            if (resting.quantity == 0) {
                order_lookup_.erase(resting.id);
                orders.pop_front();
            }
        }
        if (orders.empty()) book_side.erase(level_it);
        return remaining;
    }

    void rest_order(OrderId id, Side side, Price price, Qty quantity) {
        auto& book_side = (side == Side::Buy) ? bids_ : asks_;
        std::list<Order>& orders = book_side[price];
        orders.push_back(Order{id, side, price, quantity});
        order_lookup_[id] = Location{price, side, std::prev(orders.end())};
    }

    std::map<Price, std::list<Order>> bids_;
    std::map<Price, std::list<Order>> asks_;
    std::unordered_map<OrderId, Location> order_lookup_;
};

}  // namespace lob
