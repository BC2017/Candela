add_executable(sandbox main.cpp)
set_target_properties(sandbox PROPERTIES OUTPUT_NAME "candela-sandbox")
target_link_libraries(sandbox PRIVATE candela)
candela_apply_warnings(sandbox)

# Pure-logic animation-sampling self-test: no engine, no GPU — links only glm
# and the header-only AnimationSampling.h, like lightkeeper-leveltest.
add_executable(anim-sampling-test AnimationSamplingTest.cpp)
target_link_libraries(anim-sampling-test PRIVATE glm::glm)
target_include_directories(anim-sampling-test PRIVATE
    ${CMAKE_SOURCE_DIR}/engine/src)
candela_apply_warnings(anim-sampling-test)
