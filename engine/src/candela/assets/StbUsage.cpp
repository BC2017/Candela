// Single translation unit hosting the stb_image + stb_image_write
// implementations.

#include "candela/core/Compiler.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_FAILURE_STRINGS
#define STB_IMAGE_WRITE_IMPLEMENTATION

CD_PUSH_DISABLE_WARNINGS
#include <stb_image.h>
#include <stb_image_write.h>
CD_POP_WARNINGS
