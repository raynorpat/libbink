if(NOT DEFINED PLAYER_EXE OR NOT EXISTS "${PLAYER_EXE}")
  message(FATAL_ERROR "bink_sdl_player executable not found: ${PLAYER_EXE}")
endif()
if(NOT DEFINED ENV{BINK_TEST_MEDIA} OR "$ENV{BINK_TEST_MEDIA}" STREQUAL "")
  message(STATUS "SKIP: BINK_TEST_MEDIA not set")
  return()
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
          SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
          BINK_SDL_TEST_FRAMES=5
          "${PLAYER_EXE}" "$ENV{BINK_TEST_MEDIA}/logo_lucas.bik"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error
  TIMEOUT 120)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "bink_sdl_player failed (${result}): ${output}${error}")
endif()

# Right-arrow seek must land on the requested frame, not only the keyframe.
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
          SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
          BINK_SDL_TEST_SEEK=30
          "${PLAYER_EXE}" "$ENV{BINK_TEST_MEDIA}/logo_lucas.bik"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error
  TIMEOUT 120)
if(NOT result EQUAL 0 OR NOT output MATCHES "seeked frame 30")
  message(FATAL_ERROR "bink_sdl_player seek failed (${result}): ${output}${error}")
endif()
