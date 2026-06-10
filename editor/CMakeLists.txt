add_executable(candela-studio
    src/Commands.cpp
    src/EditorApp.cpp
    src/main.cpp
)
target_link_libraries(candela-studio PRIVATE candela candela_imgui)
candela_apply_warnings(candela-studio)
