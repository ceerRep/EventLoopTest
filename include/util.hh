#ifndef _UTIL_HH

#define _UTIL_HH

#include <type_traits>
#include <function2/function2.hpp>

template <typename F, typename V>
std::invoke_result_t<F, V> invoke_helper();

template <typename F, typename V>
std::invoke_result_t<F> invoke_helper();

template <typename F, typename V>
using invoke_helper_t = decltype(invoke_helper<F, V>());

template <typename Value>
std::enable_if_t<std::is_void_v<Value>, fu2::unique_function<void(void)>> void_function_helper();

template <typename Value>
std::enable_if_t<!std::is_void_v<Value>, fu2::unique_function<void(Value)>> void_function_helper();

template <typename Value>
using void_function_helper_t = decltype(void_function_helper<Value>());

#endif
