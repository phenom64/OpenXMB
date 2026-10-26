find_package(LibArchive REQUIRED)
find_package(ZLIB REQUIRED)

add_library(openxmb_themes STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src/themes/library.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/themes/p3t.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/themes/raf_runtime.cpp
)
add_library(OpenXMB::Themes ALIAS openxmb_themes)
target_compile_features(openxmb_themes PUBLIC cxx_std_23)
target_include_directories(openxmb_themes PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(openxmb_themes PUBLIC nlohmann_json::nlohmann_json LibArchive::LibArchive ZLIB::ZLIB)
set_target_properties(openxmb_themes PROPERTIES CXX_SCAN_FOR_MODULES OFF)

if(TARGET XMS)
  target_link_libraries(XMS PRIVATE OpenXMB::Themes)
  target_sources(XMS PUBLIC FILE_SET CXX_MODULES FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/programs/theme_manager.cppm)
endif()

if(OPENXMB_BUILD_NATIVE_TESTS)
  add_executable(openxmb_theme_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/themes/theme_library_tests.cpp)
  target_link_libraries(openxmb_theme_tests PRIVATE OpenXMB::Themes)
  set_target_properties(openxmb_theme_tests PROPERTIES CXX_SCAN_FOR_MODULES OFF)
  add_test(NAME openxmb_theme_tests COMMAND openxmb_theme_tests)
  add_executable(openxmb_raf_runtime_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/themes/raf_runtime_tests.cpp)
  target_link_libraries(openxmb_raf_runtime_tests PRIVATE OpenXMB::Themes)
  set_target_properties(openxmb_raf_runtime_tests PROPERTIES CXX_SCAN_FOR_MODULES OFF)
  add_test(NAME openxmb_raf_runtime_tests COMMAND openxmb_raf_runtime_tests)
  # Exercise the real configuration module without importing the application
  # shell or starting a renderer. Each test target owns its module output.
  add_executable(openxmb_config_style_tests
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/themes/config_style_module_tests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/config.cpp)
  target_sources(openxmb_config_style_tests PRIVATE FILE_SET CXX_MODULES FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/config.cppm
    ${CMAKE_CURRENT_SOURCE_DIR}/src/constants.cppm)
  target_compile_features(openxmb_config_style_tests PRIVATE cxx_std_23)
  target_include_directories(openxmb_config_style_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(openxmb_config_style_tests PRIVATE
    OpenXMB::ShellCore dreams::VulkanHppModule glm::glm spdlog::spdlog nlohmann_json::nlohmann_json)
  if(WIN32)
    target_link_libraries(openxmb_config_style_tests PRIVATE shell32)
  else()
    target_compile_options(openxmb_config_style_tests PRIVATE -pthread)
    target_link_options(openxmb_config_style_tests PRIVATE -pthread)
  endif()
  set_target_properties(openxmb_config_style_tests PROPERTIES CXX_SCAN_FOR_MODULES ON)
  add_test(NAME openxmb_config_style_tests COMMAND openxmb_config_style_tests)
endif()
