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

#include <concurrentqueue.h>

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
    inline static std::vector<std::unique_ptr<Eventloop>> loops;

    inline static volatile int32_t sleeping_loops;
    inline static std::condition_variable sleep_cv;
    inline static std::mutex sleep_mutex;

    std::thread th;

    int index;

    volatile bool running;

    volatile int to_sleep;
    std::condition_variable cv;
    std::mutex cv_m;

    Spinlock pending_future_lock;

    std::set<std::shared_ptr<Future<void>>> pending_futures;

    std::multimap<
        std::chrono::time_point<std::chrono::high_resolution_clock>,
        fu2::unique_function<void(void)>>
        pending_timepoints;
    moodycamel::ConcurrentQueue<fu2::unique_function<void(void)>> queue;
    // std::queue<fu2::unique_function<void(void)>> queue;

    template <typename Func, typename... Args>
    friend auto future_function_transform(Func &&func);

    void wake()
    {
        std::unique_lock lk(cv_m);

        if (to_sleep)
        {
            std::unique_lock sl(sleep_mutex);
            sleeping_loops -= 1;
        }
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

        fmt::print("Event loop running on cpu #{}\n", cpu_ind);

        running = true;

        while (running)
        {
            // TODO: pending_timepoints sleep
            fu2::unique_function<void(void)> func;

            {
                if (queue.try_dequeue(func))
                {
                }
                else if (pending_timepoints.size())
                {
                    auto now = std::chrono::high_resolution_clock::now();

                    while (pending_timepoints.size() && pending_timepoints.begin()->first <= now)
                    {
                        auto it = pending_timepoints.begin();
                        queue.enqueue(std::move(it->second));
                        pending_timepoints.erase(it);
                    }
                }
                // else
                // {
                //     using namespace std::chrono_literals;
                //     {
                //         std::lock_guard guard{sleep_mutex};
                //         std::lock_guard guard_cv{cv_m};
                //         sleeping_loops += 1;
                //         to_sleep = 1;
                //     }

                //     auto start_time = std::chrono::high_resolution_clock::now();
                //     lock.unlock();
                //     while (to_sleep && std::chrono::high_resolution_clock::now() - start_time <= 50ms)
                //         ;
                //     lock.lock();

                //     if (to_sleep)
                //     {
                //         sleep_cv.notify_all();
                //         std::unique_lock lk(cv_m, std::defer_lock);
                //         lk.lock();

                //         lock.unlock();
                //         cv.wait_for(lk, 1s, [this]
                //                     { return to_sleep != 1; });
                //         if (to_sleep)
                //         {
                //             std::cerr << fmt::format("Loop #{} sleeped\n", index);
                //             cv.wait(lk, [this]
                //                     { return to_sleep != 1; });
                //             std::cerr << fmt::format("Loop #{} waked: {}\n", index, to_sleep ? "timeout" : "notify");
                //         }
                //     }
                // }
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
            loops[i] = std::make_unique<Eventloop>(i);

        sleeping_loops = 0;

        std::thread(
            [loop_num]()
            {
                {
                    std::unique_lock lk(sleep_mutex, std::defer_lock);
                    lk.lock();

                    sleep_cv.wait(lk,
                                  [loop_num]
                                  {
                                      return sleeping_loops == loop_num;
                                  });
                }

                for (int i = 0; i < loop_num; i++)
                {
                    loops[i]->running = false;
                    loops[i]->wake();
                }
            })
            .detach();
    }

    static inline void stop_loops()
    {
        std::unique_lock lk(sleep_mutex, std::defer_lock);
        lk.lock();
        sleeping_loops = loops.size();
        sleep_cv.notify_all();
    }

    static inline Eventloop &get_loop(int index)
    {
        return *loops[index < 0 ? 0 : index];
    }
};

template <typename F, std::enable_if_t<std::is_void_v<std::invoke_result_t<F>>, bool>>
void Eventloop::call_soon(F &&func)
{
    queue.enqueue(std::forward<F>(func));

    wake();
}

template <typename F, typename Duration,
          std::enable_if_t<
              std::is_void_v<std::invoke_result_t<F>>,
              bool>>
void Eventloop::call_later(F &&func, Duration duration)
{
    if (get_cpu_index() != index)
        call_soon(
            [this, func = std::forward<F>(func), duration]() mutable
            {
                call_later(std::move(func), duration);
            });
    else
    {
        auto target_time = std::chrono::high_resolution_clock::now() + duration;
        pending_timepoints.emplace(target_time, std::forward<F>(func));
    }
}

#endif
