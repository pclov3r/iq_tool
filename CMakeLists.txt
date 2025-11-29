#=======================================================================
# Project setup
#=======================================================================
cmake_minimum_required(VERSION 3.10)
project(iq_tool C)

# Add our local 'cmake' folder to the search path
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

if(NOT CMAKE_BUILD_TYPE)
   set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the build type: Debug Release RelWithDebInfo MinSizeRel" FORCE)
   message(STATUS "Build type not specified: defaulting to 'Release'.")
endif()
message(STATUS "Build type set to '${CMAKE_BUILD_TYPE}'")

include(GNUInstallDirs)

#=======================================================================
# Options
#=======================================================================
option(WITH_RTLSDR "Enable RTL-SDR device support" OFF)
option(WITH_SDRPLAY "Enable SDRplay device support" OFF)
option(WITH_HACKRF "Enable HackRF device support" OFF)
option(WITH_BLADERF "Enable BladeRF device support" OFF)
option(BUILD_DOCUMENTATION "Enable building Doxygen documentation" OFF)

set(SNDFILE_INCLUDE_DIR "" CACHE PATH "Manual include path for libsndfile")
set(SNDFILE_LIBRARY "" CACHE PATH "Manual library path for libsndfile")
set(LIQUID_INCLUDE_DIR "" CACHE PATH "Manual include path for liquid-dsp")
set(LIQUID_LIBRARY "" CACHE PATH "Manual library path for liquid-dsp")
set(EXPAT_INCLUDE_DIR "" CACHE PATH "Manual include path for Expat")
set(EXPAT_LIBRARY "" CACHE PATH "Manual library path for Expat")

#=======================================================================
# Compiler Flags Setup
#=======================================================================
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

set(CPU_TARGET_ARCHITECTURE "native" CACHE STRING "Target CPU arch (-march=...).")

if(MSVC)
    add_compile_options(/W3)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE _USE_MATH_DEFINES)
else()
    add_compile_options(-Wall -Wextra -pedantic -Wvla -Wsign-compare -Wshadow -Wmissing-prototypes)
    add_compile_definitions(_GNU_SOURCE)
    add_compile_options(-fvisibility=hidden)

    set(RELEASE_COMPILE_OPTIONS -O3 -ftree-vectorize)
    if(NOT CPU_TARGET_ARCHITECTURE STREQUAL "")
        list(APPEND RELEASE_COMPILE_OPTIONS "-march=${CPU_TARGET_ARCHITECTURE}")
    endif()
endif()

if(MINGW)
    add_compile_definitions(__USE_MINGW_ANSI_STDIO=1)
endif()

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

#=======================================================================
# Dependencies
#=======================================================================
find_package(PkgConfig QUIET)

message(STATUS "Looking for required library: libsndfile...")
find_package(SndFile REQUIRED)

message(STATUS "Looking for required library: liquid-dsp...")
find_package(LiquidDSP REQUIRED)

message(STATUS "Looking for required library: Expat...")
find_package(Expat REQUIRED)

message(STATUS "Looking for required threading library: pthreads...")
find_package(Threads REQUIRED)

# Hardware Dependency Logic
if(WITH_RTLSDR OR WITH_HACKRF OR WITH_BLADERF)
    message(STATUS "Looking for required library: libusb-1.0...")
    find_package(LibUSB REQUIRED)
else()
    message(STATUS "libusb-1.0 search skipped (no USB-based SDRs enabled).")
endif()

if(WITH_RTLSDR)
    message(STATUS "Looking for required library: librtlsdr...")
    find_package(RtlSdr REQUIRED)
endif()

if(WITH_SDRPLAY)
    message(STATUS "Looking for required library: SDRplay API...")
    find_package(SDRplay REQUIRED)
endif()

if(WITH_HACKRF)
    message(STATUS "Looking for required library: libhackrf...")
    find_package(HackRF REQUIRED)
endif()

if(WITH_BLADERF)
    message(STATUS "Looking for required library: libbladeRF...")
    find_package(BladeRF REQUIRED)
endif()

#=======================================================================
# Sources
#=======================================================================
include_directories(include)
include_directories(${SndFile_INCLUDE_DIRS})
include_directories(${LiquidDSP_INCLUDE_DIRS})
include_directories(${Expat_INCLUDE_DIRS})

set(DSP_SOURCES
    src/agc.c
    src/resampler.c
    src/sample_convert.c
    src/iq_correct.c
    src/dc_block.c
    src/filter.c
    src/frequency_shift.c
)

set(NETWORK_SOURCES
    src/input_spyserver_client.c
)

set(OTHER_SOURCES
    src/argparse.c
    src/cli.c
    src/config.c
    src/input_rawfile.c
    src/input_wav.c
    src/log.c
    src/main.c
    src/memory_arena.c
    src/module_manager.c
    src/networking.c
    src/output_raw_file.c
    src/output_stdout.c
    src/output_wav_common.c
    src/output_wav.c
    src/output_wav_rf64.c
    src/platform.c
    src/pipeline.c
    src/presets_loader.c
    src/pre_processor.c
    src/post_processor.c
    src/queue.c
    src/ring_buffer.c
    src/sdr_packet_serializer.c
    src/setup.c
    src/signal_handler.c
    src/thread_manager.c
    src/utils.c
    src/utility_threads.c
)

if(WITH_RTLSDR)
    list(APPEND OTHER_SOURCES src/input_rtlsdr.c)
    include_directories(${RtlSdr_INCLUDE_DIRS} ${LibUSB_INCLUDE_DIRS})
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

#=======================================================================
# Target Creation
#=======================================================================
add_executable(iq_tool ${DSP_SOURCES} ${NETWORK_SOURCES} ${OTHER_SOURCES})

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
if(LTO_SUPPORTED)
    set_property(TARGET iq_tool PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    message(STATUS "Link-Time Optimization (LTO) enabled for target.")
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

# Apply Compiler Options
if(NOT MSVC)
    target_compile_options(iq_tool PRIVATE
        $<IF:$<CONFIG:Debug>,-ggdb -fsanitize=undefined -fsanitize=address -fno-omit-frame-pointer,>
        $<IF:$<CONFIG:Release>,${RELEASE_COMPILE_OPTIONS},>
    )
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        set_source_files_properties(${DSP_SOURCES} PROPERTIES COMPILE_OPTIONS "-ffast-math")
    endif()

    set_source_files_properties(${NETWORK_SOURCES} PROPERTIES COMPILE_OPTIONS "-fstack-protector-strong;-U_FORTIFY_SOURCE;-D_FORTIFY_SOURCE=2")
endif()

#=======================================================================
# Linking
#=======================================================================
message(STATUS "Linking libraries...")
target_link_libraries(iq_tool PRIVATE
    ${SndFile_LIBRARIES}
    ${LiquidDSP_LIBRARIES}
    ${Expat_LIBRARIES}
    Threads::Threads
)

if(WIN32)
    target_link_libraries(iq_tool PRIVATE shlwapi pathcch shell32 ws2_32)
else()
    target_link_libraries(iq_tool PRIVATE m)
endif()

if(WITH_RTLSDR)
    target_link_libraries(iq_tool PRIVATE ${RtlSdr_LIBRARIES} ${LibUSB_LIBRARIES})
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
message(STATUS "Installing presets to: ${PRESET_INSTALL_PATH}")

if(WITH_BLADERF AND UNIX)
    install(DIRECTORY fpga DESTINATION ${CMAKE_INSTALL_DATADIR}/${PROJECT_NAME})
    message(STATUS "Installing BladeRF FPGA files to: ${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/${PROJECT_NAME}/fpga")
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
message(STATUS "  Build Type:        ${CMAKE_BUILD_TYPE}")
message(STATUS "  CPU Target Arch:   ${CPU_TARGET_ARCHITECTURE}")
if(WIN32)
    message(STATUS "  Build Target:      Windows")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(STATUS "  Build Target:      Linux")
else()
    message(STATUS "  Build Target:      ${CMAKE_SYSTEM_NAME}")
endif()
if(LTO_SUPPORTED)
    message(STATUS "  Link-Time Opt:     ENABLED")
else()
    message(STATUS "  Link-Time Opt:     DISABLED")
endif()
message(STATUS "  Install Prefix:    ${CMAKE_INSTALL_PREFIX}")
if(BUILD_DOCUMENTATION)
    message(STATUS "  Documentation:     ENABLED (run 'make doc' to generate)")
else()
    message(STATUS "  Documentation:     DISABLED (use -DBUILD_DOCUMENTATION=ON to enable)")
endif()
message(STATUS "  libsndfile:        ${SndFile_LIBRARIES}")
message(STATUS "  liquid-dsp:        ${LiquidDSP_LIBRARIES}")
message(STATUS "  Expat:             ${Expat_LIBRARIES}")
message(STATUS "  Threads:           Threads::Threads")

if(WITH_RTLSDR)
    message(STATUS "  RTL-SDR Support:   ENABLED (${RtlSdr_LIBRARIES})")
else()
    message(STATUS "  RTL-SDR Support:   DISABLED (use -DWITH_RTLSDR=ON to enable)")
endif()
if(WITH_SDRPLAY)
    message(STATUS "  SDRplay Support:   ENABLED (${SDRplay_LIBRARIES})")
else()
    message(STATUS "  SDRplay Support:   DISABLED (use -DWITH_SDRPLAY=ON to enable)")
endif()
if(WITH_HACKRF)
    message(STATUS "  HackRF Support:    ENABLED (${HackRF_LIBRARIES})")
else()
    message(STATUS "  HackRF Support:    DISABLED (use -DWITH_HACKRF=ON to enable)")
endif()
if(WITH_BLADERF)
    message(STATUS "  BladeRF Support:   ENABLED (${BladeRF_LIBRARIES})")
else()
    message(STATUS "  BladeRF Support:   DISABLED (use -DWITH_BLADERF=ON to enable)")
endif()
message(STATUS "--------------------------------------------------")
