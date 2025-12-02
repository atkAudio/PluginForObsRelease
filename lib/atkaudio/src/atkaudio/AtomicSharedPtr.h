#pragma once

#include <atomic>
#include <memory>

namespace atk
{

/**
 * @brief Generic atomic wrapper for shared_ptr to work around std::atomic<std::shared_ptr<T>>
 *        not being supported in Apple's libc++ implementation (as of macOS 15 / Xcode 17).
 *
 * This wrapper provides the same interface as std::atomic<std::shared_ptr<T>> would,
 * but uses std::atomic<std::shared_ptr<T>*> internally, which is universally supported.
 *
 * @tparam T The type pointed to by the shared_ptr
 *
 * @note Once Apple's libc++ supports std::atomic<std::shared_ptr<T>> (check with
 *       __cpp_lib_atomic_shared_ptr feature macro), this wrapper can be replaced.
 */
template <typename T>
class AtomicSharedPtr
{
public:
    AtomicSharedPtr()
        : ptr(nullptr)
    {
    }

    explicit AtomicSharedPtr(std::shared_ptr<T> p)
        : ptr(nullptr)
    {
        store(std::move(p), std::memory_order_relaxed);
    }

    ~AtomicSharedPtr()
    {
        auto p = ptr.exchange(nullptr, std::memory_order_relaxed);
        delete p;
    }

    // Non-copyable and non-movable (like std::atomic)
    AtomicSharedPtr(const AtomicSharedPtr&) = delete;
    AtomicSharedPtr& operator=(const AtomicSharedPtr&) = delete;
    AtomicSharedPtr(AtomicSharedPtr&&) = delete;
    AtomicSharedPtr& operator=(AtomicSharedPtr&&) = delete;

    /**
     * @brief Atomically loads and returns the shared_ptr
     * @param order Memory ordering constraint
     * @return A copy of the stored shared_ptr
     * @note When the returned shared_ptr goes out of scope, it may trigger delete
     */
    std::shared_ptr<T> load(std::memory_order order = std::memory_order_seq_cst) const
    {
        auto p = ptr.load(order);
        if (p)
            return *p;
        return std::shared_ptr<T>();
    }

    /**
     * @brief Atomically stores a new shared_ptr, replacing the old one
     * @param desired The new shared_ptr to store
     * @param order Memory ordering constraint
     */
    void store(std::shared_ptr<T> desired, std::memory_order order = std::memory_order_seq_cst)
    {
        auto newPtr = new std::shared_ptr<T>(std::move(desired));
        auto oldPtr = ptr.exchange(newPtr, order);
        delete oldPtr;
    }

    /**
     * @brief Atomically replaces the stored shared_ptr and returns the old value
     * @param desired The new shared_ptr to store
     * @param order Memory ordering constraint
     * @return The previously stored shared_ptr
     */
    std::shared_ptr<T> exchange(std::shared_ptr<T> desired, std::memory_order order = std::memory_order_seq_cst)
    {
        auto newPtr = new std::shared_ptr<T>(std::move(desired));
        auto oldPtr = ptr.exchange(newPtr, order);
        std::shared_ptr<T> result;
        if (oldPtr)
        {
            result = *oldPtr;
            delete oldPtr;
        }
        return result;
    }

private:
    mutable std::atomic<std::shared_ptr<T>*> ptr;
};

} // namespace atk
