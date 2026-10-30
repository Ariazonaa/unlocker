add_library(unlocker_core STATIC
    include/unlocker/win_handle.h
    include/unlocker/nt.h
    include/unlocker/path_util.h
    include/unlocker/process.h
    include/unlocker/scanner.h
    include/unlocker/unlock.h
    include/unlocker/file_ops.h
    include/unlocker/errors.h
    include/unlocker/driver_client.h
    src/errors.cpp
    src/nt.cpp
    src/path_util.cpp
    src/process.cpp
    src/scanner.cpp
    src/unlock.cpp
    src/file_ops.cpp
    src/driver_client.cpp
)

target_include_directories(unlocker_core PUBLIC include)
target_link_libraries(unlocker_core
    PUBLIC unlocker_options
    PRIVATE Rstrtmgr Advapi32 Shell32 Netapi32 Mpr
)
