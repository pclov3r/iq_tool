#=======================================================================
# Project setup
#=======================================================================
cmake_minimum_required(VERSION 3.10)
project(iq_tool C)

# Add our local 'cmake' folder to the search path
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# Set default build type to 'Release' if not specified and not a multi-config generator
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
   set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the build type: Debug Release DebugTSan DebugMSan RelWithDebInfo MinSizeRel" FORCE)
   # Set the possible values for cmake-gui and IDE drop-down lists
   set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "DebugTSan" "DebugMSan" "MinSizeRel" "RelWithDebInfo")
   message(STATUS "Build type not specified: defaulting to 'Release'.")
endif()
message(STATUS "Build type set to '${CMAKE_BUILD_TYPE}'")

include(GNUInstallDirs)

#=======================================================================
# Options
#=======================================================================
option(WITH_RTLSDR "Enable RTL-SDR device support" OFF)
option(WITH_AIRSPY "Enable Airspy device support" OFF)
option(WITH_AIRSPYHF "Enable Airspy HF+ device support" OFF)
option(WITH_SDRPLAY "Enable SDRplay device support" OFF)
option(WITH_HACKRF "Enable HackRF device support" OFF)
option(WITH_BLADERF "Enable BladeRF device support" OFF)
option(WITH_NRSC5 "Enable NRSC5 (HD Radio) output support" OFF)
option(WITH_REDSEA "Enable Redsea RDS decoder (Requires C++17)" OFF)
option(BUILD_DOCUMENTATION "Enable building Doxygen documentation" OFF)

set(SNDFILE_INCLUDE_DIR "" CACHE PATH "Manual include path for libsndfile")
set(SNDFILE_LIBRARY "" CACHE PATH "Manual library path for libsndfile")
set(LIQUID_INCLUDE_DIR "" CACHE PATH "Manual include path for liquid-dsp")
set(LIQUID_LIBRARY "" CACHE PATH "Manual library path for liquid-dsp")
set(EXPAT_INCLUDE_DIR "" CACHE PATH "Manual include path for Expat")
set(EXPAT_LIBRARY "" CACHE PATH "Manual library path for Expat")
set(NRSC5_INCLUDE_DIR "" CACHE PATH "Manual include path for libnrsc5")
set(NRSC5_LIBRARY "" CACHE PATH "Manual library path for libnrsc5")
set(AIRSPY_INCLUDE_DIR "" CACHE PATH "Manual include path for libairspy")
set(AIRSPY_LIBRARY "" CACHE PATH "Manual library path for libairspy")
set(AIRSPYHF_INCLUDE_DIR "" CACHE PATH "Manual include path for libairspyhf")
set(AIRSPYHF_LIBRARY "" CACHE PATH "Manual library path for libairspyhf")
set(ICONV_INCLUDE_DIR "" CACHE PATH "Manual include path for libiconv")
set(ICONV_LIBRARY "" CACHE PATH "Manual library path for libiconv")

#=======================================================================
# Compiler Flags Setup
#=======================================================================
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# --- Conditional C++ Support ---
if(WITH_REDSEA)
    enable_language(CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    # We apply the definition to the target later
endif()
# -------------------------------

set(CPU_TARGET_ARCHITECTURE "native" CACHE STRING "Target CPU arch (-march=...).")

add_compile_definitions(_GNU_SOURCE)
add_compile_options(-fvisibility=hidden)

# 1. Common Flags (Safe for both C and C++)
add_compile_options(
    -Wall
    -Wextra
    -pedantic
    -Wvla
    -Wsign-compare
    -Wshadow
)

# 2. C-Only Flags (Applied ONLY to .c files)
add_compile_options($<$<COMPILE_LANGUAGE:C>:-Wmissing-prototypes>)

# 3. C++-Only Flags (Applied ONLY to .cpp/.cc files)
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Wno-unknown-pragmas>)

# Optimization Flags
set(RELEASE_COMPILE_OPTIONS -O3 -ftree-vectorize -Wno-deprecated-declarations)
if(NOT CPU_TARGET_ARCHITECTURE STREQUAL "")
    list(APPEND RELEASE_COMPILE_OPTIONS "-march=${CPU_TARGET_ARCHITECTURE}")
endif()

#=======================================================================
# Dependencies
#=======================================================================
find_package(PkgConfig QUIET)

message(STATUS "Searching for required system libraries...")
find_package(SndFile REQUIRED)
find_package(LiquidDSP REQUIRED)
find_package(Expat REQUIRED)
find_package(Threads REQUIRED)

# --- Redsea Dependency ---
if(WITH_REDSEA)
    if(ICONV_INCLUDE_DIR AND ICONV_LIBRARY)
        if(NOT TARGET Iconv::Iconv)
            add_library(Iconv::Iconv UNKNOWN IMPORTED)
            set_target_properties(Iconv::Iconv PROPERTIES
                IMPORTED_LOCATION "${ICONV_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${ICONV_INCLUDE_DIR}"
            )
        endif()
        set(Iconv_FOUND TRUE)
        set(Iconv_INCLUDE_DIRS ${ICONV_INCLUDE_DIR})
    else()
        find_package(Iconv REQUIRED)
    endif()
endif()
# -------------------------

# Hardware Dependency Logic
if(WITH_RTLSDR OR WITH_AIRSPY OR WITH_AIRSPYHF OR WITH_HACKRF OR WITH_BLADERF)
    find_package(LibUSB REQUIRED)
endif()

if(WITH_RTLSDR)
    find_package(RtlSdr REQUIRED)
endif()

if(WITH_AIRSPY)
    find_package(Airspy REQUIRED)
endif()

if(WITH_AIRSPYHF)
    find_package(AirspyHF REQUIRED)
endif()

if(WITH_SDRPLAY)
    find_package(SDRplay REQUIRED)
endif()

if(WITH_HACKRF)
    find_package(HackRF REQUIRED)
endif()

if(WITH_BLADERF)
    find_package(BladeRF REQUIRED)
endif()

if(WITH_NRSC5)
    find_package(NRSC5 REQUIRED)
endif()

#=======================================================================
# Sources
#=======================================================================
include_directories(include)
include_directories(${SndFile_INCLUDE_DIRS})
include_directories(${LiquidDSP_INCLUDE_DIRS})
include_directories(${Expat_INCLUDE_DIRS})

# --- Redsea Library Setup ---
if(WITH_REDSEA)
    include_directories(${CMAKE_SOURCE_DIR}/external/redsea)
    include_directories(${Iconv_INCLUDE_DIRS})
    file(GLOB_RECURSE REDSEA_LIB_SOURCES "${CMAKE_SOURCE_DIR}/external/redsea/src/*.cc")
else()
    set(REDSEA_LIB_SOURCES "")
endif()
# ----------------------------

set(COMPILE_WITH_FFAST_MATH_SOURCES
    src/agc.c
    src/dc_block.c
    src/filter.c
    src/frequency_shift.c
    src/iq_correction.c
    src/keyboard_handler.c
    src/main.c
    src/output_am.c
    src/output_nfm.c
    src/output_wfm.c
    src/pipeline_stages.c
    src/post_processor.c
    src/pre_processor.c
    src/resampler.c
    src/sample_convert.c
    src/utility_threads.c
)

set(NETWORK_SOURCES
    src/input_spyserver_client.c
)

set(OTHER_SOURCES
    src/argparse.c
    src/audio_output_functions.c
    src/cli.c
    src/config.c
    src/input_rawfile.c
    src/input_stdin.c
    src/input_wav.c
    src/log.c
    src/main.c
    src/mem_arena.c
    src/miniaudio.c
    src/module_registry.c
    src/networking.c
    src/output_directpipe.c
    src/output_rawfile.c
    src/output_stdout.c
    src/output_wav.c
    src/output_wav_common.c
    src/output_wav_rf64.c
    src/pipeline.c
    src/platform.c
    src/presets_loader.c
    src/queue.c
    src/ring_buffer.c
    src/packet_serializer.c
    src/sample_format_table.c
    src/signal_handler.c
    src/thread_manager.c
    src/utilities.c
    src/wait_event.c
)

# --- Add the correct Wrapper ---
if(WITH_REDSEA)
    list(APPEND OTHER_SOURCES src/redsea_wrapper.cpp)
else()
    list(APPEND OTHER_SOURCES src/redsea_wrapper_stub.c)
endif()
# -------------------------------

if(WITH_RTLSDR)
    list(APPEND OTHER_SOURCES src/input_rtlsdr.c)
    include_directories(${RtlSdr_INCLUDE_DIRS} ${LibUSB_INCLUDE_DIRS})
endif()
if(WITH_AIRSPY)
    list(APPEND OTHER_SOURCES src/input_airspy.c)
    include_directories(${Airspy_INCLUDE_DIRS} ${LibUSB_INCLUDE_DIRS})
endif()
if(WITH_AIRSPYHF)
    list(APPEND OTHER_SOURCES src/input_airspyhf.c)
    include_directories(${AirspyHF_INCLUDE_DIRS} ${LibUSB_INCLUDE_DIRS})
endif()
if(WITH_SDRPLAY)
    list(APPEND OTHER_SOURCES src/input_sdrplay.c)
    include_directories(${SDRplay_INCLUDE_DIRS})
endif()
if(WITH_HACKRF)
    list(APPEND OTHER_SOURCES src/input_hackrf.c)
    include_directories(${HackRF_INCLUDE_DIRS} ${LibUSB_INCLUDE_DIRS})
endif()
if(WITH_BLADERF)
    list(APPEND OTHER_SOURCES src/input_bladerf.c)
    include_directories(${BladeRF_INCLUDE_DIRS} ${LibUSB_INCLUDE_DIRS})
endif()
if(WITH_NRSC5)
    list(APPEND COMPILE_WITH_FFAST_MATH_SOURCES src/output_nrsc5.c)
    include_directories(${NRSC5_INCLUDE_DIRS})
endif()

#=======================================================================
# Target Creation
#=======================================================================
add_executable(iq_tool ${COMPILE_WITH_FFAST_MATH_SOURCES} ${NETWORK_SOURCES} ${OTHER_SOURCES} ${REDSEA_LIB_SOURCES})

#=======================================================================
# Target Properties
#=======================================================================

# Manually apply definitions to the target to ensure they aren't lost
if(MINGW)
    target_compile_definitions(iq_tool PRIVATE __USE_MINGW_ANSI_STDIO=1)
endif()

if(WITH_REDSEA)
    target_compile_definitions(iq_tool PRIVATE WITH_REDSEA)
endif()

if(WITH_RTLSDR)
    target_compile_definitions(iq_tool PRIVATE WITH_RTLSDR)
endif()

if(WITH_AIRSPY)
    target_compile_definitions(iq_tool PRIVATE WITH_AIRSPY)
endif()

if(WITH_AIRSPYHF)
    target_compile_definitions(iq_tool PRIVATE WITH_AIRSPYHF)
endif()

if(WITH_SDRPLAY)
    target_compile_definitions(iq_tool PRIVATE WITH_SDRPLAY)
endif()

if(WITH_HACKRF)
    target_compile_definitions(iq_tool PRIVATE WITH_HACKRF)
endif()

if(WITH_BLADERF)
    target_compile_definitions(iq_tool PRIVATE WITH_BLADERF)
endif()

if(WITH_NRSC5)
    target_compile_definitions(iq_tool PRIVATE WITH_NRSC5)
endif()


#=======================================================================
# System Checks & Configuration
#=======================================================================
include(CheckFunctionExists)
check_function_exists(strcasestr HAVE_STRCASESTR)
if(HAVE_STRCASESTR)
    target_compile_definitions(iq_tool PRIVATE HAVE_STRCASESTR)
endif()

include(CheckIPOSupported)
check_ipo_supported(RESULT LTO_SUPPORTED)

# Do not enable LTO in Debug, DebugTSan, or DebugMSan modes. It breaks debugging and sanitizers.
if(LTO_SUPPORTED)
    if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo" OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
        set_property(TARGET iq_tool PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        message(STATUS "Link-Time Optimization (LTO) enabled for target.")
    else()
        message(STATUS "Link-Time Optimization (LTO) disabled for Debug/Sanitizer builds.")
    endif()
endif()

# Git Versioning
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
        message(STATUS "Detected Git commit hash: ${GIT_HASH}")
        set(VERSION_INFO "${GIT_HASH}")
    else()
        set(VERSION_INFO "unknown-git-error")
    endif()
else()
    set(VERSION_INFO "unknown-no-git")
endif()
target_compile_definitions(iq_tool PRIVATE GIT_HASH="${VERSION_INFO}")
target_compile_definitions(iq_tool PRIVATE APP_NAME="${PROJECT_NAME}")

# Apply Compiler Options based on active Build Type
target_compile_options(iq_tool PRIVATE
    $<$<CONFIG:Debug>:-O0 -ggdb -fsanitize=address,undefined -fno-omit-frame-pointer>
    $<$<CONFIG:DebugTSan>:-O0 -ggdb -fsanitize=thread -fno-omit-frame-pointer>
    $<$<CONFIG:DebugMSan>:-O0 -ggdb -fsanitize=memory -fno-omit-frame-pointer>
    $<$<CONFIG:Release>:${RELEASE_COMPILE_OPTIONS}>
)

# Ensure Sanitizers are passed to the Linker as well (Critical for Sanitizers)
target_link_options(iq_tool PRIVATE
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>
    $<$<CONFIG:DebugTSan>:-fsanitize=thread>
    $<$<CONFIG:DebugMSan>:-fsanitize=memory>
)

# Suppress LTO type mismatch warnings for Redsea C++/liquid-dsp interop
if(WITH_REDSEA)
    target_link_options(iq_tool PRIVATE -Wno-lto-type-mismatch)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set_source_files_properties(${COMPILE_WITH_FFAST_MATH_SOURCES} PROPERTIES COMPILE_OPTIONS "-ffast-math")
endif()

set_source_files_properties(${NETWORK_SOURCES} PROPERTIES COMPILE_OPTIONS "-fstack-protector-strong;-U_FORTIFY_SOURCE;-D_FORTIFY_SOURCE=2")

#=======================================================================
# Linking
#=======================================================================
target_link_libraries(iq_tool PRIVATE
    ${SndFile_LIBRARIES}
    ${LiquidDSP_LIBRARIES}
    ${Expat_LIBRARIES}
    Threads::Threads
)

if(WIN32)
    target_link_libraries(iq_tool PRIVATE shlwapi pathcch shell32 ws2_32 ole32)
else()
    target_link_libraries(iq_tool PRIVATE m)
endif()

if(WITH_REDSEA)
    target_link_libraries(iq_tool PRIVATE Iconv::Iconv)
endif()

if(WITH_RTLSDR)
    target_link_libraries(iq_tool PRIVATE ${RtlSdr_LIBRARIES} ${LibUSB_LIBRARIES})
endif()
if(WITH_AIRSPY)
    target_link_libraries(iq_tool PRIVATE ${Airspy_LIBRARIES} ${LibUSB_LIBRARIES})
endif()
if(WITH_AIRSPYHF)
    target_link_libraries(iq_tool PRIVATE ${AirspyHF_LIBRARIES} ${LibUSB_LIBRARIES})
endif()
if(WITH_SDRPLAY AND NOT WIN32)
    target_link_libraries(iq_tool PRIVATE ${SDRplay_LIBRARIES})
endif()
if(WITH_HACKRF)
    target_link_libraries(iq_tool PRIVATE ${HackRF_LIBRARIES} ${LibUSB_LIBRARIES})
endif()
if(WITH_BLADERF)
    target_link_libraries(iq_tool PRIVATE ${BladeRF_LIBRARIES} ${LibUSB_LIBRARIES})
endif()
if(WITH_NRSC5)
    target_link_libraries(iq_tool PRIVATE ${NRSC5_LIBRARIES})
    target_link_libraries(iq_tool PRIVATE ${CMAKE_DL_LIBS})
endif()

#=======================================================================
# Install & Config
#=======================================================================
configure_file(iq_tool_presets.conf ${CMAKE_CURRENT_BINARY_DIR}/iq_tool_presets.conf COPYONLY)

install(TARGETS iq_tool RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(UNIX)
    set(PRESET_INSTALL_PATH "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_SYSCONFDIR}/${PROJECT_NAME}")
    install(FILES iq_tool_presets.conf DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/${PROJECT_NAME})
else()
    set(PRESET_INSTALL_PATH "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}")
    install(FILES iq_tool_presets.conf DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(WITH_BLADERF AND UNIX)
    install(DIRECTORY fpga DESTINATION ${CMAKE_INSTALL_DATADIR}/${PROJECT_NAME})
endif()

if(BUILD_DOCUMENTATION)
    find_package(Doxygen REQUIRED)
    configure_file(${CMAKE_SOURCE_DIR}/doc/Doxyfile.in ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile @ONLY)
    add_custom_target(doc COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endif()

#=======================================================================
# Summary
#=======================================================================
message(STATUS "--------------------------------------------------")
message(STATUS "Configuration Summary:")
message(STATUS "  Build Type:          ${CMAKE_BUILD_TYPE}")
message(STATUS "  CPU Target Arch:     ${CPU_TARGET_ARCHITECTURE}")
message(STATUS "  Build Target:        ${CMAKE_SYSTEM_NAME}")
if(LTO_SUPPORTED AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(STATUS "  Link-Time Opt:       ENABLED")
else()
    message(STATUS "  Link-Time Opt:       DISABLED")
endif()
message(STATUS "  Install Prefix:      ${CMAKE_INSTALL_PREFIX}")
if(BUILD_DOCUMENTATION)
    message(STATUS "  Documentation:       ENABLED (run 'make doc' to generate)")
else()
    message(STATUS "  Documentation:       DISABLED (use -DBUILD_DOCUMENTATION=ON to enable)")
endif()
message(STATUS "  libsndfile:          ${SndFile_LIBRARIES}")
message(STATUS "  liquid-dsp:          ${LiquidDSP_LIBRARIES}")
message(STATUS "  Expat:               ${Expat_LIBRARIES}")
message(STATUS "  Threads:             Threads::Threads")

if(WITH_REDSEA)
    message(STATUS "  Redsea RDS:          ENABLED")
else()
    message(STATUS "  Redsea RDS:          DISABLED (use -DWITH_REDSEA=ON to enable)")
endif()

if(WITH_RTLSDR)
    message(STATUS "  RTL-SDR Support:     ENABLED (${RtlSdr_LIBRARIES})")
else()
    message(STATUS "  RTL-SDR Support:     DISABLED (use -DWITH_RTLSDR=ON to enable)")
endif()
if(WITH_AIRSPY)
    message(STATUS "  Airspy Support:      ENABLED (${Airspy_LIBRARIES})")
else()
    message(STATUS "  Airspy Support:      DISABLED (use -DWITH_AIRSPY=ON to enable)")
endif()
if(WITH_AIRSPYHF)
    message(STATUS "  Airspy HF+ Support:  ENABLED (${AirspyHF_LIBRARIES})")
else()
    message(STATUS "  Airspy HF+ Support:  DISABLED (use -DWITH_AIRSPYHF=ON to enable)")
endif()
if(WITH_SDRPLAY)
    message(STATUS "  SDRplay Support:     ENABLED (${SDRplay_LIBRARIES})")
else()
    message(STATUS "  SDRplay Support:     DISABLED (use -DWITH_SDRPLAY=ON to enable)")
endif()
if(WITH_HACKRF)
    message(STATUS "  HackRF Support:      ENABLED (${HackRF_LIBRARIES})")
else()
    message(STATUS "  HackRF Support:      DISABLED (use -DWITH_HACKRF=ON to enable)")
endif()
if(WITH_BLADERF)
    message(STATUS "  BladeRF Support:     ENABLED (${BladeRF_LIBRARIES})")
else()
    message(STATUS "  BladeRF Support:     DISABLED (use -DWITH_BLADERF=ON to enable)")
endif()
if(WITH_NRSC5)
    message(STATUS "  NRSC5 Support:       ENABLED (${NRSC5_LIBRARIES})")
else()
    message(STATUS "  NRSC5 Support:       DISABLED (use -DWITH_NRSC5=ON to enable)")
endif()
message(STATUS "--------------------------------------------------")
