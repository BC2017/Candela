# Lightkeeper — a small game built on the engine (see src/main.cpp).
add_executable(candela-lightkeeper
    src/main.cpp
)

target_link_libraries(candela-lightkeeper PRIVATE candela)

target_include_directories(candela-lightkeeper PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_compile_definitions(candela-lightkeeper PRIVATE
    LIGHTKEEPER_CONTENT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/content"
)

candela_apply_warnings(candela-lightkeeper)

# Pure-logic level self-test: no engine, no GPU — runs on any build machine.
add_executable(lightkeeper-leveltest
    src/LevelTest.cpp
)

target_link_libraries(lightkeeper-leveltest PRIVATE glm::glm)

target_include_directories(lightkeeper-leveltest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

candela_apply_warnings(lightkeeper-leveltest)
