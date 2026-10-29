# Keep fallback artwork as a replaceable pack, independent of the renderer.
file(GLOB_RECURSE _openxmb_shell_sources CONFIGURE_DEPENDS
  LIST_DIRECTORIES false RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/assets/shell"
  "${CMAKE_CURRENT_SOURCE_DIR}/assets/shell/*")
foreach(_openxmb_shell_source IN LISTS _openxmb_shell_sources)
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/assets/shell/${_openxmb_shell_source}"
    "${CMAKE_CURRENT_BINARY_DIR}/share/shell/${_openxmb_shell_source}" COPYONLY)
  if(WIN32)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/assets/shell/${_openxmb_shell_source}"
      "${CMAKE_CURRENT_BINARY_DIR}/shell/${_openxmb_shell_source}" COPYONLY)
  endif()
endforeach()
if(WIN32)
  install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets/shell/" DESTINATION shell)
else()
  install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets/shell/" DESTINATION "${OPENXMB_SHELL_INSTALL_DIR}")
endif()
