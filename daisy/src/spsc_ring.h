// Single-producer single-consumer ring between an interrupt or the audio callback and the main loop (gReplies: the
// main loop on both ends). N: power of two.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

template <class T, size_t N> class SpscRing {
    static_assert(N && (N & (N - 1)) == 0, "N must be a power of two");

public:
    bool push(const T& v) {
        const uint32_t h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) == N) return false;
        buf_[h & (N - 1)] = v;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }
    bool pop(T& v) {
        const uint32_t t = tail_.load(std::memory_order_relaxed);
        if (head_.load(std::memory_order_acquire) == t) return false;
        v = buf_[t & (N - 1)];
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }
    // Consumer only: the oldest element, left in the ring, or nullptr when empty.
    const T* front() const {
        const uint32_t t = tail_.load(std::memory_order_relaxed);
        return head_.load(std::memory_order_acquire) == t ? nullptr : &buf_[t & (N - 1)];
    }

private:
    T buf_[N];
    std::atomic<uint32_t> head_{0}, tail_{0};
};
