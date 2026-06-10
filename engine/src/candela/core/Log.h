#pragma once

#include "candela/core/Compiler.h"

#include <spdlog/spdlog.h>

#include <memory>

namespace candela {

class Log {
public:
    static void init();
    static spdlog::logger& get();

private:
    static std::shared_ptr<spdlog::logger> s_logger;
};

} // namespace candela

#define CD_TRACE(...) ::candela::Log::get().trace(__VA_ARGS__)
#define CD_INFO(...) ::candela::Log::get().info(__VA_ARGS__)
#define CD_WARN(...) ::candela::Log::get().warn(__VA_ARGS__)
#define CD_ERROR(...) ::candela::Log::get().error(__VA_ARGS__)

// Always-on assert (engine-development phase). Optional message arguments are
// formatted with spdlog/fmt: CD_ASSERT(x > 0, "x was {}", x);
#define CD_ASSERT(cond, ...)                                                   \
    do {                                                                       \
        if (!(cond)) {                                                         \
            CD_ERROR("Assertion failed: `{}` at {}:{}", #cond, __FILE__,       \
                     __LINE__);                                                \
            __VA_OPT__(CD_ERROR(__VA_ARGS__);)                                 \
            CD_DEBUGBREAK();                                                   \
        }                                                                      \
    } while (0)
