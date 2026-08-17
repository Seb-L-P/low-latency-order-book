#pragma once

#include "lob/types.hpp"

namespace lob {

// All resting orders at one price, in strict time priority: oldest at the
// front, newest at the back. "Intrusive" means the linked-list pointers
// (Order::prev/next) live inside the Order object itself rather than in a
// separate node wrapper (as std::list would use) -- one fewer allocation
// per order, and no extra pointer indirection to reach the payload once
// you're at a node. push_back, and removal of *any* node given a pointer
// to it (front, back, or middle), are both O(1): exactly the two
// operations a matching engine needs on its hot path (a new order joins
// the back of the queue; a fill or cancel can remove any order in it).
class PriceLevel {
public:
    Price price = 0;
    Qty total_quantity = 0;  // sum of resting quantity, maintained incrementally for O(1) depth queries
    int order_count = 0;
    Order* head = nullptr;   // front of queue = highest time priority
    Order* tail = nullptr;   // back of queue = most recently added

    bool empty() const { return head == nullptr; }

    void push_back(Order* order) {
        order->prev = tail;
        order->next = nullptr;
        if (tail != nullptr) {
            tail->next = order;
        } else {
            head = order;
        }
        tail = order;
        total_quantity += order->quantity;
        ++order_count;
    }

    // Unlinks `order` from wherever it sits in the list. Does not free it --
    // that's the pool's job, once the caller is done with it.
    void remove(Order* order) {
        if (order->prev != nullptr) {
            order->prev->next = order->next;
        } else {
            head = order->next;
        }
        if (order->next != nullptr) {
            order->next->prev = order->prev;
        } else {
            tail = order->prev;
        }
        total_quantity -= order->quantity;
        --order_count;
        order->prev = order->next = nullptr;
    }
};

}  // namespace lob
