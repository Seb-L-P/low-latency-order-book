#pragma once

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "lob/object_pool.hpp"
#include "lob/price_level.hpp"
#include "lob/types.hpp"

namespace lob {

// Price-time priority matching engine over a bounded, contiguous integer
// price range [min_price, max_price]. Both sides get their own flat array
// of PriceLevel, indexed directly by `price - min_price` -- O(1) access to
// any level, no tree traversal. This works because real order books trade
// in a bounded range around the current price for any given instrument (a
// stock doesn't jump from $100 to $10,000 between two consecutive orders),
// so pre-allocating the full range up front is cheap and turns "find this
// price level" from a pointer-chasing tree walk into a single array index.
//
// Cancels are O(1) via an OrderId -> Order* hash lookup, then an O(1)
// intrusive-list unlink (PriceLevel::remove).
//
// The one place this design pays a real cost: when the best bid/ask level
// empties out, finding the *next* best level means scanning outward
// through the array until a non-empty level turns up (refresh_best_after_
// empty below). In real order flow the touch rarely jumps far in a single
// step, so this is cheap on average -- but a book that empties out
// dramatically on one side has a genuine O(range) worst case. A tree/heap
// based design doesn't have that failure mode, at the cost of being slower
// on average due to pointer-chasing and worse cache locality. That's a
// deliberate, stated tradeoff -- see the README -- not an oversight.
class OrderBook {
public:
    OrderBook(Price min_price, Price max_price, std::size_t order_pool_capacity)
        : min_price_(min_price),
          max_price_(max_price),
          bid_levels_(static_cast<std::size_t>(max_price - min_price + 1)),
          ask_levels_(static_cast<std::size_t>(max_price - min_price + 1)),
          order_pool_(order_pool_capacity) {
        if (max_price <= min_price) {
            throw std::invalid_argument("OrderBook: max_price must exceed min_price");
        }
        for (std::size_t i = 0; i < bid_levels_.size(); ++i) {
            bid_levels_[i].price = min_price_ + static_cast<Price>(i);
            ask_levels_[i].price = min_price_ + static_cast<Price>(i);
        }
        best_ask_index_ = static_cast<int>(ask_levels_.size());  // one-past-end sentinel = "no asks"
        order_lookup_.reserve(order_pool_capacity);
    }

    std::vector<Trade> add_limit_order(OrderId id, Side side, Price price, Qty quantity) {
        require_in_range(price);
        std::vector<Trade> trades;
        Qty remaining = match_against_book(id, side, price, quantity, trades);
        if (remaining > 0) {
            rest_order(id, side, price, remaining);
        }
        return trades;
    }

    // Matches against the book regardless of price, up to the opposite
    // side's full depth. Never rests a remainder -- the standard exchange
    // treatment of an unfilled market order.
    std::vector<Trade> add_market_order(OrderId id, Side side, Qty quantity) {
        std::vector<Trade> trades;
        Price unbounded = (side == Side::Buy) ? max_price_ : min_price_;
        match_against_book(id, side, unbounded, quantity, trades);
        return trades;
    }

    bool cancel_order(OrderId id) {
        auto it = order_lookup_.find(id);
        if (it == order_lookup_.end()) return false;
        Order* order = it->second;
        Side side = order->side;
        Price price = order->price;
        PriceLevel& level = level_for(side, price);
        bool was_best = is_best(side, price);
        level.remove(order);
        order_lookup_.erase(it);
        order_pool_.release(order);
        if (was_best && level.empty()) {
            refresh_best_after_empty(side);
        }
        return true;
    }

    // Reduces a resting order's quantity in place, preserving its position
    // in its price level's time-priority queue -- matching real exchange
    // behavior: shrinking an order doesn't cost you your place in line,
    // but growing it or repricing it would (not supported here; that's a
    // cancel + new order on a real exchange too).
    bool reduce_quantity(OrderId id, Qty new_quantity) {
        auto it = order_lookup_.find(id);
        if (it == order_lookup_.end()) return false;
        Order* order = it->second;
        if (new_quantity <= 0 || new_quantity >= order->quantity) return false;
        level_for(order->side, order->price).total_quantity -= (order->quantity - new_quantity);
        order->quantity = new_quantity;
        return true;
    }

    bool has_bid() const { return best_bid_index_ >= 0; }
    bool has_ask() const { return best_ask_index_ < static_cast<int>(ask_levels_.size()); }
    Price best_bid() const { return bid_levels_[static_cast<std::size_t>(best_bid_index_)].price; }
    Price best_ask() const { return ask_levels_[static_cast<std::size_t>(best_ask_index_)].price; }

    Qty depth_at(Side side, Price price) const {
        require_in_range(price);
        return level_for(side, price).total_quantity;
    }

    std::size_t order_count() const { return order_lookup_.size(); }
    std::size_t pool_in_use() const { return order_pool_.in_use(); }

private:
    std::size_t index_of(Price price) const { return static_cast<std::size_t>(price - min_price_); }

    void require_in_range(Price price) const {
        if (price < min_price_ || price > max_price_) {
            throw std::out_of_range("OrderBook: price outside configured book range");
        }
    }

    PriceLevel& level_for(Side side, Price price) {
        return (side == Side::Buy ? bid_levels_ : ask_levels_)[index_of(price)];
    }
    const PriceLevel& level_for(Side side, Price price) const {
        return (side == Side::Buy ? bid_levels_ : ask_levels_)[index_of(price)];
    }

    bool is_best(Side side, Price price) const {
        if (side == Side::Buy) return has_bid() && price == best_bid();
        return has_ask() && price == best_ask();
    }

    // Matches an incoming order of `side` at limit `price` (a market order
    // passes an unbounded price) against the opposite book, walking levels
    // outward from the touch while still marketable. Returns unfilled qty.
    Qty match_against_book(OrderId aggressor_id, Side side, Price price, Qty quantity, std::vector<Trade>& trades) {
        Qty remaining = quantity;
        if (side == Side::Buy) {
            while (remaining > 0 && has_ask() && best_ask() <= price) {
                PriceLevel& level = ask_levels_[static_cast<std::size_t>(best_ask_index_)];
                remaining = drain_level(aggressor_id, level, remaining, trades);
                if (level.empty()) refresh_best_after_empty(Side::Sell);
            }
        } else {
            while (remaining > 0 && has_bid() && best_bid() >= price) {
                PriceLevel& level = bid_levels_[static_cast<std::size_t>(best_bid_index_)];
                remaining = drain_level(aggressor_id, level, remaining, trades);
                if (level.empty()) refresh_best_after_empty(Side::Buy);
            }
        }
        return remaining;
    }

    // Matches the incoming order against resting orders in `level`,
    // front-to-back (time priority), until the incoming quantity or the
    // level itself is exhausted. Frees fully-filled resting orders to the
    // pool.
    Qty drain_level(OrderId aggressor_id, PriceLevel& level, Qty remaining, std::vector<Trade>& trades) {
        while (remaining > 0 && !level.empty()) {
            Order* resting = level.head;
            Qty trade_qty = std::min(remaining, resting->quantity);
            trades.push_back(Trade{aggressor_id, resting->id, level.price, trade_qty});
            remaining -= trade_qty;
            resting->quantity -= trade_qty;
            level.total_quantity -= trade_qty;
            if (resting->quantity == 0) {
                level.remove(resting);
                order_lookup_.erase(resting->id);
                order_pool_.release(resting);
            }
        }
        return remaining;
    }

    void rest_order(OrderId id, Side side, Price price, Qty quantity) {
        Order* order = order_pool_.acquire(Order{id, side, price, quantity});
        level_for(side, price).push_back(order);
        order_lookup_[id] = order;
        int idx = static_cast<int>(index_of(price));
        if (side == Side::Buy) {
            if (!has_bid() || idx > best_bid_index_) best_bid_index_ = idx;
        } else {
            if (!has_ask() || idx < best_ask_index_) best_ask_index_ = idx;
        }
    }

    void refresh_best_after_empty(Side side) {
        if (side == Side::Buy) {
            int idx = best_bid_index_;
            while (idx >= 0 && bid_levels_[static_cast<std::size_t>(idx)].empty()) --idx;
            best_bid_index_ = idx;
        } else {
            int idx = best_ask_index_;
            int n = static_cast<int>(ask_levels_.size());
            while (idx < n && ask_levels_[static_cast<std::size_t>(idx)].empty()) ++idx;
            best_ask_index_ = idx;
        }
    }

    Price min_price_;
    Price max_price_;
    std::vector<PriceLevel> bid_levels_;
    std::vector<PriceLevel> ask_levels_;
    int best_bid_index_ = -1;   // -1 = no resting bids
    int best_ask_index_;        // set to ask_levels_.size() (one-past-end) in the constructor body = "no asks"

    std::unordered_map<OrderId, Order*> order_lookup_;
    ObjectPool<Order> order_pool_;
};

}  // namespace lob
