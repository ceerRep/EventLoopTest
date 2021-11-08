#include "spinlock.hh"
#include <chrono>
#include <cstdint>
#include <fmt/core.h>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include <fmt/format.h>

#include <EventLoop.hh>
#include <Future.hh>
#include <Semaphore.hh>
#include <vector>

class ConcurrentSemaphore
{
    Spinlock lk;
    int num;

    std::list<std::unique_ptr<Promise<void>>> pending_list;

public:
    ConcurrentSemaphore(int num) : num(num) {}
    ConcurrentSemaphore(const ConcurrentSemaphore &) = delete;
    ConcurrentSemaphore(ConcurrentSemaphore &&) = default;

    Future<void> wait()
    {
        std::lock_guard guard{lk};
        num--;

        if (num >= 0)
            return make_ready_future();
        else
        {
            pending_list.emplace_back(new Promise<void>);
            return pending_list.back()->get_future();
        }
    }

    void signal()
    {
        std::lock_guard guard{lk};
        num++;

        if (num <= 0)
        {
            auto ppro = std::move(pending_list.front());
            pending_list.pop_front();

            auto loopno = ppro->get_loop_index();

            Eventloop::get_loop(loopno).call_soon(
                [ppro = std::move(ppro)]()
                {
                    ppro->resolve();
                });
        }
    }
};

using namespace std::chrono_literals;

struct
{
    template <typename T>
    auto &operator<<(T &&) { return *this; }
} debug;

class StdMapBackend
{
    struct bucket
    {
        std::map<uint64_t, std::string> storage;
        ConcurrentSemaphore sem{1};
    };

    int bucket_num;
    inline static std::vector<bucket> buckets;

    inline static bucket &getBucketAt(int ind)
    {
        return buckets[ind];
    }

    inline static bucket &getBucket()
    {
        return buckets[Eventloop::get_cpu_index()];
    }

public:
    class Cursor
    {
    private:
        Semaphore *sem;
        StdMapBackend *backend;

        Cursor(StdMapBackend *backend, Semaphore *sem) : backend(backend), sem(sem) {}
        friend class StdMapBackend;

    public:
        Cursor() : backend(nullptr) {}
        Cursor(const Cursor &) = delete;
        Cursor(Cursor &&r)
        {
            backend = r.backend;
            sem = r.sem;
            r.backend = nullptr;
            r.sem = nullptr;
        }

        ~Cursor()
        {
            if (backend)
            {
                sem->signal();
                // fmt::print("Signaled\n");
            }
        }

        Future<std::tuple<Cursor, std::string>> get(uint64_t key)
        {
            int index = (key * 19260817) % backend->bucket_num;

            auto &bucket = getBucketAt(index);

            return bucket.sem.wait().then(
                [key, &bucket, cursor = std::move(*this)]() mutable
                {
                    std::string value = bucket.storage[key];
                    bucket.sem.signal();
                    return std::tuple<Cursor, std::string>{std::move(cursor), value};
                });
        }

        Future<Cursor> set(uint64_t key, const std::string &value)
        {
            int index = (key * 19260817) % backend->bucket_num;

            auto &bucket = getBucketAt(index);

            return bucket.sem.wait().then(
                [key, value, &bucket, cursor = std::move(*this)]() mutable
                {
                    bucket.storage[key] = value;
                    bucket.sem.signal();
                    return std::move(cursor);
                });
        }

        Future<Cursor> remove(uint64_t key)
        {
            int index = (key * 19260817) % backend->bucket_num;

            auto &bucket = getBucketAt(index);

            return bucket.sem.wait().then(
                [key, &bucket, cursor = std::move(*this)]() mutable
                {
                    bucket.storage.erase(key);
                    bucket.sem.signal();
                    return std::move(cursor);
                });
        }
    };

    StdMapBackend(int bucket_num) : bucket_num(bucket_num)
    {
        buckets.resize(bucket_num);
    }

    Future<Cursor> get_cursor()
    {
        static thread_local Semaphore pending{16};
        return pending.wait()
            .then(
                [this]()
                {
                    return Cursor(this, &pending);
                });
    }
};

#include <config.hpp>

Future<void> do_set(StdMapBackend &backend, uint64_t now_key, uint64_t end_key, uint64_t step)
{
    return backend.get_cursor()
        .then(
            [=, &backend](StdMapBackend::Cursor cursor) mutable
            {
                return cursor.set(now_key, std::to_string(now_key));
            })
        .then(
            [=, &backend](auto) -> Future<void>
            {
                auto next_key = now_key + step;

                if (next_key >= end_key)
                    return make_ready_future();
                else
                    return do_set(backend, next_key, end_key, step);
            });
}

Future<void> do_get(std::vector<int64_t> &latencies, StdMapBackend &backend, uint64_t now_key, uint64_t end_key, uint64_t step)
{
    auto start = rdtscp();
    return backend.get_cursor()
        .then(
            [=, &backend](StdMapBackend::Cursor cursor) mutable
            {
                return cursor.get(now_key);
            })
        .then(
            [=, &backend, &latencies](auto pack) -> Future<void>
            {
                auto end = rdtscp();
                latencies[now_key] = end - start;

                auto &&[cursor, value] = pack;

                assert(std::to_string(now_key) == value);

                auto next_key = now_key + step;

                if (next_key >= end_key)
                    return make_ready_future();
                else
                    return do_get(latencies, backend, next_key, end_key, step);
            });
}

int main(void)
{
    std::vector<int64_t> latencies(N);

    Eventloop::initialize_event_loops(THREAD_NUM);
    StdMapBackend backend(BUCKET_NUM);

    Eventloop::get_loop(0).call_soon(
        [&]()
        {
            std::vector<Future<void>> futures;

            for (int ind = 0; ind < THREAD_NUM; ind++)
                futures.emplace_back(std::move(
                    submit_to(
                        ind,
                        [&backend, ind]()
                        {
                            std::vector<Future<void>> futures;

                            for (int i = 0; i < WORKER_PER_CORE; i++)
                                futures.emplace_back(std::move(
                                    do_set(
                                        backend,
                                        N / THREAD_NUM * ind + i,
                                        N / THREAD_NUM * (ind + 1),
                                        WORKER_PER_CORE)));

                            return when_all(futures.begin(), futures.end())
                                .then(
                                    [ind]()
                                    { fmt::print("#{} Load done\n", ind); });
                        })));

            return when_all(futures.begin(), futures.end())
                .then(
                    []()
                    {
                        fmt::print("All Load done\n");
                    })
                .then(
                    [&backend, &latencies]()
                    {
                        std::vector<Future<void>> futures;
                        for (int ind = 0; ind < THREAD_NUM; ind++)
                            futures.emplace_back(
                                submit_to(
                                    ind,
                                    [&backend, ind, &latencies]()
                                    {
                                        std::vector<Future<void>> futures;

                                        for (int i = 0; i < WORKER_PER_CORE; i++)
                                            futures.emplace_back(std::move(
                                                do_get(
                                                    latencies,
                                                    backend,
                                                    N / THREAD_NUM * ind + i,
                                                    N / THREAD_NUM * (ind + 1),
                                                    WORKER_PER_CORE)));

                                        return when_all(futures.begin(), futures.end())
                                            .then(
                                                [ind]()
                                                { fmt::print("#{} Get done\n", ind); });
                                    }));

                        return when_all(futures.begin(), futures.end())
                            .then(
                                [&latencies]()
                                {
                                    fmt::print("All Get done\n");
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

                                    // std::ofstream fout("latencies.csv");

                                    // for (auto l : latencies)
                                    //     fout << l << '\n';

                                    // fout.close();
                                });
                        ;
                    });
        });

    for (int i = 0; i < THREAD_NUM; i++)
        Eventloop::get_loop(i).run();

    for (int i = 0; i < THREAD_NUM; i++)
        Eventloop::get_loop(i).join();
}
