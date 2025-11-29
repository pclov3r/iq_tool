include(FindPackageHandleStandardArgs)

if(SDRPLAY_INCLUDE_DIR AND SDRPLAY_LIBRARY)
    if(EXISTS "${SDRPLAY_INCLUDE_DIR}/sdrplay_api.h" AND EXISTS "${SDRPLAY_LIBRARY}")
        set(SDRplay_INCLUDE_DIRS ${SDRPLAY_INCLUDE_DIR})
        set(SDRplay_LIBRARIES ${SDRPLAY_LIBRARY})
        set(SDRPLAY_FOUND_MANUAL TRUE)
    endif()
endif()

if(NOT SDRPLAY_FOUND_MANUAL)
    set(SDRPLAY_SEARCH_PATHS /usr/local "$ENV{ProgramFiles}/SDRplay/API")
    find_path(SDRplay_INCLUDE_DIR NAMES sdrplay_api.h HINTS ${SDRPLAY_SEARCH_PATHS} PATH_SUFFIXES include inc)
    
    if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(SDRPLAY_LIB_SUFFIX "x64")
    elseif(WIN32)
        set(SDRPLAY_LIB_SUFFIX "x86")
    else()
        set(SDRPLAY_LIB_SUFFIX "")
    endif()

    find_library(SDRplay_LIBRARY NAMES sdrplay_api HINTS ${SDRPLAY_SEARCH_PATHS} PATH_SUFFIXES lib ${SDRPLAY_LIB_SUFFIX})

    if(SDRplay_INCLUDE_DIR AND SDRplay_LIBRARY)
        set(SDRplay_INCLUDE_DIRS ${SDRplay_INCLUDE_DIR})
        set(SDRplay_LIBRARIES ${SDRplay_LIBRARY})
    endif()
endif()

find_package_handle_standard_args(SDRplay REQUIRED_VARS SDRplay_LIBRARIES SDRplay_INCLUDE_DIRS)
