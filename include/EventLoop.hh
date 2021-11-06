#ifndef _EVENT_LOOP_HH

#define _EVENT_LOOP_HH

#include <any>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <thread>
#include <type_traits>
#include <vector>

#include <fmt/core.h>
#include <fmt/format.h>

#include <function2/function2.hpp>

#include <boost/lockfree/queue.hpp>

#include "FutureBase.hh"
#include "cpu.hh"
#include "spinlock.hh"

template <typename R>
class Future;

class Eventloop
{
    inline static thread_local int cpu_ind = -1;
    inline static std::vector<Eventloop *> loops;
    inline static int32_t active_loop_num;

    std::thread th;

    int index;

    volatile int to_sleep;
    std::condition_variable cv;
    std::mutex cv_m;

    Spinlock queue_lock;
    Spinlock pending_future_lock;

    std::set<std::shared_ptr<Future<void>>> pending_futures;

    std::multimap<
        std::chrono::time_point<std::chrono::high_resolution_clock>,
        fu2::unique_function<void(void)>>
        pending_timepoints;
    std::queue<fu2::unique_function<void(void)>> queue;

    template <typename Func, typename... Args>
    friend auto future_function_transform(Func &&func);

    void wake()
    {
        std::lock_guard<std::mutex> lk(cv_m);
        to_sleep = 0;
        cv.notify_all();
    }

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
        th = std::thread([this]()
                         { run_inplace(); });
    }

    void run_inplace()
    {
        if (int result = assignToThisCore(index); result)
            std::cerr << fmt::format("Failed to bind cpu core {}, error code: {}\n", index, result);

        if (cpu_ind != -1)
            throw std::logic_error(fmt::format("cpu_ind should be -1, get {}", cpu_ind));
        cpu_ind = index;

        __atomic_add_fetch(&active_loop_num, 1, __ATOMIC_SEQ_CST);

        fmt::print("Event loop running on cpu #{}\n", cpu_ind);

        while (true)
        {
            decltype(queue)::value_type func;

            {
                std::unique_lock lock{queue_lock, std::defer_lock};
                lock.lock();
                if (queue.size())
                {
                    func = std::move(queue.front());
                    queue.pop();
                }
                else if (pending_timepoints.size())
                {
                    auto now = std::chrono::high_resolution_clock::now();

                    while (pending_timepoints.size() && pending_timepoints.begin()->first <= now)
                    {
                        auto it = pending_timepoints.begin();
                        queue.push(std::move(it->second));
                        pending_timepoints.erase(it);
                    }
                }
                else
                {
                    lock.unlock();

                    if (__atomic_fetch_sub(&active_loop_num, 1, __ATOMIC_SEQ_CST) == 1)
                    {
                        break;
                    }
                    else
                    {
                        using namespace std::chrono_literals;

                        std::unique_lock lk(cv_m, std::defer_lock);
                        lk.lock();
                        to_sleep = 1;
                        cv.wait_for(lk, 1s, [this]
                                    { return to_sleep != 1; });
                        if (to_sleep)
                        {
                            std::cerr << fmt::format("Loop #{} sleeped\n", index);
                            cv.wait_for(lk, 1s, [this]
                                        { return to_sleep != 1; });
                            std::cerr << fmt::format("Loop #{} waked: {}\n", index, to_sleep ? "timeout" : "notify");
                        }
                    }

                    __atomic_add_fetch(&active_loop_num, 1, __ATOMIC_SEQ_CST);
                }
            }

            if (func)
                func();
        }
    }

    void join()
    {
        if (th.joinable())
            th.join();
    }

    static inline int get_cpu_index()
    {
        return cpu_ind;
    }

    static inline void initialize_event_loops(int loop_num)
    {
        get_loop_id = &get_cpu_index;

        loops.resize(loop_num);

        for (int i = 0; i < loop_num; i++)
            loops[i] = new Eventloop(i);

        active_loop_num = 0;
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
    queue.emplace(std::forward<F>(func));

    wake();
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

    wake();
}

#endif
