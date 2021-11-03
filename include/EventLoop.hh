#ifndef _EVENT_LOOP_HH

#define _EVENT_LOOP_HH

#include <any>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <type_traits>
#include <vector>

#include <fmt/format.h>

#include <function2/function2.hpp>

#include "cpu.hh"
#include "spinlock.hh"
#include "FutureBase.hh"

template <typename R>
class Future;

class Eventloop
{
    inline static thread_local int cpu_ind = -1;
    inline static std::vector<Eventloop *> loops;

    int index;

    Spinlock lock;

    std::set<std::shared_ptr<Future<void>>> pending_futures;
    std::list<fu2::unique_function<void(void)>> queue;

public:
    Eventloop(int index) : index(index) {}

    template <typename F,
              std::enable_if_t<
                  std::is_void_v<std::invoke_result_t<F>>,
                  bool> = true>
    void call_soon(F &&func);

    template <typename F,
              std::enable_if_t<
                  std::is_base_of_v<FutureBase, std::invoke_result_t<F>>,
                  bool> = true>
    void call_soon(F &&func);

    void run()
    {
        std::thread th([this]()
                       { run_inplace(); });
        th.detach();
    }

    void run_inplace()
    {
        if (cpu_ind != -1)
            throw std::logic_error(fmt::format("cpu_ind should be -1, get {}", cpu_ind));
        cpu_ind = index;

        while (true)
        {
            decltype(queue)::value_type func;

            {
                std::lock_guard guard{lock};
                if (queue.size())
                {
                    func = std::move(queue.front());
                    queue.pop_front();
                }
                else
                    break;
            }

            func();
        }
    }

    static inline int get_cpu_index()
    {
        return cpu_ind;
    }

    static inline void initialize_event_loops(int loop_num)
    {
        loops.resize(loop_num);

        for (int i = 0; i < loop_num; i++)
            loops[i] = new Eventloop(i);
    }

    static inline Eventloop &get_loop(int index)
    {
        return *loops[index];
    }
};

template <typename F, std::enable_if_t<std::is_void_v<std::invoke_result_t<F>>, bool>>
void Eventloop::call_soon(F &&func)
{
    if (cpu_ind != -1)
        if (cpu_ind != index)
            throw std::invalid_argument(fmt::format("Call in wrong thread, should be {}, get {}", index, cpu_ind));

    std::lock_guard guard{lock};
    queue.emplace_back(std::move(func));
}

#endif
