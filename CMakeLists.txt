#=======================================================================
# Project setup
#=======================================================================
#
# This build system is robust. The source code, however, is not immune
# to a misplaced 'rm -rf *' in the build directory. This has been
# empirically verified. Twice.
#
# Please use 'make clean'. Your future self will thank you.

cmake_minimum_required(VERSION 3.10)

project(iq_resample_tool C)

if(NOT CMAKE_BUILD_TYPE)
   set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the build type: Debug Release RelWithDebInfo MinSizeRel" FORCE)
   message(STATUS "Build type not specified: defaulting to 'Release'.")
endif()
message(STATUS "Build type set to '${CMAKE_BUILD_TYPE}'")

include(GNUInstallDirs)

#=======================================================================
# Optional Feature Toggles
#=======================================================================
option(WITH_RTLSDR "Enable RTL-SDR device support (requires librtlsdr)" OFF)
option(WITH_SDRPLAY "Enable SDRplay device support (requires SDRplay API library)" OFF)
option(WITH_HACKRF "Enable HackRF device support (requires libhackrf)" OFF)
option(WITH_BLADERF "Enable BladeRF device support (requires libbladeRF)" OFF)
option(BUILD_DOCUMENTATION "Enable building Doxygen documentation (requires Doxygen)" OFF)

#=======================================================================
# Compiler specific setup & flags
#=======================================================================
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# New variable to control the target CPU architecture for GCC/Clang
# Default to 'native' for convenience on local builds, but allow override.
set(CPU_TARGET_ARCHITECTURE "native" CACHE STRING "Specify target CPU architecture for GCC/Clang (-march=...). Set to 'native' for host CPU, or specific architecture like 'x86-64', 'x86-64-v2', 'x86-64-v3'.")
set_property(CACHE PROPERTY STRINGS CPU_TARGET_ARCHITECTURE "native;x86-64;x86-64-v2;x86-64-v3;x86-64-v4") # Optional: provide hints for GUI tools

if(MSVC)
    # --- MSVC ---
    add_compile_options(/W3)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
    add_compile_definitions(_USE_MATH_DEFINES)
else()
    # --- GCC / Clang ---
    # These add_compile_options apply to all build types for GCC/Clang
    add_compile_options(-Wall -Wextra -pedantic)
    add_compile_options(
        -Wvla
        -Wsign-compare
        -Wshadow
        -Wmissing-prototypes
    )
    if(("${CMAKE_C_COMPILER_ID}" MATCHES "Clang") OR ("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU" AND CMAKE_C_COMPILER_VERSION VERSION_GREATER_EQUAL "7.0.0"))
        add_compile_options(-Wimplicit-fallthrough)
    endif()
    add_compile_definitions(_GNU_SOURCE)
    add_compile_options(-fvisibility=hidden)

    option(FORCE_COLORED_BUILD "Always produce ANSI-colored build output (GNU/Clang only)." FALSE)
    if(FORCE_COLORED_BUILD)
        if("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU")
            add_compile_options(-fdiagnostics-color=always)
        elseif("${CMAKE_C_COMPILER_ID}" MATCHES "Clang")
            add_compile_options(-fcolor-diagnostics)
        endif()
    endif()

endif()

if(MINGW)
    add_compile_definitions(__USE_MINGW_ANSI_STDIO=1)
endif()

# Add the conditional compile definitions if features are enabled
if(WITH_RTLSDR)
    add_compile_definitions(WITH_RTLSDR)
endif()
if(WITH_SDRPLAY)
    add_compile_definitions(WITH_SDRPLAY)
endif()
if(WITH_HACKRF)
    add_compile_definitions(WITH_HACKRF)
endif()
if(WITH_BLADERF)
    add_compile_definitions(WITH_BLADERF)
endif()

if(WITH_RTLSDR OR WITH_SDRPLAY OR WITH_HACKRF OR WITH_BLADERF)
    add_compile_definitions(ANY_SDR_SUPPORT_ENABLED)
endif()

#=======================================================================
# Options for Manual Dependency Paths (Optional Overrides)
#=======================================================================
set(SNDFILE_INCLUDE_DIR "" CACHE PATH "Manual include path override for libsndfile")
set(SNDFILE_LIBRARY "" CACHE PATH "Manual library file path override for libsndfile")
set(LIQUID_INCLUDE_DIR "" CACHE PATH "Manual include path override for liquid-dsp")
set(LIQUID_LIBRARY "" CACHE PATH "Manual library file path override for liquid-dsp")
set(EXPAT_INCLUDE_DIR "" CACHE PATH "Manual include path override for Expat")
set(EXPAT_LIBRARY "" CACHE PATH "Manual library file path override for Expat")
if(WITH_RTLSDR)
    set(RTLSDR_INCLUDE_DIR "" CACHE PATH "Manual include path override for librtlsdr")
    set(RTLSDR_LIBRARY "" CACHE PATH "Manual library file path override for librtlsdr")
endif()
if(WITH_SDRPLAY)
    set(SDRPLAY_INCLUDE_DIR "" CACHE PATH "Manual include path override for SDRplay API")
    set(SDRPLAY_LIBRARY "" CACHE PATH "Manual library file path override for SDRplay API")
endif()
if(WITH_HACKRF OR WITH_BLADERF OR WITH_RTLSDR)
    set(LIBUSB_INCLUDE_DIR "" CACHE PATH "Manual include path override for libusb-1.0")
    set(LIBUSB_LIBRARY "" CACHE PATH "Manual library file path override for libusb-1.0")
endif()
if(WITH_HACKRF)
    set(HACKRF_INCLUDE_DIR "" CACHE PATH "Manual include path override for libhackrf")
    set(HACKRF_LIBRARY "" CACHE PATH "Manual library file path override for libhackrf")
endif()
if(WITH_BLADERF)
    set(BLADERF_INCLUDE_DIR "" CACHE PATH "Manual include path override for libbladeRF")
    set(BLADERF_LIBRARY "" CACHE PATH "Manual library file path override for libbladeRF")
endif()

#=======================================================================
# Find Dependencies
#=======================================================================
find_package(PkgConfig QUIET)

# --- libsndfile ---
message(STATUS "Looking for required library: libsndfile...")
set(FINAL_SNDFILE_INCLUDE_DIRS "")
set(FINAL_SNDFILE_LIBRARIES "")
set(SNDFILE_FOUND_OVERALL FALSE)
if(NOT SNDFILE_INCLUDE_DIR STREQUAL "" AND NOT SNDFILE_LIBRARY STREQUAL "")
    message(STATUS "Checking manual paths for libsndfile...")
    set(header_path "${SNDFILE_INCLUDE_DIR}/sndfile.h")
    if(EXISTS "${header_path}" AND EXISTS "${SNDFILE_LIBRARY}")
        message(STATUS "Using manual libsndfile paths.")
        set(FINAL_SNDFILE_INCLUDE_DIRS ${SNDFILE_INCLUDE_DIR})
        set(FINAL_SNDFILE_LIBRARIES ${SNDFILE_LIBRARY})
        set(SNDFILE_FOUND_OVERALL TRUE)
    else()
        message(WARNING "Manual libsndfile paths specified but invalid/incomplete. Ignoring.")
    endif()
    unset(header_path)
endif()
if(NOT SNDFILE_FOUND_OVERALL AND NOT CMAKE_CROSSCOMPILING)
    if(PKG_CONFIG_FOUND)
        message(STATUS "Attempting pkg-config for libsndfile (native build)...")
        pkg_check_modules(PC_SNDFILE QUIET sndfile)
        if(PC_SNDFILE_FOUND)
            message(STATUS "Found libsndfile via pkg-config: ${PC_SNDFILE_VERSION}")
            set(FINAL_SNDFILE_INCLUDE_DIRS ${PC_SNDFILE_INCLUDE_DIRS})
            set(FINAL_SNDFILE_LIBRARIES ${PC_SNDFILE_LINK_LIBRARIES})
            set(SNDFILE_FOUND_OVERALL TRUE)
        endif()
    endif()
endif()
if(NOT SNDFILE_FOUND_OVERALL)
    if(CMAKE_CROSSCOMPILING)
        message(STATUS "Attempting find_package for libsndfile (cross-compile)...")
    else()
        message(STATUS "Did not find libsndfile via manual paths or pkg-config, trying find_package...")
    endif()
    find_package(SndFile QUIET)
    if(SndFile_FOUND AND DEFINED SndFile_INCLUDE_DIRS AND SndFile_INCLUDE_DIRS AND DEFINED SndFile_LIBRARIES AND SndFile_LIBRARIES)
         message(STATUS "Found libsndfile via find_package.")
         set(FINAL_SNDFILE_INCLUDE_DIRS ${SndFile_INCLUDE_DIRS})
         set(FINAL_SNDFILE_LIBRARIES ${SndFile_LIBRARIES})
         set(SNDFILE_FOUND_OVERALL TRUE)
    else()
         message(STATUS "Could not find libsndfile via find_package.")
         if(SndFile_FOUND)
             message(WARNING "find_package(SndFile) succeeded but did not set expected variables.")
         endif()
         set(SNDFILE_FOUND_OVERALL FALSE)
    endif()
endif()
if(NOT SNDFILE_FOUND_OVERALL)
    message(FATAL_ERROR "Could not find libsndfile. Please install development package, set CMAKE_PREFIX_PATH/CMAKE_FIND_ROOT_PATH, or set valid SNDFILE_INCLUDE_DIR and SNDFILE_LIBRARY.")
endif()


# --- liquid-dsp ---
message(STATUS "Looking for required library: liquid-dsp...")
set(FINAL_LIQUIDDSP_INCLUDE_DIRS "")
set(FINAL_LIQUIDDSP_LIBRARIES "")
set(LIQUIDDSP_FOUND_OVERALL FALSE)
if(NOT LIQUID_INCLUDE_DIR STREQUAL "" AND NOT LIQUID_LIBRARY STREQUAL "")
    message(STATUS "Checking manual paths for liquid-dsp...")
    set(header_path "")
    if(EXISTS "${LIQUID_INCLUDE_DIR}/liquid.h")
         set(header_path "${LIQUID_INCLUDE_DIR}/liquid.h")
    elseif(EXISTS "${LIQUID_INCLUDE_DIR}/liquid/liquid.h")
         set(header_path "${LIQUID_INCLUDE_DIR}/liquid/liquid.h")
    endif()
    if(NOT header_path STREQUAL "" AND EXISTS "${LIQUID_LIBRARY}")
        message(STATUS "Using manual liquid-dsp paths.")
        set(FINAL_LIQUIDDSP_INCLUDE_DIRS ${LIQUID_INCLUDE_DIR})
        set(FINAL_LIQUIDDSP_LIBRARIES ${LIQUID_LIBRARY})
        set(LIQUIDDSP_FOUND_OVERALL TRUE)
    else()
        message(WARNING "Manual liquid-dsp paths specified but invalid/incomplete. Ignoring.")
        if(header_path STREQUAL "")
           message(WARNING "  -> Header not found at expected paths: ${LIQUID_INCLUDE_DIR}/liquid.h or ${LIQUID_INCLUDE_DIR}/liquid/liquid.h")
        endif()
        if(NOT EXISTS "${LIQUID_LIBRARY}")
           message(WARNING "  -> Library not found at expected path: ${LIQUID_LIBRARY}")
        endif()
    endif()
    unset(header_path)
endif()
if(NOT LIQUIDDSP_FOUND_OVERALL)
    if(CMAKE_CROSSCOMPILING)
         message(STATUS "Attempting explicit search for liquid-dsp (cross-compile fallback)...")
    else()
         message(STATUS "Manual paths not used/valid for liquid-dsp, trying explicit search (native fallback)...")
    endif()
    find_path(LIQUID_TEMP_INCLUDE_DIR NAMES liquid/liquid.h liquid.h HINTS ENV LiquidInclude ${CMAKE_FIND_ROOT_PATH}/include ${CMAKE_INSTALL_PREFIX}/include ${CMAKE_PREFIX_PATH}/include PATHS /usr/local/include /usr/include /opt/local/include DOC "LiquidDSP include directory")
    find_library(LIQUID_TEMP_LIBRARY NAMES liquid HINTS ENV LiquidLib ${CMAKE_FIND_ROOT_PATH}/lib ${CMAKE_FIND_ROOT_PATH}/lib64 ${CMAKE_INSTALL_PREFIX}/lib ${CMAKE_INSTALL_PREFIX}/lib64 ${CMAKE_PREFIX_PATH}/lib ${CMAKE_PREFIX_PATH}/lib64 PATHS /usr/local/lib /usr/local/lib64 /usr/lib /usr/lib64 /opt/local/lib DOC "LiquidDSP library file")
    if(LIQUID_TEMP_INCLUDE_DIR AND LIQUID_TEMP_LIBRARY)
        message(STATUS "Found liquid-dsp via explicit search: Include=${LIQUID_TEMP_INCLUDE_DIR}, Library=${LIQUID_TEMP_LIBRARY}")
        set(FINAL_LIQUIDDSP_INCLUDE_DIRS ${LIQUID_TEMP_INCLUDE_DIR})
        set(FINAL_LIQUIDDSP_LIBRARIES ${LIQUID_TEMP_LIBRARY})
        set(LIQUIDDSP_FOUND_OVERALL TRUE)
    else()
        message(STATUS "Could not find liquid-dsp via explicit search.")
        if(NOT LIQUID_TEMP_INCLUDE_DIR)
            message(STATUS " -> Reason: liquid.h or liquid/liquid.h not found in searched paths.")
        endif()
        if(NOT LIQUID_TEMP_LIBRARY)
            message(STATUS " -> Reason: libliquid library not found in searched paths.")
        endif()
    endif()
    unset(LIQUID_TEMP_INCLUDE_DIR CACHE)
    unset(LIQUID_TEMP_LIBRARY CACHE)
endif()
if(NOT LIQUIDDSP_FOUND_OVERALL)
    message(FATAL_ERROR "Could not find liquid-dsp using any method. Please install development package, set CMAKE_PREFIX_PATH/CMAKE_FIND_ROOT_PATH, or set valid LIQUID_INCLUDE_DIR and LIQUID_LIBRARY.")
endif()


# --- Expat ---
message(STATUS "Looking for required library: Expat...")
set(FINAL_EXPAT_INCLUDE_DIRS "")
set(FINAL_EXPAT_LIBRARIES "")
set(EXPAT_FOUND_OVERALL FALSE)
if(NOT EXPAT_INCLUDE_DIR STREQUAL "" AND NOT EXPAT_LIBRARY STREQUAL "")
    message(STATUS "Checking manual paths for Expat...")
    set(header_path "${EXPAT_INCLUDE_DIR}/expat.h")
    if(EXISTS "${header_path}" AND EXISTS "${EXPAT_LIBRARY}")
        message(STATUS "Using manual Expat paths.")
        set(FINAL_EXPAT_INCLUDE_DIRS ${EXPAT_INCLUDE_DIR})
        set(FINAL_EXPAT_LIBRARIES ${EXPAT_LIBRARY})
        set(EXPAT_FOUND_OVERALL TRUE)
    else()
        message(WARNING "Manual Expat paths specified but invalid/incomplete. Ignoring.")
    endif()
    unset(header_path)
endif()
if(NOT EXPAT_FOUND_OVERALL AND NOT CMAKE_CROSSCOMPILING)
    if(PKG_CONFIG_FOUND)
        message(STATUS "Attempting pkg-config for Expat (native build)...")
        pkg_check_modules(PC_EXPAT QUIET expat)
        if(PC_EXPAT_FOUND)
            message(STATUS "Found Expat via pkg-config: ${PC_EXPAT_VERSION}")
            set(FINAL_EXPAT_INCLUDE_DIRS ${PC_EXPAT_INCLUDE_DIRS})
            set(FINAL_EXPAT_LIBRARIES ${PC_EXPAT_LINK_LIBRARIES})
            set(EXPAT_FOUND_OVERALL TRUE)
        endif()
    endif()
endif()
if(NOT EXPAT_FOUND_OVERALL)
    if(CMAKE_CROSSCOMPILING)
        message(STATUS "Attempting find_package for Expat (cross-compile)...")
    else()
        message(STATUS "Did not find Expat via manual paths or pkg-config, trying find_package...")
    endif()
    find_package(EXPAT QUIET)
    if(EXPAT_FOUND AND DEFINED EXPAT_INCLUDE_DIRS AND EXPAT_INCLUDE_DIRS AND DEFINED EXPAT_LIBRARIES AND EXPAT_LIBRARIES)
         message(STATUS "Found Expat via find_package.")
         set(FINAL_EXPAT_INCLUDE_DIRS ${EXPAT_INCLUDE_DIRS})
         set(FINAL_EXPAT_LIBRARIES ${EXPAT_LIBRARIES})
         set(EXPAT_FOUND_OVERALL TRUE)
    else()
         message(STATUS "Could not find Expat via find_package.")
         if(EXPAT_FOUND)
             message(WARNING "find_package(EXPAT) succeeded but did not set expected variables (EXPAT_INCLUDE_DIRS, EXPAT_LIBRARIES).")
         endif()
         set(EXPAT_FOUND_OVERALL FALSE)
    endif()
endif()
if(NOT EXPAT_FOUND_OVERALL)
    message(FATAL_ERROR "Could not find Expat. Please install development package (e.g., libexpat1-dev, expat-devel), set CMAKE_PREFIX_PATH/CMAKE_FIND_ROOT_PATH, or set valid EXPAT_INCLUDE_DIR and EXPAT_LIBRARY.")
endif()


# --- pthreads ---
message(STATUS "Looking for required threading library: pthreads...")
set(PTHREADS_FOUND_OVERALL FALSE)
find_package(Threads REQUIRED)
if(Threads_FOUND)
     message(STATUS "Found threading library support (Threads::Threads target available)")
     set(FINAL_PTHREADS_LIBRARIES Threads::Threads)
     set(PTHREADS_FOUND_OVERALL TRUE)
else()
     message(STATUS "Could not find threading library support via find_package(Threads).")
     set(PTHREADS_FOUND_OVERALL FALSE)
endif()
if(NOT PTHREADS_FOUND_OVERALL)
     message(FATAL_ERROR "Could not find pthreads library. Ensure your system has pthreads development files installed.")
endif()

# --- libusb-1.0 (Dependency for multiple SDRs) ---
if(WITH_HACKRF OR WITH_BLADERF OR WITH_RTLSDR)
    message(STATUS "Looking for required library: libusb-1.0...")
    set(FINAL_LIBUSB_INCLUDE_DIRS "")
    set(FINAL_LIBUSB_LIBRARIES "")
    set(LIBUSB_FOUND_OVERALL FALSE)

    if(NOT LIBUSB_INCLUDE_DIR STREQUAL "" AND NOT LIBUSB_LIBRARY STREQUAL "")
        message(STATUS "Checking manual paths for libusb-1.0...")
        set(header_path "${LIBUSB_INCLUDE_DIR}/libusb.h")
        if(EXISTS "${header_path}" AND EXISTS "${LIBUSB_LIBRARY}")
            message(STATUS "Using manual libusb-1.0 paths.")
            set(FINAL_LIBUSB_INCLUDE_DIRS ${LIBUSB_INCLUDE_DIR})
            set(FINAL_LIBUSB_LIBRARIES ${LIBUSB_LIBRARY})
            set(LIBUSB_FOUND_OVERALL TRUE)
        else()
            message(WARNING "Manual libusb-1.0 paths specified but invalid/incomplete. Ignoring.")
        endif()
        unset(header_path)
    endif()

    if(NOT LIBUSB_FOUND_OVERALL AND NOT CMAKE_CROSSCOMPILING)
        if(PKG_CONFIG_FOUND)
            message(STATUS "Attempting pkg-config for libusb-1.0 (native build)...")
            pkg_check_modules(PC_LIBUSB QUIET libusb-1.0)
            if(PC_LIBUSB_FOUND)
                message(STATUS "Found libusb-1.0 via pkg-config: ${PC_LIBUSB_VERSION}")
                set(FINAL_LIBUSB_INCLUDE_DIRS ${PC_LIBUSB_INCLUDE_DIRS})
                set(FINAL_LIBUSB_LIBRARIES ${PC_LIBUSB_LINK_LIBRARIES})
                set(LIBUSB_FOUND_OVERALL TRUE)
            endif()
        endif()
    endif()

    if(NOT LIBUSB_FOUND_OVERALL)
        if(CMAKE_CROSSCOMPILING)
            message(STATUS "Attempting find_package for libusb-1.0 (cross-compile fallback)...")
        else()
            message(STATUS "Did not find libusb-1.0 via manual paths or pkg-config, trying find_package...")
        endif()
        find_package(LibUSB-1.0 QUIET)
        if(LibUSB-1_0_FOUND AND DEFINED LibUSB-1_0_INCLUDE_DIRS AND LibUSB-1_0_INCLUDE_DIRS AND DEFINED LibUSB-1_0_LIBRARIES AND LibUSB-1_0_LIBRARIES)
            message(STATUS "Found libusb-1.0 via find_package.")
            set(FINAL_LIBUSB_INCLUDE_DIRS ${LibUSB-1_0_INCLUDE_DIRS})
            set(FINAL_LIBUSB_LIBRARIES ${LibUSB-1_0_LIBRARIES})
            set(LIBUSB_FOUND_OVERALL TRUE)
        else()
            message(STATUS "Could not find libusb-1.0 via find_package.")
        endif()
    endif()

    if(NOT LIBUSB_FOUND_OVERALL)
        message(FATAL_ERROR "Could not find libusb-1.0, which is required for SDR support. Please install libusb-1.0-dev, set CMAKE_PREFIX_PATH, or set valid LIBUSB_INCLUDE_DIR and LIBUSB_LIBRARY.")
    endif()
else()
    message(STATUS "libusb-1.0 search skipped (no USB-based SDRs enabled).")
endif()


# --- librtlsdr ---
if(WITH_RTLSDR)
    message(STATUS "Looking for optional library: librtlsdr...")
    set(FINAL_RTLSDR_INCLUDE_DIRS "")
    set(FINAL_RTLSDR_LIBRARIES "")
    set(RTLSDR_FOUND_OVERALL FALSE)

    if(NOT RTLSDR_INCLUDE_DIR STREQUAL "" AND NOT RTLSDR_LIBRARY STREQUAL "")
        message(STATUS "Checking manual paths for librtlsdr...")
        set(header_path "${RTLSDR_INCLUDE_DIR}/rtl-sdr.h")
        if(EXISTS "${header_path}" AND EXISTS "${RTLSDR_LIBRARY}")
            message(STATUS "Using manual librtlsdr paths.")
            set(FINAL_RTLSDR_INCLUDE_DIRS ${RTLSDR_INCLUDE_DIR})
            set(FINAL_RTLSDR_LIBRARIES ${RTLSDR_LIBRARY})
            set(RTLSDR_FOUND_OVERALL TRUE)
        else()
            message(WARNING "Manual librtlsdr paths specified but invalid/incomplete. Ignoring.")
        endif()
        unset(header_path)
    endif()

    if(NOT RTLSDR_FOUND_OVERALL AND NOT CMAKE_CROSSCOMPILING)
        if(PKG_CONFIG_FOUND)
            message(STATUS "Attempting pkg-config for librtlsdr (native build)...")
            pkg_check_modules(PC_RTLSDR QUIET librtlsdr)
            if(PC_RTLSDR_FOUND)
                message(STATUS "Found librtlsdr via pkg-config: ${PC_RTLSDR_VERSION}")
                set(FINAL_RTLSDR_INCLUDE_DIRS ${PC_RTLSDR_INCLUDE_DIRS})
                set(FINAL_RTLSDR_LIBRARIES ${PC_RTLSDR_LINK_LIBRARIES})
                set(RTLSDR_FOUND_OVERALL TRUE)
            endif()
        endif()
    endif()

    if(NOT RTLSDR_FOUND_OVERALL)
        message(STATUS "Did not find librtlsdr via manual paths or pkg-config, trying explicit search...")
        find_path(RTLSDR_TEMP_INCLUDE_DIR NAMES rtl-sdr.h HINTS ${CMAKE_FIND_ROOT_PATH}/include ${CMAKE_INSTALL_PREFIX}/include ${CMAKE_PREFIX_PATH}/include PATHS /usr/local/include /usr/include)
        find_library(RTLSDR_TEMP_LIBRARY NAMES rtlsdr HINTS ${CMAKE_FIND_ROOT_PATH}/lib ${CMAKE_INSTALL_PREFIX}/lib ${CMAKE_PREFIX_PATH}/lib PATHS /usr/local/lib /usr/lib)
        if(RTLSDR_TEMP_INCLUDE_DIR AND RTLSDR_TEMP_LIBRARY)
            message(STATUS "Found librtlsdr via explicit search: Include=${RTLSDR_TEMP_INCLUDE_DIR}, Library=${RTLSDR_TEMP_LIBRARY}")
            set(FINAL_RTLSDR_INCLUDE_DIRS ${RTLSDR_TEMP_INCLUDE_DIR})
            set(FINAL_RTLSDR_LIBRARIES ${RTLSDR_TEMP_LIBRARY})
            set(RTLSDR_FOUND_OVERALL TRUE)
        else()
            message(STATUS "Could not find librtlsdr via explicit search.")
        endif()
        unset(RTLSDR_TEMP_INCLUDE_DIR CACHE)
        unset(RTLSDR_TEMP_LIBRARY CACHE)
    endif()

    if(NOT RTLSDR_FOUND_OVERALL)
        message(FATAL_ERROR "Could not find librtlsdr, but WITH_RTLSDR was enabled. Please install librtlsdr-dev, set CMAKE_PREFIX_PATH, or set valid RTLSDR_INCLUDE_DIR and RTLSDR_LIBRARY.")
    endif()
endif()


# --- SDRplay API ---
if(WITH_SDRPLAY)
    message(STATUS "Looking for optional library: SDRplay API...")
    set(FINAL_SDRPLAY_INCLUDE_DIRS "")
    set(FINAL_SDRPLAY_LIBRARIES "")
    set(SDRPLAY_FOUND_OVERALL FALSE)

    if(NOT SDRPLAY_INCLUDE_DIR STREQUAL "" AND NOT SDRPLAY_LIBRARY STREQUAL "")
        message(STATUS "Checking manual paths for SDRplay API...")
        set(header_path "${SDRPLAY_INCLUDE_DIR}/sdrplay_api.h")
        if(EXISTS "${header_path}" AND EXISTS "${SDRPLAY_LIBRARY}")
            message(STATUS "Using manual SDRplay API paths.")
            set(FINAL_SDRPLAY_INCLUDE_DIRS ${SDRPLAY_INCLUDE_DIR})
            set(FINAL_SDRPLAY_LIBRARIES ${SDRPLAY_LIBRARY})
            set(SDRPLAY_FOUND_OVERALL TRUE)
        else()
            message(WARNING "Manual SDRplay API paths specified but invalid/incomplete. Ignoring.")
        endif()
        unset(header_path)
    endif()

    if(NOT SDRPLAY_FOUND_OVERALL)
        message(STATUS "Manual paths not used/valid for SDRplay API, trying explicit search...")
        set(SDRPLAY_SEARCH_PATHS /usr/local "$ENV{ProgramFiles}/SDRplay/API")
        find_path(SDRPLAY_TEMP_INCLUDE_DIR NAMES sdrplay_api.h HINTS ${SDRPLAY_SEARCH_PATHS} PATH_SUFFIXES include inc)
        if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(SDRPLAY_LIB_SUFFIX "x64")
        elseif(WIN32)
            set(SDRPLAY_LIB_SUFFIX "x86")
        else()
            set(SDRPLAY_LIB_SUFFIX "")
        endif()
        find_library(SDRPLAY_TEMP_LIBRARY NAMES sdrplay_api HINTS ${SDRPLAY_SEARCH_PATHS} PATH_SUFFIXES lib ${SDRPLAY_LIB_SUFFIX})
        if(SDRPLAY_TEMP_INCLUDE_DIR AND SDRPLAY_TEMP_LIBRARY)
            message(STATUS "Found SDRplay API via explicit search: Include=${SDRPLAY_TEMP_INCLUDE_DIR}, Library=${SDRPLAY_TEMP_LIBRARY}")
            set(FINAL_SDRPLAY_INCLUDE_DIRS ${SDRPLAY_TEMP_INCLUDE_DIR})
            set(FINAL_SDRPLAY_LIBRARIES ${SDRPLAY_TEMP_LIBRARY})
            set(SDRPLAY_FOUND_OVERALL TRUE)
        else()
            message(STATUS "Could not find SDRplay API via explicit search.")
        endif()
        unset(SDRPLAY_TEMP_INCLUDE_DIR CACHE)
        unset(SDRPLAY_TEMP_LIBRARY CACHE)
    endif()

    if(NOT SDRPLAY_FOUND_OVERALL)
        message(FATAL_ERROR "Could not find SDRplay API, but WITH_SDRPLAY was enabled. Please install the SDRplay API, set CMAKE_PREFIX_PATH, or set valid SDRPLAY_INCLUDE_DIR and SDRPLAY_LIBRARY.")
    endif()
endif()


# --- libhackrf ---
if(WITH_HACKRF)
    message(STATUS "Looking for optional library: libhackrf...")
    set(FINAL_HACKRF_INCLUDE_DIRS "")
    set(FINAL_HACKRF_LIBRARIES "")
    set(HACKRF_FOUND_OVERALL FALSE)

    if(NOT HACKRF_INCLUDE_DIR STREQUAL "" AND NOT HACKRF_LIBRARY STREQUAL "")
        message(STATUS "Checking manual paths for libhackrf...")
        set(header_path "${HACKRF_INCLUDE_DIR}/hackrf.h")
        if(EXISTS "${header_path}" AND EXISTS "${HACKRF_LIBRARY}")
            message(STATUS "Using manual libhackrf paths.")
            set(FINAL_HACKRF_INCLUDE_DIRS ${HACKRF_INCLUDE_DIR})
            set(FINAL_HACKRF_LIBRARIES ${HACKRF_LIBRARY})
            set(HACKRF_FOUND_OVERALL TRUE)
        else()
            message(WARNING "Manual libhackrf paths specified but invalid/incomplete. Ignoring.")
        endif()
        unset(header_path)
    endif()

    if(NOT HACKRF_FOUND_OVERALL AND NOT CMAKE_CROSSCOMPILING)
        if(PKG_CONFIG_FOUND)
            message(STATUS "Attempting pkg-config for libhackrf (native build)...")
            pkg_check_modules(PC_HACKRF QUIET libhackrf)
            if(PC_HACKRF_FOUND)
                message(STATUS "Found libhackrf via pkg-config: ${PC_HACKRF_VERSION}")
                set(FINAL_HACKRF_INCLUDE_DIRS ${PC_HACKRF_INCLUDE_DIRS})
                set(FINAL_HACKRF_LIBRARIES ${PC_HACKRF_LINK_LIBRARIES})
                set(HACKRF_FOUND_OVERALL TRUE)
            endif()
        endif()
    endif()

    if(NOT HACKRF_FOUND_OVERALL)
        message(STATUS "Did not find libhackrf via manual paths or pkg-config, trying find_package...")
        find_package(HackRF QUIET)
        if(HackRF_FOUND AND DEFINED HackRF_INCLUDE_DIRS AND HackRF_INCLUDE_DIRS AND DEFINED HackRF_LIBRARIES AND HackRF_LIBRARIES)
            message(STATUS "Found libhackrf via find_package.")
            set(FINAL_HACKRF_INCLUDE_DIRS ${HackRF_INCLUDE_DIRS})
            set(FINAL_HACKRF_LIBRARIES ${HackRF_LIBRARIES})
            set(HACKRF_FOUND_OVERALL TRUE)
        else()
            message(STATUS "Could not find libhackrf via find_package.")
            set(HACKRF_FOUND_OVERALL FALSE)
        endif()
    endif()

    if(NOT HACKRF_FOUND_OVERALL)
        message(FATAL_ERROR "Could not find libhackrf, but WITH_HACKRF was enabled. Please install libhackrf-dev, set CMAKE_PREFIX_PATH, or set valid HACKRF_INCLUDE_DIR and HACKRF_LIBRARY.")
    endif()
endif()

# --- libbladeRF ---
if(WITH_BLADERF)
    message(STATUS "Looking for optional library: libbladeRF...")
    set(FINAL_BLADERF_INCLUDE_DIRS "")
    set(FINAL_BLADERF_LIBRARIES "")
    set(BLADERF_FOUND_OVERALL FALSE)

    if(NOT BLADERF_INCLUDE_DIR STREQUAL "" AND NOT BLADERF_LIBRARY STREQUAL "")
        message(STATUS "Checking manual paths for libbladeRF...")
        set(header_path "${BLADERF_INCLUDE_DIR}/libbladeRF.h")
        if(EXISTS "${header_path}" AND EXISTS "${BLADERF_LIBRARY}")
            message(STATUS "Using manual libbladeRF paths.")
            set(FINAL_BLADERF_INCLUDE_DIRS ${BLADERF_INCLUDE_DIR})
            set(FINAL_BLADERF_LIBRARIES ${BLADERF_LIBRARY})
            set(BLADERF_FOUND_OVERALL TRUE)
        else()
            message(WARNING "Manual libbladeRF paths specified but invalid/incomplete. Ignoring.")
        endif()
        unset(header_path)
    endif()

    if(NOT BLADERF_FOUND_OVERALL AND NOT CMAKE_CROSSCOMPILING)
        if(PKG_CONFIG_FOUND)
            message(STATUS "Attempting pkg-config for libbladeRF (native build)...")
            pkg_check_modules(PC_BLADERF QUIET libbladeRF)
            if(PC_BLADERF_FOUND)
                message(STATUS "Found libbladeRF via pkg-config: ${PC_BLADERF_VERSION}")
                set(FINAL_BLADERF_INCLUDE_DIRS ${PC_BLADERF_INCLUDE_DIRS})
                set(FINAL_BLADERF_LIBRARIES ${PC_BLADERF_LINK_LIBRARIES})
                set(BLADERF_FOUND_OVERALL TRUE)
            endif()
        endif()
    endif()

    if(NOT BLADERF_FOUND_OVERALL)
        message(STATUS "Did not find libbladeRF via manual paths or pkg-config, trying find_package...")
        find_package(BladeRF QUIET)
        if(BladeRF_FOUND AND DEFINED BladeRF_INCLUDE_DIRS AND BladeRF_INCLUDE_DIRS AND DEFINED BladeRF_LIBRARIES AND BladeRF_LIBRARIES)
            message(STATUS "Found libbladeRF via find_package.")
            set(FINAL_BLADERF_INCLUDE_DIRS ${BladeRF_INCLUDE_DIRS})
            set(FINAL_BLADERF_LIBRARIES ${BladeRF_LIBRARIES})
            set(BLADERF_FOUND_OVERALL TRUE)
        else()
            message(STATUS "Could not find libbladeRF via find_package.")
            set(BLADERF_FOUND_OVERALL FALSE)
        endif()
    endif()

    if(NOT BLADERF_FOUND_OVERALL)
        message(FATAL_ERROR "Could not find libbladeRF, but WITH_BLADERF was enabled. Please install libbladeRF-dev, set CMAKE_PREFIX_PATH, or set valid BLADERF_INCLUDE_DIR and BLADERF_LIBRARY.")
    endif()
endif()

#=======================================================================
# Project Sources and Target Definition
#=======================================================================
include_directories(include)
include_directories(${FINAL_SNDFILE_INCLUDE_DIRS})
include_directories(${FINAL_LIQUIDDSP_INCLUDE_DIRS})
include_directories(${FINAL_EXPAT_INCLUDE_DIRS})
if(WITH_RTLSDR)
    include_directories(${FINAL_RTLSDR_INCLUDE_DIRS})
    include_directories(${FINAL_LIBUSB_INCLUDE_DIRS})
endif()
if(WITH_SDRPLAY)
    include_directories(${FINAL_SDRPLAY_INCLUDE_DIRS})
endif()
if(WITH_HACKRF)
    include_directories(${FINAL_HACKRF_INCLUDE_DIRS})
    include_directories(${FINAL_LIBUSB_INCLUDE_DIRS})
endif()
if(WITH_BLADERF)
    include_directories(${FINAL_BLADERF_INCLUDE_DIRS})
    include_directories(${FINAL_LIBUSB_INCLUDE_DIRS})
endif()

# Define the list of DSP-specific source files
set(DSP_SOURCES
    src/resampler.c
    src/sample_convert.c
    src/iq_correct.c
    src/dc_block.c
    src/filter.c
    src/frequency_shift.c
)

# Define the list of all other (non-DSP) source files
set(OTHER_SOURCES
    src/argparse.c
    src/cli.c
    src/config.c
    src/file_writer.c
    src/input_manager.c
    src/input_rawfile.c
    src/input_wav.c
    src/file_write_buffer.c
    src/io_threads.c
    src/log.c
    src/main.c
    src/memory_arena.c
    src/platform.c
    src/presets_loader.c
    src/pre_processor.c
    src/processing_threads.c
    src/queue.c
    src/sdr_packet_serializer.c
    src/setup.c
    src/signal_handler.c
    src/utils.c
)

if(WITH_RTLSDR)
    list(APPEND OTHER_SOURCES src/input_rtlsdr.c)
endif()
if(WITH_SDRPLAY)
    list(APPEND OTHER_SOURCES src/input_sdrplay.c)
endif()
if(WITH_HACKRF)
    list(APPEND OTHER_SOURCES src/input_hackrf.c)
endif()
if(WITH_BLADERF)
    list(APPEND OTHER_SOURCES src/input_bladerf.c)
endif()

# Create the executable from ALL source files
add_executable(iq_resample_tool ${DSP_SOURCES} ${OTHER_SOURCES})

#=======================================================================
# Check for system function availability
#=======================================================================
include(CheckFunctionExists)

check_function_exists(strcasestr HAVE_STRCASESTR)
if(HAVE_STRCASESTR)
    target_compile_definitions(iq_resample_tool PRIVATE HAVE_STRCASESTR)
endif()

# Enable Link-Time Optimization (LTO) if the compiler supports it.
# This should be placed after the target is defined.
include(CheckIPOSupported)
check_ipo_supported(RESULT LTO_SUPPORTED)
if(LTO_SUPPORTED)
    set_property(TARGET iq_resample_tool PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    message(STATUS "Link-Time Optimization (LTO) enabled for target.")
else()
    message(STATUS "Link-Time Optimization (LTO) not supported by this compiler.")
endif()

#=======================================================================
# Versioning (Git Commit Hash)
#=======================================================================
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE GIT_RESULT
        ERROR_QUIET
    )
    if(GIT_RESULT EQUAL 0)
        set(VERSION_INFO "${GIT_HASH}")
        message(STATUS "Detected Git commit hash: ${VERSION_INFO}")
    else()
        set(VERSION_INFO "unknown-git-error")
        message(STATUS "Could not get Git commit hash.")
    endif()
else()
    set(VERSION_INFO "unknown-no-git")
    message(STATUS "Git not found. Version will be unknown.")
endif()

# Pass the hash to the C code as a preprocessor macro: #define GIT_HASH "..."
target_compile_definitions(iq_resample_tool PRIVATE GIT_HASH="${VERSION_INFO}")
#=======================================================================

# Apply special compile options ONLY to the DSP source files for Release builds
if(NOT MSVC AND CMAKE_BUILD_TYPE STREQUAL "Release")
    set_source_files_properties(${DSP_SOURCES}
        PROPERTIES COMPILE_OPTIONS "-ffast-math"
    )
endif()

target_compile_definitions(iq_resample_tool PRIVATE APP_NAME="${PROJECT_NAME}")

configure_file(iq_resample_tool_presets.conf ${CMAKE_CURRENT_BINARY_DIR}/iq_resample_tool_presets.conf COPYONLY)

if(NOT MSVC)
    set(DEBUG_COMPILE_OPTIONS
        -ggdb
        -fsanitize=undefined
        -fsanitize=address
        -fno-omit-frame-pointer
    )
    set(RELEASE_COMPILE_OPTIONS
        -O3
        -ftree-vectorize
    )
    if(NOT CPU_TARGET_ARCHITECTURE STREQUAL "")
        list(APPEND RELEASE_COMPILE_OPTIONS "-march=${CPU_TARGET_ARCHITECTURE}")
    endif()
    
    # Apply the standard options to the ENTIRE executable target.
    # The DSP files will get these flags PLUS the -ffast-math flag from above.
    target_compile_options(iq_resample_tool PRIVATE
        $<IF:$<CONFIG:Debug>,${DEBUG_COMPILE_OPTIONS},>
        $<IF:$<CONFIG:Release>,${RELEASE_COMPILE_OPTIONS},>
    )
endif()


#=======================================================================
# Link Libraries
#=======================================================================
message(STATUS "Linking libraries...")

target_link_libraries(iq_resample_tool PRIVATE
    ${FINAL_SNDFILE_LIBRARIES}
    ${FINAL_LIQUIDDSP_LIBRARIES}
    ${FINAL_EXPAT_LIBRARIES}
    ${FINAL_PTHREADS_LIBRARIES}
)

if(WIN32)
    target_link_libraries(iq_resample_tool PRIVATE
        shlwapi
        pathcch
        shell32
    )
else()
    target_link_libraries(iq_resample_tool PRIVATE m)
endif()

if(WITH_RTLSDR)
    target_link_libraries(iq_resample_tool PRIVATE
        ${FINAL_RTLSDR_LIBRARIES}
        ${FINAL_LIBUSB_LIBRARIES}
    )
endif()
if(WITH_SDRPLAY)
    if(NOT WIN32)
        target_link_libraries(iq_resample_tool PRIVATE ${FINAL_SDRPLAY_LIBRARIES})
    endif()
endif()
if(WITH_HACKRF)
    target_link_libraries(iq_resample_tool PRIVATE
        ${FINAL_HACKRF_LIBRARIES}
        ${FINAL_LIBUSB_LIBRARIES}
    )
endif()
if(WITH_BLADERF)
    target_link_libraries(iq_resample_tool PRIVATE
        ${FINAL_BLADERF_LIBRARIES}
        ${FINAL_LIBUSB_LIBRARIES}
    )
endif()

#=======================================================================
# Doxygen Documentation (Optional)
#=======================================================================
if(BUILD_DOCUMENTATION)
    # If the user wants documentation, Doxygen is now a required dependency.
    find_package(Doxygen REQUIRED)
    message(STATUS "Doxygen found, 'doc' target will be available.")

    # Configure our Doxyfile.in template.
    configure_file(
        ${CMAKE_SOURCE_DIR}/doc/Doxyfile.in
        ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
        @ONLY
    )

    # Add the custom 'doc' target.
    add_custom_target(doc
        COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Generating documentation with Doxygen..."
        VERBATIM
    )
endif()


#=======================================================================
# Installation / Uninstall / Summary
#=======================================================================
install(TARGETS iq_resample_tool
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(UNIX)
    install(FILES iq_resample_tool_presets.conf DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/${PROJECT_NAME})
    message(STATUS "Installing presets to: ${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_SYSCONFDIR}/${PROJECT_NAME}")
else()
    install(FILES iq_resample_tool_presets.conf DESTINATION ${CMAKE_INSTALL_BINDIR})
    message(STATUS "Installing presets to: ${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}")
endif()

if(WITH_BLADERF)
    if(UNIX)
        install(DIRECTORY fpga DESTINATION ${CMAKE_INSTALL_DATADIR}/${PROJECT_NAME})
        message(STATUS "Installing BladeRF FPGA files to: ${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/${PROJECT_NAME}/fpga")
    endif()
endif()

file(READ "${CMAKE_CURRENT_LIST_DIR}/cmake/uninstall.cmake.in" UNINSTALL_SCRIPT_CONTENT)
configure_file("${CMAKE_CURRENT_LIST_DIR}/cmake/uninstall.cmake.in" "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake" @ONLY)
add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
    COMMENT "Running uninstall target"
)

message(STATUS "--------------------------------------------------")
message(STATUS "Configuration Summary:")
message(STATUS "  Build Type:        ${CMAKE_BUILD_TYPE}")
message(STATUS "  CPU Target Arch:   ${CPU_TARGET_ARCHITECTURE}")
if(WIN32)
    message(STATUS "  Build Target:      Windows")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(STATUS "  Build Target:      Linux")
elseif(UNIX)
    message(STATUS "  Build Target:      ${CMAKE_SYSTEM_NAME} (UNIX-like)")
else()
    message(STATUS "  Build Target:      ${CMAKE_SYSTEM_NAME} (Unknown)")
endif()
if(LTO_SUPPORTED)
    message(STATUS "  Link-Time Opt:     ENABLED")
else()
    message(STATUS "  Link-Time Opt:     DISABLED (not supported by compiler)")
endif()
message(STATUS "  Install Prefix:    ${CMAKE_INSTALL_PREFIX}")
if(BUILD_DOCUMENTATION)
    message(STATUS "  Documentation:     ENABLED (run 'make doc' to generate)")
else()
    message(STATUS "  Documentation:     DISABLED (use -DBUILD_DOCUMENTATION=ON to enable)")
endif()
message(STATUS "  libsndfile:        ${FINAL_SNDFILE_LIBRARIES}")
message(STATUS "  liquid-dsp:        ${FINAL_LIQUIDDSP_LIBRARIES}")
message(STATUS "  Expat:             ${FINAL_EXPAT_LIBRARIES}")
message(STATUS "  Threads:           ${FINAL_PTHREADS_LIBRARIES}")
if(WITH_HACKRF OR WITH_BLADERF OR WITH_RTLSDR)
    message(STATUS "  libusb:            ${FINAL_LIBUSB_LIBRARIES}")
endif()
if(WITH_RTLSDR)
    message(STATUS "  RTL-SDR Support:   ENABLED (${FINAL_RTLSDR_LIBRARIES})")
else()
    message(STATUS "  RTL-SDR Support:   DISABLED (use -DWITH_RTLSDR=ON to enable)")
endif()
if(WITH_SDRPLAY)
    message(STATUS "  SDRplay Support:   ENABLED (${FINAL_SDRPLAY_LIBRARIES})")
else()
    message(STATUS "  SDRplay Support:   DISABLED (use -DWITH_SDRPLAY=ON to enable)")
endif()
if(WITH_HACKRF)
    message(STATUS "  HackRF Support:    ENABLED (${FINAL_HACKRF_LIBRARIES})")
else()
    message(STATUS "  HackRF Support:    DISABLED (use -DWITH_HACKRF=ON to enable)")
endif()
if(WITH_BLADERF)
    message(STATUS "  BladeRF Support:   ENABLED (${FINAL_BLADERF_LIBRARIES})")
else()
    message(STATUS "  BladeRF Support:   DISABLED (use -DWITH_BLADERF=ON to enable)")
endif()
message(STATUS "--------------------------------------------------")
