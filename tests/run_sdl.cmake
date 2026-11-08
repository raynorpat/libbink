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

# 30 frames of logo audio, dummy drivers. The queue must not block the run.
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
          SDL_VIDEODRIVER=dummy SDL_VIDEO_DRIVER=dummy
          SDL_AUDIODRIVER=dummy SDL_AUDIO_DRIVER=dummy
          BINK_SDL_TEST_FRAMES=30
          "${PLAYER_EXE}" "$ENV{BINK_TEST_MEDIA}/logo_lucas.bik"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error
  TIMEOUT 10)
set(audio_bytes "")
if(output MATCHES "audio bytes ([0-9]+)")
  set(audio_bytes "${CMAKE_MATCH_1}")
endif()
if(NOT result EQUAL 0 OR audio_bytes STREQUAL "" OR NOT audio_bytes GREATER 134400)
  message(FATAL_ERROR
    "bink_sdl_player 30-frame audio failed (${result}) bytes '${audio_bytes}': ${output}${error}")
endif()

# A missing audio driver still decodes, and the SDL error is printed.
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
          SDL_VIDEODRIVER=dummy SDL_VIDEO_DRIVER=dummy
          SDL_AUDIODRIVER=not-a-real-driver SDL_AUDIO_DRIVER=not-a-real-driver
          BINK_SDL_TEST_FRAMES=30
          "${PLAYER_EXE}" "$ENV{BINK_TEST_MEDIA}/logo_lucas.bik"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error
  TIMEOUT 10)
set(audio_bytes "")
if(output MATCHES "audio bytes ([0-9]+)")
  set(audio_bytes "${CMAKE_MATCH_1}")
endif()
if(NOT result EQUAL 0 OR audio_bytes STREQUAL "" OR NOT audio_bytes GREATER 134400
    OR NOT error MATCHES "not-a-real-driver")
  message(FATAL_ERROR
    "bink_sdl_player bad-driver audio failed (${result}) bytes '${audio_bytes}': ${output}${error}")
endif()

# No audio tracks: success, and the reported byte count is zero.
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
          SDL_VIDEODRIVER=dummy SDL_VIDEO_DRIVER=dummy
          SDL_AUDIODRIVER=dummy SDL_AUDIO_DRIVER=dummy
          BINK_SDL_TEST_FRAMES=1
          "${PLAYER_EXE}" "$ENV{BINK_TEST_MEDIA}/Robotica_1080_bink_4100k.bik"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error
  TIMEOUT 10)
if(NOT result EQUAL 0 OR NOT output MATCHES "audio bytes 0([^0-9]|$)")
  message(FATAL_ERROR
    "bink_sdl_player silent file failed (${result}): ${output}${error}")
endif()
