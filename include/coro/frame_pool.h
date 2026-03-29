#pragma once

#include <cstddef>
#include <vector>
#include <mutex>
#include <atomic>

namespace coro {

class FramePool {
public:
    FramePool() = default;
    ~FramePool();

    // Non-copyable, non-movable
    FramePool(const FramePool&) = delete;
    FramePool& operator=(const FramePool&) = delete;
    FramePool(FramePool&&) = delete;
    FramePool& operator=(FramePool&&) = delete;

    // Initialize pool with block_size and initial_count
    void Init(size_t block_size, size_t initial_count);

    // Allocate a block (returns nullptr if size > block_size)
    void* Allocate(size_t size);

    // Deallocate a block
    void Deallocate(void* block);

    // Get current block size
    size_t block_size() const { return block_size_; }

private:
    // Free list node (intrusive)
    struct FreeNode {
        FreeNode* next;
    };

    std::atomic<size_t> block_size_{0};
    std::vector<void*> allocated_blocks_;  // All allocated memory
    FreeNode* free_list_{nullptr};
    std::mutex mutex_;
};

} // namespace coro