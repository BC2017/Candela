include(FetchContent)

# Dependency build options must be set before MakeAvailable.
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

set(VK_BOOTSTRAP_TEST OFF CACHE BOOL "" FORCE)

# Tracy client is compiled in but dormant until a profiler connects.
set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)

FetchContent_Declare(volk
  GIT_REPOSITORY https://github.com/zeux/volk.git
  GIT_TAG 1.4.304
  GIT_SHALLOW ON)

FetchContent_Declare(vkbootstrap
  GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap.git
  GIT_TAG v1.3.290
  GIT_SHALLOW ON)

FetchContent_Declare(vma
  GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
  GIT_TAG v3.1.0
  GIT_SHALLOW ON)

FetchContent_Declare(glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG 3.4
  GIT_SHALLOW ON)

FetchContent_Declare(glm
  GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG 1.0.1
  GIT_SHALLOW ON)

FetchContent_Declare(spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG v1.15.0
  GIT_SHALLOW ON)

FetchContent_Declare(tracy
  GIT_REPOSITORY https://github.com/wolfpld/tracy.git
  GIT_TAG v0.11.1
  GIT_SHALLOW ON)

FetchContent_Declare(fastgltf
  GIT_REPOSITORY https://github.com/spnda/fastgltf.git
  GIT_TAG v0.8.0
  GIT_SHALLOW ON)

FetchContent_Declare(stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG master
  GIT_SHALLOW ON)

FetchContent_Declare(entt
  GIT_REPOSITORY https://github.com/skypjack/entt.git
  GIT_TAG v3.13.2
  GIT_SHALLOW ON)

set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
  GIT_SHALLOW ON)

FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.91.5-docking
  GIT_SHALLOW ON)

# ImGuizmo's last tag (1.83) predates ImGui 1.91 API changes; pin master
# (commit from 2026-06, compatible with the docking 1.91.x line). Its own
# CMakeLists can't see our ImGui — fetch sources only (bogus SOURCE_SUBDIR
# prevents add_subdirectory) and compile ImGuizmo.cpp into candela_imgui.
FetchContent_Declare(imguizmo
  GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
  GIT_TAG be8aa4aeab86b402701c8c1df011bd8cd776760b
  SOURCE_SUBDIR does-not-exist)

# miniaudio is a single header (plus a bundled CMake that builds tests/extras
# we don't want). Fetch sources only — a bogus SOURCE_SUBDIR keeps FetchContent
# from add_subdirectory-ing it — and expose the header via an INTERFACE target
# below, the same pattern as stb. The implementation is compiled in exactly one
# TU (engine/src/candela/audio/MiniaudioImpl.cpp).
FetchContent_Declare(miniaudio
  GIT_REPOSITORY https://github.com/mackron/miniaudio.git
  GIT_TAG 0.11.21
  GIT_SHALLOW ON
  SOURCE_SUBDIR does-not-exist)

FetchContent_MakeAvailable(volk vkbootstrap vma glfw glm spdlog tracy fastgltf stb
# Jolt Physics. Its buildable CMakeLists lives under Build/, hence
# SOURCE_SUBDIR. All ABI-affecting JPH_* defines are carried by the exported
# `Jolt` target's INTERFACE_COMPILE_DEFINITIONS and inherited automatically by
# every TU that includes Jolt headers — this is the single source of truth,
# eliminating the classic "headers/lib built with different flags" corruption.
set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE) # respect our toolchain flags
# Jolt defaults this ON, which forces /MT on its target while every other dep
# uses the default dynamic runtime (/MD) — a mismatch that fails to link on
# MSVC. Force the dynamic runtime so all targets agree.
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
# Force the SSE2 x86-64 baseline: both MSVC and GCC compile SSE2 intrinsics
# with no special -march flag, so consumers need no arch flags to match Jolt's
# ABI. (Any USE_* left ON emits a JPH_USE_* interface define whose intrinsics
# GCC refuses to compile without a matching -msseX/-mavx flag on our targets.)
set(USE_SSE4_1 OFF CACHE BOOL "" FORCE)
set(USE_SSE4_2 OFF CACHE BOOL "" FORCE)
set(USE_AVX OFF CACHE BOOL "" FORCE)
set(USE_AVX2 OFF CACHE BOOL "" FORCE)
set(USE_AVX512 OFF CACHE BOOL "" FORCE)
set(USE_LZCNT OFF CACHE BOOL "" FORCE)
set(USE_TZCNT OFF CACHE BOOL "" FORCE)
set(USE_F16C OFF CACHE BOOL "" FORCE)
set(USE_FMADD OFF CACHE BOOL "" FORCE)
set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
set(CROSS_PLATFORM_DETERMINISTIC OFF CACHE BOOL "" FORCE)
set(DOUBLE_PRECISION OFF CACHE BOOL "" FORCE) # RVec3 == Vec3 (float)
FetchContent_Declare(jolt
  GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
  GIT_TAG v5.3.0
  GIT_SHALLOW ON
  SOURCE_SUBDIR Build)

FetchContent_MakeAvailable(volk vkbootstrap vma glfw glm spdlog tracy fastgltf stb
                           entt nlohmann_json imgui imguizmo miniaudio jolt)

# Without an installed SDK (CI), volk has no vulkan.h on its include path —
# point every consumer at the Vulkan::Headers target explicitly.
target_link_libraries(volk PUBLIC Vulkan::Headers)

# Dear ImGui has no CMake build — wrap the core plus the GLFW + Vulkan
# (dynamic rendering, volk) backends in a static lib. ImGuizmo rides along.
add_library(candela_imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
  ${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp
)
target_include_directories(candela_imgui PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
  ${imguizmo_SOURCE_DIR}/src
)
target_compile_definitions(candela_imgui PUBLIC
  IMGUI_IMPL_VULKAN_USE_VOLK
  VK_NO_PROTOTYPES
  # The editor loads its UI font from ImGui's bundled fonts.
  CANDELA_IMGUI_FONT_DIR="${imgui_SOURCE_DIR}/misc/fonts"
)
target_link_libraries(candela_imgui PUBLIC volk::volk glfw)

# stb has no CMake build — expose it as an interface include target.
add_library(stb INTERFACE)
target_include_directories(stb INTERFACE ${stb_SOURCE_DIR})

# miniaudio has no usable CMake for us — expose the header as an interface
# target. On Linux the implementation TU needs pthread/dl/m; Windows and macOS
# pull their audio backends from the OS SDK automatically.
add_library(miniaudio INTERFACE)
target_include_directories(miniaudio INTERFACE ${miniaudio_SOURCE_DIR})
if(UNIX AND NOT APPLE)
  find_package(Threads REQUIRED)
  target_link_libraries(miniaudio INTERFACE Threads::Threads ${CMAKE_DL_LIBS} m)
endif()
