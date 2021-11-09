#include <chrono>
#include <cstdint>
#include <fmt/core.h>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <variant>

#include <fmt/format.h>

#include <concurrentqueue.h>

#include <EventLoop.hh>
#include <Future.hh>
#include <Semaphore.hh>

using namespace std::chrono_literals;

struct
{
    template <typename T>
    auto &operator<<(T &&) { return *this; }
} debug;

class StdMapBackend
{
    struct request
    {
        enum
        {
            GET,
            SET,
            DELETE
        };
        int type;
        uint64_t key;
        std::string value;
        std::unique_ptr<Promise<void>> p_promise_void;
        std::unique_ptr<Promise<std::string>> p_promise_string;
    };

    struct bucket
    {
        std::map<uint64_t, std::string> storage;
        moodycamel::ConcurrentQueue<request> queue;
    };

    int bucket_num;

    inline static bool running;
    inline static std::vector<bucket> buckets;
    inline static std::vector<std::thread> threads;

    inline static bucket &getBucketAt(int ind)
    {
        return buckets[ind];
    }

    inline static bucket &getBucket()
    {
        return buckets[Eventloop::get_cpu_index()];
    }

    void worker(bucket &bucket, bool &running)
    {
        request req;
        while (running)
        {
            if (bucket.queue.try_dequeue(req))
            {
                switch (req.type)
                {
                case request::GET:
                {
                    auto &loop = Eventloop::get_loop(req.p_promise_string->get_loop_index());
                    loop.call_soon(
                        [p_promise = std::move(req.p_promise_string), value = bucket.storage[req.key]]() mutable
                        {
                            p_promise->resolve(std::move(value));
                        });
                    break;
                }
                case request::SET:
                {
                    auto &loop = Eventloop::get_loop(req.p_promise_void->get_loop_index());
                    bucket.storage[req.key] = req.value;
                    loop.call_soon(
                        [p_promise = std::move(req.p_promise_void)]() mutable
                        {
                            p_promise->resolve();
                        });
                    break;
                }
                case request::DELETE:
                {
                    auto &loop = Eventloop::get_loop(req.p_promise_void->get_loop_index());
                    bucket.storage.erase(req.key);
                    loop.call_soon(
                        [p_promise = std::move(req.p_promise_void)]() mutable
                        {
                            p_promise->resolve();
                        });
                    break;
                }
                }
            }
        }
    }

public:
    StdMapBackend(int bucket_num) : bucket_num(bucket_num)
    {
        buckets.resize(bucket_num);
        start_worker_thread();
    }

    ~StdMapBackend()
    {
        running = false;

        for (auto &thread : threads)
        {
            if (thread.joinable())
                thread.join();
        }
    }

    Future<std::string> get(uint64_t key)
    {
        int index = (key * 19260817) % bucket_num;

        auto unique_pro = std::make_unique<Promise<std::string>>();
        auto fut = unique_pro->get_future();

        request req{request::GET, key, "", nullptr, std::move(unique_pro)};
        getBucketAt(index).queue.enqueue(std::move(req));

        return fut;
    }

    Future<void> set(uint64_t key, const std::string &value)
    {
        int index = (key * 19260817) % bucket_num;

        auto unique_pro = std::make_unique<Promise<void>>();
        auto fut = unique_pro->get_future();

        request req{request::SET, key, value, std::move(unique_pro), nullptr};
        getBucketAt(index).queue.enqueue(std::move(req));

        return fut;
    }

    Future<void> remove(uint64_t key)
    {
        int index = (key * 19260817) % bucket_num;

        auto unique_pro = std::make_unique<Promise<void>>();
        auto fut = unique_pro->get_future();

        request req{request::SET, key, nullptr, std::move(unique_pro), nullptr};
        getBucketAt(index).queue.enqueue(std::move(req));

        return fut;
    }

    void start_worker_thread()
    {
        running = true;
        for (auto &bucket : buckets)
        {
            threads.emplace_back(
                [this, &bucket]()
                { worker(bucket, running); });
        }
    }
};

#include <config.hpp>

Future<void> do_set(StdMapBackend &backend, uint64_t now_key, uint64_t end_key, uint64_t step)
{
    return backend.set(now_key, std::to_string(now_key))
        .then(
            [=, &backend]() -> Future<void>
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
    // fmt::print("{} start\n", now_key);
    return backend.get(now_key)
        .then(
            [=, &backend, &latencies](auto value) -> Future<void>
            {
                auto end = rdtscp();
                latencies[now_key] = end - start;

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
    std::vector<int64_t> latencies(N, -1);

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
                                    {
                                        sum += l;
                                    }
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
                    })
                .finally(
                    []()
                    {
                        Eventloop::stop_loops();
                    });
        });

    for (int i = 0; i < THREAD_NUM; i++)
        Eventloop::get_loop(i).run();

    for (int i = 0; i < THREAD_NUM; i++)
        Eventloop::get_loop(i).join();
}
