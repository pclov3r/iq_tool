include(FindPackageHandleStandardArgs)

if(LIQUID_INCLUDE_DIR AND LIQUID_LIBRARY)
    message(STATUS "Checking manual paths for liquid-dsp...")
    if((EXISTS "${LIQUID_INCLUDE_DIR}/liquid.h" OR EXISTS "${LIQUID_INCLUDE_DIR}/liquid/liquid.h") AND EXISTS "${LIQUID_LIBRARY}")
        set(LiquidDSP_INCLUDE_DIRS ${LIQUID_INCLUDE_DIR})
        set(LiquidDSP_LIBRARIES ${LIQUID_LIBRARY})
        set(LIQUID_FOUND_MANUAL TRUE)
    else()
        message(WARNING "Manual liquid-dsp paths specified but invalid/incomplete. Ignoring.")
    endif()
endif()

if(NOT LIQUID_FOUND_MANUAL)
    if(CMAKE_CROSSCOMPILING)
         message(STATUS "Attempting explicit search for liquid-dsp (cross-compile fallback)...")
    else()
         message(STATUS "Manual paths not used/valid for liquid-dsp, trying explicit search (native fallback)...")
    endif()

    find_path(LiquidDSP_INCLUDE_DIR NAMES liquid/liquid.h liquid.h
        HINTS ENV LiquidInclude PATHS /usr/local/include /usr/include /opt/local/include)
    find_library(LiquidDSP_LIBRARY NAMES liquid
        HINTS ENV LiquidLib PATHS /usr/local/lib /usr/local/lib64 /usr/lib /usr/lib64 /opt/local/lib)
    
    if(LiquidDSP_INCLUDE_DIR AND LiquidDSP_LIBRARY)
        set(LiquidDSP_INCLUDE_DIRS ${LiquidDSP_INCLUDE_DIR})
        set(LiquidDSP_LIBRARIES ${LiquidDSP_LIBRARY})
    endif()
endif()

find_package_handle_standard_args(LiquidDSP REQUIRED_VARS LiquidDSP_LIBRARIES LiquidDSP_INCLUDE_DIRS)
