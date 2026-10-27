add_library(openxmb_music_core STATIC
  "${PROJECT_SOURCE_DIR}/src/music/queue.cpp"
  "${PROJECT_SOURCE_DIR}/src/music/player.cpp"
)
add_library(OpenXMB::MusicCore ALIAS openxmb_music_core)
target_compile_features(openxmb_music_core PUBLIC cxx_std_23)
set_target_properties(openxmb_music_core PROPERTIES CXX_SCAN_FOR_MODULES OFF)
target_include_directories(openxmb_music_core PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_include_directories(openxmb_music_core PRIVATE ${OPENXMB_FFMPEG_INCLUDE_DIRS})
target_link_directories(openxmb_music_core PUBLIC ${OPENXMB_FFMPEG_LIBRARY_DIRS})
target_link_libraries(openxmb_music_core PUBLIC SDL2::SDL2 SDL2_mixer::SDL2_mixer
  ${OPENXMB_FFMPEG_LIBRARIES})

if(OPENXMB_BUILD_NATIVE_TESTS)
  add_executable(openxmb_music_queue_tests "${PROJECT_SOURCE_DIR}/tests/music/queue_tests.cpp")
  target_link_libraries(openxmb_music_queue_tests PRIVATE OpenXMB::MusicCore)
  set_target_properties(openxmb_music_queue_tests PROPERTIES CXX_SCAN_FOR_MODULES OFF)
  add_test(NAME openxmb_music_queue_tests COMMAND openxmb_music_queue_tests)

  add_executable(openxmb_music_playback_tests "${PROJECT_SOURCE_DIR}/tests/music/playback_tests.cpp")
  target_link_libraries(openxmb_music_playback_tests PRIVATE OpenXMB::MusicCore)
  set_target_properties(openxmb_music_playback_tests PROPERTIES CXX_SCAN_FOR_MODULES OFF)
  add_test(NAME openxmb_music_playback_tests COMMAND openxmb_music_playback_tests)
  set_tests_properties(openxmb_music_playback_tests PROPERTIES ENVIRONMENT "SDL_AUDIODRIVER=dummy" TIMEOUT 20)

  find_program(OPENXMB_MUSIC_FFMPEG_EXECUTABLE ffmpeg)
  find_package(Python3 QUIET COMPONENTS Interpreter)
  if(OPENXMB_MUSIC_FFMPEG_EXECUTABLE AND Python3_Interpreter_FOUND)
    add_test(NAME openxmb_music_format_tests
      COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/music/format_fixtures.py"
        --ffmpeg "${OPENXMB_MUSIC_FFMPEG_EXECUTABLE}"
        --player "$<TARGET_FILE:openxmb_music_playback_tests>")
    set_tests_properties(openxmb_music_format_tests PROPERTIES ENVIRONMENT "SDL_AUDIODRIVER=dummy" TIMEOUT 40)
  endif()
endif()
