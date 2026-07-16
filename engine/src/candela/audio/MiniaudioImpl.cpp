// Single translation unit hosting the miniaudio implementation. Every other TU
// includes <miniaudio.h> for declarations only. Mirrors the VmaUsage.cpp /
// StbUsage.cpp single-header pattern; this file is compiled with warnings
// disabled (see engine/CMakeLists.txt) because miniaudio's implementation is
// far too warning-heavy for our -Wall -Werror / /W4 /WX level.

#include "candela/core/Compiler.h"

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING // playback only — we never write WAV/etc.

CD_PUSH_DISABLE_WARNINGS
#include <miniaudio.h>
CD_POP_WARNINGS
