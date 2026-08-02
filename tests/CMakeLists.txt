# mayhem-b200 tests
# SPDX-License-Identifier: GPL-2.0-or-later

file(GLOB MB200_TEST_SOURCES CONFIGURE_DEPENDS test_*.cpp)

add_executable(mb200_tests ${MB200_TEST_SOURCES} runner.cpp)
target_link_libraries(mb200_tests PRIVATE mb200)
target_include_directories(mb200_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME mb200_tests COMMAND mb200_tests)

# The test binary links uhd.lib, so it needs uhd.dll on PATH at run time even
# though nothing in the tests opens a radio.
if(WIN32)
    set_tests_properties(mb200_tests PROPERTIES
        ENVIRONMENT "PATH=${UHD_ROOT}/bin;$ENV{PATH}")
endif()
