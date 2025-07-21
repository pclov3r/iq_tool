#=======================================================================
# Project setup
#=======================================================================
cmake_minimum_required(VERSION 3.10)

project(nrsc5_resample_tool C)

if(NOT CMAKE_BUILD_TYPE)
   set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the build type: Debug Release RelWithDebInfo MinSizeRel" FORCE)
   message(STATUS "Build type not specified: defaulting to 'Release'.")
endif()
message(STATUS "Build type set to '${CMAKE_BUILD_TYPE}'")

include(GNUInstallDirs)

#=======================================================================
# Compiler specific setup & flags
#=======================================================================
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(MSVC)
    # --- MSVC ---
    add_compile_options(/W3)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
    add_compile_definitions(_USE_MATH_DEFINES)
else()
    # --- GCC / Clang ---
    add_compile_options(-Wall -Wextra -pedantic)
    add_compile_options(
        -Wvla
        -Wsign-compare
        -Wshadow
        -Wmissing-prototypes
    )
    # Check Clang version or if GCC version is >= 7 for -Wimplicit-fallthrough
    if(("${CMAKE_C_COMPILER_ID}" MATCHES "Clang") OR ("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU" AND CMAKE_C_COMPILER_VERSION VERSION_GREATER_EQUAL "7.0.0"))
        add_compile_options(-Wimplicit-fallthrough)
    endif()
    add_compile_definitions(_POSIX_C_SOURCE=200809L)
    add_compile_options(-fvisibility=hidden)
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -ggdb -fsanitize=undefined -fsanitize=address -fno-omit-frame-pointer" CACHE STRING "Flags used for Debug build" FORCE)
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O2" CACHE STRING "Flags used for Release build" FORCE)

    option(FORCE_COLORED_BUILD "Always produce ANSI-colored build output (GNU/Clang only)." FALSE)
    if(FORCE_COLORED_BUILD)
        if("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU")
            add_compile_options(-fdiagnostics-color=always)
        elseif("${CMAKE_C_COMPILER_ID}" MATCHES "Clang")
            add_compile_options(-fcolor-diagnostics)
        endif()
    endif()
endif() # End MSVC vs GCC/Clang block

# Corrected MinGW definition check
if(MINGW)
    add_compile_definitions(__USE_MINGW_ANSI_STDIO=1)
    # MinGW uses Windows threading model or winpthreads, find_package(Threads) should handle it.
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

#=======================================================================
# Find Dependencies (Required)
#=======================================================================
find_package(PkgConfig QUIET)

# --- libsndfile ---
message(STATUS "Looking for required library: libsndfile...")
set(FINAL_SNDFILE_INCLUDE_DIRS "")
set(FINAL_SNDFILE_LIBRARIES "")
set(SNDFILE_FOUND_OVERALL FALSE)

# 1. Check Manual Paths
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

# 2. Try pkg-config (if not found manually and native build)
if(NOT SNDFILE_FOUND_OVERALL AND NOT CMAKE_CROSSCOMPILING)
    if(PKG_CONFIG_FOUND)
        message(STATUS "Attempting pkg-config for libsndfile (native build)...")
        pkg_check_modules(PC_SNDFILE QUIET sndfile)
        if(PC_SNDFILE_FOUND)
            message(STATUS "Found libsndfile via pkg-config: ${PC_SNDFILE_VERSION}")
            set(FINAL_SNDFILE_INCLUDE_DIRS ${PC_SNDFILE_INCLUDE_DIRS})
            set(FINAL_SNDFILE_LIBRARIES ${PC_SNDFILE_LINK_LIBRARIES})
            set(SNDFILE_FOUND_OVERALL TRUE)
        endif() # End PC_SNDFILE_FOUND
    endif() # End PKG_CONFIG_FOUND
endif() # End check for pkg-config

# 3. Try find_package (if not found yet)
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

# 4. Final Check
if(NOT SNDFILE_FOUND_OVERALL)
    message(FATAL_ERROR "Could not find libsndfile. Please install development package, set CMAKE_PREFIX_PATH/CMAKE_FIND_ROOT_PATH, or set valid SNDFILE_INCLUDE_DIR and SNDFILE_LIBRARY.")
endif()


# --- liquid-dsp ---
message(STATUS "Looking for required library: liquid-dsp...")
set(FINAL_LIQUIDDSP_INCLUDE_DIRS "")
set(FINAL_LIQUIDDSP_LIBRARIES "")
set(LIQUIDDSP_FOUND_OVERALL FALSE)

# 1. Manual Path Check
if(NOT LIQUID_INCLUDE_DIR STREQUAL "" AND NOT LIQUID_LIBRARY STREQUAL "")
    message(STATUS "Checking manual paths for liquid-dsp...")
    set(header_path "") # Initialize variable
    # liquid.h can be in include/ or include/liquid/ depending on install
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
    unset(header_path) # Unset regardless of if block outcome
endif() # End Manual Path Check


# 2. Fallback: Explicit Search (Only runs if manual check failed OR wasn't specified)
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

endif() # End manual vs explicit search block

# 3. Final Check
if(NOT LIQUIDDSP_FOUND_OVERALL)
    message(FATAL_ERROR "Could not find liquid-dsp using any method. Please install development package, set CMAKE_PREFIX_PATH/CMAKE_FIND_ROOT_PATH, or set valid LIQUID_INCLUDE_DIR and LIQUID_LIBRARY.")
endif()


# --- Expat ---
message(STATUS "Looking for required library: Expat...")
set(FINAL_EXPAT_INCLUDE_DIRS "")
set(FINAL_EXPAT_LIBRARIES "")
set(EXPAT_FOUND_OVERALL FALSE)

# 1. Check Manual Paths
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

# 2. Try pkg-config (if not found manually and native build)
if(NOT EXPAT_FOUND_OVERALL AND NOT CMAKE_CROSSCOMPILING)
    if(PKG_CONFIG_FOUND)
        message(STATUS "Attempting pkg-config for Expat (native build)...")
        pkg_check_modules(PC_EXPAT QUIET expat)
        if(PC_EXPAT_FOUND)
            message(STATUS "Found Expat via pkg-config: ${PC_EXPAT_VERSION}")
            set(FINAL_EXPAT_INCLUDE_DIRS ${PC_EXPAT_INCLUDE_DIRS})
            set(FINAL_EXPAT_LIBRARIES ${PC_EXPAT_LINK_LIBRARIES})
            set(EXPAT_FOUND_OVERALL TRUE)
        endif() # End PC_EXPAT_FOUND
    endif() # End PKG_CONFIG_FOUND
endif() # End check for pkg-config

# 3. Try find_package (if not found yet)
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

# 4. Final Check for Expat
if(NOT EXPAT_FOUND_OVERALL)
    message(FATAL_ERROR "Could not find Expat. Please install development package (e.g., libexpat1-dev, expat-devel), set CMAKE_PREFIX_PATH/CMAKE_FIND_ROOT_PATH, or set valid EXPAT_INCLUDE_DIR and EXPAT_LIBRARY.")
endif()


# --- pthreads (POSIX Threads) ---
# Required for multithreading logic on POSIX systems
message(STATUS "Looking for required threading library: pthreads...")
set(PTHREADS_FOUND_OVERALL FALSE)
# Use the standard CMake module to find threading libraries
# This handles pthreads on POSIX, and appropriate libs on Windows/other OSes
find_package(Threads REQUIRED) # Use REQUIRED to make CMake fail if not found

if(Threads_FOUND)
     message(STATUS "Found threading library support (Threads::Threads target available)")
     # The Threads::Threads imported target handles includes and libraries automatically
     set(FINAL_PTHREADS_LIBRARIES Threads::Threads)
     set(PTHREADS_FOUND_OVERALL TRUE)
else()
    # This block should technically not be reached due to REQUIRED above,
    # but kept for clarity / potential fallback if REQUIRED was removed.
     message(STATUS "Could not find threading library support via find_package(Threads).")
     set(PTHREADS_FOUND_OVERALL FALSE)
endif()

# Final check (primarily relevant if REQUIRED wasn't used above)
if(NOT PTHREADS_FOUND_OVERALL)
     message(FATAL_ERROR "Could not find pthreads library. Ensure your system has pthreads development files installed.")
endif()
# --- End pthreads Section ---


#=======================================================================
# Project Sources and Target Definition
#=======================================================================
include_directories(include) # Project's own include dir
# Use the FINAL variables consistently
include_directories(${FINAL_SNDFILE_INCLUDE_DIRS})
include_directories(${FINAL_LIQUIDDSP_INCLUDE_DIRS})
include_directories(${FINAL_EXPAT_INCLUDE_DIRS})
# Note: find_package(Threads) usually doesn't require explicit include_directories
# if using the Threads::Threads target.

set(SOURCES
    src/cli.c
    src/config.c
    src/main.c
    src/metadata.c
    src/platform.c
    src/queue.c       # Added new queue source file
    src/resample.c
    src/setup.c
    src/utils.c
)

add_executable(nrsc5_resample_tool ${SOURCES})

#=======================================================================
# Link Libraries
#=======================================================================
message(STATUS "Linking libraries...")

# Start a list of libraries common to all platforms
set(TARGET_LINK_LIBS
    ${FINAL_SNDFILE_LIBRARIES}
    ${FINAL_LIQUIDDSP_LIBRARIES}
    ${FINAL_EXPAT_LIBRARIES}
    ${FINAL_PTHREADS_LIBRARIES}  # pthreads (Threads::Threads) is needed on all platforms using it
)

if(WIN32)
    # Add Windows-specific system libraries
    find_library(SHLWAPI_LIBRARY NAMES shlwapi Shlwapi)
    find_library(PATHCCH_LIBRARY NAMES pathcch PathCch)

    if(NOT SHLWAPI_LIBRARY)
        set(SHLWAPI_LIBRARY shlwapi) # Assume linker can find it
        message(STATUS "Assuming linker will find shlwapi")
    else()
        message(STATUS "Found Windows library: ${SHLWAPI_LIBRARY}")
    endif()
    if(NOT PATHCCH_LIBRARY)
        set(PATHCCH_LIBRARY pathcch) # Assume linker can find it
        message(STATUS "Assuming linker will find pathcch")
    else()
        message(STATUS "Found Windows library: ${PATHCCH_LIBRARY}")
    endif()

    list(APPEND TARGET_LINK_LIBS
        ${SHLWAPI_LIBRARY}
        ${PATHCCH_LIBRARY}
        # Add other Win-specific libs like ws2_32 if needed by dependencies
        # ws2_32
    )
else()
    # Add POSIX-specific libraries (like math library)
    list(APPEND TARGET_LINK_LIBS m)
endif()

# Perform the actual linking using the constructed list
target_link_libraries(nrsc5_resample_tool PRIVATE ${TARGET_LINK_LIBS})
#=======================================================================
#                                                               <<<<< END OF REPLACED SECTION
#=======================================================================
# Installation / Uninstall / Summary
#=======================================================================
install(TARGETS nrsc5_resample_tool
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# --- Uninstall target (unchanged) ---
# Create the uninstall script content
file(READ "${CMAKE_CURRENT_LIST_DIR}/cmake/uninstall.cmake.in" UNINSTALL_SCRIPT_CONTENT)
# Configure the script (replace placeholders - none in this example, but good practice)
configure_file("${CMAKE_CURRENT_LIST_DIR}/cmake/uninstall.cmake.in" "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake" @ONLY)

# Add the custom target
add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
    COMMENT "Running uninstall target"
)
# --- End Uninstall target ---

# --- Summary Message ---
message(STATUS "--------------------------------------------------")
message(STATUS "Configuration Summary:")
message(STATUS "  Build Type:      ${CMAKE_BUILD_TYPE}")
if(WIN32)
    message(STATUS "  Build Target:    Windows")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(STATUS "  Build Target:    Linux")
elseif(UNIX)
    message(STATUS "  Build Target:    ${CMAKE_SYSTEM_NAME} (UNIX-like)")
else()
    message(STATUS "  Build Target:    ${CMAKE_SYSTEM_NAME} (Unknown)")
endif()
message(STATUS "  Install Prefix:  ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  C Compiler Path: ${CMAKE_C_COMPILER}")
message(STATUS "  libsndfile:      ${FINAL_SNDFILE_LIBRARIES}")
message(STATUS "  liquid-dsp:      ${FINAL_LIQUIDDSP_LIBRARIES}")
message(STATUS "  Expat:           ${FINAL_EXPAT_LIBRARIES}")
message(STATUS "  Threads:         ${FINAL_PTHREADS_LIBRARIES}") # Use variable for consistency
message(STATUS "--------------------------------------------------")
