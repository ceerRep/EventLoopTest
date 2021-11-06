#include "stacktrace.hh"

#include <memory>
#include <sstream>
#include <vector>

#include <execinfo.h>

std::string get_stack_trace()
{
    static std::vector<void *> buffer(128);

    std::stringstream ss;
    int size = backtrace(buffer.data(), buffer.size());

    auto symbols = std::unique_ptr<char *, decltype(&free)>(backtrace_symbols(buffer.data(), size), free);

    for (int i = 0; i < size; i++)
        ss << symbols.get()[i] << '\n';

    return ss.str();
}
