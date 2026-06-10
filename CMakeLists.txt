cmake_minimum_required(VERSION 3.28)
project(Candela VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

# Strict warnings for Candela targets only — third-party code is built with its
# own defaults, so this must not be a global add_compile_options.
function(candela_apply_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX /permissive- /Zc:preprocessor /utf-8)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
  endif()
endfunction()

find_package(Vulkan REQUIRED)

include(cmake/Dependencies.cmake)

add_subdirectory(engine)
add_subdirectory(sandbox)
add_subdirectory(editor)
