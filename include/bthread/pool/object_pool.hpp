#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <new>

namespace bthread {

/**
 * @brief Lock-free object pool using Treiber stack (LIFO).
 *
 * Thread Safety:
 * - Acquire(): Safe from multiple threads (lock-free)
 * - Release(): Safe from multiple threads (lock-free)
 *
 * Design:
 * - Uses intrusive linkage: T must have `std::atomic<T*> pool_next` field
 * - LIFO order: better cache locality (recently used objects still hot)
 * - Pool limit: excess objects are deleted, not stored
 *
 * Usage:
 * ```cpp
 * struct MyObject {
 *     std::atomic<MyObject*> pool_next{nullptr};  // Required for pool linkage
 *     // ... other fields ...
 * };
 *
 * ObjectPool<MyObject> pool(64);  // Pool up to 64 objects
 *
 * MyObject* obj = pool.Acquire();  // Get from pool or allocate
 * // ... use object ...
 * pool.Release(obj);               // Return to pool or delete
 * ```
 *
 * @tparam T Object type with `pool_next` atomic pointer field
 */
template<typename T>
class ObjectPool {
public:
    /**
     * @brief Construct pool with maximum size.
     * @param max_size Maximum objects to keep in pool (excess deleted)
     */
    explicit ObjectPool(size_t max_size = 64)
        : max_size_(max_size), head_(nullptr), count_(0) {}

    ~ObjectPool() {
        // Drain pool - delete all cached objects
        T* current = head_.load(std::memory_order_acquire);
        while (current) {
            T* next = current->pool_next.load(std::memory_order_relaxed);
            delete current;
            current = next;
        }
    }

    // Disable copy and move
    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;
    ObjectPool(ObjectPool&&) = delete;
    ObjectPool& operator=(ObjectPool&&) = delete;

    /**
     * @brief Acquire an object from pool.
     * @return Object pointer (from pool or newly allocated)
     *
     * If pool is empty, allocates new object via `new T()`.
     */
    T* Acquire() {
        // Try pop from pool (Treiber stack pop)
        T* obj = head_.load(std::memory_order_acquire);
        while (obj) {
            T* next = obj->pool_next.load(std::memory_order_relaxed);
            if (head_.compare_exchange_weak(obj, next,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                // Successfully popped
                count_.fetch_sub(1, std::memory_order_relaxed);
                obj->pool_next.store(nullptr, std::memory_order_relaxed);
                return obj;
            }
            // CAS failed, retry with new head
        }

        // Pool empty - allocate new
        return new T();
    }

    /**
     * @brief Release an object back to pool.
     * @param obj Object to release (must not be nullptr)
     *
     * If pool is full (count >= max_size), deletes object instead.
     */
    void Release(T* obj) {
        if (!obj) return;

        // Check pool size limit
        size_t current_count = count_.load(std::memory_order_relaxed);
        if (current_count >= max_size_) {
            // Pool full - delete object
            delete obj;
            return;
        }

        // Try push to pool (Treiber stack push)
        obj->pool_next.store(nullptr, std::memory_order_relaxed);
        T* expected = head_.load(std::memory_order_acquire);
        do {
            obj->pool_next.store(expected, std::memory_order_relaxed);
        } while (!head_.compare_exchange_weak(expected, obj,
                std::memory_order_acq_rel, std::memory_order_acquire));

        count_.fetch_add(1, std::memory_order_relaxed);
    }

    /**
     * @brief Get current pool size.
     */
    size_t size() const {
        return count_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Get maximum pool size.
     */
    size_t max_size() const {
        return max_size_;
    }

private:
    size_t max_size_;
    std::atomic<T*> head_{nullptr};
    std::atomic<size_t> count_{0};
};

} // namespace bthread