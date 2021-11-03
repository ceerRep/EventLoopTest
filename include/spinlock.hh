#ifndef _SPINLOCK_HH

#define _SPINLOCK_HH

#include <atomic>

class Spinlock
{
    std::atomic_flag _lock = ATOMIC_FLAG_INIT;

public:
    void lock()
    {
        while (_lock.test_and_set(std::memory_order_acquire))
        { // acquire lock
// Since C++20, it is possible to update atomic_flag's
// value only when there is a chance to acquire the lock.
// See also: https://stackoverflow.com/questions/62318642
#if defined(__cpp_lib_atomic_flag_test)
            while (_lock.test(std::memory_order_relaxed)) // test lock
#endif
                ; // spin
        }
    }

    void unlock()
    {
        _lock.clear(std::memory_order_release);
    }
};

#endif
