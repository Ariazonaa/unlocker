add_library(unlocker_shell SHARED
    explorer_command.cpp
    unlocker_shell.def
)
unlocker_version_resource(unlocker_shell "Unlocker Explorer-Erweiterung (IExplorerCommand)" unlocker_shell.dll VFT_DLL)
target_link_libraries(unlocker_shell PRIVATE
    unlocker_options
    shlwapi shell32 ole32 runtimeobject
)
