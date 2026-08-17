#include "lob/price_level.hpp"
#include "lob/types.hpp"
#include "test_framework.hpp"

using namespace lob;

TEST_CASE(price_level_push_back_maintains_fifo_order) {
    PriceLevel level;
    Order a{1, Side::Buy, 100, 10};
    Order b{2, Side::Buy, 100, 20};
    Order c{3, Side::Buy, 100, 30};
    level.push_back(&a);
    level.push_back(&b);
    level.push_back(&c);

    CHECK_EQ(level.head, &a);
    CHECK_EQ(level.tail, &c);
    CHECK_EQ(level.head->next, &b);
    CHECK_EQ(level.head->next->next, &c);
    CHECK_EQ(level.total_quantity, static_cast<Qty>(60));
    CHECK_EQ(level.order_count, 3);
}

TEST_CASE(price_level_remove_from_head) {
    PriceLevel level;
    Order a{1, Side::Buy, 100, 10}, b{2, Side::Buy, 100, 20};
    level.push_back(&a);
    level.push_back(&b);
    level.remove(&a);
    CHECK_EQ(level.head, &b);
    CHECK(b.prev == nullptr);
    CHECK_EQ(level.total_quantity, static_cast<Qty>(20));
}

TEST_CASE(price_level_remove_from_tail) {
    PriceLevel level;
    Order a{1, Side::Buy, 100, 10}, b{2, Side::Buy, 100, 20};
    level.push_back(&a);
    level.push_back(&b);
    level.remove(&b);
    CHECK_EQ(level.tail, &a);
    CHECK(a.next == nullptr);
}

TEST_CASE(price_level_remove_from_middle) {
    PriceLevel level;
    Order a{1, Side::Buy, 100, 10}, b{2, Side::Buy, 100, 20}, c{3, Side::Buy, 100, 30};
    level.push_back(&a);
    level.push_back(&b);
    level.push_back(&c);
    level.remove(&b);
    CHECK_EQ(a.next, &c);
    CHECK_EQ(c.prev, &a);
    CHECK_EQ(level.order_count, 2);
}

TEST_CASE(price_level_remove_only_element_empties_it) {
    PriceLevel level;
    Order a{1, Side::Buy, 100, 10};
    level.push_back(&a);
    level.remove(&a);
    CHECK(level.empty());
    CHECK(level.head == nullptr);
    CHECK(level.tail == nullptr);
}
