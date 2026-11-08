cmake_minimum_required(VERSION 3.16)
project(libbink VERSION 0.1.0 LANGUAGES C)

include(GNUInstallDirs)

option(BINK_BUILD_STATIC "Build the static libbink library" ON)
option(BINK_BUILD_SHARED "Build the shared libbink library" ON)
option(BINK_BUILD_TOOLS "Build the example programs" OFF)
option(BINK_BUILD_TESTS "Build the test programs" ON)
set(BINK_SDL_VERSION "AUTO" CACHE STRING
  "SDL version for bink_sdl_player: AUTO, 2 or 3")
set_property(CACHE BINK_SDL_VERSION PROPERTY STRINGS AUTO 2 3)

function(bink_set_compile_settings target)
  set_target_properties(${target} PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W3)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra)
  endif()
endfunction()

if(BINK_BUILD_STATIC)
  add_library(bink_static STATIC src/bink.c src/bink_bits.c)
  bink_set_compile_settings(bink_static)
  target_include_directories(bink_static PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  set_target_properties(bink_static PROPERTIES OUTPUT_NAME bink)
endif()

if(BINK_BUILD_SHARED)
  add_library(bink_shared SHARED src/bink.c src/bink_bits.c)
  bink_set_compile_settings(bink_shared)
  target_include_directories(bink_shared PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  set_target_properties(bink_shared PROPERTIES
    OUTPUT_NAME bink
    VERSION ${PROJECT_VERSION}
    SOVERSION 0
    WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

if(BINK_BUILD_SHARED)
  set(BINK_LINK_TARGET bink_shared)
else()
  set(BINK_LINK_TARGET bink_static)
endif()
add_library(Bink::bink ALIAS ${BINK_LINK_TARGET})

if(BINK_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
