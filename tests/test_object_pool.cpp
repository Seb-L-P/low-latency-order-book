#include "lob/object_pool.hpp"
#include "lob/types.hpp"
#include "test_framework.hpp"

using namespace lob;

TEST_CASE(pool_acquire_returns_distinct_slots) {
    ObjectPool<Order> pool(4);
    Order* a = pool.acquire(Order{1, Side::Buy, 100, 10});
    Order* b = pool.acquire(Order{2, Side::Buy, 101, 20});
    CHECK(a != b);
    CHECK_EQ(a->id, static_cast<OrderId>(1));
    CHECK_EQ(b->id, static_cast<OrderId>(2));
    CHECK_EQ(pool.in_use(), static_cast<std::size_t>(2));
    CHECK_EQ(pool.available(), static_cast<std::size_t>(2));
}

TEST_CASE(pool_release_allows_slot_reuse) {
    ObjectPool<Order> pool(2);
    Order* a = pool.acquire(Order{1, Side::Buy, 100, 10});
    pool.release(a);
    CHECK_EQ(pool.available(), static_cast<std::size_t>(2));
    Order* b = pool.acquire(Order{2, Side::Sell, 200, 5});
    CHECK_EQ(pool.in_use(), static_cast<std::size_t>(1));
    CHECK_EQ(b->id, static_cast<OrderId>(2));
}

TEST_CASE(pool_exhaustion_throws) {
    ObjectPool<Order> pool(1);
    pool.acquire(Order{1, Side::Buy, 100, 10});
    bool threw = false;
    try {
        pool.acquire(Order{2, Side::Buy, 100, 10});
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    CHECK(threw);
}
