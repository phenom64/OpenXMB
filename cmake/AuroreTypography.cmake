# Extend the pinned dependency's CPU atlas with translated, shaped UI glyphs.
# Keep the change reproducible until the engine revision includes this API.
set(OPENXMB_AURORE_TYPOGRAPHY_PATCH "${CMAKE_CURRENT_LIST_DIR}/aurore-translated-glyphs.patch")
function(openxmb_configure_aurore_typography source)
  file(READ "${source}/src/components/font_renderer.cppm" font)
  if(font MATCHES "void set_preload_texts")
    return()
  endif()
  find_package(Git REQUIRED)
  execute_process(COMMAND "${GIT_EXECUTABLE}" apply --check "${OPENXMB_AURORE_TYPOGRAPHY_PATCH}"
    WORKING_DIRECTORY "${source}" RESULT_VARIABLE check ERROR_VARIABLE diagnostic)
  if(NOT check EQUAL 0)
    message(FATAL_ERROR "AuroreEngine typography patch no longer applies: ${diagnostic}")
  endif()
  execute_process(COMMAND "${GIT_EXECUTABLE}" apply "${OPENXMB_AURORE_TYPOGRAPHY_PATCH}"
    WORKING_DIRECTORY "${source}" COMMAND_ERROR_IS_FATAL ANY)
endfunction()
