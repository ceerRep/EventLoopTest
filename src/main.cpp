#include <chrono>
#include <fmt/core.h>
#include <fmt/format.h>
#include <functional>
#include <iostream>
#include <memory>

#include "EventLoop.hh"
#include "Semaphore.hh"
#include <Future.hh>

using namespace std::chrono_literals;

std::function<void(void)> a;
fu2::unique_function<void(void)> then_body;

struct ConstructorTest
{
    bool flag;

    ConstructorTest() : flag(true)
    {
        fmt::print("Normal contrust\n");
    }

    ConstructorTest(const ConstructorTest &r) : flag(r.flag)
    {
        fmt::print("Copy construct\n");
    }

    ConstructorTest(ConstructorTest &&r) : flag(r.flag)
    {
        r.flag = false;
        fmt::print("Move construct\n");
    }

    ~ConstructorTest()
    {
        if (flag)
            fmt::print("Destruct\n");
    }
};

int main(void)
{
    Eventloop::initialize_event_loops(1);
    auto &loop = Eventloop::get_loop(0);

    Promise<int> p2;
    Future<int> f2;

    Promise<void> p3;
    Future<void> f3;

    loop.call_soon(
        [&]()
        {
            std::cout << 114514 << std::endl;

            Promise<int> promise;

            auto future = promise.get_future();

            auto p1 = std::move(promise);
            auto f1 = std::move(future);

            f2 = std::move(
                f1.then(
                    std::move(
                        [&p3](int v)
                        {
                            std::cout << v << std::endl;
                            p3.resolve();
                            return v;
                        })));

            p2 = std::move(p1);

            return p3.get_future().then([]()
                                        { std::cout << "Test" << std::endl; });
        });

    loop.call_soon([&p2]()
                   { p2.resolve(1919810); });

    loop.call_soon([]()
                   {
        ConstructorTest test;
        return make_ready_future(test).then([](ConstructorTest x){
            fmt::print("{}\n", fmt::ptr(&x));
        }); });

    loop.call_later(
        [&loop]()
        {
            fmt::print("3s later\n");

            std::shared_ptr<Semaphore> sem = std::make_shared<Semaphore>(1);
            Future<void> futs[10];

            for (int i = 0; i < 10; i++)
            {
                auto promise = Promise<void>();
                futs[i] = promise.get_future();
                loop.call_later(
                    [sem, i, &loop, promise = std::move(promise)]() mutable
                    {
                        fmt::print("+{}\n", i);
                        return sem->wait().then(
                            [sem, i, &loop, promise = std::move(promise), tester = ConstructorTest()]() mutable
                            {
                                std::cerr << fmt::format("-{} {}\n", i, fmt::ptr(&tester));
                                promise.resolve();
                                loop.call_later([sem]()
                                                { sem->signal(); },
                                                1s);
                            });
                    },
                    1s);
            }

            return when_all(futs, futs + 10)
                .then(
                    []()
                    { fmt::print("All done\n"); });
        },
        3s);

    loop.run_inplace();

    std::cerr << "Leave main\n";
}
