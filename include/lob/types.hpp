#pragma once

#include <cstdint>

namespace lob {

// Prices are integer tick counts, never floating point. A double can't
// represent most decimal prices exactly (e.g. 0.1 has no exact binary
// representation), which means naive float comparisons ("is this the best
// price level") can silently misbehave right at the boundary between two
// price levels -- exactly where a matching engine can least afford it.
// Real exchanges represent price the same way: an integer number of ticks,
// with the tick size (e.g. $0.01) applied only at the display/reporting
// boundary, never inside the matching logic itself.
using Price = std::int64_t;
using Qty = std::int64_t;
using OrderId = std::uint64_t;

enum class Side : std::uint8_t { Buy, Sell };

struct Trade {
    OrderId aggressor_id;
    OrderId resting_id;
    Price price;   // always the resting order's price, not the aggressor's --
                    // standard price-time-priority matching rule: the
                    // passive side sets the execution price.
    Qty quantity;
};

// One aggregated price level as seen from outside the book -- what a
// market-data feed would publish as L2 depth. Deliberately a plain value
// type with no pointers into book internals, so a snapshot stays valid
// after the book mutates.
struct LevelView {
    Price price;
    Qty quantity;      // total resting quantity at this price
    int order_count;   // number of resting orders at this price
};

// One resting order. Intentionally lean and pointer-linked (no owned
// containers inside) so it fits cheaply in an intrusive doubly-linked list
// per price level -- see price_level.hpp.
struct Order {
    OrderId id;
    Side side;
    Price price;
    Qty quantity;   // remaining (unfilled) quantity
    Order* prev = nullptr;
    Order* next = nullptr;
};

}  // namespace lob
