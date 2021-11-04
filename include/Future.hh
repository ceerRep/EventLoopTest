#ifndef _FUTURE_HH

#define _FUTURE_HH

#include <any>
#include <fmt/core.h>
#include <functional>
#include <iostream>
#include <memory>
#include <type_traits>
#include <typeinfo>
#include <utility>

#include <function2/function2.hpp>

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
                .call_soon([body = std::move(then_body), value = std::move(value)]() mutable
                           { 
                               if constexpr (!std::is_void_v<Value>)
                               body(std::move(*value)); 
                               else
                               body(); });
        }
    }

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

    template <typename F>
    Future<invoke_helper_t<F, Value>> then(F &&body);
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
        }
    }

    template <typename V = Value>
    std::enable_if_t<std::is_void_v<V>, void> resolve()
    {
        if (future)
        {
            future->ready = true;
            future->try_enqueue();
        }
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

template <typename Value>
template <typename F>
inline Future<invoke_helper_t<F, Value>> Future<Value>::then(F &&body)
{
    using RetType = invoke_helper_t<F, Value>;

    auto spfuture = std::make_shared<Future>(std::move(*this));

    Promise<RetType> promise;
    Future<RetType> future = promise.get_future();

    if constexpr (!std::is_void_v<Value>)
    {
        spfuture->then_body = std::move(
            [promise = std::move(promise),
             body = std::forward<F>(body)](Value value) mutable
            {
                if constexpr (!std::is_void_v<RetType>)
                    promise.resolve(body(std::move(value)));
                else
                {
                    body(std::move(value));
                    promise.resolve();
                }
            });
    }
    else
    {
        spfuture->then_body = std::move(
            [promise = std::move(promise),
             body = std::forward<F>(body)]() mutable
            {
                if constexpr (!std::is_void_v<RetType>)
                    promise.resolve(body());
                else
                {
                    body();
                    promise.resolve();
                }
            });
    }

    future.prev_future = spfuture;

    spfuture->try_enqueue();

    return future;
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

template <typename Func,
          std::enable_if_t<
              std::is_base_of_v<FutureBase, std::invoke_result_t<Func>>,
              bool>>
void Eventloop::call_soon(Func &&func_)
{
    call_soon(
        [func = std::forward<Func>(func_), this]() mutable -> void
        {
            auto fut = func();
            using Result_t = typename decltype(fut)::value_type;

            std::shared_ptr<Future<void>> sp = std::make_shared<Future<void>>();

            pending_futures.insert(sp);

            if constexpr (!std::is_void_v<Result_t>)
            {
                (*sp) = std::move(
                    fut.then(
                        [this, sp](Result_t)
                        {
                            pending_futures.erase(sp);
                        }));
            }
            else
            {
                (*sp) = std::move(
                    fut.then(
                        [this, sp]()
                        {
                            pending_futures.erase(sp);
                        }));
            }
        });
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

#endif
