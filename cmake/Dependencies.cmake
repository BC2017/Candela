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

FetchContent_MakeAvailable(volk vkbootstrap vma glfw glm spdlog tracy fastgltf stb
                           entt nlohmann_json imgui imguizmo)

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
)
target_link_libraries(candela_imgui PUBLIC volk::volk glfw)

# stb has no CMake build — expose it as an interface include target.
add_library(stb INTERFACE)
target_include_directories(stb INTERFACE ${stb_SOURCE_DIR})
