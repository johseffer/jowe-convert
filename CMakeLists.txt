cmake_minimum_required(VERSION 3.25)

project(jowe_convert
  VERSION 0.2.1
  DESCRIPTION "Convert images, audio, video and 3D on any OS"
  HOMEPAGE_URL "https://github.com/johseffer/jowe-convert"
  LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(EMSCRIPTEN)
  set(_jc_heic_default OFF)
else()
  set(_jc_heic_default ON)
endif()

option(JOWE_CONVERT_BUILD_CLI "Build the jvconvert CLI and MCP server" ON)
option(JOWE_CONVERT_BUILD_TESTS "Build the unit tests" ${PROJECT_IS_TOP_LEVEL})
option(JOWE_CONVERT_WITH_HEIC_ENCODER "Encode HEIC with kvazaar (BSD-3)" ${_jc_heic_default})
option(JOWE_CONVERT_WITH_OS_CODECS "Use OS codecs (Media Foundation, WIC, AudioToolbox, ImageIO) where present" ON)

if(EMSCRIPTEN)
  add_compile_options(-fexceptions)
  add_link_options(-fexceptions -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=2097152)
endif()

include(cmake/Dependencies.cmake)

add_subdirectory(src)

if(JOWE_CONVERT_BUILD_CLI)
  add_subdirectory(cli)
endif()

if(JOWE_CONVERT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
