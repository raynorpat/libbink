cmake_minimum_required(VERSION 3.16)
project(libbink VERSION 0.1.0 LANGUAGES C)

include(GNUInstallDirs)

option(BINK_BUILD_STATIC "Build the static libbink library" ON)
option(BINK_BUILD_SHARED "Build the shared libbink library" ON)
option(BINK_BUILD_ENCODER "Build the libbink encoder library" ON)
option(BINK_BUILD_TOOLS "Build the example programs" OFF)
option(BINK_BUILD_TESTS "Build the test programs" ON)
if(BINK_BUILD_TOOLS AND NOT BINK_BUILD_ENCODER)
  message(STATUS
    "BINK_BUILD_TOOLS requires the encoder; enabling BINK_BUILD_ENCODER")
  set(BINK_BUILD_ENCODER ON CACHE BOOL
    "Build the libbink encoder library" FORCE)
endif()
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
  add_library(bink_static STATIC src/bink.c src/bink_bits.c src/bink_dsp.c
              src/bink_video.c src/bink_audio.c)
  bink_set_compile_settings(bink_static)
  target_include_directories(bink_static PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  set_target_properties(bink_static PROPERTIES OUTPUT_NAME bink)
  if(NOT MSVC)
    target_link_libraries(bink_static PUBLIC m)
  endif()
endif()

if(BINK_BUILD_SHARED)
  add_library(bink_shared SHARED src/bink.c src/bink_bits.c src/bink_dsp.c
              src/bink_video.c src/bink_audio.c)
  bink_set_compile_settings(bink_shared)
  target_include_directories(bink_shared PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  set_target_properties(bink_shared PROPERTIES
    OUTPUT_NAME bink
    VERSION ${PROJECT_VERSION}
    SOVERSION 0
    WINDOWS_EXPORT_ALL_SYMBOLS ON)
  if(NOT MSVC)
    target_link_libraries(bink_shared PUBLIC m)
  endif()
endif()

if(BINK_BUILD_SHARED)
  set(BINK_LINK_TARGET bink_shared)
else()
  set(BINK_LINK_TARGET bink_static)
endif()
add_library(Bink::bink ALIAS ${BINK_LINK_TARGET})

if(BINK_BUILD_ENCODER)
  if(NOT TARGET ${BINK_LINK_TARGET})
    message(FATAL_ERROR
      "BINK_BUILD_ENCODER requires BINK_BUILD_STATIC or BINK_BUILD_SHARED")
  endif()
  add_library(bink_encoder STATIC
    src/encoder/bink_encoder.c
    src/encoder/bink_enc_video.c
    src/encoder/bink_enc_audio.c
    src/encoder/bink_enc_dct.c
    src/encoder/bink_enc_image.c
    src/encoder/bink_enc_wav.c
    src/encoder/bink_enc_verify.c
    src/encoder/bink_enc_stb.c)
  bink_set_compile_settings(bink_encoder)
  target_include_directories(bink_encoder PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/encoder>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  target_include_directories(bink_encoder PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb)
  target_link_libraries(bink_encoder PUBLIC ${BINK_LINK_TARGET})
  set_target_properties(bink_encoder PROPERTIES OUTPUT_NAME binkenc)
  add_library(Bink::encoder ALIAS bink_encoder)
endif()

if(BINK_BUILD_TOOLS)
  add_executable(bink_dump src/tools/bink_dump.c)
  bink_set_compile_settings(bink_dump)
  target_link_libraries(bink_dump PRIVATE ${BINK_LINK_TARGET})

  add_executable(bink_build src/tools/bink_build.c)
  bink_set_compile_settings(bink_build)
  target_link_libraries(bink_build PRIVATE bink_encoder)

  # The SDL player is optional: prefer SDL3, fall back to SDL2, and skip the
  # example rather than breaking the rest of the tool build.
  if(NOT BINK_SDL_VERSION MATCHES "^(AUTO|2|3)$")
    message(FATAL_ERROR
      "BINK_SDL_VERSION must be AUTO, 2 or 3 (got '${BINK_SDL_VERSION}')")
  endif()

  set(BINK_SDL_TARGET "")
  set(BINK_USE_SDL3 OFF)

  if(BINK_SDL_VERSION STREQUAL "AUTO" OR BINK_SDL_VERSION STREQUAL "3")
    find_package(SDL3 QUIET CONFIG)
    if(SDL3_FOUND)
      set(BINK_SDL_TARGET SDL3::SDL3)
      set(BINK_USE_SDL3 ON)
    elseif(BINK_SDL_VERSION STREQUAL "3")
      message(FATAL_ERROR
        "BINK_SDL_VERSION=3 but no SDL3 CMake package was found.")
    endif()
  endif()

  if(BINK_SDL_TARGET STREQUAL "" AND
      (BINK_SDL_VERSION STREQUAL "AUTO" OR BINK_SDL_VERSION STREQUAL "2"))
    find_package(SDL2 QUIET CONFIG)
    if(SDL2_FOUND)
      set(BINK_SDL_TARGET SDL2::SDL2)
    elseif(BINK_SDL_VERSION STREQUAL "2")
      message(FATAL_ERROR
        "BINK_SDL_VERSION=2 but no SDL2 CMake package was found.")
    endif()
  endif()

  if(BINK_SDL_TARGET STREQUAL "")
    message(WARNING "SDL2/SDL3 not found; skipping bink_sdl_player.")
  else()
    add_executable(bink_sdl_player src/tools/bink_sdl_player.c
                   src/tools/bink_mix.c src/tools/bink_playback.c)
    add_executable(bink_sdlgl_player src/tools/bink_sdlgl_player.c
                   src/tools/bink_mix.c src/tools/bink_playback.c)
    bink_set_compile_settings(bink_sdl_player)
    bink_set_compile_settings(bink_sdlgl_player)
    if(BINK_USE_SDL3)
      target_compile_definitions(bink_sdl_player PRIVATE BINK_USE_SDL3=1)
      target_compile_definitions(bink_sdlgl_player PRIVATE BINK_USE_SDL3=1)
    elseif(TARGET SDL2::SDL2main)
      target_link_libraries(bink_sdl_player PRIVATE SDL2::SDL2main)
      target_link_libraries(bink_sdlgl_player PRIVATE SDL2::SDL2main)
    endif()
    target_link_libraries(bink_sdl_player PRIVATE
      ${BINK_LINK_TARGET} ${BINK_SDL_TARGET})
    target_link_libraries(bink_sdlgl_player PRIVATE
      ${BINK_LINK_TARGET} ${BINK_SDL_TARGET})
    if(APPLE)
      # OpenGL is deprecated on macOS; the API still works and the entry
      # points are loaded at run time, so only the header warnings are
      # worth silencing.
      target_compile_options(bink_sdlgl_player PRIVATE
        -Wno-deprecated-declarations)
    endif()
    message(STATUS "bink_sdl_player: linking ${BINK_LINK_TARGET} and ${BINK_SDL_TARGET}")
  endif()
endif()

if(BINK_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(BINK_BUILD_STATIC)
  install(TARGETS bink_static
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

if(BINK_BUILD_SHARED)
  install(TARGETS bink_shared
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

install(FILES src/bink.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(BINK_BUILD_ENCODER)
  install(TARGETS bink_encoder ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(FILES src/encoder/bink_encoder.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

if(BINK_BUILD_TOOLS)
  install(TARGETS bink_dump bink_build RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  if(TARGET bink_sdl_player)
    install(TARGETS bink_sdl_player RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()
endif()
