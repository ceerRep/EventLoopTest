//          Copyright Nat Goodspeed + Oliver Kowalke 2015.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#include <boost/fiber/fiber.hpp>
#include <boost/fiber/mutex.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <boost/assert.hpp>

#include <boost/fiber/all.hpp>
#include <fmt/core.h>

#ifndef BARRIER_H
#define BARRIER_H

#include <cstddef>
#include <condition_variable>
#include <mutex>

#include <boost/assert.hpp>

class barrier {
private:
	std::size_t             initial_;
	std::size_t             current_;
	bool                    cycle_{ true };
    std::mutex              mtx_{};
    std::condition_variable cond_{};

public:
	explicit barrier( std::size_t initial) :
        initial_{ initial },
        current_{ initial_ } {
        BOOST_ASSERT ( 0 != initial);
    }

    barrier( barrier const&) = delete;
    barrier & operator=( barrier const&) = delete;

    bool wait() {
        std::unique_lock< std::mutex > lk( mtx_);
        const bool cycle = cycle_;
        if ( 0 == --current_) {
            cycle_ = ! cycle_;
            current_ = initial_;
            lk.unlock(); // no pessimization
            cond_.notify_all();
            return true;
        } else {
            cond_.wait( lk, [&](){ return cycle != cycle_; });
        }
        return false;
    }
};

#endif // BARRIER_H

class StdMapBackend
{
    struct bucket
    {
        std::map<uint64_t, std::string> storage;
        boost::fibers::mutex lock;
    };

    int bucket_num;

    std::vector<bucket> buckets;

    inline bucket &getBucketAt(int ind)
    {
        return buckets[ind];
    }

public:
    StdMapBackend(int bucket_num) : bucket_num(bucket_num), buckets(bucket_num)
    {
    }

    std::string get(uint64_t key)
    {
        int index = (key * 19260817) % bucket_num;

        auto &bucket = getBucketAt(index);
        
        std::lock_guard guard {bucket.lock};

        return bucket.storage[key];
    }

    void set(uint64_t key, const std::string &value)
    {
        int index = (key * 19260817) % bucket_num;

        auto &bucket = getBucketAt(index);

        std::lock_guard guard {bucket.lock};

        bucket.storage[key] = value;
    }

    void remove(uint64_t key)
    {
        int index = (key * 19260817) % bucket_num;

        auto &bucket = getBucketAt(index);

        std::lock_guard guard {bucket.lock};

        bucket.storage.erase(key);
    }
};

static std::mutex mtx_setter;
static std::size_t fiber_count{ 0 }, setter_count{0};
static std::mutex mtx_count{};
static boost::fibers::condition_variable_any cnd_count{}, cnd_setter{};
typedef std::unique_lock< std::mutex > lock_type;

#include <config.hpp>
#define REAL_THREAD_NUM  (THREAD_NUM + BUCKET_NUM)

/*****************************************************************************
*   example fiber function
*****************************************************************************/
//[fiber_fn_ws
void worker(std::vector<uint64_t> &latencies, StdMapBackend& backend, uint64_t th_num, int th_ind) {
    try {
        for (int i = N / th_num * th_ind, end = N / th_num * (th_ind + 1); i < N; i++)
            backend.set(i, std::to_string(i));
        fmt::print("#{} Load done\n", th_ind);
        lock_type lk(mtx_setter);

        if (0 == --setter_count) {
            lk.unlock();
            cnd_setter.notify_all();
        }
        else
            cnd_setter.wait(lk, []() {return 0 == setter_count; });

        if (th_ind == 0)
            fmt::print("All Load done\n");

        for (int i = N / th_num * th_ind, end = N / th_num * (th_ind + 1); i < N; i++)
        {
            auto s = rdtscp();
            auto value = backend.get(i);
            auto e = rdtscp();

            latencies[i] = e - s;
            assert(value == std::to_string(i));
        }
        fmt::print("#{} Set done\n", th_ind);
    } catch ( ... ) {
    }
    lock_type lk( mtx_count);
    if ( 0 == --fiber_count) { /*< Decrement fiber counter for each completed fiber. >*/
        lk.unlock();
        cnd_count.notify_all(); /*< Notify all fibers waiting on `cnd_count`. >*/
    }
}
//]

/*****************************************************************************
*   example thread function
*****************************************************************************/
//[thread_fn_ws
void thread( barrier * b) {
    std::ostringstream buffer;
    buffer << "thread started " << std::this_thread::get_id() << std::endl;
    std::cout << buffer.str() << std::flush;
    boost::fibers::use_scheduling_algorithm< boost::fibers::algo::shared_work >(); /*<
        Install the scheduling algorithm `boost::fibers::algo::shared_work` in order to
        join the work sharing.
    >*/
    b->wait(); /*< sync with other threads: allow them to start processing >*/
    lock_type lk( mtx_count);
    cnd_count.wait( lk, [](){ return 0 == fiber_count; } ); /*<
        Suspend main fiber and resume worker fibers in the meanwhile.
        Main fiber gets resumed (e.g returns from `condition_variable_any::wait()`)
        if all worker fibers are complete.
    >*/
    BOOST_ASSERT( 0 == fiber_count);
}
//]

/*****************************************************************************
*   main()
*****************************************************************************/
int main( int argc, char *argv[]) {
    std::vector<uint64_t> latencies(N);
    std::cout << "main thread started " << std::this_thread::get_id() << std::endl;
//[main_ws
    boost::fibers::use_scheduling_algorithm< boost::fibers::algo::shared_work >(); /*<
        Install the scheduling algorithm `boost::fibers::algo::shared_work` in the main thread
        too, so each new fiber gets launched into the shared pool.
    >*/

    StdMapBackend backend(BUCKET_NUM);

    for (int i = 0; i < REAL_THREAD_NUM; i++)
    {
        boost::fibers::fiber([i, &latencies, &backend]()
                             { worker(latencies, backend, REAL_THREAD_NUM, i); }).detach();
        setter_count++;
        fiber_count++;
    }
    barrier b( REAL_THREAD_NUM);
    std::vector<std::thread> threads;
    for (int i = 1; i < REAL_THREAD_NUM; i++)
        threads.emplace_back(thread, &b);
    
    b.wait(); /*< sync with other threads: allow them to start processing >*/
    {
        lock_type/*< `lock_type` is typedef'ed as __unique_lock__< [@http://en.cppreference.com/w/cpp/thread/mutex `std::mutex`] > >*/ lk( mtx_count);
        cnd_count.wait( lk, [](){ return 0 == fiber_count; } ); /*<
            Suspend main fiber and resume worker fibers in the meanwhile.
            Main fiber gets resumed (e.g returns from `condition_variable_any::wait()`)
            if all worker fibers are complete.
        >*/
    } /*<
        Releasing lock of mtx_count is required before joining the threads, otherwise
        the other threads would be blocked inside condition_variable::wait() and
        would never return (deadlock).
    >*/
    BOOST_ASSERT( 0 == fiber_count);
    for ( std::thread & t : threads) { /*< wait for threads to terminate >*/
        t.join();
    }
//]
    std::cout << "All Set done" << std::endl;

    fmt::print("Sorting latencies...\n");

    double sum = 0;
    std::sort(latencies.begin(), latencies.end());
    for (auto l : latencies)
        sum += l;

    fmt::print(
        "Avg:   {}\n"
        "Mid:   {}\n",
        sum / latencies.size(),
        latencies[N / 2]);

    for (int i = 90; i <= 99; i++)
        fmt::print("{}:    {}\n", i, latencies[int(N * 0.01 * i)]);

    fmt::print(
        "99.9:  {}\n"
        "99.99: {}\n",
        latencies[int(N * 0.999)],
        latencies[int(N * 0.9999)]);
    return EXIT_SUCCESS;
}
