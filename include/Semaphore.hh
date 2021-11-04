#ifndef _SEMAPHORE_HH

#define _SEMAPHORE_HH

#include <list>

#include "EventLoop.hh"
#include "Future.hh"

class Semaphore
{
    int num;

    std::list<Promise<void>> pending_list;

public:
    Semaphore(int num) : num(num) {}
    Semaphore(const Semaphore&) = delete;
    Semaphore(Semaphore&&) = default;

    Future<void> wait()
    {
        num--;

        if (num >= 0)
            return make_ready_future();
        else
        {
            pending_list.emplace_back();
            return pending_list.back().get_future();
        }
    }

    void signal()
    {
        num++;

        if (num <= 0)
        {
            pending_list.front().resolve();
            pending_list.pop_front();
        }
    }
};

#endif
