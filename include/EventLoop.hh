#ifndef _EVENT_LOOP_HH

#define _EVENT_LOOP_HH

#include <any>
#include <chrono>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <type_traits>
#include <vector>

#include <fmt/format.h>

#include <function2/function2.hpp>

#include "FutureBase.hh"
#include "cpu.hh"
#include "spinlock.hh"

template <typename R>
class Future;

class Eventloop
{
    inline static thread_local int cpu_ind = -1;
    inline static std::vector<Eventloop *> loops;

    int index;

    Spinlock queue_lock;

    Spinlock pending_future_lock;

    std::set<std::shared_ptr<Future<void>>> pending_futures;
    std::multimap<
        std::chrono::time_point<std::chrono::high_resolution_clock>,
        fu2::unique_function<void(void)>>
        pending_timepoints;
    std::list<fu2::unique_function<void(void)>> queue;

    template <typename Func, typename ...Args>
    friend auto future_function_transform(Func &&func);

public:
    Eventloop(int index) : index(index) {}

    template <typename Func,
              std::enable_if_t<
                  std::is_void_v<std::invoke_result_t<Func>>,
                  bool> = true>
    void call_soon(Func &&func);

    template <typename Func, typename Duration,
              std::enable_if_t<
                  std::is_void_v<std::invoke_result_t<Func>>,
                  bool> = true>
    void call_later(Func &&func, Duration duration);

    template <typename Func,
              std::enable_if_t<
                  std::is_base_of_v<FutureBase, std::invoke_result_t<Func>>,
                  bool> = true>
    void call_soon(Func &&func);

    template <typename Func, typename Duration,
              std::enable_if_t<
                  std::is_base_of_v<FutureBase, std::invoke_result_t<Func>>,
                  bool> = true>
    void call_later(Func &&func, Duration duration);

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
                std::lock_guard guard{queue_lock};
                if (queue.size())
                {
                    func = std::move(queue.front());
                    queue.pop_front();
                }
                else if (pending_timepoints.size())
                {
                    auto now = std::chrono::high_resolution_clock::now();

                    while (pending_timepoints.size() && pending_timepoints.begin()->first <= now)
                    {
                        auto it = pending_timepoints.begin();
                        queue.push_back(std::move(it->second));
                        pending_timepoints.erase(it);
                    }
                }
                else
                    break;
            }

            if (func)
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
        return *loops[index < 0 ? 0 : index];
    }
};

template <typename F, std::enable_if_t<std::is_void_v<std::invoke_result_t<F>>, bool>>
void Eventloop::call_soon(F &&func)
{
    std::lock_guard guard{queue_lock};
    queue.emplace_back(std::forward<F>(func));
}

template <typename F, typename Duration,
          std::enable_if_t<
              std::is_void_v<std::invoke_result_t<F>>,
              bool>>
void Eventloop::call_later(F &&func, Duration duration)
{
    std::lock_guard guard{queue_lock};
    auto target_time = std::chrono::high_resolution_clock::now() + duration;
    pending_timepoints.emplace(target_time, std::forward<F>(func));
}

#endif
