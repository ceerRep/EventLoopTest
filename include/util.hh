#ifndef _UTIL_HH

#define _UTIL_HH

#include <function2/function2.hpp>
#include <type_traits>

template <typename Value>
class Future;

template <typename F, typename V>
std::invoke_result_t<F, V> invoke_helper();

template <typename F, typename V>
std::invoke_result_t<F> invoke_helper();

template <typename F, typename V>
using invoke_helper_t = std::remove_reference_t<decltype(invoke_helper<F, V>())>;

template <typename Value>
std::enable_if_t<std::is_void_v<Value>, fu2::unique_function<void(void)>> void_function_helper();

template <typename Value>
std::enable_if_t<!std::is_void_v<Value>, fu2::unique_function<void(Value)>> void_function_helper();

template <typename Value>
using void_function_helper_t = decltype(void_function_helper<Value>());

template <typename V>
struct void_type_helper
{
    using type = V;
};

template <>
struct void_type_helper<void>
{
    using type = int;
};

template <typename V>
using void_type_helper_t = typename void_type_helper<V>::type;

template <typename V>
struct remove_future
{
    using type = V;
};

template <typename V>
struct remove_future<Future<V>>
{
    using type = V;
};

template <typename V>
using remove_future_t = typename remove_future<V>::type;

template <typename... Args>
struct argument_traits
{
    enum
    {
        nargs = sizeof...(Args)
    };

    template <size_t i>
    struct arg
    {
        typedef typename std::tuple_element<i, std::tuple<Args...>>::type type;
    };
};

#endif
