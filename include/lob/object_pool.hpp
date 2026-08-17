#pragma once

#include <cstddef>
#include <new>
#include <utility>
#include <vector>

namespace lob {

// Fixed-capacity pool allocator. `new`/`delete` on the hot path is exactly
// what a low-latency system avoids: the general-purpose heap allocator can
// take a lock, walk free lists of varying-size blocks, and touch memory
// that's cold in cache -- none of which has a predictable, bounded cost.
// This pool instead grabs one large block up front, and acquire()/release()
// are both just a vector push/pop plus a placement-new or explicit
// destructor call -- O(1) with no allocator call on the hot path at all.
//
// Fixed capacity is a deliberate constraint, not an oversight: a real
// matching engine sizes its order pool to the maximum resting-order count
// it's willing to support and fails loudly (here, throws) if that's
// exceeded, rather than silently falling back to slow, unbounded
// allocation exactly when the book is busiest.
template <typename T>
class ObjectPool {
public:
    explicit ObjectPool(std::size_t capacity)
        : storage_(static_cast<T*>(::operator new(capacity * sizeof(T)))), capacity_(capacity) {
        free_list_.reserve(capacity);
        for (std::size_t i = capacity; i-- > 0;) {
            free_list_.push_back(storage_ + i);
        }
    }

    ~ObjectPool() { ::operator delete(storage_); }

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    template <typename... Args>
    T* acquire(Args&&... args) {
        if (free_list_.empty()) {
            throw std::bad_alloc();
        }
        T* slot = free_list_.back();
        free_list_.pop_back();
        return ::new (static_cast<void*>(slot)) T(std::forward<Args>(args)...);
    }

    void release(T* obj) {
        obj->~T();
        free_list_.push_back(obj);
    }

    std::size_t capacity() const { return capacity_; }
    std::size_t available() const { return free_list_.size(); }
    std::size_t in_use() const { return capacity_ - free_list_.size(); }

private:
    T* storage_;
    std::size_t capacity_;
    std::vector<T*> free_list_;
};

}  // namespace lob
