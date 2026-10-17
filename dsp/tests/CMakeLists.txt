# Native unit tests + offline render tool (no framework, plain executables).
add_executable(absynth_tests tests.cpp)
target_link_libraries(absynth_tests PRIVATE absynth)

add_executable(absynth_render render.cpp)
target_link_libraries(absynth_render PRIVATE absynth)

add_executable(absynth_session session.cpp)
target_link_libraries(absynth_session PRIVATE absynth)

enable_testing()
add_test(NAME unit COMMAND absynth_tests ${CMAKE_SOURCE_DIR}/patches)
