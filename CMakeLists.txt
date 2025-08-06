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
option(WITH_SDRPLAY "Enable SDRplay device support (requires SDRplay API library)" OFF)
option(WITH_HACKRF "Enable HackRF device support (requires libhackrf)" OFF)

#=======================================================================
# Compiler specific setup & flags
#=======================================================================
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

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
    add_compile_definitions(_POSIX_C_SOURCE=200809L)
    add_compile_options(-fvisibility=hidden)

    # Removed the direct 'set(CMAKE_C_FLAGS_DEBUG/RELEASE ... FORCE)' here.
    # Instead, we'll define the flags as lists and apply them with target_compile_options later.

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
if(WITH_SDRPLAY)
    add_compile_definitions(WITH_SDRPLAY)
endif()
if(WITH_HACKRF)
    add_compile_definitions(WITH_HACKRF)
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
if(WITH_SDRPLAY)
    set(SDRPLAY_INCLUDE_DIR "" CACHE PATH "Manual include path override for SDRplay API")
    set(SDRPLAY_LIBRARY "" CACHE PATH "Manual library file path override for SDRplay API")
endif()
if(WITH_HACKRF)
    set(HACKRF_INCLUDE_DIR "" CACHE PATH "Manual include path override for libhackrf")
    set(HACKRF_LIBRARY "" CACHE PATH "Manual library file path override for libhackrf")
    set(LIBUSB_INCLUDE_DIR "" CACHE PATH "Manual include path override for libusb-1.0")
    set(LIBUSB_LIBRARY "" CACHE PATH "Manual library file path override for libusb-1.0")
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

# --- START ADDITION FOR LIBUSB-1.0 ---
# This block is added to find libusb, which libhackrf depends on.
# It prioritizes manual paths passed from the build script.
message(STATUS "Looking for required library: libusb-1.0 (for HackRF support)...")
set(FINAL_LIBUSB_INCLUDE_DIRS "")
set(FINAL_LIBUSB_LIBRARIES "")
set(LIBUSB_FOUND_OVERALL FALSE)

if(WITH_HACKRF) # Only search for libusb if HackRF is enabled
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
            if(NOT EXISTS "${header_path}")
                message(WARNING "  -> Header not found at expected path: ${header_path}")
            endif()
            if(NOT EXISTS "${LIBUSB_LIBRARY}")
                message(WARNING "  -> Library not found at expected path: ${LIBUSB_LIBRARY}")
            endif()
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
        # Use find_package for libusb-1.0
        find_package(LibUSB-1.0 QUIET)
        if(LibUSB-1_0_FOUND AND DEFINED LibUSB-1_0_INCLUDE_DIRS AND LibUSB-1_0_INCLUDE_DIRS AND DEFINED LibUSB-1_0_LIBRARIES AND LibUSB-1_0_LIBRARIES)
            message(STATUS "Found libusb-1.0 via find_package.")
            set(FINAL_LIBUSB_INCLUDE_DIRS ${LibUSB-1_0_INCLUDE_DIRS})
            set(FINAL_LIBUSB_LIBRARIES ${LibUSB-1_0_LIBRARIES})
            set(LIBUSB_FOUND_OVERALL TRUE)
        else()
            message(STATUS "Could not find libusb-1.0 via find_package.")
            if(LibUSB-1_0_FOUND)
                message(WARNING "find_package(LibUSB-1.0) succeeded but did not set expected variables (LibUSB-1_0_INCLUDE_DIRS, LibUSB-1_0_LIBRARIES).")
            endif()
        endif()
    endif()

    if(NOT LIBUSB_FOUND_OVERALL)
        message(FATAL_ERROR "Could not find libusb-1.0, which is required for HackRF support. Please install libusb-1.0-dev, set CMAKE_PREFIX_PATH, or set valid LIBUSB_INCLUDE_DIR and LIBUSB_LIBRARY.")
    endif()
else()
    message(STATUS "libusb-1.0 search skipped (HackRF support not enabled).")
endif() # End WITH_HACKRF check for libusb search
# --- END ADDITION FOR LIBUSB-1.0 ---


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
endif() # End WITH_SDRPLAY block


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
endif() # End WITH_HACKRF block


#=======================================================================
# Project Sources and Target Definition
#=======================================================================
include_directories(include)
include_directories(${FINAL_SNDFILE_INCLUDE_DIRS})
include_directories(${FINAL_LIQUIDDSP_INCLUDE_DIRS})
include_directories(${FINAL_EXPAT_INCLUDE_DIRS})
if(WITH_SDRPLAY)
    include_directories(${FINAL_SDRPLAY_INCLUDE_DIRS})
endif()
if(WITH_HACKRF)
    include_directories(${FINAL_HACKRF_INCLUDE_DIRS})
    include_directories(${FINAL_LIBUSB_INCLUDE_DIRS})
endif()

set(SOURCES
    src/cli.c
    src/config.c
    src/dc_block.c          # ADDED
    src/file_writer.c
    src/iq_correct.c        # ADDED
    src/log.c
    src/main.c
    src/platform.c
    src/presets_loader.c
    src/queue.c
    src/resample.c
    src/sample_convert.c
    src/spectrum_shift.c
    src/signal_handler.c
    src/setup.c
    src/utils.c
    src/input_manager.c
    src/input_rawfile.c
    src/input_wav.c
)

if(WITH_SDRPLAY)
    list(APPEND SOURCES src/input_sdrplay.c)
endif()
if(WITH_HACKRF)
    list(APPEND SOURCES src/input_hackrf.c)
endif()

add_executable(iq_resample_tool ${SOURCES})

# This copies the source config file to the build directory.
# The install command will then pick it up from here.
configure_file(iq_resample_tool_presets.conf ${CMAKE_CURRENT_BINARY_DIR}/iq_resample_tool_presets.conf COPYONLY)

# Apply build-type specific compile options using generator expressions
if(NOT MSVC) # These flags are specific to GCC/Clang
    # Define Debug-specific flags
    set(DEBUG_COMPILE_OPTIONS
        -ggdb
        -fsanitize=undefined
        -fsanitize=address
        -fno-omit-frame-pointer
    )

    # Define Release-specific flags
    set(RELEASE_COMPILE_OPTIONS
        -O3
        -ftree-vectorize
    )

    # Add -march flag based on CPU_TARGET_ARCHITECTURE variable for Release builds
    if(NOT CPU_TARGET_ARCHITECTURE STREQUAL "")
        list(APPEND RELEASE_COMPILE_OPTIONS "-march=${CPU_TARGET_ARCHITECTURE}")
    endif()

    target_compile_options(iq_resample_tool PRIVATE
        $<IF:$<CONFIG:Debug>,${DEBUG_COMPILE_OPTIONS},>
        $<IF:$<CONFIG:Release>,${RELEASE_COMPILE_OPTIONS},>
        # Add other build types if needed, e.g., RelWithDebInfo, MinSizeRel
        # $<IF:$<CONFIG:RelWithDebInfo>,...,>
        # $<IF:$<CONFIG:MinSizeRel>,...,>
    )
endif()


#=======================================================================
# Link Libraries
#=======================================================================
message(STATUS "Linking libraries...")

set(TARGET_LINK_LIBS
    ${FINAL_SNDFILE_LIBRARIES}
    ${FINAL_LIQUIDDSP_LIBRARIES}
    ${FINAL_EXPAT_LIBRARIES}
    ${FINAL_PTHREADS_LIBRARIES}
)

if(WIN32)
    find_library(SHLWAPI_LIBRARY NAMES shlwapi Shlwapi)
    find_library(PATHCCH_LIBRARY NAMES pathcch PathCch)
    if(NOT SHLWAPI_LIBRARY)
        set(SHLWAPI_LIBRARY shlwapi)
        message(STATUS "Assuming linker will find shlwapi")
    else()
        message(STATUS "Found Windows library: ${SHLWAPI_LIBRARY}")
    endif()
    if(NOT PATHCCH_LIBRARY)
        set(PATHCCH_LIBRARY pathcch)
        message(STATUS "Assuming linker will find pathcch")
    else()
        message(STATUS "Found Windows library: ${PATHCCH_LIBRARY}")
    endif()
    list(APPEND TARGET_LINK_LIBS
        ${SHLWAPI_LIBRARY}
        ${PATHCCH_LIBRARY}
    )
else()
    list(APPEND TARGET_LINK_LIBS m)
endif()

if(WITH_SDRPLAY)
    # For Windows, we use explicit linking (LoadLibrary) to avoid a startup
    # dependency error. Therefore, we DO NOT link sdrplay_api.lib here.
    # For other platforms (like Linux), we link against the shared library normally.
    if(NOT WIN32)
        list(APPEND TARGET_LINK_LIBS ${FINAL_SDRPLAY_LIBRARIES})
    endif()
endif()

if(WITH_HACKRF)
    list(APPEND TARGET_LINK_LIBS
        ${FINAL_HACKRF_LIBRARIES}
        ${FINAL_LIBUSB_LIBRARIES}
    )
endif()

target_link_libraries(iq_resample_tool PRIVATE ${TARGET_LINK_LIBS})

#=======================================================================
# Installation / Uninstall / Summary
#=======================================================================
install(TARGETS iq_resample_tool
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Install the config file
if(UNIX)
    # On UNIX-like systems, install to /usr/local/etc/<APP_NAME>/
    # This hardcodes /usr/local/etc, ensuring the specific path requested.
    install(FILES iq_resample_tool_presets.conf DESTINATION "/usr/local/etc/${PROJECT_NAME}")
    message(STATUS "Installing presets to: /usr/local/etc/${PROJECT_NAME}")
else() # For Windows
    # On Windows, install alongside the executable
    install(FILES iq_resample_tool_presets.conf DESTINATION ${CMAKE_INSTALL_BINDIR})
    message(STATUS "Installing presets to: ${CMAKE_INSTALL_BINDIR}")
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
if(WIN32)
    message(STATUS "  Build Target:      Windows")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(STATUS "  Build Target:      Linux")
elseif(UNIX)
    message(STATUS "  Build Target:      ${CMAKE_SYSTEM_NAME} (UNIX-like)")
else()
    message(STATUS "  Build Target:      ${CMAKE_SYSTEM_NAME} (Unknown)")
endif()
message(STATUS "  Install Prefix:    ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  libsndfile:        ${FINAL_SNDFILE_LIBRARIES}")
message(STATUS "  liquid-dsp:        ${FINAL_LIQUIDDSP_LIBRARIES}")
message(STATUS "  Expat:             ${FINAL_EXPAT_LIBRARIES}")
message(STATUS "  Threads:           ${FINAL_PTHREADS_LIBRARIES}")
if(WITH_SDRPLAY)
    message(STATUS "  SDRplay Support:   ENABLED (${FINAL_SDRPLAY_LIBRARIES})")
else()
    message(STATUS "  SDRplay Support:   DISABLED (use -DWITH_SDRPLAY=ON to enable)")
endif()
if(WITH_HACKRF)
    message(STATUS "  HackRF Support:    ENABLED (${FINAL_HACKRF_LIBRARIES})")
    message(STATUS "  libusb:            ${FINAL_LIBUSB_LIBRARIES}")
else()
    message(STATUS "  HackRF Support:    DISABLED (use -DWITH_HACKRF=ON to enable)")
endif()
message(STATUS "  CPU Target Arch:   ${CPU_TARGET_ARCHITECTURE}")
message(STATUS "--------------------------------------------------")
