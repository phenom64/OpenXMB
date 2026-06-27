if(NOT DEFINED OPENXMB_SOURCE_DIR)
  message(FATAL_ERROR "OPENXMB_SOURCE_DIR is required")
endif()

get_filename_component(_source_root "${OPENXMB_SOURCE_DIR}" ABSOLUTE)
set(_roots
  "${_source_root}/assets"
  "${_source_root}/cmake"
  "${_source_root}/include"
  "${_source_root}/shaders"
  "${_source_root}/src"
  "${_source_root}/tests"
  "${_source_root}/tools"
)
set(_files
  "${_source_root}/CMakeLists.txt"
  "${_source_root}/CMakePresets.json"
)
foreach(_root IN LISTS _roots)
  if(IS_DIRECTORY "${_root}")
    file(GLOB_RECURSE _root_files LIST_DIRECTORIES false "${_root}/*")
    list(APPEND _files ${_root_files})
  endif()
endforeach()

# Construct prohibited strings so this checker does not match its own source.
string(CONCAT _branch_code "R" "SX" "[-_ ]?" "26")
string(CONCAT _legacy_identity "Syndromatic " "Engineering Bharat Britannia")
set(_violations)
foreach(_file IN LISTS _files)
  if(_file STREQUAL "${CMAKE_CURRENT_LIST_FILE}" OR IS_DIRECTORY "${_file}")
    continue()
  endif()
  if(NOT _file MATCHES "\\.(cmake|json|cpp|cxx|cc|hpp|h|cppm|py|vert|frag|comp|glsl|md|txt)$" AND
     NOT _file MATCHES "CMakeLists\\.txt$")
    continue()
  endif()
  file(READ "${_file}" _contents LIMIT 16777216)
  if(_contents MATCHES "${_branch_code}")
    list(APPEND _violations "branch codename leaked into ${_file}")
  endif()
  string(FIND "${_contents}" "${_legacy_identity}" _legacy_position)
  if(NOT _legacy_position EQUAL -1)
    list(APPEND _violations "obsolete startup identity remains in ${_file}")
  endif()
endforeach()

if(_violations)
  list(JOIN _violations "\n  - " _formatted)
  message(FATAL_ERROR "OpenXMB source invariants failed:\n  - ${_formatted}")
endif()

message(STATUS "OpenXMB source invariants passed")
