cmake_minimum_required(VERSION 3.21)
project(SignalAnalyzer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(SA_BUILD_GUI   "Build the Qt 6 desktop application" ON)
option(SA_BUILD_TESTS "Build unit and integration tests"   ON)
set(SA_SANITIZER "" CACHE STRING "Sanitizer for GCC/Clang builds: '', 'address' or 'thread'")

# ---------------------------------------------------------------------------
# Shared warning/sanitizer settings
# ---------------------------------------------------------------------------
add_library(sa_options INTERFACE)
if(MSVC)
  target_compile_options(sa_options INTERFACE /W4 /permissive- /utf-8)
else()
  target_compile_options(sa_options INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion
                                              -Wno-sign-conversion)
  if(SA_SANITIZER)
    target_compile_options(sa_options INTERFACE -fsanitize=${SA_SANITIZER} -fno-omit-frame-pointer -g)
    target_link_options(sa_options INTERFACE -fsanitize=${SA_SANITIZER})
  endif()
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Core library: networking, ring buffer, DSP, threading. No Qt dependency.
# ---------------------------------------------------------------------------
add_library(sa_core
  core/src/Protocol.cpp
  core/src/Dsp.cpp
  core/src/Udp.cpp
  core/src/Pipeline.cpp)
target_include_directories(sa_core PUBLIC core/include)
target_link_libraries(sa_core PUBLIC Threads::Threads PRIVATE sa_options)
if(WIN32)
  target_link_libraries(sa_core PUBLIC ws2_32)
endif()

# Headless front-end
add_executable(sa_cli cli/main.cpp)
target_link_libraries(sa_cli PRIVATE sa_core sa_options)

# ---------------------------------------------------------------------------
# Qt GUI
# ---------------------------------------------------------------------------
if(SA_BUILD_GUI)
  find_package(Qt6 QUIET COMPONENTS Widgets)
  if(Qt6_FOUND)
    qt_standard_project_setup()
    qt_add_executable(signal_analyzer
      app/main.cpp
      app/MainWindow.cpp
      app/MainWindow.hpp
      app/PlotWidget.cpp
      app/PlotWidget.hpp)
    target_link_libraries(signal_analyzer PRIVATE sa_core sa_options Qt6::Widgets)
    set_target_properties(signal_analyzer PROPERTIES WIN32_EXECUTABLE ON MACOSX_BUNDLE ON)
  else()
    message(WARNING "Qt6 Widgets not found: skipping the GUI. "
                    "Set CMAKE_PREFIX_PATH to your Qt install or pass -DSA_BUILD_GUI=OFF.")
  endif()
endif()

# ---------------------------------------------------------------------------
# Tests (GoogleTest fetched at configure time)
# ---------------------------------------------------------------------------
if(SA_BUILD_TESTS)
  # Use an installed GoogleTest if there is one, otherwise fetch it.
  find_package(GTest CONFIG QUIET)
  if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG        v1.15.2
      GIT_SHALLOW    ON)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()

  enable_testing()
  add_executable(sa_tests
    tests/test_ring_buffer.cpp
    tests/test_dsp.cpp
    tests/test_protocol_pipeline.cpp)
  target_link_libraries(sa_tests PRIVATE sa_core sa_options GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(sa_tests DISCOVERY_TIMEOUT 30)
endif()
