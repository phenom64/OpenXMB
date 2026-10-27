# Keep fallback artwork as a replaceable pack, independent of the renderer.
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/assets/shell/"
  DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/share/shell")
if(WIN32)
  file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/assets/shell/"
    DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/shell")
  install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets/shell/" DESTINATION shell)
else()
  install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets/shell/" DESTINATION "${OPENXMB_SHELL_INSTALL_DIR}")
endif()
