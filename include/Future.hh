#ifndef _FUTURE_HH

#define _FUTURE_HH

#include <any>
#include <fmt/core.h>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <utility>

#include <function2/function2.hpp>
#include <vector>

#include "EventLoop.hh"
#include "FutureBase.hh"
#include "spinlock.hh"
#include "stacktrace.hh"
#include "util.hh"

template <typename Value>
class Promise;

template <typename Value>
class Future : public FutureBase
{
    Promise<Value> *promise;

    bool ready;
    void_type_helper_t<Value> value;

    std::any prev_future;
    void_function_helper_t<Value> then_body;
    fu2::unique_function<void(void)> finally_body;

    Future(Promise<Value> *promise) : promise(promise), ready(false) {}

    void try_enqueue()
    {
        if (ready && then_body)
        {
            Eventloop::get_loop(Eventloop::get_cpu_index())
                .call_soon(
                    [body = std::move(then_body),
                     finally_body = std::move(finally_body),
                     value = std::move(value),
                     prev_fut = std::move(prev_future)]() mutable
                    {
                        if constexpr (!std::is_void_v<Value>)
                            body(std::move(value));
                        else
                            body();
                        if (!finally_body.empty())
                            finally_body();
                    });
        }
    }

    template <typename Func, typename... V>
    auto generate_future_chain(Func &&body);

    template <typename Value1>
    friend class Promise;

    template <typename Value1>
    friend class Future;

public:
    using value_type = Value;

    Future() : promise(nullptr), ready(false) {}
    Future(Future &&fut)
    {
        *this = std::move(fut);
    }
    Future(const Future &fut) = delete;
    ~Future();

    Future &operator=(Future &&fut);

    template <typename Func>
    auto then(Func &&body);

    auto finally(fu2::unique_function<void(void)> &&body)
    {
        finally_body = std::move(body);
        Future ret = std::move(*this);
        return ret;
    }
};

template <typename Value>
class Promise
{
    int loopno;
    Future<Value> *future;

    template <typename Value1>
    friend class Future;

    template <typename Value1>
    friend class Promise;

public:
    using value_type = Value;

    Promise() : future(nullptr), loopno(Eventloop::get_cpu_index()) {}
    Promise(Promise &&pro)
    {
        *this = std::move(pro);
    }
    Promise(const Promise &) = delete;

    ~Promise()
    {
        // std::cerr << fmt::format("Promise {} destroyed, future is {}\n", fmt::ptr(this), fmt::ptr(future));
        if (future)
        {
            // asm("int3");
            if (!future->ready)
            {
                std::cerr << fmt::format("Warning: Broken promise of type {} at {}\nStack trace: \n{}",
                                         typeid(Future<Value>).name(),
                                         fmt::ptr(future),
                                         get_stack_trace());
            }
        }
    }

    Promise &operator=(Promise &&pro)
    {
        // std::cerr << fmt::format("Promise {} moved from {}, future is {}\n", fmt::ptr(this), fmt::ptr(&pro), fmt::ptr(future));
        future = pro.future;
        loopno = pro.loopno;
        pro.future = nullptr;

        future->promise = this;

        return *this;
    }

    Future<Value> get_future()
    {
        Future future(this);
        this->future = &future;

        return std::move(future);
    }

    template <typename... Args>
    void resolve(Args &&...args)
    {
        if (loopno == -1)
            throw std::runtime_error(fmt::format("Invalid loopno: {}", loopno));

        if (int current_loop = Eventloop::get_cpu_index(); loopno != current_loop)
        {
            // Submit to correct loop
            Eventloop::get_loop(loopno).call_soon(
                [pro = std::move(*this), args = std::tuple(std::move(args)...)]() mutable
                {
                    std::apply(
                        [&pro](auto &&...args) mutable
                        {
                            pro.resolve(std::move(args)...);
                        },
                        std::move(args));
                });

            return;
        }

        if (future)
        {
            if constexpr (!std::is_void_v<Value>)
                new (&(future->value)) decltype(future->value){std::forward<Args>(args)...};

            future->ready = true;

            future->try_enqueue();

            future->promise = nullptr;
            future = nullptr;
        }
        else
            std::cerr << fmt::format("Warning: trying to resolve a promise without future\nStack trace: \n{}", get_stack_trace());
    }

    int get_loop_index() const
    {
        return loopno;
    }
};

template <typename Value>
inline Future<Value>::~Future()
{
    // std::cerr << fmt::format("Future {} destroyed, promise is {}\n", fmt::ptr(this), fmt::ptr(promise));
    if (promise)
    {
        promise->future = nullptr;
    }
}

template <typename Value>
template <typename Func, typename... V>
auto Future<Value>::generate_future_chain(Func &&body)
{
    using RetType = invoke_helper_t<Func, Value>;
    using RealRetType = remove_future_t<RetType>;

    auto spfuture = std::make_shared<Future<Value>>(std::move(*this));

    Promise<RealRetType> promise;
    Future<RealRetType> future = promise.get_future();
    future.finally_body = std::move(finally_body);
    spfuture->then_body = std::move(
        [promise = std::move(promise),
         body = std::forward<Func>(body)](V... args) mutable
        {
            if constexpr (std::is_base_of_v<FutureBase, RetType>)
            {
                auto inner_lambda = [&](auto... vs) mutable
                {
                    auto shared_fut = std::make_shared<Future<void>>();
                    auto fut_tmp =
                        body(std::move(args)...)
                            .then(
                                [shared_fut, promise = std::move(promise)](std::remove_pointer_t<decltype(vs)>... vs1) mutable
                                { promise.resolve(std::forward<std::remove_pointer_t<decltype(vs)>>(vs1)...); });
                    *shared_fut = std::move(fut_tmp);
                };
                if constexpr (!std::is_void_v<RealRetType>)
                {
                    inner_lambda((RealRetType *)nullptr);
                }
                else
                {
                    inner_lambda();
                }
            }
            else
            {
                if constexpr (!std::is_void_v<RealRetType>)
                {
                    promise.resolve(body(std::move(args)...));
                }
                else
                {
                    body(std::move(args)...);
                    promise.resolve();
                }
            }
        });

    future.prev_future = spfuture;

    spfuture->try_enqueue();

    return future;
}

template <typename Value>
template <typename Func>
[[nodiscard]] inline auto Future<Value>::then(Func &&body)
{
    if constexpr (std::is_void_v<Value>)
        return std::move(generate_future_chain<Func>(std::forward<Func>(body)));
    else
        return std::move(generate_future_chain<Func, Value>(std::forward<Func>(body)));
}

template <typename Value>
inline Future<Value> &Future<Value>::operator=(Future<Value> &&fut)
{
    // std::cerr << fmt::format("Future {} moved from {}, promise is {}\n", fmt::ptr(this), fmt::ptr(&fut), fmt::ptr(fut.promise));

    promise = fut.promise;
    ready = fut.ready;
    new (&value) decltype(value){std::move(fut.value)};
    then_body = std::move(fut.then_body);
    finally_body = std::move(fut.finally_body);
    prev_future = std::move(fut.prev_future);

    fut.ready = false;
    fut.promise = nullptr;

    if (promise)
    {
        promise->future = this;
    }

    return *this;
}

template <typename Iterator>
[[nodiscard]] Future<void> when_all(Iterator begin, Iterator end)
{
    static_assert(std::is_same_v<std::remove_reference_t<decltype(*begin)>, Future<void>>, "Iterator should point to Future<void>");

    auto vct = std::make_shared<std::vector<Future<void>>>();

    while (begin != end)
    {
        vct->emplace_back(std::move(*begin));
        begin++;
    }

    std::shared_ptr<int> counter = std::make_shared<int>(vct->size());
    std::shared_ptr<Promise<void>> promise = std::make_shared<Promise<void>>();

    for (auto &fut : *vct)
    {
        auto next_fut = fut.then(
            [counter, promise, vct]()
            {
                (*counter)--;

                if (*counter == 0)
                    promise->resolve();
            });
        fut = std::move(next_fut);
    }

    return promise->get_future();
}

template <typename Func, typename... Args>
auto future_function_transform(Func &&func_)
{
    using RetType = typename std::invoke_result_t<Func, Args...>::value_type;
    return fu2::unique_function<RetType(Args...)>(
        std::move(
            [func = std::forward<Func>(func_), &loop = Eventloop::get_loop(Eventloop::get_cpu_index())](Args... args) mutable -> void
            {
                auto fut = func(args...);

                std::shared_ptr<Future<void>> sp = std::make_shared<Future<void>>();

                {
                    std::lock_guard guard{loop.pending_future_lock};
                    loop.pending_futures.insert(sp);
                }

                auto inner_lambda = [&](auto... rs) mutable
                {
                    (*sp) = std::move(
                        fut.then(
                            [&loop, sp](std::remove_pointer_t<decltype(rs)>...)
                            {
                                std::lock_guard guard{loop.pending_future_lock};
                                loop.pending_futures.erase(sp);
                            }));
                };

                if constexpr (!std::is_void_v<RetType>)
                {
                    inner_lambda((RetType *)nullptr);
                }
                else
                {
                    inner_lambda();
                }
            }));
}

template <typename Func,
          std::enable_if_t<
              std::is_base_of_v<FutureBase, std::invoke_result_t<Func>>,
              bool>>
void Eventloop::call_soon(Func &&func_)
{
    call_soon(std::move(future_function_transform<Func>(std::forward<Func>(func_))));
}

template <typename Func, typename Duration,
          std::enable_if_t<
              std::is_base_of_v<FutureBase, std::invoke_result_t<Func>>,
              bool>>
void Eventloop::call_later(Func &&func_, Duration duration)
{
    call_later(std::move(future_function_transform<Func>(std::forward<Func>(func_))), duration);
}

template <typename T, typename Value = std::remove_reference_t<T>>
[[nodiscard]] Future<Value> make_ready_future(T &&value)
{
    Promise<Value> promise;
    auto fut = promise.get_future();
    promise.resolve(std::forward<T>(value));

    return fut;
}

[[nodiscard]] inline Future<void> make_ready_future()
{
    Promise<void> promise;
    auto fut = promise.get_future();
    promise.resolve();

    return fut;
}

template <typename Duration>
[[nodiscard]] inline Future<void> async_sleep(Duration duration)
{
    auto promise = Promise<void>();
    auto future = promise.get_future();

    Eventloop::get_loop(Eventloop::get_cpu_index())
        .call_later([promise = std::move(promise)]() mutable
                    { promise.resolve(); },
                    duration);

    return std::move(future);
}

template <typename Func>
[[nodiscard]] inline Future<void> submit_to(int n, Func &&func)
{
    auto unique_promise = std::make_unique<Promise<void>>();
    auto fut = unique_promise->get_future();

    Eventloop::get_loop(n).call_soon(
        [unique_promise = std::move(unique_promise), func = std::forward<Func>(func), loop = Eventloop::get_cpu_index()]() mutable
        {
            if constexpr (std::is_void_v<Func>)
            {
                func();
                Eventloop::get_loop(loop).call_soon(
                    [unique_promise = std::move(unique_promise)]() mutable
                    {
                        unique_promise->resolve();
                    });
            }
            else if constexpr (std::is_base_of_v<FutureBase, std::invoke_result_t<Func>>)
            {
                auto result = func().then(
                    [unique_promise = std::move(unique_promise), loop]() mutable
                    {
                        Eventloop::get_loop(loop).call_soon(
                            [unique_promise = std::move(unique_promise)]() mutable
                            {
                                unique_promise->resolve();
                            });
                    });
                return result;
            }
            else
            {
                static_assert(
                    std::is_void_v<Func> || std::is_base_of_v<FutureBase, std::invoke_result_t<Func>>,
                    "Func type should be either void or Future");
            }
        });

    return fut;
}

#endif
