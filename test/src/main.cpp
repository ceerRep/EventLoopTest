#include <cstdint>
#include <fmt/core.h>
#include <iostream>
#include <map>

#include <fmt/format.h>

#include <EventLoop.hh>
#include <Future.hh>
#include <Semaphore.hh>
#include <string>
#include <thread>

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
    StdMapBackend(int bucket_num) : bucket_num(bucket_num) {}

    Future<std::string> get(uint64_t key)
    {
        int index = key % bucket_num;

        Promise<std::string> pro;
        auto fut = pro.get_future();

        Eventloop::get_loop(index).call_soon([key, pro = std::move(pro)]() mutable
                                             {
            auto &bucket = getBucket();

            return bucket.sem.wait().then([key, pro = std::move(pro), &bucket]() mutable {
                pro.resolve(bucket.storage[key]);
                bucket.sem.signal();
            }); });

        return fut;
    }

    Future<void> set(uint64_t key, const std::string &value)
    {
        int index = key % bucket_num;

        Promise<void> pro;
        auto fut = pro.get_future();

        Eventloop::get_loop(index).call_soon([key, value, pro = std::move(pro)]() mutable
                                             {
            auto &bucket = getBucket();

            return bucket.sem.wait().then([key, value, pro = std::move(pro), &bucket]() mutable {
                bucket.storage[key] = value;
                pro.resolve();
                bucket.sem.signal();
            }); });

        return fut;
    }

    Future<void> remove(uint64_t key)
    {
        int index = key % bucket_num;

        Promise<void> pro;
        auto fut = pro.get_future();

        Eventloop::get_loop(index).call_soon([key, pro = std::move(pro)]() mutable
                                             {
            auto &bucket = getBucket();

            return bucket.sem.wait().then([key, pro = std::move(pro), &bucket]() mutable {
                bucket.storage.erase(key);
                pro.resolve();
                bucket.sem.signal();
            }); });

        return fut;
    }
};

#define THREAD_NUM 8
#define N 10000000

int main(void)
{
    Eventloop::initialize_event_loops(THREAD_NUM);
    StdMapBackend backend(THREAD_NUM);

    for (int ind = 0; ind < THREAD_NUM; ind++)
        Eventloop::get_loop(ind)
            .call_soon(
                [&backend, ind]()
                {
                    std::vector<Future<void>> futs;

                    for (int i = N / THREAD_NUM * ind; i < N / THREAD_NUM * (ind + 1); i++)
                        futs.emplace_back(std::move(
                            backend.set(i, std::to_string(i))
                                .then(
                                    [i]()
                                    {
                                        //   fmt::print("{} Done\n", i);
                                    })));

                    return when_all(futs.begin(), futs.end())
                        .then(
                            []()
                            { fmt::print("Load done\n"); })
                        .then(
                            [&backend, ind]()
                            {
                                std::vector<Future<void>> futs;

                                for (int i = N / THREAD_NUM * ind; i < N / THREAD_NUM * (ind + 1); i++)
                                    futs.emplace_back(std::move(
                                        backend.get(i)
                                            .then(
                                                [i](std::string s)
                                                {
                                                    //   fmt::print("{} {} {} get\n", std::to_string(i) == s, i, s);
                                                })));

                                return when_all(futs.begin(), futs.end());
                            });
                });

    for (int i = 0; i < THREAD_NUM; i++)
        Eventloop::get_loop(i).run();

    for (int i = 0; i < THREAD_NUM; i++)
        Eventloop::get_loop(i).join();
}
