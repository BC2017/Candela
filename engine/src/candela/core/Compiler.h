#pragma once

// Cross-compiler helpers. Candela builds with MSVC (primary) and GCC/Clang
// (CLion's bundled MinGW today, Linux later).

#if defined(_MSC_VER)
    #define CD_DEBUGBREAK() __debugbreak()
#else
    #define CD_DEBUGBREAK() __builtin_trap()
#endif

// Silences all warnings around third-party includes our warning level trips on.
#if defined(_MSC_VER)
    #define CD_PUSH_DISABLE_WARNINGS __pragma(warning(push, 0))
    #define CD_POP_WARNINGS __pragma(warning(pop))
#elif defined(__GNUC__)
    #define CD_PUSH_DISABLE_WARNINGS                                           \
        _Pragma("GCC diagnostic push")                                         \
        _Pragma("GCC diagnostic ignored \"-Wunused-parameter\"")               \
        _Pragma("GCC diagnostic ignored \"-Wunused-variable\"")                \
        _Pragma("GCC diagnostic ignored \"-Wmissing-field-initializers\"")
    #define CD_POP_WARNINGS _Pragma("GCC diagnostic pop")
#else
    #define CD_PUSH_DISABLE_WARNINGS
    #define CD_POP_WARNINGS
#endif
