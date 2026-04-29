#pragma once

#include <atomic>
#include <memory>
#include <vector>

namespace atk
{

// Atomic shared_ptr wrapper compatible with Apple libc++ (which lacks std::atomic<shared_ptr>).
// Uses a spinlock for synchronization - safe for infrequent updates from UI thread
// with frequent reads from audio thread.
//
// THREAD SAFETY:
// - All operations are thread-safe
// - Readers spin briefly if a write is in progress (nanoseconds)
//
// DESTRUCTION SAFETY:
// - Old values are held by the writer until the next store()
// - This ensures destruction happens on the writer thread, not reader
//
// USAGE CONTRACT:
// - Writers should be infrequent (UI thread updates)
// - Readers can be frequent (audio thread)
//
// NOTE: memory_order parameters are accepted for API compatibility but ignored.
template <typename T>
class AtomicSharedPtr
{
public:
    AtomicSharedPtr()
        : spinlock(new std::atomic<bool>(false))
    {
    }

    explicit AtomicSharedPtr(std::shared_ptr<T> p)
        : ptr(std::move(p))
        , spinlock(new std::atomic<bool>(false))
    {
    }

    ~AtomicSharedPtr()
    {
        delete spinlock;
    }

    // Non-copyable and non-movable
    AtomicSharedPtr(const AtomicSharedPtr&) = delete;
    AtomicSharedPtr& operator=(const AtomicSharedPtr&) = delete;
    AtomicSharedPtr(AtomicSharedPtr&&) = delete;
    AtomicSharedPtr& operator=(AtomicSharedPtr&&) = delete;

    std::shared_ptr<T> load([[maybe_unused]] std::memory_order order = std::memory_order_acquire) const
    {
        lock();
        auto result = ptr;
        unlock();
        return result;
    }

    void store(std::shared_ptr<T> desired, [[maybe_unused]] std::memory_order order = std::memory_order_release)
    {
        (void)exchange(std::move(desired));
    }

    [[nodiscard]] std::shared_ptr<T>
    exchange(std::shared_ptr<T> desired, [[maybe_unused]] std::memory_order order = std::memory_order_acq_rel)
    {
        lock();
        auto old = std::move(ptr);
        ptr = std::move(desired);
        unlock();

        // Keep old values alive to ensure destruction happens here (writer thread),
        // not when reader's copy goes out of scope.
        if (old)
            retained.push_back(std::move(old));

        // Remove entries where we're the sole owner (refcount == 1)
        // Safe to delete - no readers have copies
        auto it = retained.begin();
        while (it != retained.end())
            if (it->use_count() == 1)
                it = retained.erase(it);
            else
                ++it;

        return retained.empty() ? nullptr : retained.back();
    }

private:
    void lock() const
    {
        bool expected = false;
        while (!spinlock->compare_exchange_weak(expected, true, std::memory_order_acquire))
        {
            expected = false;
            while (spinlock->load(std::memory_order_relaxed))
                ;
        }
    }

    void unlock() const
    {
        spinlock->store(false, std::memory_order_release);
    }

    std::shared_ptr<T> ptr;
    std::atomic<bool>* spinlock;

    // Prevent destruction on reader thread by keeping old values alive
    std::vector<std::shared_ptr<T>> retained;
};

} // namespace atk
