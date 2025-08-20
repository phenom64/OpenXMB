if(CMAKE_CXX_COMPILER_ID STREQUAL Clang)
  function(target_use_i18n TARGET)
    cmake_parse_arguments(PARSE_ARGV 1 I18N "NODOMAIN;NODATE" "DOMAIN;COMMENT;BASEPATH;POT_FILE;POT_TARGET" "")

    if(DEFINED I18N_UNPARSED_ARGUMENTS)
      message(WARNING "Ignoring unexpected arguments ${I18N_UNPARSED_ARGUMENTS}")
    endif()

    target_link_libraries(${TARGET} PRIVATE i18n::i18n-lib)
    if(I18N_NODOMAIN)
      target_compile_options(${TARGET} PRIVATE "SHELL:-Xclang -plugin-arg-i18n -Xclang nodomain")
    endif()

    if(DEFINED I18N_DOMAIN)
      target_compile_options(${TARGET} PRIVATE "SHELL:-Xclang -plugin-arg-i18n -Xclang domain=${I18N_DOMAIN}")
    endif()

    if(DEFINED I18N_COMMENT)
      target_compile_options(${TARGET} PRIVATE "SHELL:-Xclang -plugin-arg-i18n -Xclang comment=${I18N_COMMENT}")
    endif()

    if(NOT DEFINED I18N_BASEPATH)
      set(I18N_BASEPATH "${CMAKE_SOURCE_DIR}")
    endif()
    target_compile_options(${TARGET} PRIVATE "SHELL:-Xclang -plugin-arg-i18n -Xclang basepath=${I18N_BASEPATH}")

    if(NOT DEFINED I18N_POT_FILE)
      set(I18N_POT_FILE "${TARGET}.pot")
    endif()

    if(NOT DEFINED I18N_POT_TARGET)
      set(I18N_POT_TARGET "${TARGET}_pot")
    endif()

    if(DEFINED I18N_NODATE)
      set(I18N_NODATE ${CMAKE_COMMAND} -E env "SOURCE_DATE_EPOCH=1500000000" "TZ=UTC")
    else()
      set(I18N_NODATE)
    endif()

    get_target_property(srcs ${TARGET} SOURCES)
    get_target_property(modules ${TARGET} CXX_MODULE_SET)
    set(srcs ${srcs} ${modules})
    list(REMOVE_DUPLICATES srcs)
    foreach(src ${srcs})
      cmake_path(IS_ABSOLUTE src is_absolute_path)
      if(is_absolute_path)
        cmake_path(RELATIVE_PATH src)
      endif()
      set_source_files_properties(${src} PROPERTIES OBJECT_OUTPUTS "$<LIST:TRANSFORM,$<FILTER:$<TARGET_OBJECTS:${TARGET}>,INCLUDE,${src}.o>,APPEND,.poc>")
    endforeach()
    add_custom_command(OUTPUT "${I18N_POT_FILE}"
      COMMAND ${I18N_NODATE}
      $<TARGET_FILE:i18n::i18n-merge-pot> "--package=${PROJECT_NAME}" "--version=${PROJECT_VERSION}" "--output=${I18N_POT_FILE}" "$<JOIN:$<TARGET_OBJECTS:${TARGET}>,.poc;>.poc"
      DEPENDS "$<JOIN:$<TARGET_OBJECTS:${TARGET}>,.poc;>.poc"
      COMMAND_EXPAND_LISTS)
    add_custom_target("${I18N_POT_TARGET}" ALL DEPENDS "${I18N_POT_FILE}")
    add_dependencies("${I18N_POT_TARGET}" "${TARGET}")
  endfunction()
else()

  # get_target_property(I18N_EXTRACT_PATH i18n::i18n-extract)
  if(NOT DEFINED I18N_CLANG_RESOURCE_DIR)
    execute_process(COMMAND clang -print-resource-dir
      OUTPUT_VARIABLE I18N_POTENTIAL_RESOURCE_DIR
      RESULT_VARIABLE I18N_HAS_KNOWN_RESOURCE_DIR
      ERROR_QUIET)
    if(I18N_HAS_KNOWN_RESOURCE_DIR EQUAL 0)
      string(STRIP "${I18N_POTENTIAL_RESOURCE_DIR}" I18N_POTENTIAL_RESOURCE_DIR)
    else()
      unset(I18N_POTENTIAL_RESOURCE_DIR)
    endif()
  endif()
  set(I18N_CLANG_RESOURCE_DIR "${I18N_POTENTIAL_RESOURCE_DIR}" CACHE PATH "Path to clang's resource directory. Leave empty to determine based on i18n++'s install location.")
  mark_as_advanced(I18N_CLANG_RESOURCE_DIR)

  function(target_use_i18n TARGET)
    cmake_parse_arguments(PARSE_ARGV 1 I18N "NODOMAIN;NODATE" "DOMAIN;COMMENT;BASEPATH;POT_FILE;POT_TARGET" "")

    set_target_properties("${TARGET}" PROPERTIES EXPORT_COMPILE_COMMANDS ON)

    if(DEFINED I18N_UNPARSED_ARGUMENTS)
      message(WARNING "Ignoring unexpected arguments ${I18N_UNPARSED_ARGUMENTS}")
    endif()

    set(I18N_EXTRACT_ARGS "-p=${PROJECT_BINARY_DIR}")

    target_link_libraries(${TARGET} PRIVATE i18n::i18n-lib)
    if(I18N_NODOMAIN)
      list(APPEND I18N_EXTRACT_ARGS "--nodomain")
    endif()

    if(DEFINED I18N_DOMAIN)
      list(APPEND I18N_EXTRACT_ARGS "--domain=${I18N_DOMAIN}")
    endif()

    if(DEFINED I18N_COMMENT)
      list(APPEND I18N_EXTRACT_ARGS "--comment=${I18N_COMMENT}")
    endif()

    if(NOT DEFINED I18N_BASEPATH)
      set(I18N_BASEPATH "${CMAKE_SOURCE_DIR}")
    endif()
    list(APPEND I18N_EXTRACT_ARGS "--basepath=${I18N_BASEPATH}")

    if(NOT DEFINED I18N_POT_FILE)
      set(I18N_POT_FILE "${TARGET}.pot")
    endif()

    if(NOT DEFINED I18N_POT_TARGET)
      set(I18N_POT_TARGET "${TARGET}_pot")
    endif()

    if(DEFINED I18N_NODATE)
      set(I18N_NODATE ${CMAKE_COMMAND} -E env "SOURCE_DATE_EPOCH=1500000000" "TZ=UTC")
    else()
      set(I18N_NODATE)
    endif()

    if(NOT I18N_CLANG_RESOURCE_DIR STREQUAL "")
      list(APPEND I18N_EXTRACT_ARGS "--extra-arg=-resource-dir=${I18N_CLANG_RESOURCE_DIR}")
    endif()

    get_target_property(srcs ${TARGET} SOURCES)
    foreach(src ${srcs})
      get_source_file_property(location "${src}" LOCATION)
      add_custom_command(TARGET "${TARGET}" POST_BUILD
        COMMAND i18n::i18n-extract ${I18N_EXTRACT_ARGS} "-o=$<FILTER:$<TARGET_OBJECTS:${TARGET}>,INCLUDE,${src}>.poc" "${location}"
        VERBATIM)
        # BYPRODUCTS "$<FILTER:$<TARGET_OBJECTS:${TARGET}>,INCLUDE,${src}>.poc" VERBATIM)
    endforeach()
    add_custom_command(OUTPUT "${I18N_POT_FILE}"
      COMMAND ${I18N_NODATE}
      $<TARGET_FILE:i18n::i18n-merge-pot> "--package=${PROJECT_NAME}" "--version=${PROJECT_VERSION}" "--output=${I18N_POT_FILE}" "$<JOIN:$<TARGET_OBJECTS:${TARGET}>,.poc;>.poc"
      # DEPENDS "$<JOIN:$<TARGET_OBJECTS:${TARGET}>,.poc;>.poc"
      COMMAND_EXPAND_LISTS)
    add_custom_target("${I18N_POT_TARGET}" ALL DEPENDS "${I18N_POT_FILE}")
    add_dependencies("${I18N_POT_TARGET}" "${TARGET}")
  endfunction()
endif()
