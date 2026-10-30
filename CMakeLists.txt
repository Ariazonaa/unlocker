cmake_minimum_required(VERSION 3.25)

project(unlocker VERSION 0.1.2 LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "Unlocker benötigt MSVC (Visual Studio 2022 Build Tools oder neuer).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Gemeinsame Compiler-/Linker-Einstellungen für alle Targets
add_library(unlocker_options INTERFACE)
target_compile_definitions(unlocker_options INTERFACE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN NOMINMAX STRICT
    _WIN32_WINNT=0x0A00 WINVER=0x0A00
    $<$<COMPILE_LANGUAGE:CXX>:UNLOCKER_VERSION_STRING="${PROJECT_VERSION}">
    $<$<COMPILE_LANGUAGE:CXX>:UNLOCKER_VERSION_WSTRING=L"${PROJECT_VERSION}">
)
target_compile_options(unlocker_options INTERFACE
    $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /EHsc /Gy /Gw>
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CONFIG:Release>>:/O2 /Oi /GL /Zi>
)
target_link_options(unlocker_options INTERFACE
    $<$<CONFIG:Release>:/OPT:REF /OPT:ICF /LTCG /INCREMENTAL:NO /DEBUG:FULL>
)

# Versionsressource (Version, Produktname, Beschreibung) für jedes ausgelieferte Binary
function(unlocker_version_resource target description original_filename filetype)
    set(RC_DESCRIPTION "${description}")
    set(RC_ORIGINAL_FILENAME "${original_filename}")
    set(RC_INTERNAL_NAME "${target}")
    set(RC_FILETYPE "${filetype}")
    set(rc_file "${CMAKE_CURRENT_BINARY_DIR}/${target}_version.rc")
    configure_file("${CMAKE_SOURCE_DIR}/src/common/version.rc.in" "${rc_file}" @ONLY)
    target_sources(${target} PRIVATE "${rc_file}")
endfunction()

# Optionaler Kernel-Treiber (v2): nur mit WDK und explizit -DUNLOCKER_BUILD_DRIVER=ON.
# Ohne WDK wird der Treiber übersprungen (Meldung in src/driver/CMakeLists.txt).
option(UNLOCKER_BUILD_DRIVER "unlocker_driver.sys bauen (braucht WDK mit km/)" OFF)

add_subdirectory(src/core)
add_subdirectory(src/app)
add_subdirectory(src/cli)
add_subdirectory(src/shell)
if(UNLOCKER_BUILD_DRIVER)
    # Der Treiber-Quellcode ist nicht Teil des öffentlichen Repos (nur die IOCTL-Schnittstelle in src/driver/include)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/driver/CMakeLists.txt")
        add_subdirectory(src/driver)
    else()
        message(STATUS "Treiber-Quellcode fehlt – unlocker_driver.sys wird nicht gebaut")
    endif()
endif()

enable_testing()
add_subdirectory(tests)
