#include "candela/core/Log.h"

#include <spdlog/sinks/stdout_color_sinks.h>

namespace candela {

std::shared_ptr<spdlog::logger> Log::s_logger;

void Log::init() {
    s_logger = spdlog::stdout_color_mt("candela");
    s_logger->set_pattern("[%T.%e] [%^%l%$] %v");
    s_logger->set_level(spdlog::level::trace);
}

spdlog::logger& Log::get() {
    return *s_logger;
}

} // namespace candela
