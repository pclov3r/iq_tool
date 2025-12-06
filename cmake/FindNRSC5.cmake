include(FindPackageHandleStandardArgs)

if(NRSC5_INCLUDE_DIR AND NRSC5_LIBRARY)
    message(STATUS "Checking manual paths for libnrsc5...")
    if(EXISTS "${NRSC5_INCLUDE_DIR}/nrsc5.h" AND EXISTS "${NRSC5_LIBRARY}")
        set(NRSC5_INCLUDE_DIRS ${NRSC5_INCLUDE_DIR})
        set(NRSC5_LIBRARIES ${NRSC5_LIBRARY})
        set(NRSC5_FOUND_MANUAL TRUE)
    else()
        message(WARNING "Manual libnrsc5 paths specified but invalid/incomplete. Ignoring.")
    endif()
endif()

if(NOT NRSC5_FOUND_MANUAL)
    if(CMAKE_CROSSCOMPILING)
         message(STATUS "Attempting explicit search for libnrsc5 (cross-compile)...")
    else()
         message(STATUS "Attempting explicit search for libnrsc5 (native)...")
    endif()

    find_path(NRSC5_INCLUDE_DIR NAMES nrsc5.h
        HINTS ENV NRSC5Include PATHS /usr/local/include /usr/include /opt/local/include)

    find_library(NRSC5_LIBRARY NAMES nrsc5 libnrsc5
        HINTS ENV NRSC5Lib PATHS /usr/local/lib /usr/local/lib64 /usr/lib /usr/lib64 /opt/local/lib)

    if(NOT NRSC5_LIBRARY AND EXISTS "/usr/local/lib/libnrsc5.so")
        message(STATUS "Found libnrsc5 library at explicit location: /usr/local/lib/libnrsc5.so")
        set(NRSC5_LIBRARY "/usr/local/lib/libnrsc5.so")
    endif()

    if(NOT NRSC5_INCLUDE_DIR AND EXISTS "/usr/local/include/nrsc5.h")
        message(STATUS "Found libnrsc5 headers at explicit location: /usr/local/include")
        set(NRSC5_INCLUDE_DIR "/usr/local/include")
    endif()

    if(NRSC5_INCLUDE_DIR AND NRSC5_LIBRARY)
        set(NRSC5_INCLUDE_DIRS ${NRSC5_INCLUDE_DIR})
        set(NRSC5_LIBRARIES ${NRSC5_LIBRARY})
    endif()
endif()

find_package_handle_standard_args(NRSC5 REQUIRED_VARS NRSC5_LIBRARIES NRSC5_INCLUDE_DIRS)
