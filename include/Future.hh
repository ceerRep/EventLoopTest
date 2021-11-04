#ifndef _FUTURE_HH

#define _FUTURE_HH

#include <any>
#include <fmt/core.h>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <type_traits>
#include <typeinfo>
#include <utility>

#include <function2/function2.hpp>
#include <vector>

#include "EventLoop.hh"
#include "FutureBase.hh"
#include "util.hh"

template <typename Value>
class Promise;

template <typename Value>
class Future : public FutureBase
{
    Promise<Value> *promise;

    bool ready;
    std::shared_ptr<Value> value;

    std::any prev_future;
    void_function_helper_t<Value> then_body;

    Future(Promise<Value> *promise) : promise(promise), ready(false) {}

    void try_enqueue()
    {
        if (ready && then_body)
        {
            Eventloop::get_loop(Eventloop::get_cpu_index())
                .call_soon([body = std::move(then_body), value = std::move(value), prev_fut = std::move(prev_future)]() mutable
                           { 
                                if constexpr (!std::is_void_v<Value>)
                                    body(std::move(*value)); 
                                else
                                    body(); });
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
};

template <typename Value>
class Promise
{
    Future<Value> *future;

    template <typename Value1>
    friend class Future;

    template <typename Value1>
    friend class Promise;

public:
    using value_type = Value;

    Promise() : future(nullptr) {}
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
                std::cerr << fmt::format("Warning: Broken promise of type {} at {}\n", typeid(Future<Value>).name(), fmt::ptr(future));
            }
            future->promise = nullptr;
        }
    }

    Promise &operator=(Promise &&pro)
    {
        // std::cerr << fmt::format("Promise {} moved from {}, future is {}\n", fmt::ptr(this), fmt::ptr(&pro), fmt::ptr(future));
        future = pro.future;
        pro.future = nullptr;
        if (future)
            future->promise = this;

        return *this;
    }

    Future<Value> get_future()
    {
        Future future(this);
        this->future = &future;

        return std::move(future);
    }

    template <typename V = Value>
    std::enable_if_t<!std::is_void_v<V>, void> resolve(V &&value)
    {
        if (future)
        {
            future->value = std::make_shared<Value>(std::forward<V>(value));
            future->ready = true;

            future->try_enqueue();

            future->promise = nullptr;
            future = nullptr;
        }
        else
            std::cerr << "Warning: trying to resolve a promise without future\n";
    }

    template <typename V = Value>
    std::enable_if_t<std::is_void_v<V>, void> resolve()
    {
        if (future)
        {
            future->ready = true;

            future->try_enqueue();

            future->promise = nullptr;
            future = nullptr;
        }
        else
            std::cerr << "Warning: trying to resolve a promise without future\n";
    }
};

template <typename Value>
inline Future<Value>::~Future()
{
    // std::cerr << fmt::format("Future {} destroyed, promise is {}\n", fmt::ptr(this), fmt::ptr(promise));
    if (promise)
    {
        // asm("int3");
        promise->future = nullptr;
    }
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

                if constexpr (!std::is_void_v<RetType>)
                {
                    (*sp) = std::move(
                        fut.then(
                            [&loop, sp](RetType)
                            {
                                std::lock_guard guard{loop.pending_future_lock};
                                loop.pending_futures.erase(sp);
                            }));
                }
                else
                {
                    (*sp) = std::move(
                        fut.then(
                            [&loop, sp]()
                            {
                                std::lock_guard guard{loop.pending_future_lock};
                                loop.pending_futures.erase(sp);
                            }));
                }
            }));
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
    spfuture->then_body = std::move(
        [promise = std::move(promise),
         body = std::forward<Func>(body)](V... args) mutable
        {
            if constexpr (std::is_base_of_v<FutureBase, RetType>)
            {
                if constexpr (!std::is_void_v<RealRetType>)
                {
                    auto shared_fut = std::make_shared<Future<void>>();
                    auto fut_tmp = body(std::move(args)...).then([shared_fut, promise = std::move(promise)](RealRetType v) mutable
                                                                 { promise.resolve(v); });
                    *shared_fut = std::move(fut_tmp);
                }
                else
                {
                    auto shared_fut = std::make_shared<Future<void>>();
                    auto fut_tmp = body(std::move(args)...).then([shared_fut, promise = std::move(promise)]() mutable
                                                                 { promise.resolve(); });
                    *shared_fut = std::move(fut_tmp);
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
inline auto Future<Value>::then(Func &&body)
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
    value = std::move(fut.value);
    then_body = std::move(fut.then_body);
    prev_future = std::move(fut.prev_future);

    fut.ready = false;
    fut.promise = nullptr;

    if (promise)
        promise->future = this;

    return *this;
}

template <typename Iterator>
Future<void> when_all(Iterator begin, Iterator end)
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
Future<Value> make_ready_future(T &&value)
{
    Promise<Value> promise;
    auto fut = promise.get_future();
    promise.resolve(std::forward<T>(value));

    return fut;
}

inline Future<void> make_ready_future()
{
    Promise<void> promise;
    auto fut = promise.get_future();
    promise.resolve();

    return fut;
}

template <typename Duration>
inline Future<void> async_sleep(Duration duration)
{
    auto promise = Promise<void>();
    auto future = promise.get_future();

    Eventloop::get_loop(Eventloop::get_cpu_index())
        .call_later([promise = std::move(promise)]() mutable
                    { promise.resolve(); },
                    duration);

    return std::move(future);
}

#endif
