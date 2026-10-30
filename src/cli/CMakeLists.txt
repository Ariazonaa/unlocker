add_executable(unlockerc main.cpp cli.manifest ${CMAKE_SOURCE_DIR}/src/common/strings.cpp)
unlocker_version_resource(unlockerc "Unlocker Kommandozeile" unlockerc.exe VFT_APP)
target_include_directories(unlockerc PRIVATE ${CMAKE_SOURCE_DIR}/src/common)
target_link_libraries(unlockerc PRIVATE unlocker_core)
