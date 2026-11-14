# Copyright Source Authors. All rights reserved.
#
# Builds the CEF bridge used by the HTML GameUI:
#   * libcef_dll_wrapper  - CEF C++ wrapper (from the CEF binary distribution)
#   * htmlhost            - static lib with a CEF-free API (htmlhost.h)
#   * gameui_cefsub       - CEF helper process executable
#
# CEF requires C++20 while the engine builds as C++17, hence the separate
# CMake project. gameui.vpc links htmlhost.lib + libcef_dll_wrapper.lib.
#
# Usage (from a VS x64 developer prompt):
#   cmake -S gameui/cefbridge -B gameui/cefbridge/out -A x64
#   cmake --build gameui/cefbridge/out --config Release

cmake_minimum_required(VERSION 3.21)
project(gameui_cefbridge CXX)

set_property(GLOBAL PROPERTY OS_FOLDERS ON)

if(NOT DEFINED CEF_ROOT)
  set(CEF_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../thirdparty/cef")
endif()
if(NOT EXISTS "${CEF_ROOT}/cmake/FindCEF.cmake")
  message(FATAL_ERROR "CEF binary distribution not found at ${CEF_ROOT}. Run build_htmlui_thirdparty.bat first.")
endif()
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CEF_ROOT}/cmake")

# No sandbox: the helper process is a plain executable.
set(USE_SANDBOX OFF CACHE BOOL "" FORCE)

find_package(CEF REQUIRED)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_subdirectory(${CEF_LIBCEF_DLL_WRAPPER_PATH} libcef_dll_wrapper)

# The minimal distribution only ships a Release libcef.lib; it is ABI-safe to
# use it for Debug builds too (C API boundary).
ADD_LOGICAL_TARGET("libcef_lib" "${CEF_ROOT}/Release/libcef.lib" "${CEF_ROOT}/Release/libcef.lib")

add_library(htmlhost STATIC htmlhost.cpp htmlhost.h)
SET_LIBRARY_TARGET_PROPERTIES(htmlhost)
add_dependencies(htmlhost libcef_dll_wrapper)
target_include_directories(htmlhost PRIVATE "${CEF_ROOT}")

add_executable(gameui_cefsub WIN32 subprocess_main.cpp gameui_cefsub.manifest)
SET_EXECUTABLE_TARGET_PROPERTIES(gameui_cefsub)
add_dependencies(gameui_cefsub libcef_dll_wrapper)
target_include_directories(gameui_cefsub PRIVATE "${CEF_ROOT}")
target_link_libraries(gameui_cefsub libcef_lib libcef_dll_wrapper ${CEF_STANDARD_LIBS})
