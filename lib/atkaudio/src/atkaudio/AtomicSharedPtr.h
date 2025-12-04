#pragma once

#include <atomic>
#include <memory>

namespace atk
{

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

    std::shared_ptr<T> load(std::memory_order order = std::memory_order_seq_cst) const
    {
        auto p = ptr.load(order);
        if (p)
            return *p;
        return std::shared_ptr<T>();
    }

    void store(std::shared_ptr<T> desired, std::memory_order order = std::memory_order_seq_cst)
    {
        auto newPtr = new std::shared_ptr<T>(std::move(desired));
        auto oldPtr = ptr.exchange(newPtr, order);
        delete oldPtr;
    }

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
