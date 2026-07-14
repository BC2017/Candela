# Emberwake — procedural action dungeon-crawler built on the engine.
add_executable(candela-emberwake
    src/main.cpp
)

target_link_libraries(candela-emberwake PRIVATE candela candela_imgui)

target_include_directories(candela-emberwake PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_compile_definitions(candela-emberwake PRIVATE
    EMBERWAKE_CONTENT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/content"
)

candela_apply_warnings(candela-emberwake)

# Pure-logic dungeon self-test: generates hundreds of seeded dungeons and
# proves connectivity + placement invariants. No engine, no GPU.
add_executable(emberwake-dungeontest
    src/DungeonTest.cpp
)

target_link_libraries(emberwake-dungeontest PRIVATE glm::glm)

target_include_directories(emberwake-dungeontest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

candela_apply_warnings(emberwake-dungeontest)
