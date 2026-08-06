# mayhem-b200 — PortaPack Mayhem for the Ettus USRP B200
#
# Copyright (C) 2026 mayhem-b200 contributors
# SPDX-License-Identifier: GPL-2.0-or-later

cmake_minimum_required(VERSION 3.20)
project(mayhem_b200 VERSION 0.1.0 LANGUAGES CXX)

# C++20 is required by the vendored PortaPack sources: ui_painter.hpp builds
# ui::Style with designated initializers.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Dependencies
#
# UHD on Windows ships without a CMake package config, so the paths are cache
# variables with sane defaults for the stock installer layout. UHD 4.10's public
# headers are Boost-free apart from <boost/format.hpp> and
# <boost/core/noncopyable.hpp>, both header-only — hence a header path rather
# than linking any Boost library.
# ---------------------------------------------------------------------------

if(WIN32)
    set(UHD_ROOT "C:/Program Files/UHD" CACHE PATH "UHD install prefix")
    set(BOOST_INCLUDE_DIR "C:/vcpkg/installed/x64-windows/include"
        CACHE PATH "Directory containing boost/ headers")
else()
    set(UHD_ROOT "/usr/local" CACHE PATH "UHD install prefix")
    set(BOOST_INCLUDE_DIR "/usr/include" CACHE PATH "Directory containing boost/ headers")
endif()

find_path(UHD_INCLUDE_DIR uhd/usrp/multi_usrp.hpp
    HINTS "${UHD_ROOT}/include"
    DOC "UHD include directory")
find_library(UHD_LIBRARY NAMES uhd
    HINTS "${UHD_ROOT}/lib" "${UHD_ROOT}/lib64"
    DOC "UHD library")

if(NOT UHD_INCLUDE_DIR OR NOT UHD_LIBRARY)
    message(FATAL_ERROR
        "UHD not found. Set -DUHD_ROOT=<path to UHD install>.\n"
        "  UHD_INCLUDE_DIR = ${UHD_INCLUDE_DIR}\n"
        "  UHD_LIBRARY     = ${UHD_LIBRARY}")
endif()

if(NOT EXISTS "${BOOST_INCLUDE_DIR}/boost/format.hpp")
    message(FATAL_ERROR
        "Boost headers not found at ${BOOST_INCLUDE_DIR}. "
        "UHD's public headers include <boost/format.hpp>. "
        "Set -DBOOST_INCLUDE_DIR=<dir containing boost/>.")
endif()

message(STATUS "UHD include: ${UHD_INCLUDE_DIR}")
message(STATUS "UHD library: ${UHD_LIBRARY}")
message(STATUS "Boost headers: ${BOOST_INCLUDE_DIR}")

# ---------------------------------------------------------------------------
# Warnings
# ---------------------------------------------------------------------------

add_library(mb200_flags INTERFACE)
if(MSVC)
    # /W4 without /WX: the vendored PortaPack sources are not warning-clean under
    # MSVC and rewriting them would defeat the point of keeping them verbatim.
    target_compile_options(mb200_flags INTERFACE /W4 /permissive- /Zc:__cplusplus /utf-8 /MP)
    target_compile_definitions(mb200_flags INTERFACE
        NOMINMAX WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(mb200_flags INTERFACE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# Core library — everything except main() and the tests
# ---------------------------------------------------------------------------

file(GLOB MB200_UI_SOURCES     CONFIGURE_DEPENDS src/ui/*.cpp)
file(GLOB MB200_CORE_SOURCES   CONFIGURE_DEPENDS src/core/*.cpp)
file(GLOB MB200_DSP_SOURCES    CONFIGURE_DEPENDS src/dsp/*.cpp)
file(GLOB MB200_RADIO_SOURCES  CONFIGURE_DEPENDS src/radio/*.cpp)
file(GLOB MB200_AUDIO_SOURCES  CONFIGURE_DEPENDS src/audio/*.cpp)
file(GLOB MB200_APPS_SOURCES   CONFIGURE_DEPENDS src/apps/*.cpp)
file(GLOB MB200_REMOTE_SOURCES CONFIGURE_DEPENDS src/remote/*.cpp)

# OBJECT, not STATIC: apps self-register through a file-scope Registrar and are
# reached only through the registry, never named directly. A static library's
# linker would discard those objects as unreferenced and the apps would silently
# disappear from the menu. An object library forces every object into the final
# link, so the registrars always run. (See src/apps/app_registry.hpp.)
add_library(mb200 OBJECT
    ${MB200_UI_SOURCES}
    ${MB200_CORE_SOURCES}
    ${MB200_DSP_SOURCES}
    ${MB200_RADIO_SOURCES}
    ${MB200_AUDIO_SOURCES}
    ${MB200_APPS_SOURCES}
    ${MB200_REMOTE_SOURCES}
)

target_include_directories(mb200 PUBLIC
    src
    src/ui
    src/core
    src/dsp
    src/radio
    src/audio
    src/apps
    src/remote
)
target_include_directories(mb200 SYSTEM PUBLIC "${UHD_INCLUDE_DIR}" "${BOOST_INCLUDE_DIR}")
target_link_libraries(mb200 PUBLIC mb200_flags "${UHD_LIBRARY}")

if(WIN32)
    # gdi32/user32: the window and framebuffer blit. winmm: waveOut/waveIn audio.
    # ws2_32: Winsock, for NetworkRadio's sdrlink client (src/radio/network_radio.cpp).
    target_link_libraries(mb200 PUBLIC gdi32 user32 winmm ws2_32)
endif()

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------

add_executable(mayhem-b200 src/main.cpp)
target_link_libraries(mayhem-b200 PRIVATE mb200)

# Copy uhd.dll next to the binary so the app runs from the build tree.
if(WIN32)
    find_file(UHD_DLL uhd.dll HINTS "${UHD_ROOT}/bin")
    if(UHD_DLL)
        add_custom_command(TARGET mayhem-b200 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${UHD_DLL}" "$<TARGET_FILE_DIR:mayhem-b200>")
    endif()
endif()

# ---------------------------------------------------------------------------
# Hardware self-test (needs a real USRP; built on demand)
# ---------------------------------------------------------------------------

add_executable(mb200_hwtest EXCLUDE_FROM_ALL tools/hw_selftest.cpp)
target_link_libraries(mb200_hwtest PRIVATE mb200)

add_executable(mb200_appsmoke EXCLUDE_FROM_ALL tools/app_smoketest.cpp)
target_link_libraries(mb200_appsmoke PRIVATE mb200)

if(WIN32)
    find_file(UHD_DLL_HW uhd.dll HINTS "${UHD_ROOT}/bin")
    if(UHD_DLL_HW)
        add_custom_command(TARGET mb200_hwtest POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${UHD_DLL_HW}" "$<TARGET_FILE_DIR:mb200_hwtest>")
        add_custom_command(TARGET mb200_appsmoke POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${UHD_DLL_HW}" "$<TARGET_FILE_DIR:mb200_appsmoke>")
    endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

enable_testing()
add_subdirectory(tests)
