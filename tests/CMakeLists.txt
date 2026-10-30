add_executable(core_tests core_tests.cpp ${CMAKE_SOURCE_DIR}/src/cli/cli.manifest)
target_link_libraries(core_tests PRIVATE unlocker_core)

add_test(NAME core_tests COMMAND core_tests)

# Drag & Drop-Selbsttest (Sandbox): eigener COM-Thread, kein ctest-Teil – von tests\sandbox\run-tests.ps1 aufgerufen.
add_executable(drop_self_test drop_self_test.cpp drop_source.cpp)
target_link_libraries(drop_self_test PRIVATE unlocker_options ole32 user32 gdi32)
