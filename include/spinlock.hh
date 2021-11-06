#ifndef _SPINLOCK_HH

#define _SPINLOCK_HH

#include <atomic>
#include <stdexcept>
#include <thread>

inline static int (*get_loop_id)();

class Spinlock
{
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
        if (_lock != get_loop_id())
            throw std::runtime_error("Unlock in wrong thread");

        num--;

        if (num < 0)
            throw std::runtime_error("Lock over unlocked");

        if (!num)
            _lock = -1;
    }

    bool try_lock()
    {
        int32_t dfl_id = -1, loop_id = get_loop_id();
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
