#include <algorithm>
#include <cassert>
#include <condition_variable>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fmt/core.h>

#include <concurrentqueue.h>

#include "spinlock.hh"
#include "cpu.hh"

#include <config.hpp>

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
        std::promise<void> *p_promise_void;
        std::promise<std::string> *p_promise_string;
    };

    struct bucket
    {
        std::map<uint64_t, std::string> storage;
        moodycamel::ConcurrentQueue<request> queue;
    };

    int bucket_num;

    bool running;
    std::vector<bucket> buckets;
    std::vector<std::thread> threads;

    inline bucket &getBucketAt(int ind)
    {
        return buckets[ind];
    }

    void worker(int id, bucket &bucket, bool &running)
    {
        assignToThisCore(THREAD_NUM + id);
        request req;
        while (running)
        {
            if (bucket.queue.try_dequeue(req))
            {
                switch (req.type)
                {
                case request::GET:
                {
                    req.p_promise_string->set_value(bucket.storage[req.key]);
                    break;
                }
                case request::SET:
                {
                    bucket.storage[req.key] = req.value;
                    req.p_promise_void->set_value();
                    break;
                }
                case request::DELETE:
                {
                    bucket.storage.erase(req.key);
                    req.p_promise_void->set_value();
                    break;
                }
                }
            }
        }
    }

public:
    StdMapBackend(int bucket_num) : bucket_num(bucket_num), buckets(bucket_num)
    {
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

    std::string get(uint64_t key)
    {
        int index = (key * 19260817) % bucket_num;

        auto &bucket = getBucketAt(index);

        std::promise<std::string> pro;
        auto fut = pro.get_future();

        request req{request::GET, key, "", nullptr, &pro};
        bucket.queue.enqueue(std::move(req));

        return fut.get();
    }

    void set(uint64_t key, const std::string &value)
    {
        int index = (key * 19260817) % bucket_num;

        auto &bucket = getBucketAt(index);

        std::promise<void> pro;
        auto fut = pro.get_future();

        request req{request::SET, key, value, &pro, nullptr};
        bucket.queue.enqueue(std::move(req));

        return fut.get();
    }

    void remove(uint64_t key)
    {
        int index = (key * 19260817) % bucket_num;

        auto &bucket = getBucketAt(index);

        std::promise<void> pro;
        auto fut = pro.get_future();

        request req{request::DELETE, key, "", &pro, nullptr};
        bucket.queue.enqueue(std::move(req));

        return fut.get();
    }

    void start_worker_thread()
    {
        running = true;
        for (int i = 0; i < buckets.size(); i++)
        {
            threads.emplace_back(
                [this, &bucket = buckets[i], i]()
                { worker(i, bucket, running); });
        }
    }
};

#define REAL_WORKER_NUM (THREAD_NUM * WORKER_PER_CORE)

StdMapBackend backend(BUCKET_NUM);

std::mutex lw_m, rw_m;
std::condition_variable lw_cv, rw_cv;
volatile int loading_workers, running_workers;

void worker(int id, uint64_t begin, uint64_t end, std::vector<uint64_t> &latencies)
{
    assignToCores(0, THREAD_NUM);
    fmt::print("#{} {} - {}\n", id, begin, end);
    for (int i = begin; i < end; i++)
        backend.set(i, std::to_string(i));

    fmt::print("#{} Load Done\n", id);

    {
        std::unique_lock lk_lw(lw_m);
        loading_workers--;

        if (loading_workers)
            lw_cv.wait(lk_lw,
                       [id]()
                       { return loading_workers == 0; });
        else
        {
            lk_lw.unlock();
            lw_cv.notify_all();
        }
    }

    for (int i = begin; i < end; i++)
    {
        auto st = rdtscp();
        // auto value = backend.get(i);
        auto ed = rdtscp();

        latencies[i] = ed - st;

        // assert(std::to_string(i) == value);
    }

    fmt::print("#{} Get Done\n", id);

    std::unique_lock lk_rw(rw_m);
    --running_workers;
    rw_cv.notify_all();
}

int main(void)
{
    std::unique_lock lk_lw(lw_m), lk_rw(rw_m);
    loading_workers = running_workers = REAL_WORKER_NUM;

    new (&backend) StdMapBackend(BUCKET_NUM);

    backend.start_worker_thread();

    std::vector<uint64_t> latencies(N, 0);

    for (int i = 0; i < REAL_WORKER_NUM; i++)
        std::thread(
            [i, &latencies]()
            {
                worker(i, N / REAL_WORKER_NUM * i, N / REAL_WORKER_NUM * (i + 1), latencies);
            })
            .detach();

    lw_cv.wait(lk_lw, []()
               { return loading_workers == 0; });
    lk_lw.unlock();

    fmt::print("All Load Done\n");

    rw_cv.wait(lk_rw, []()
               { return running_workers == 0; });

    fmt::print("All Get Done\n");

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
