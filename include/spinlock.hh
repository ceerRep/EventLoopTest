#ifndef _SPINLOCK_HH

#define _SPINLOCK_HH

#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <thread>

inline static int (*get_loop_id)();

class Spinlock
{
    inline static thread_local int sign = 0;
    int32_t _lock = -1;
    int32_t num = 0;

public:
    void lock()
    {
        while (!try_lock())
        {
            ;
        }
    }

    void unlock()
    {
        if (_lock != (int32_t)(intptr_t)(&sign))
            throw std::runtime_error("Unlock in wrong thread");

        num--;

        if (num < 0)
            throw std::runtime_error("Lock over unlocked");

        if (!num)
            _lock = -1;
    }

    bool try_lock()
    {
        int32_t dfl_id = -1, loop_id = (int32_t)(intptr_t)(&sign);
        if (_lock == loop_id)
        {
            num++;
            return true;
        }

        if (__atomic_compare_exchange_n(&_lock, &dfl_id, loop_id, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        {
            num++;
            return true;
        }

        return false;
    }
};

#endif
