// include/bthread/platform/stack.hpp
/**
 * @file stack.hpp
 * @brief Stack allocation for bthreads.
 *
 * Provides stack allocation and deallocation with guard pages
 * for stack overflow detection.
 */

#pragma once

#include <cstddef>

namespace bthread {
namespace platform {

/**
 * @brief Allocate a stack with guard page.
 *
 * Allocates memory for a bthread stack with a guard page at the
 * beginning to detect stack overflows.
 *
 * @param size Stack size in bytes (will be rounded up to page size)
 * @return Pointer to stack memory, or nullptr on failure
 */
void* AllocateStack(size_t size);

/**
 * @brief Deallocate a stack.
 *
 * Frees memory allocated by AllocateStack.
 *
 * @param stack Pointer to stack memory
 * @param size Stack size (must match allocation)
 */
void DeallocateStack(void* stack, size_t size);

/**
 * @brief RAII wrapper for stack allocation.
 */
class Stack {
public:
    explicit Stack(size_t size = DEFAULT_STACK_SIZE);
    ~Stack();

    // Disable copy
    Stack(const Stack&) = delete;
    Stack& operator=(const Stack&) = delete;

    // Allow move
    Stack(Stack&& other) noexcept;
    Stack& operator=(Stack&& other) noexcept;

    /// Get stack data pointer
    void* data() const { return data_; }

    /// Get stack size
    size_t size() const { return size_; }

    /// Check if valid
    bool valid() const { return data_ != nullptr; }

private:
    static constexpr size_t DEFAULT_STACK_SIZE = 1024 * 1024;  // 1MB

    void* data_{nullptr};
    size_t size_{0};
};

} // namespace platform
} // namespace bthread