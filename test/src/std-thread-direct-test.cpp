#include <algorithm>
#include <cassert>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fmt/core.h>

#include "cpu.hh"
#include "spinlock.hh"

#include <config.hpp>

#define REAL_THREAD_NUM (THREAD_NUM + BUCKET_NUM)
#define REAL_WORKER_NUM (THREAD_NUM * WORKER_PER_CORE)

class StdMapBackend
{
    struct bucket
    {
        std::map<uint64_t, std::string> storage;
        Spinlock lock;
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

    ~StdMapBackend()
    {
        for (auto &bucket : buckets)
        {
            fmt::print("{}\n", bucket.storage.size());
        }
    }

    std::string get(uint64_t key)
    {
        int index = (key * 19260817) % bucket_num;

        auto &bucket = getBucketAt(index);

        std::lock_guard guard{bucket.lock};

        return bucket.storage[key];
    }

    void set(uint64_t key, const std::string &value)
    {
        int index = (key * 19260817) % bucket_num;

        auto &bucket = getBucketAt(index);

        std::lock_guard guard{bucket.lock};

        bucket.storage[key] = value;
    }

    void remove(uint64_t key)
    {
        int index = (key * 19260817) % bucket_num;

        auto &bucket = getBucketAt(index);

        std::lock_guard guard{bucket.lock};

        bucket.storage.erase(key);
    }
};

StdMapBackend backend(BUCKET_NUM);

std::mutex lw_m, rw_m;
std::condition_variable lw_cv, rw_cv;
volatile int loading_workers, running_workers;

void worker(int id, uint64_t begin, uint64_t end, std::vector<uint64_t> &latencies)
{
    assignToCores(0, REAL_THREAD_NUM);
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
