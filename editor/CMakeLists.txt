add_executable(candela-studio
    src/Commands.cpp
    src/EditorApp.cpp
    src/main.cpp
)
target_link_libraries(candela-studio PRIVATE candela candela_imgui)
if(WIN32)
  # Native open-file dialog for File > Import Blender Scene.
  target_link_libraries(candela-studio PRIVATE comdlg32)
endif()
candela_apply_warnings(candela-studio)
