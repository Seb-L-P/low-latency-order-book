#include "lob/order_book.hpp"
#include "test_framework.hpp"

using namespace lob;

namespace {
OrderBook make_book() { return OrderBook(90, 110, 100); }
}  // namespace

TEST_CASE(resting_non_crossing_order_updates_best_bid) {
    OrderBook book = make_book();
    auto trades = book.add_limit_order(1, Side::Buy, 100, 10);
    CHECK(trades.empty());
    CHECK(book.has_bid());
    CHECK(!book.has_ask());
    CHECK_EQ(book.best_bid(), static_cast<Price>(100));
}

TEST_CASE(crossing_order_trades_at_resting_price_not_aggressor_price) {
    OrderBook book = make_book();
    book.add_limit_order(1, Side::Sell, 101, 5);
    auto trades = book.add_limit_order(2, Side::Buy, 103, 5);  // aggressor willing to pay 103
    CHECK_EQ(trades.size(), static_cast<std::size_t>(1));
    CHECK_EQ(trades[0].price, static_cast<Price>(101));  // executes at the resting (passive) price
    CHECK_EQ(trades[0].aggressor_id, static_cast<OrderId>(2));
    CHECK_EQ(trades[0].resting_id, static_cast<OrderId>(1));
    CHECK_EQ(trades[0].quantity, static_cast<Qty>(5));
    CHECK(!book.has_ask());  // fully filled, nothing left resting
}

TEST_CASE(partial_fill_walks_multiple_resting_orders_in_time_priority) {
    OrderBook book = make_book();
    book.add_limit_order(1, Side::Sell, 100, 5);  // first in queue
    book.add_limit_order(2, Side::Sell, 100, 5);  // second in queue
    auto trades = book.add_limit_order(3, Side::Buy, 100, 7);

    CHECK_EQ(trades.size(), static_cast<std::size_t>(2));
    CHECK_EQ(trades[0].resting_id, static_cast<OrderId>(1));  // time priority: id 1 first
    CHECK_EQ(trades[0].quantity, static_cast<Qty>(5));
    CHECK_EQ(trades[1].resting_id, static_cast<OrderId>(2));
    CHECK_EQ(trades[1].quantity, static_cast<Qty>(2));
    CHECK_EQ(book.depth_at(Side::Sell, 100), static_cast<Qty>(3));  // id 2 has 3 left
}

TEST_CASE(incoming_order_walks_multiple_price_levels) {
    OrderBook book = make_book();
    book.add_limit_order(1, Side::Sell, 100, 5);
    book.add_limit_order(2, Side::Sell, 101, 5);
    auto trades = book.add_limit_order(3, Side::Buy, 101, 8);

    CHECK_EQ(trades.size(), static_cast<std::size_t>(2));
    CHECK_EQ(trades[0].price, static_cast<Price>(100));
    CHECK_EQ(trades[0].quantity, static_cast<Qty>(5));
    CHECK_EQ(trades[1].price, static_cast<Price>(101));
    CHECK_EQ(trades[1].quantity, static_cast<Qty>(3));
    CHECK_EQ(book.best_ask(), static_cast<Price>(101));  // 2 units left resting there
}

TEST_CASE(unfilled_remainder_rests_at_incoming_orders_own_price) {
    OrderBook book = make_book();
    book.add_limit_order(1, Side::Sell, 100, 5);
    auto trades = book.add_limit_order(2, Side::Buy, 100, 20);

    CHECK_EQ(trades.size(), static_cast<std::size_t>(1));
    CHECK(!book.has_ask());
    CHECK(book.has_bid());
    CHECK_EQ(book.best_bid(), static_cast<Price>(100));
    CHECK_EQ(book.depth_at(Side::Buy, 100), static_cast<Qty>(15));
}

TEST_CASE(cancel_frees_pool_slot_and_removes_from_book) {
    OrderBook book = make_book();
    book.add_limit_order(1, Side::Buy, 100, 10);
    CHECK_EQ(book.pool_in_use(), static_cast<std::size_t>(1));

    CHECK(book.cancel_order(1));
    CHECK_EQ(book.pool_in_use(), static_cast<std::size_t>(0));
    CHECK_EQ(book.order_count(), static_cast<std::size_t>(0));
    CHECK(!book.has_bid());

    CHECK(!book.cancel_order(1));    // already canceled
    CHECK(!book.cancel_order(999));  // never existed
}

TEST_CASE(cancel_of_touch_level_refreshes_best_price) {
    OrderBook book = make_book();
    book.add_limit_order(1, Side::Buy, 100, 5);
    book.add_limit_order(2, Side::Buy, 99, 5);
    CHECK_EQ(book.best_bid(), static_cast<Price>(100));

    book.cancel_order(1);
    CHECK(book.has_bid());
    CHECK_EQ(book.best_bid(), static_cast<Price>(99));
}

TEST_CASE(reduce_quantity_preserves_time_priority) {
    OrderBook book = make_book();
    book.add_limit_order(1, Side::Sell, 100, 10);  // first in queue
    book.add_limit_order(2, Side::Sell, 100, 10);  // second in queue
    CHECK(book.reduce_quantity(1, 3));              // shrink the first order

    auto trades = book.add_limit_order(3, Side::Buy, 100, 3);
    CHECK_EQ(trades.size(), static_cast<std::size_t>(1));
    CHECK_EQ(trades[0].resting_id, static_cast<OrderId>(1));  // still matched first despite being smaller
}

TEST_CASE(reduce_quantity_rejects_non_reductions) {
    OrderBook book = make_book();
    book.add_limit_order(1, Side::Buy, 100, 10);
    CHECK(!book.reduce_quantity(1, 10));  // not a reduction
    CHECK(!book.reduce_quantity(1, 15));  // an increase
    CHECK(!book.reduce_quantity(1, 0));   // non-positive
    CHECK(!book.reduce_quantity(999, 1)); // doesn't exist
}

TEST_CASE(market_order_walks_book_and_never_rests_unfilled_remainder) {
    OrderBook book = make_book();
    book.add_limit_order(1, Side::Sell, 100, 5);
    book.add_limit_order(2, Side::Sell, 101, 5);
    auto trades = book.add_market_order(3, Side::Buy, 20);  // more than the book's total depth (10)

    CHECK_EQ(trades.size(), static_cast<std::size_t>(2));
    Qty total_traded = trades[0].quantity + trades[1].quantity;
    CHECK_EQ(total_traded, static_cast<Qty>(10));
    CHECK(!book.has_ask());
    CHECK_EQ(book.order_count(), static_cast<std::size_t>(0));  // remainder did not rest
}

TEST_CASE(out_of_range_price_throws) {
    OrderBook book = make_book();
    bool threw = false;
    try {
        book.add_limit_order(1, Side::Buy, 1000, 10);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    CHECK(threw);
}
