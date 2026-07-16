# Tumble — a physics playground built on the engine (see src/main.cpp).
add_executable(candela-tumble
    src/main.cpp
)

target_link_libraries(candela-tumble PRIVATE candela)

target_include_directories(candela-tumble PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_compile_definitions(candela-tumble PRIVATE
    TUMBLE_CONTENT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/content"
)

candela_apply_warnings(candela-tumble)
