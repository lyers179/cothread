#include "coro/frame_pool.h"
#include <cstdlib>

namespace coro {

FramePool::~FramePool() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (void* block : allocated_blocks_) {
        std::free(block);
    }
}

void FramePool::Init(size_t block_size, size_t initial_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    block_size_ = block_size;

    // Ensure block size is at least sizeof(FreeNode) for intrusive list
    if (block_size_ < sizeof(FreeNode)) {
        block_size_ = sizeof(FreeNode);
    }

    for (size_t i = 0; i < initial_count; ++i) {
        void* block = std::malloc(block_size_);
        if (block) {
            allocated_blocks_.push_back(block);
            FreeNode* node = static_cast<FreeNode*>(block);
            node->next = free_list_;
            free_list_ = node;
        }
    }
}

void* FramePool::Allocate(size_t size) {
    if (size > block_size_ || block_size_ == 0) {
        return nullptr;  // Fall back to heap
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (free_list_) {
        FreeNode* node = free_list_;
        free_list_ = node->next;
        return node;
    }

    // Expand pool
    void* block = std::malloc(block_size_);
    if (block) {
        allocated_blocks_.push_back(block);
    }
    return block;
}

void FramePool::Deallocate(void* block) {
    if (!block) return;

    std::lock_guard<std::mutex> lock(mutex_);
    FreeNode* node = static_cast<FreeNode*>(block);
    node->next = free_list_;
    free_list_ = node;
}

} // namespace coro