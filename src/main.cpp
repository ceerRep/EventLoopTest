#include <functional>
#include <iostream>
#include <memory>

#include "EventLoop.hh"
#include <Future.hh>

// TODO: warning broken promise

std::function<void(void)> a;
fu2::unique_function<void(void)> then_body;

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

            return p3.get_future().then([](){
                std::cout << "Test" << std::endl;
            });

        });

    loop.call_soon([&p2]()
                   { p2.resolve(1919810); });

    loop.run_inplace();

    std::cerr << "Leave main\n";
}
