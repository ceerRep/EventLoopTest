#include <chrono>
#include <cstdint>
#include <fmt/core.h>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include <fmt/format.h>

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
    struct bucket
    {
        std::map<uint64_t, std::string> storage;
        Semaphore sem{1};
    };

    int bucket_num;

    inline static bucket &getBucket()
    {
        thread_local static bucket b;
        return b;
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

            auto unique_pro = std::make_unique<Promise<std::tuple<Cursor, std::string>>>();
            auto fut = unique_pro->get_future();

            Eventloop::get_loop(index).call_soon(
                [key, unique_pro = std::move(unique_pro), cursor = std::move(*this), loop = Eventloop::get_cpu_index()]() mutable
                {
                    auto &bucket = getBucket();

                    return bucket.sem.wait().then(
                        [key, unique_pro = std::move(unique_pro), &bucket, cursor = std::move(cursor), loop]() mutable
                        {
                            Eventloop::get_loop(loop).call_soon(
                                [unique_pro = std::move(unique_pro), cursor = std::move(cursor), result = bucket.storage[key]]() mutable
                                {
                                    unique_pro->resolve(std::move(std::tuple<Cursor, std::string>{std::move(cursor), result}));
                                });
                            bucket.sem.signal();
                        });
                });

            return fut;
        }

        Future<Cursor> set(uint64_t key, const std::string &value)
        {
            int index = (key * 19260817) % backend->bucket_num;

            auto unique_pro = std::make_unique<Promise<Cursor>>();
            auto fut = unique_pro->get_future();

            Eventloop::get_loop(index).call_soon(
                [key, value, unique_pro = std::move(unique_pro), cursor = std::move(*this), loop = Eventloop::get_cpu_index()]() mutable
                {
                    auto &bucket = getBucket();

                    return bucket.sem.wait().then(
                        [key, value, unique_pro = std::move(unique_pro), &bucket, cursor = std::move(cursor), loop]() mutable
                        {
                            bucket.storage[key] = value;
                            Eventloop::get_loop(loop).call_soon(
                                [unique_pro = std::move(unique_pro), cursor = std::move(cursor)]() mutable
                                {
                                    unique_pro->resolve(std::move(cursor));
                                });
                            bucket.sem.signal();
                        });
                });

            return fut;
        }

        Future<Cursor> remove(uint64_t key)
        {
            int index = (key * 19260817) % backend->bucket_num;

            auto unique_pro = std::make_unique<Promise<Cursor>>();
            auto fut = unique_pro->get_future();

            Eventloop::get_loop(index).call_soon(
                [key, unique_pro = std::move(unique_pro), cursor = std::move(*this), loop = Eventloop::get_cpu_index()]() mutable
                {
                    auto &bucket = getBucket();

                    return bucket.sem.wait().then(
                        [key, unique_pro = std::move(unique_pro), &bucket, cursor = std::move(cursor), loop]() mutable
                        {
                            bucket.storage.erase(key);
                            Eventloop::get_loop(loop).call_soon(
                                [unique_pro = std::move(unique_pro), cursor = std::move(cursor)]() mutable
                                {
                                    unique_pro->resolve(std::move(cursor));
                                });
                            bucket.sem.signal();
                        });
                });

            return fut;
        }
    };

    StdMapBackend(int bucket_num) : bucket_num(bucket_num) {}

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

#define THREAD_NUM 8
#define N 10000000

int main(void)
{
    std::vector<int64_t> latencies(N);

    Eventloop::initialize_event_loops(THREAD_NUM);
    StdMapBackend backend(THREAD_NUM);

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
                            std::vector<Future<void>> futs;

                            for (int i = N / THREAD_NUM * ind; i < N / THREAD_NUM * (ind + 1); i++)
                                futs.emplace_back(std::move(
                                    backend.get_cursor().then(
                                        [i](StdMapBackend::Cursor cursor) mutable
                                        {
                                            return cursor.set(i, std::to_string(i)).then([](auto) {});
                                        })));

                            return when_all(futs.begin(), futs.end())
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
                                        std::vector<Future<void>> futs;

                                        for (int i = N / THREAD_NUM * ind; i < N / THREAD_NUM * (ind + 1); i++)
                                            futs.emplace_back(std::move(
                                                make_ready_future().then(
                                                    [&backend, i, &latencies]()
                                                    {
                                                        return backend.get_cursor().then(
                                                            [i, &latencies](StdMapBackend::Cursor cursor)
                                                            {
                                                                // fmt::print("Started\n");
                                                                return cursor.get(i).then(
                                                                    [i, &latencies, start_time = std::chrono::high_resolution_clock::now()](std::tuple<StdMapBackend::Cursor, std::string> args)
                                                                    {
                                                                        auto &&[cursor, s] = args;
                                                                        // fmt::print("Returned\n");
                                                                        auto end_time = std::chrono::high_resolution_clock::now();
                                                                        auto duration = end_time - start_time;
                                                                        latencies[i] =
                                                                            std::chrono::duration_cast<std::chrono::nanoseconds>(duration)
                                                                                .count();
                                                                    });
                                                            });
                                                    })));

                                        return when_all(futs.begin(), futs.end())
                                            .then(
                                                [ind]()
                                                {
                                                    fmt::print("#{} Get done\n", ind);
                                                });
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
