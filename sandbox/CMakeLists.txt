add_executable(sandbox main.cpp)
set_target_properties(sandbox PROPERTIES OUTPUT_NAME "candela-sandbox")
target_link_libraries(sandbox PRIVATE candela)
candela_apply_warnings(sandbox)
