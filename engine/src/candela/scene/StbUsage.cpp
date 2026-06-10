// Single translation unit hosting the stb_image implementation.

#include "candela/core/Compiler.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_FAILURE_STRINGS

CD_PUSH_DISABLE_WARNINGS
#include <stb_image.h>
CD_POP_WARNINGS
