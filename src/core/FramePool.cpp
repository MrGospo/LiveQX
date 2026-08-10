#include "core/FramePool.h"
#include <cassert>
#include <mutex>
#include <vector>

#include "utils/CpuAffinity.h"

// ─── Impl ────────────────────────────────────────────────────────────────────

struct FramePool::Impl {
    std::unique_ptr<uint8_t[]> storage;   // one pre-allocated arena for all frames
    std::vector<uint8_t*>      free_list; // raw pointers into storage
    mutable std::mutex         mutex;
};

// ─── FramePool ────────────────────────────────────────────────────────────────

FramePool::FramePool(int count, int width, int height)
    : count_(count), width_(width), height_(height) {
    assert(count > 0 && width > 0 && height > 0);

    impl_ = std::make_shared<Impl>();

    const size_t frame_bytes = static_cast<size_t>(width) * height * 4; // RGBA
    impl_->storage = std::make_unique<uint8_t[]>(
        static_cast<size_t>(count) * frame_bytes);  // ONE malloc here, never again

    impl_->free_list.reserve(count);
    for (int i = 0; i < count; ++i)
        impl_->free_list.push_back(impl_->storage.get() + i * frame_bytes);
}

Frame FramePool::acquire() {
    uint8_t* ptr = nullptr;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->free_list.empty())
            return Frame{};          // pool exhausted — caller must handle gracefully
        ptr = impl_->free_list.back();
        impl_->free_list.pop_back(); // O(1), no allocation
    }

    // Custom deleter: returns the raw pointer to the free list.
    // Captures impl_ by value — that's a shared_ptr copy (atomic refcount bump),
    // not a heap allocation. The large pixel buffer itself is never freed/reallocated.
    //
    // Note: std::shared_ptr<uint8_t[]>(ptr, deleter) does allocate a small control
    // block (~32 B). For truly zero-allocation acquire(), replace with
    // std::allocate_shared + a fixed-size block pool allocator.
    auto impl = impl_;
    Frame f;
    f.data = std::shared_ptr<uint8_t[]>(
        ptr,
        [impl](uint8_t* p) noexcept {
            std::lock_guard lock(impl->mutex);
            impl->free_list.push_back(p);   // return buffer, O(1), no free()
        });
    f.width  = width_;
    f.height = height_;
    return f;
}

void FramePool::bindMemoryToNumaNode(int node) {
    if (node < 0 || !impl_ || !impl_->storage) return;
    const size_t frame_bytes = static_cast<size_t>(width_) * height_ * 4;
    const size_t total       = static_cast<size_t>(count_) * frame_bytes;
    numa::bindMemoryToNode(impl_->storage.get(), total, node);
}

int FramePool::available() const {
    std::lock_guard lock(impl_->mutex);
    return static_cast<int>(impl_->free_list.size());
}
