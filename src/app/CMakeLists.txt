add_executable(unlocker WIN32
    main.cpp
    main_window.cpp
    main_window.h
    controls.cpp
    controls.h
    dialog.cpp
    dialog.h
    drop_target.cpp
    drop_target.h
    instance.cpp
    instance.h
    settings.cpp
    settings.h
    shell_package.cpp
    shell_package.h
    theme.cpp
    theme.h
    resource.h
    app.rc
    app.manifest
    ${CMAKE_SOURCE_DIR}/src/common/strings.cpp
    ${CMAKE_SOURCE_DIR}/src/common/strings.h
)

unlocker_version_resource(unlocker "Unlocker - gesperrte Dateien und Ordner freigeben" unlocker.exe VFT_APP)
target_include_directories(unlocker PRIVATE ${CMAKE_SOURCE_DIR}/src/common)
target_link_libraries(unlocker PRIVATE
    unlocker_core
    comctl32 dwmapi uxtheme shell32 shlwapi ole32 gdi32 gdiplus user32
    WindowsApp  # C++/WinRT (PackageManager für den Kontextmenü-Schalter), Header aus dem Windows SDK
)
