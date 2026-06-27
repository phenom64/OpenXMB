# cmakelists.txt for the OpenXMB project
# ™ & © 2025-2026 Syndromatic Ltd Britannia, Syndromatic Ltd Bharat. All rights reserved.
# Designed by Kavish Krishnakumar in Manchester.
# Licensed with the GPLv3, see LICENSE

cmake_minimum_required(VERSION 3.28)
cmake_policy(SET CMP0097 NEW)
cmake_policy(SET CMP0144 NEW)
if(POLICY CMP0207)
  cmake_policy(SET CMP0207 NEW)
endif()

# ANSI color codes for terminal output
string(ASCII 27 ESC)
set(COLOR_RESET   "${ESC}[0m")
set(COLOR_CYAN    "${ESC}[34m")
set(COLOR_RED     "${ESC}[31m")
set(COLOR_YELLOW  "${ESC}[33m")
set(COLOR_GREEN   "${ESC}[32m")

project(OpenXMB VERSION 0.1.0 LANGUAGES CXX)

# OpenXMB uses CTest for backend-neutral engine and product contract tests.
# Keep this independent of third-party BUILD_TESTING cache options below.
enable_testing()

message(STATUS "${COLOR_GREEN}Starting OpenXMB Builder...${COLOR_RESET}")
message(STATUS "${COLOR_YELLOW}™ & © 2025-2026 Syndromatic Ltd Britannia, Syndromatic Ltd Bharat. All rights reserved.${COLOR_RESET}")

if(WIN32 AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "OpenXMB's current C++23 module/#embed build is supported on Windows with LLVM/Clang + Ninja. Use the windows-vcpkg preset or set CMAKE_CXX_COMPILER=clang++.")
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(_openxmb_clang_scan_deps_names clang-scan-deps)
  if(CMAKE_CXX_COMPILER_VERSION)
    string(REGEX MATCH "^[0-9]+" _openxmb_clang_major "${CMAKE_CXX_COMPILER_VERSION}")
    if(_openxmb_clang_major)
      list(PREPEND _openxmb_clang_scan_deps_names "clang-scan-deps-${_openxmb_clang_major}")
    endif()
  endif()
  find_program(CLANG_SCAN_DEPS_EXECUTABLE NAMES ${_openxmb_clang_scan_deps_names})
  if(NOT CLANG_SCAN_DEPS_EXECUTABLE)
    message(FATAL_ERROR "clang-scan-deps is required for C++23 module dependency scanning with Clang.")
  endif()
  set(CMAKE_CXX_COMPILER_CLANG_SCAN_DEPS "${CLANG_SCAN_DEPS_EXECUTABLE}" CACHE FILEPATH "Path to clang-scan-deps" FORCE)
endif()

# Platform Detection
if(APPLE)
  set(PLATFORM_NAME "Mac OS X")
elseif(WIN32)
  set(PLATFORM_NAME "Windows")
elseif(UNIX)
  if(CMAKE_SYSTEM_NAME MATCHES "Linux")
    if(EXISTS "/etc/os-release")
      file(READ "/etc/os-release" OS_RELEASE_CONTENT)
      string(REGEX MATCH "ID=ubuntu" IS_UBUNTU "${OS_RELEASE_CONTENT}")
      if(IS_UBUNTU)
        set(PLATFORM_NAME "SynOS")
      else()
        set(PLATFORM_NAME "Linux")
      endif()
    else()
      set(PLATFORM_NAME "Linux")
    endif()
  else()
    set(PLATFORM_NAME "Unix (BSDs)")
  endif()
else()
  set(PLATFORM_NAME "Unknown")
endif()
message(STATUS "Running on ${COLOR_RED}${PLATFORM_NAME}${COLOR_RESET}")

# -----------------------------------------------------------------------------
# Build-type defaults and global optimizations
# -----------------------------------------------------------------------------
# If user did not provide a build type, default to Release for better perf.
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

# Optionally enable CPU-specific tuning (off by default for portability)
option(OPENXMB_ENABLE_NATIVE_OPTIMIZATIONS "Enable -march=native or /arch:..." OFF)
option(OPENXMB_ENABLE_FAST_MATH "Enable fast-math where safe" OFF)
option(OPENXMB_ENABLE_IPO "Enable interprocedural optimization/LTO" OFF)

set(OPENXMB_APPLE_PREFIXES "" CACHE STRING "Additional macOS dependency prefixes, separated by semicolons")
set(OPENXMB_APPLE_INCLUDE_DIRS "")
if(APPLE)
  set(_openxmb_apple_prefix_candidates ${OPENXMB_APPLE_PREFIXES})
  if(NOT _openxmb_apple_prefix_candidates)
    set(_openxmb_apple_prefix_candidates "/opt/homebrew" "/usr/local")
  endif()
  foreach(_prefix IN LISTS _openxmb_apple_prefix_candidates)
    if(IS_DIRECTORY "${_prefix}")
      list(APPEND CMAKE_PREFIX_PATH "${_prefix}")
      if(IS_DIRECTORY "${_prefix}/opt/ffmpeg")
        list(APPEND CMAKE_PREFIX_PATH "${_prefix}/opt/ffmpeg")
      endif()
      if(IS_DIRECTORY "${_prefix}/include")
        list(APPEND OPENXMB_APPLE_INCLUDE_DIRS "${_prefix}/include")
      endif()
    endif()
  endforeach()
endif()

# Interprocedural optimization (LTO/IPO) for Release builds if supported
include(CheckIPOSupported)
check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_ERROR)
if(OPENXMB_ENABLE_IPO AND IPO_SUPPORTED)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
elseif(NOT OPENXMB_ENABLE_IPO)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO OFF)
  message(STATUS "${COLOR_YELLOW}IPO/LTO disabled (enable OPENXMB_ENABLE_IPO after validating the active compiler/module combination).${COLOR_RESET}")
else()
  message(STATUS "${COLOR_YELLOW}IPO/LTO not supported: ${IPO_ERROR}${COLOR_RESET}")
endif()

if(NOT DEFINED BUILD_VERSION)
  set(BUILD_VERSION ${PROJECT_VERSION})
endif()
message(STATUS "${COLOR_CYAN}Building version ${BUILD_VERSION}${COLOR_RESET}")

# --- Feature Options ---
option(ENABLE_BROWSER "Enable CEF-based browser module" OFF)
option(ENABLE_VIDEO_PLAYER "Enable FFmpeg video player" ON)
option(ENABLE_DISC_MEDIA "Enable DVD/Blu-ray support" OFF)
option(ENABLE_LIBRETRO "Enable libretro core host" OFF)
option(OPENXMB_BUILD_NATIVE_TESTS "Build deterministic native OpenXMB contract tests" ON)
option(OPENXMB_IMPORT_XMB_WEB_COMPAT "Import the local-only xmb-web compatibility asset pack" OFF)
set(OPENXMB_XMB_WEB_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../xmb-web" CACHE PATH
  "Pinned local TheGammaSqueeze/xmb-web checkout used for the restricted compatibility pack")
set(HWACCEL "auto" CACHE STRING "Preferred hardware video decode backend")
set_property(CACHE HWACCEL PROPERTY STRINGS auto off vaapi d3d11 videotoolbox)

# Interface/FX debug overlays (fonts, UI visuals)
option(INTERFACE_FX_DEBUG "Enable interface/UI graphics debug overlays" OFF)

# Set color for ENABLE_BROWSER
if(ENABLE_BROWSER)
    set(BROWSER_COLOR "${COLOR_GREEN}")
else()
    set(BROWSER_COLOR "${COLOR_RED}")
endif()

# Set color for ENABLE_VIDEO_PLAYER
if(ENABLE_VIDEO_PLAYER)
    set(VIDEO_PLAYER_COLOR "${COLOR_GREEN}")
else()
    set(VIDEO_PLAYER_COLOR "${COLOR_RED}")
endif()

# Set color for ENABLE_DISC_MEDIA
if(ENABLE_DISC_MEDIA)
    set(DISC_MEDIA_COLOR "${COLOR_GREEN}")
else()
    set(DISC_MEDIA_COLOR "${COLOR_RED}")
endif()

# Set color for ENABLE_LIBRETRO
if(ENABLE_LIBRETRO)
    set(LIBRETRO_COLOR "${COLOR_GREEN}")
else()
    set(LIBRETRO_COLOR "${COLOR_RED}")
endif()

# Set color for INTERFACE_FX_DEBUG
if(INTERFACE_FX_DEBUG)
    set(DEBUG_COLOR "${COLOR_GREEN}")
else()
    set(DEBUG_COLOR "${COLOR_RED}")
endif()

message(STATUS "${COLOR_YELLOW}Enabled features: ${COLOR_RESET}BROWSER=${BROWSER_COLOR}${ENABLE_BROWSER}${COLOR_RESET} VIDEO=${VIDEO_PLAYER_COLOR}${ENABLE_VIDEO_PLAYER}${COLOR_RESET} DISC=${DISC_MEDIA_COLOR}${ENABLE_DISC_MEDIA}${COLOR_RESET} LIBRETRO=${LIBRETRO_COLOR}${ENABLE_LIBRETRO}${COLOR_RESET} HWACCEL=${HWACCEL}${COLOR_RESET} DEBUG=${DEBUG_COLOR}${INTERFACE_FX_DEBUG}${COLOR_RESET}")

set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT WIN32)
  add_compile_options(-fPIC)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wno-nullability-completeness)
endif()

# Propagate HWACCEL preference for conditional code
add_compile_definitions(OPENXMB_HWACCEL_STR="${HWACCEL}")

find_package(glm REQUIRED)
find_package(Freetype REQUIRED)
find_package(Gettext QUIET)
find_package(Intl REQUIRED)
find_package(fmt REQUIRED)
find_package(PkgConfig QUIET)
find_package(FFMPEG QUIET)
if(FFMPEG_FOUND)
  set(OPENXMB_FFMPEG_INCLUDE_DIRS ${FFMPEG_INCLUDE_DIRS})
  set(OPENXMB_FFMPEG_LIBRARY_DIRS ${FFMPEG_LIBRARY_DIRS})
  set(OPENXMB_FFMPEG_LIBRARIES ${FFMPEG_LIBRARIES})
elseif(PkgConfig_FOUND)
  pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavformat libavutil libswscale libswresample)
  set(OPENXMB_FFMPEG_INCLUDE_DIRS ${FFMPEG_INCLUDE_DIRS})
  set(OPENXMB_FFMPEG_LIBRARY_DIRS ${FFMPEG_LIBRARY_DIRS})
  set(OPENXMB_FFMPEG_LIBRARIES ${FFMPEG_LIBRARIES})
else()
  message(FATAL_ERROR "FFmpeg was not found. Install FFmpeg development libraries or use vcpkg/pkg-config.")
endif()

set(SDL_BUILDING_LIBRARY ON CACHE BOOL "" FORCE)
find_package(SDL2 REQUIRED)
find_package(SDL2_image REQUIRED)
find_package(SDL2_mixer REQUIRED)

function(openxmb_append_sdl2_parent_include target_name)
  if(NOT TARGET ${target_name})
    return()
  endif()
  get_target_property(_openxmb_sdl2_target_includes ${target_name} INTERFACE_INCLUDE_DIRECTORIES)
  if(NOT _openxmb_sdl2_target_includes)
    return()
  endif()
  foreach(_sdl2_include_dir IN LISTS _openxmb_sdl2_target_includes)
    if(IS_ABSOLUTE "${_sdl2_include_dir}")
      get_filename_component(_sdl2_include_parent "${_sdl2_include_dir}" DIRECTORY)
      if(IS_DIRECTORY "${_sdl2_include_parent}/SDL2")
        set_property(TARGET ${target_name} APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${_sdl2_include_parent}")
      endif()
    endif()
  endforeach()
endfunction()

openxmb_append_sdl2_parent_include(SDL2::SDL2)
openxmb_append_sdl2_parent_include(SDL2_image::SDL2_image)
openxmb_append_sdl2_parent_include(SDL2_mixer::SDL2_mixer)

include(FetchContent)

if(APPLE)
  find_package(Vulkan REQUIRED)
endif()

FetchContent_Declare(i18n++
  GIT_REPOSITORY https://github.com/JnCrMx/i18n-cpp.git
  GIT_TAG        trunk    #trunk = master branch
)

# We are using our own stable, maintained fork of the rendering engine.
# Allow overriding the engine source with a local path for development.
set(DREAMS_BUILD_WITH_POSIX_THREADS ON CACHE BOOL "" FORCE)
set(DREAMS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(DREAMS_BUILD_V2_TESTS ON CACHE BOOL "Build Aurore v2 contract tests during OpenXMB integration" FORCE)
set(DREAMS_FIND_PACKAGES ON CACHE BOOL "Allow AuroreEngine to create required package targets" FORCE)

set(DREAMRENDER_LOCAL "" CACHE PATH "Path to local AuroreEngine checkout (optional)")
if(DREAMRENDER_LOCAL)
  get_filename_component(_dreamrender_local "${DREAMRENDER_LOCAL}" ABSOLUTE)
  if(NOT EXISTS "${_dreamrender_local}/CMakeLists.txt")
    message(FATAL_ERROR "DREAMRENDER_LOCAL does not point to an AuroreEngine checkout: ${_dreamrender_local}")
  endif()
  message(STATUS "${COLOR_YELLOW}Using local AuroreEngine at ${COLOR_CYAN}${_dreamrender_local}${COLOR_RESET}")
  add_subdirectory("${_dreamrender_local}" "${CMAKE_CURRENT_BINARY_DIR}/_deps/dreamrender-build")
else()
  FetchContent_Declare(dreamrender
    GIT_REPOSITORY https://github.com/phenom64/AuroreEngine.git
    GIT_TAG        a55db74aab697a7642e439ebf0fa9222f40683ff
    OVERRIDE_FIND_PACKAGE
  )
  FetchContent_GetProperties(dreamrender)
  if(NOT dreamrender_POPULATED)
    FetchContent_Populate(dreamrender)
    add_subdirectory("${dreamrender_SOURCE_DIR}" "${dreamrender_BINARY_DIR}")
  endif()
endif()

if(TARGET SDL2::SDL2 AND TARGET dreams_sdl2Module)
  get_target_property(_openxmb_sdl2_includes SDL2::SDL2 INTERFACE_INCLUDE_DIRECTORIES)
  foreach(_sdl2_include_dir IN LISTS _openxmb_sdl2_includes)
    get_filename_component(_sdl2_include_parent "${_sdl2_include_dir}" DIRECTORY)
    if(EXISTS "${_sdl2_include_parent}/SDL2/SDL.h")
      target_include_directories(dreams_sdl2Module PUBLIC "${_sdl2_include_parent}")
    endif()
  endforeach()
endif()
FetchContent_Declare(argparse
  GIT_REPOSITORY https://github.com/p-ranav/argparse.git
  GIT_TAG        v3.1
)

FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.12.0
)


# For i18n++, we disable the optional tools (plugin, merge tool) and tests
# by setting these cache variables before FetchContent makes the project available.
# prevents it from needing dependencies like Clang and fmt blah blah etc.
set(BUILD_PLUGIN OFF CACHE BOOL "Disable i18n++ Clang plugin" FORCE)
set(BUILD_MERGE OFF CACHE BOOL "Disable i18n++ merge tool" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "Disable i18n++ tests" FORCE)

# make both dependencies available. CMake will use the options we just set
# when it configures i18n++.
FetchContent_MakeAvailable(i18n++ argparse nlohmann_json)

add_library(i18nModule STATIC)
target_sources(i18nModule PUBLIC FILE_SET CXX_MODULES FILES third_party/i18n.cppm)
target_compile_features(i18nModule PUBLIC cxx_std_20)
target_link_libraries(i18nModule PUBLIC i18n::i18n-lib)
if(NOT WIN32)
  target_compile_options(i18nModule PRIVATE -pthread)
endif()

add_library(argparseModule STATIC)
target_sources(argparseModule PUBLIC FILE_SET CXX_MODULES BASE_DIRS ${argparse_SOURCE_DIR}/module FILES ${argparse_SOURCE_DIR}/module/argparse.cppm)
target_compile_features(argparseModule PUBLIC cxx_std_23)
target_link_libraries(argparseModule PUBLIC argparse::argparse)
if(NOT WIN32)
  target_compile_options(argparseModule PRIVATE -pthread)
endif()

# Backend-neutral OpenXMB model and presentation substrate. Keeping this out of
# the module/Vulkan graph makes the asset parser, clocks, and layout testable by
# Clang, GCC, and MSVC in isolation.
add_library(openxmb_shell_core STATIC
  src/xmb/model/wave_data.cpp
  src/xmb/presentation/background.cpp
  src/xmb/presentation/boot_timeline.cpp
  src/xmb/presentation/layout.cpp
  src/xmb/presentation/status_bar.cpp
)
add_library(OpenXMB::ShellCore ALIAS openxmb_shell_core)
target_compile_features(openxmb_shell_core PUBLIC cxx_std_23)
target_include_directories(openxmb_shell_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
set_target_properties(openxmb_shell_core PROPERTIES CXX_SCAN_FOR_MODULES OFF)

add_library(openxmb_media_core STATIC
  src/media/catalog.cpp
  src/media/clock.cpp
  src/media/player_state.cpp
)
add_library(OpenXMB::MediaCore ALIAS openxmb_media_core)
target_compile_features(openxmb_media_core PUBLIC cxx_std_23)
target_include_directories(openxmb_media_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
set_target_properties(openxmb_media_core PROPERTIES CXX_SCAN_FOR_MODULES OFF)

add_library(openxmb_catalog_core STATIC
  src/xmb/model/catalog.cpp
  src/xmb/model/navigation_state.cpp
  src/xmb/model/settings_actions.cpp
  src/xmb/presentation/settings_scene.cpp
)
add_library(OpenXMB::CatalogCore ALIAS openxmb_catalog_core)
target_compile_features(openxmb_catalog_core PUBLIC cxx_std_23)
target_include_directories(openxmb_catalog_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_link_libraries(openxmb_catalog_core PUBLIC nlohmann_json::nlohmann_json)
set_target_properties(openxmb_catalog_core PROPERTIES CXX_SCAN_FOR_MODULES OFF)

add_library(openxmb_settings_core STATIC
  src/settings/domain.cpp
)
add_library(OpenXMB::SettingsCore ALIAS openxmb_settings_core)
target_compile_features(openxmb_settings_core PUBLIC cxx_std_23)
target_include_directories(openxmb_settings_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
set_target_properties(openxmb_settings_core PROPERTIES CXX_SCAN_FOR_MODULES OFF)

add_library(openxmb_dialogs_core STATIC
  src/dialogs/model.cpp
  src/dialogs/state_machine.cpp
  src/dialogs/recovery.cpp
)
add_library(OpenXMB::DialogsCore ALIAS openxmb_dialogs_core)
target_compile_features(openxmb_dialogs_core PUBLIC cxx_std_23)
target_include_directories(openxmb_dialogs_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
set_target_properties(openxmb_dialogs_core PROPERTIES CXX_SCAN_FOR_MODULES OFF)

add_library(openxmb_localization_core STATIC
  src/localization/catalog.cpp
  src/localization/locale.cpp
  src/localization/message.cpp
)
add_library(OpenXMB::LocalizationCore ALIAS openxmb_localization_core)
target_compile_features(openxmb_localization_core PUBLIC cxx_std_23)
target_include_directories(openxmb_localization_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
set_target_properties(openxmb_localization_core PROPERTIES CXX_SCAN_FOR_MODULES OFF)

if(OPENXMB_IMPORT_XMB_WEB_COMPAT)
  find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
  if(NOT EXISTS "${OPENXMB_XMB_WEB_SOURCE}/wave_geo.bin")
    message(FATAL_ERROR
      "OPENXMB_IMPORT_XMB_WEB_COMPAT requires the pinned xmb-web checkout at "
      "${OPENXMB_XMB_WEB_SOURCE}. Disable the option for the clean Classic fallback.")
  endif()

  set(OPENXMB_COMPAT_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/shell/compat/xmb-ui-compat")
  set(_openxmb_compat_manifest
    "${OPENXMB_COMPAT_DIR}/manifest.json")
  set(_openxmb_compat_family_inputs)
  foreach(_openxmb_compat_family IN ITEMS
      assets backgrounds dialogs icons images normalmaps photos video)
    file(GLOB_RECURSE _openxmb_compat_family_files
      CONFIGURE_DEPENDS
      LIST_DIRECTORIES FALSE
      "${OPENXMB_XMB_WEB_SOURCE}/${_openxmb_compat_family}/*")
    list(APPEND _openxmb_compat_family_inputs
      ${_openxmb_compat_family_files})
  endforeach()
  add_custom_command(
    OUTPUT "${_openxmb_compat_manifest}"
    BYPRODUCTS
      "${OPENXMB_COMPAT_DIR}/data/month-gradient.json"
      "${OPENXMB_COMPAT_DIR}/normalmaps/nmap_000.png"
      "${OPENXMB_COMPAT_DIR}/wave/wave_geo.bin"
      "${OPENXMB_COMPAT_DIR}/wave/wave_seq2.bin"
      "${OPENXMB_COMPAT_DIR}/wave/wave_seq2.json"
      "${OPENXMB_COMPAT_DIR}/wave/wave_boot.bin"
      "${OPENXMB_COMPAT_DIR}/wave/wave_boot.json"
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tools/xmb/import_xmb_web.py"
      --source "${OPENXMB_XMB_WEB_SOURCE}"
      --output "${OPENXMB_COMPAT_DIR}"
    DEPENDS
      tools/xmb/import_xmb_web.py
      assets/xmb/manifests/xmb-web-source.json
      "${OPENXMB_XMB_WEB_SOURCE}/wave_geo.bin"
      "${OPENXMB_XMB_WEB_SOURCE}/wave_seq2.bin"
      "${OPENXMB_XMB_WEB_SOURCE}/wave_seq2.json"
      "${OPENXMB_XMB_WEB_SOURCE}/wave_boot.bin"
      "${OPENXMB_XMB_WEB_SOURCE}/wave_boot.json"
      "${OPENXMB_XMB_WEB_SOURCE}/normalmaps/nmap_000.png"
      "${OPENXMB_XMB_WEB_SOURCE}/index.html"
      ${_openxmb_compat_family_inputs}
    COMMENT "Verifying and importing the local-only OpenXMB xmb-web compatibility pack"
    VERBATIM
  )
  add_custom_target(openxmb_compat_assets ALL
    DEPENDS "${_openxmb_compat_manifest}")
endif()

set(XMS_SOURCES
  src/app/shell.cpp
  src/app/components/choice_overlay.cpp
  src/app/components/main_menu.cpp
  src/app/components/message_overlay.cpp
  src/app/components/news_display.cpp
  src/app/components/progress_overlay.cpp
  src/app/components/startup_overlay.cpp
  src/app/layers/blur_layer.cpp
  src/menu/applications_menu.cpp
  src/menu/files_menu.cpp
  src/menu/settings_menu.cpp
  src/menu/users_menu.cpp
  src/render/shaders.cpp
  src/config.cpp
  src/main.cpp
  src/utils.cpp
  src/programs.cpp
)
set(XMS_MODULE_SOURCES
  src/app/module.cppm
  src/app/shell.cppm
  src/app/component.cppm
  src/app/components/choice_overlay.cppm
  src/app/components/main_menu.cppm
  src/app/components/message_overlay.cppm
  src/app/components/news_display.cppm
  src/app/components/progress_overlay.cppm
  src/app/components/startup_overlay.cppm
  src/app/layers/blur_layer.cppm
  src/config.cppm
  src/constants.cppm
  src/menu/applications_menu.cppm
  src/menu/base.cppm
  src/menu/files_menu.cppm
  src/menu/settings_menu.cppm
  src/menu/users_menu.cppm
  src/menu/utils.cppm
  src/programs.cppm
  src/programs/base_viewer.cppm
  src/programs/image_viewer.cppm
  src/programs/text_viewer.cppm
  src/render/module.cppm
  src/render/shaders.cppm
  src/render/components/wave_renderer.cppm
  src/render/components/original_renderer.cppm
  src/render/components/original_particles.cppm
  src/xmb/presentation/root_scene.cppm
  src/xmb/renderer/captured_wave_renderer.cppm
  src/xmb/renderer/monthly_background_renderer.cppm
  src/xmb/renderer/settings_scene_renderer.cppm
  src/xmb/renderer/status_bar_renderer.cppm
  src/utils.cppm
)
list(APPEND XMS_MODULE_SOURCES src/debug.cppm)

if(ENABLE_VIDEO_PLAYER)
  list(APPEND XMS_MODULE_SOURCES src/programs/video_player.cppm)
  add_compile_definitions(OPENXMB_ENABLE_VIDEO_PLAYER=1)
else()
  add_compile_definitions(OPENXMB_ENABLE_VIDEO_PLAYER=0)
endif()

if(ENABLE_BROWSER)
  list(APPEND XMS_MODULE_SOURCES src/modules/browser.cppm)
  add_compile_definitions(OPENXMB_ENABLE_BROWSER=1)
else()
  add_compile_definitions(OPENXMB_ENABLE_BROWSER=0)
endif()

if(ENABLE_LIBRETRO)
  list(APPEND XMS_MODULE_SOURCES src/modules/libretro.cppm)
  add_compile_definitions(OPENXMB_ENABLE_LIBRETRO=1)
else()
  add_compile_definitions(OPENXMB_ENABLE_LIBRETRO=0)
endif()

if(ENABLE_DISC_MEDIA)
  list(APPEND XMS_MODULE_SOURCES src/modules/disc.cppm)
  add_compile_definitions(OPENXMB_ENABLE_DISC_MEDIA=1)
else()
  add_compile_definitions(OPENXMB_ENABLE_DISC_MEDIA=0)
endif()
set(XMS_SHADERS
  shaders/blur.comp
  shaders/downsample.comp
  shaders/upsample.comp
  shaders/wave.vert
  shaders/wave.frag
  shaders/original.vert
  shaders/original.frag
  shaders/original_particles.vert
  shaders/original_particles.frag
  shaders/captured_wave.vert
  shaders/captured_wave.frag
  shaders/monthly_background.vert
  shaders/monthly_background.frag
  shaders/yuv420p_decode.comp
)

add_executable(XMS ${XMS_SOURCES})
if(TARGET openxmb_compat_assets)
  add_dependencies(XMS openxmb_compat_assets)
endif()
target_sources(XMS PUBLIC
  FILE_SET CXX_MODULES
  BASE_DIRS src
  FILES ${XMS_MODULE_SOURCES}
)

# Extra optimization flags and definitions (safe-by-default)
if(MSVC)
  target_compile_options(XMS PRIVATE
    $<$<CONFIG:Release>:/O2 /Oi /GL>
    $<$<CONFIG:RelWithDebInfo>:/O2 /Oi>
  )
  # Link-time code generation in Release
  target_link_options(XMS PRIVATE $<$<CONFIG:Release>:/LTCG>)
else()
  target_compile_options(XMS PRIVATE
    $<$<CONFIG:Release>:-O2 -DNDEBUG>
    $<$<CONFIG:RelWithDebInfo>:-O2>
    $<$<AND:$<CONFIG:Release>,$<BOOL:${OPENXMB_ENABLE_FAST_MATH}>>:-ffast-math>
    $<$<BOOL:${OPENXMB_ENABLE_NATIVE_OPTIMIZATIONS}>:-march=native>
  )
  # If IPO is not supported, attempt LTO flags in Release
  if(NOT IPO_SUPPORTED)
    target_link_options(XMS PRIVATE $<$<CONFIG:Release>:-flto>)
  endif()
endif()

# Reduce logging overhead in Release by compiling out debug/trace
target_compile_definitions(XMS PRIVATE
  $<$<CONFIG:Release>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO>
)

if(NOT WIN32)
  target_compile_options(XMS PRIVATE -pthread)
endif()

target_include_directories(XMS PRIVATE include/ ${OPENXMB_FFMPEG_INCLUDE_DIRS} ${nlohmann_json_SOURCE_DIR}/include)
if(APPLE)
  target_include_directories(XMS PRIVATE ${Vulkan_INCLUDE_DIRS} ${OPENXMB_APPLE_INCLUDE_DIRS})
endif()

target_compile_options(XMS PRIVATE --embed-dir=${CMAKE_CURRENT_BINARY_DIR}/shaders)
target_compile_options(XMS PRIVATE --embed-dir=${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(XMS PRIVATE dreams::dreamrender)
target_link_libraries(XMS PRIVATE OpenXMB::ShellCore)
target_link_libraries(XMS PRIVATE OpenXMB::MediaCore)
target_link_libraries(XMS PRIVATE OpenXMB::CatalogCore)
target_link_libraries(XMS PRIVATE OpenXMB::SettingsCore)
target_link_libraries(XMS PRIVATE OpenXMB::DialogsCore)
target_link_libraries(XMS PRIVATE OpenXMB::LocalizationCore)
target_link_libraries(XMS PRIVATE dreams::sdl2Module)
target_link_libraries(XMS PRIVATE dreams::glmModule)
target_link_libraries(XMS PRIVATE dreams::VulkanHppModule)
target_link_libraries(XMS PRIVATE dreams::vmaModule)
target_link_libraries(XMS PRIVATE dreams::spdlogModule)
target_link_libraries(XMS PRIVATE Freetype::Freetype)
target_link_libraries(XMS PRIVATE i18nModule)
target_link_libraries(XMS PRIVATE argparseModule)
target_link_libraries(XMS PRIVATE nlohmann_json::nlohmann_json)

target_link_directories(XMS PRIVATE ${OPENXMB_FFMPEG_LIBRARY_DIRS})

target_link_libraries(XMS PRIVATE ${OPENXMB_FFMPEG_LIBRARIES})
target_link_libraries(XMS PRIVATE i18n::i18n-lib)
if(TARGET Intl::Intl)
  target_link_libraries(XMS PRIVATE Intl::Intl)
endif()
target_link_libraries(XMS PRIVATE fmt::fmt)

if(WIN32 AND DEFINED _VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(_openxmb_vcpkg_runtime_dir
      "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/bin")
  else()
    set(_openxmb_vcpkg_runtime_dir
      "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
  endif()
  add_custom_target(openxmb_stage_windows_runtime ALL
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
      "${_openxmb_vcpkg_runtime_dir}"
      "$<TARGET_FILE_DIR:XMS>"
    DEPENDS XMS
    COMMENT "Staging vcpkg runtime libraries beside OpenXMB"
    VERBATIM)
endif()

if(OPENXMB_BUILD_NATIVE_TESTS)
  function(openxmb_configure_native_test target_name)
    target_compile_features(${target_name} PRIVATE cxx_std_23)
    target_include_directories(${target_name} PRIVATE include)
    if(MSVC)
      target_compile_options(${target_name} PRIVATE /W4 /WX)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
      target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
  endfunction()

  add_test(NAME openxmb_source_invariants
    COMMAND "${CMAKE_COMMAND}"
      -DOPENXMB_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckSourceInvariants.cmake)

  add_executable(openxmb_layout_timeline_tests
    tests/xmb/layout_timeline_tests.cpp
  )
  target_sources(openxmb_layout_timeline_tests PRIVATE
    FILE_SET CXX_MODULES
    BASE_DIRS src
    FILES src/xmb/presentation/root_scene.cppm
  )
  target_link_libraries(openxmb_layout_timeline_tests PRIVATE OpenXMB::ShellCore)
  openxmb_configure_native_test(openxmb_layout_timeline_tests)
  add_test(NAME openxmb_layout_timeline_tests
    COMMAND openxmb_layout_timeline_tests)

  add_executable(openxmb_wave_data_tests
    tests/xmb/wave_data_tests.cpp
  )
  target_link_libraries(openxmb_wave_data_tests PRIVATE OpenXMB::ShellCore)
  set_target_properties(openxmb_wave_data_tests PROPERTIES CXX_SCAN_FOR_MODULES OFF)
  openxmb_configure_native_test(openxmb_wave_data_tests)
  add_test(NAME openxmb_wave_data_tests
    COMMAND openxmb_wave_data_tests
      "${CMAKE_CURRENT_SOURCE_DIR}/../xmb-web")

  add_executable(openxmb_identity_tests
    tests/xmb/identity_tests.cpp
  )
  set_target_properties(openxmb_identity_tests PROPERTIES CXX_SCAN_FOR_MODULES OFF)
  openxmb_configure_native_test(openxmb_identity_tests)
  add_test(NAME openxmb_identity_tests
    COMMAND openxmb_identity_tests
      "${CMAKE_CURRENT_SOURCE_DIR}")

  add_executable(openxmb_background_tests
    tests/xmb/background_tests.cpp
  )
  target_link_libraries(openxmb_background_tests PRIVATE OpenXMB::ShellCore)
  set_target_properties(openxmb_background_tests PROPERTIES CXX_SCAN_FOR_MODULES OFF)
  openxmb_configure_native_test(openxmb_background_tests)
  add_test(NAME openxmb_background_tests
    COMMAND openxmb_background_tests)

  add_executable(openxmb_status_bar_tests
    tests/xmb/status_bar_tests.cpp
  )
  target_link_libraries(openxmb_status_bar_tests PRIVATE OpenXMB::ShellCore)
  set_target_properties(openxmb_status_bar_tests PROPERTIES CXX_SCAN_FOR_MODULES OFF)
  openxmb_configure_native_test(openxmb_status_bar_tests)
  add_test(NAME openxmb_status_bar_tests
    COMMAND openxmb_status_bar_tests)

  add_executable(openxmb_live_crossbar_contract_tests
    tests/xmb/live_crossbar_contract_tests.cpp
  )
  target_sources(openxmb_live_crossbar_contract_tests PRIVATE
    FILE_SET CXX_MODULES
    BASE_DIRS src
    FILES src/xmb/presentation/root_scene.cppm
  )
  target_link_libraries(openxmb_live_crossbar_contract_tests PRIVATE OpenXMB::ShellCore)
  openxmb_configure_native_test(openxmb_live_crossbar_contract_tests)
  add_test(NAME openxmb_live_crossbar_contract_tests
    COMMAND openxmb_live_crossbar_contract_tests)

  add_executable(openxmb_media_foundation_tests
    tests/media/media_foundation_tests.cpp
  )
  target_link_libraries(openxmb_media_foundation_tests PRIVATE OpenXMB::MediaCore)
  set_target_properties(openxmb_media_foundation_tests PROPERTIES CXX_SCAN_FOR_MODULES OFF)
  openxmb_configure_native_test(openxmb_media_foundation_tests)
  add_test(NAME openxmb_media_foundation_tests
    COMMAND openxmb_media_foundation_tests)

  add_executable(openxmb_catalog_state_tests
    tests/xmb/catalog_state_tests.cpp
  )
  target_link_libraries(openxmb_catalog_state_tests PRIVATE OpenXMB::CatalogCore)
  set_target_properties(openxmb_catalog_state_tests PROPERTIES CXX_SCAN_FOR_MODULES OFF)
  openxmb_configure_native_test(openxmb_catalog_state_tests)
  add_test(NAME openxmb_catalog_state_tests
    COMMAND openxmb_catalog_state_tests)

  add_executable(openxmb_settings_domain_tests
    tests/settings/settings_domain_tests.cpp
  )
  target_link_libraries(openxmb_settings_domain_tests PRIVATE OpenXMB::SettingsCore)
  set_target_properties(openxmb_settings_domain_tests PROPERTIES CXX_SCAN_FOR_MODULES OFF)
  openxmb_configure_native_test(openxmb_settings_domain_tests)
  add_test(NAME openxmb_settings_domain_tests
    COMMAND openxmb_settings_domain_tests)

  add_executable(openxmb_dialog_wizard_state_tests
    tests/dialogs/dialog_wizard_state_tests.cpp
  )
  target_link_libraries(openxmb_dialog_wizard_state_tests PRIVATE OpenXMB::DialogsCore)
  set_target_properties(openxmb_dialog_wizard_state_tests PROPERTIES CXX_SCAN_FOR_MODULES OFF)
  openxmb_configure_native_test(openxmb_dialog_wizard_state_tests)
  add_test(NAME openxmb_dialog_wizard_state_tests
    COMMAND openxmb_dialog_wizard_state_tests)

  add_executable(openxmb_settings_scene_tests
    tests/xmb/settings_scene_tests.cpp
  )
  target_link_libraries(openxmb_settings_scene_tests PRIVATE OpenXMB::CatalogCore)
  set_target_properties(openxmb_settings_scene_tests PROPERTIES CXX_SCAN_FOR_MODULES OFF)
  openxmb_configure_native_test(openxmb_settings_scene_tests)
  add_test(NAME openxmb_settings_scene_tests
    COMMAND openxmb_settings_scene_tests)

  add_executable(openxmb_settings_actions_tests
    tests/xmb/settings_actions_tests.cpp
  )
  target_link_libraries(openxmb_settings_actions_tests PRIVATE OpenXMB::CatalogCore)
  set_target_properties(openxmb_settings_actions_tests PROPERTIES CXX_SCAN_FOR_MODULES OFF)
  openxmb_configure_native_test(openxmb_settings_actions_tests)
  add_test(NAME openxmb_settings_actions_tests
    COMMAND openxmb_settings_actions_tests)

  add_executable(openxmb_localization_core_tests
    tests/localization/localization_core_tests.cpp
  )
  target_link_libraries(openxmb_localization_core_tests PRIVATE OpenXMB::LocalizationCore)
  set_target_properties(openxmb_localization_core_tests PROPERTIES CXX_SCAN_FOR_MODULES OFF)
  openxmb_configure_native_test(openxmb_localization_core_tests)
  add_test(NAME openxmb_localization_core_tests
    COMMAND openxmb_localization_core_tests)
endif()

if(APPLE)
  target_link_libraries(XMS PRIVATE ${Vulkan_LIBRARIES})
  find_library(ICONV_LIBRARY iconv)
  if(ICONV_LIBRARY)
    target_link_libraries(XMS PRIVATE ${ICONV_LIBRARY})
  endif()
  target_link_libraries(XMS PRIVATE "-framework CoreVideo -framework Cocoa -framework IOKit -framework Carbon -framework ForceFeedback -framework Metal")
endif()

if(UNIX AND NOT APPLE)
  target_link_libraries(XMS PRIVATE dl X11)
endif()

foreach(shader ${XMS_SHADERS})
  dreams_add_shader(XMS ${shader})
endforeach()
target_include_directories(XMS PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/shaders)

# Wire the interface/FX debug option to a preprocessor define usable in code
if(INTERFACE_FX_DEBUG)
  target_compile_definitions(XMS PRIVATE IFXDEBUG=1)
endif()

# =============================================================================
# --- Installation ---
# =============================================================================
include(GNUInstallDirs)

# We rename the final executable to "XMS.bin". A launcher script named "XMS"
# will be created to set up the environment and run this binary.
set_target_properties(XMS PROPERTIES OUTPUT_NAME "XMS.bin")

set(OPENXMB_CONFIG_INSTALL_DIR "${CMAKE_INSTALL_DATADIR}/OpenXMB")
set(OPENXMB_SHELL_INSTALL_DIR "${CMAKE_INSTALL_DATADIR}/shell")
set(OPENXMB_ICON_ASSET_MAP
  photo300.png    icon_category_photo.png
  music300.png    icon_category_music.png
  movie300.png    icon_category_video.png
  movie300.png    icon_category_tv.png
  settings300.png icon_category_settings.png
  settings300.png icon_settings_personalization.png
  settings300.png icon_settings_video.png
  settings300.png icon_settings_input.png
  settings300.png icon_settings_debug.png
  settings300.png icon_settings_toggle-background-blur.png
  settings300.png icon_settings-toggle-ingame-mode.png
  settings300.png icon_settings_update.png
  settings300.png icon_settings_reset.png
  settings300.png icon_bug.png
  settings300.png icon_licenses.png
  settings300.png icon_license.png
  settings300.png icon_settings_background-type.png
  settings300.png icon_settings_language.png
  settings300.png icon_settings_vsync.png
  settings300.png icon_settings_sample-count.png
  settings300.png icon_settings_max-fps.png
  settings300.png icon_settings_icon-glass-refraction.png
  settings300.png icon_settings_controller-type.png
  settings300.png icon_settings_controller-rumble.png
  settings300.png icon_settings_controller-analog-stick.png
  settings300.png icon_settings_show-fps.png
  settings300.png icon_settings_show-mem.png
  game300.png     icon_category_game.png
  game300.png     icon_category_application.png
  network300.png  icon_category_network.png
  friends350.png  icon_category_users.png
  friends350.png  icon_category_friends.png
)
set(OPENXMB_SOUND_ASSETS
  NSE.questionMark.wav
  NSE.questionMark.ogg
  NSE.ui.Confirm.wav
  NSE.ui.Confirm.ogg
  NSE.ui.Cancel.wav
  NSE.ui.Cancel.ogg
  NSE.clicker.Cancel.wav
  NSE.clicker.Cancel.ogg
)
set(OPENXMB_PLACEHOLDER_ICON_ASSETS
  icon_action_lock.png
  icon_action_logout.png
  icon_action_poweroff.png
  icon_action_quit.png
  icon_action_reboot.png
  icon_action_suspend.png
  icon_cursor.png
  icon_files_file.png
  icon_files_folder.png
  icon_settings_autostart.png
  icon_settings_controller-cursor-speed.png
  icon_settings_date-time-format.png
  icon_settings_hide-login1-options.png
  icon_settings_hide-power-options.png
)
set(OPENXMB_BUTTON_ICON_TYPES default keyboard playstation xbox steam ouya)
set(OPENXMB_BUTTON_ICON_ACTIONS left right up down ok cancel options extra)

function(openxmb_install_file_if_exists source destination)
  if(EXISTS "${source}")
    install(FILES "${source}" DESTINATION "${destination}" ${ARGN})
  endif()
endfunction()

function(openxmb_configure_copy_if_exists source destination)
  if(EXISTS "${source}")
    configure_file("${source}" "${destination}" COPYONLY)
  endif()
endfunction()

# On Unix platforms (macOS, Linux), handle all deployment steps.
if(UNIX)
    # On macOS, set the RPATH so the executable can find Homebrew libraries.
    # This must be done BEFORE the install() command for the target.
    if(APPLE)
        find_package(Vulkan REQUIRED)
        # Using @executable_path makes the application more portable by telling
        # the dynamic linker to look for libraries relative to the executable's location.
        # We also add the Vulkan library's actual directory as a fallback.
        get_filename_component(VULKAN_LIB_DIR ${Vulkan_LIBRARIES} DIRECTORY)
        set_target_properties(XMS PROPERTIES
            INSTALL_RPATH "@executable_path/../lib;${VULKAN_LIB_DIR}"
        )
    endif()

    # Install the main executable binary.
    install(TARGETS XMS
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    )

    # Install the default config.json so the launcher script can copy it on first run.
    install(
        FILES ${CMAKE_CURRENT_SOURCE_DIR}/config.json
        DESTINATION "${OPENXMB_CONFIG_INSTALL_DIR}"
    )

    # Install assets from interfaceFX (icons, sounds, fonts) into share/shell
    install(CODE "
      file(MAKE_DIRECTORY \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${OPENXMB_SHELL_INSTALL_DIR}/icons\")
      file(MAKE_DIRECTORY \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${OPENXMB_SHELL_INSTALL_DIR}/sounds\")
    ")
    # Fonts
    openxmb_install_file_if_exists(
        "${CMAKE_CURRENT_SOURCE_DIR}/interfaceFX/TypefaceServer/Play-Regular.ttf"
        "${OPENXMB_SHELL_INSTALL_DIR}"
        RENAME Play-Regular.ttf
    )
    # Sounds
    # Use clicker sound for tick/OK feedback
    set(_snd_dir "${CMAKE_CURRENT_SOURCE_DIR}/interfaceFX/AudioServer")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/interfaceFX/AudioServer/NSE.clicker.Opt.wav")
      openxmb_install_file_if_exists("${_snd_dir}/NSE.clicker.Opt.wav" "${OPENXMB_SHELL_INSTALL_DIR}/sounds" RENAME ok.wav)
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/interfaceFX/AudioServer/NSE.ui.Confirm.wav")
      # Fallback to confirm if clicker is unavailable
      openxmb_install_file_if_exists("${_snd_dir}/NSE.ui.Confirm.wav" "${OPENXMB_SHELL_INSTALL_DIR}/sounds" RENAME ok.wav)
    endif()
    # Additional UI sounds (copied as-is if present)
    foreach(_snd IN LISTS OPENXMB_SOUND_ASSETS)
      openxmb_install_file_if_exists("${_snd_dir}/${_snd}" "${OPENXMB_SHELL_INSTALL_DIR}/sounds")
    endforeach()
    # Startup jingle (used by startup overlay)
    openxmb_install_file_if_exists("${_snd_dir}/NSE.startup.ogg" "${OPENXMB_SHELL_INSTALL_DIR}/sounds" RENAME startup.ogg)
    openxmb_install_file_if_exists("${_snd_dir}/NSE.startup.GameBoot.wav" "${OPENXMB_SHELL_INSTALL_DIR}/sounds" RENAME startup.wav)
    # Category icons mapping (use existing as placeholders where needed)
    set(_icon_src_dir "${CMAKE_CURRENT_SOURCE_DIR}/interfaceFX/GraphicsServer/HomeMenu")
    list(LENGTH OPENXMB_ICON_ASSET_MAP _icon_map_len)
    math(EXPR _icon_map_last "${_icon_map_len} - 1")
    foreach(_icon_index RANGE 0 ${_icon_map_last} 2)
      math(EXPR _icon_name_index "${_icon_index} + 1")
      list(GET OPENXMB_ICON_ASSET_MAP ${_icon_index} _icon_source)
      list(GET OPENXMB_ICON_ASSET_MAP ${_icon_name_index} _icon_name)
      openxmb_install_file_if_exists("${_icon_src_dir}/${_icon_source}" "${OPENXMB_SHELL_INSTALL_DIR}/icons" RENAME "${_icon_name}")
    endforeach()
    foreach(_icon_name IN LISTS OPENXMB_PLACEHOLDER_ICON_ASSETS)
      openxmb_install_file_if_exists("${_icon_src_dir}/settings300.png" "${OPENXMB_SHELL_INSTALL_DIR}/icons" RENAME "${_icon_name}")
    endforeach()
    foreach(_type IN LISTS OPENXMB_BUTTON_ICON_TYPES)
      foreach(_action IN LISTS OPENXMB_BUTTON_ICON_ACTIONS)
        openxmb_install_file_if_exists("${_icon_src_dir}/settings300.png" "${OPENXMB_SHELL_INSTALL_DIR}/icons" RENAME "icon_button_${_type}_${_action}.png")
      endforeach()
    endforeach()

    # Configure our launcher script template and install it.
    configure_file(
        scripts/XMS.sh.in
        ${CMAKE_CURRENT_BINARY_DIR}/XMS.sh
        @ONLY
    )
    # Ensure the build-folder script is executable for direct use pre-install
    file(CHMOD ${CMAKE_CURRENT_BINARY_DIR}/XMS.sh
        PERMISSIONS
          OWNER_READ OWNER_WRITE OWNER_EXECUTE
          GROUP_READ GROUP_EXECUTE
          WORLD_READ WORLD_EXECUTE
    )
    install(
        FILES ${CMAKE_CURRENT_BINARY_DIR}/XMS.sh
        DESTINATION "${CMAKE_INSTALL_BINDIR}"
        RENAME "XMS"
        PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ GROUP_EXECUTE GROUP_READ WORLD_EXECUTE WORLD_READ
    )

    # -------------------------------------------------------------------------
    # Dev assets for running from the build tree (no install). We mirror a
    # minimal asset set into ${CMAKE_CURRENT_BINARY_DIR}/share/shell
    # so the launcher can find icons/sounds/fonts.
    # -------------------------------------------------------------------------
    set(_dev_shell_dir "${CMAKE_CURRENT_BINARY_DIR}/share/shell")
    file(MAKE_DIRECTORY "${_dev_shell_dir}/icons")
    file(MAKE_DIRECTORY "${_dev_shell_dir}/sounds")
    # Font
    openxmb_configure_copy_if_exists(
      "${CMAKE_CURRENT_SOURCE_DIR}/interfaceFX/TypefaceServer/Play-Regular.ttf"
      "${_dev_shell_dir}/Play-Regular.ttf"
    )
    # Sounds
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/interfaceFX/AudioServer/NSE.clicker.Opt.wav")
      openxmb_configure_copy_if_exists("${_snd_dir}/NSE.clicker.Opt.wav" "${_dev_shell_dir}/sounds/ok.wav")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/interfaceFX/AudioServer/NSE.ui.Confirm.wav")
      openxmb_configure_copy_if_exists("${_snd_dir}/NSE.ui.Confirm.wav" "${_dev_shell_dir}/sounds/ok.wav")
    endif()
    foreach(_snd IN LISTS OPENXMB_SOUND_ASSETS)
      openxmb_configure_copy_if_exists("${_snd_dir}/${_snd}" "${_dev_shell_dir}/sounds/${_snd}")
    endforeach()
    openxmb_configure_copy_if_exists("${_snd_dir}/NSE.startup.ogg" "${_dev_shell_dir}/sounds/startup.ogg")
    openxmb_configure_copy_if_exists("${_snd_dir}/NSE.startup.GameBoot.wav" "${_dev_shell_dir}/sounds/startup.wav")
    # Icons (map available HomeMenu and settings placeholders)
    foreach(_icon_index RANGE 0 ${_icon_map_last} 2)
      math(EXPR _icon_name_index "${_icon_index} + 1")
      list(GET OPENXMB_ICON_ASSET_MAP ${_icon_index} _icon_source)
      list(GET OPENXMB_ICON_ASSET_MAP ${_icon_name_index} _icon_name)
      openxmb_configure_copy_if_exists("${_icon_src_dir}/${_icon_source}" "${_dev_shell_dir}/icons/${_icon_name}")
    endforeach()
    foreach(_icon_name IN LISTS OPENXMB_PLACEHOLDER_ICON_ASSETS)
      openxmb_configure_copy_if_exists("${_icon_src_dir}/settings300.png" "${_dev_shell_dir}/icons/${_icon_name}")
    endforeach()
    # Placeholder button icons to avoid load errors until dedicated glyphs exist.
    if(EXISTS "${_icon_src_dir}/settings300.png")
      foreach(_type IN LISTS OPENXMB_BUTTON_ICON_TYPES)
        foreach(_action IN LISTS OPENXMB_BUTTON_ICON_ACTIONS)
          openxmb_configure_copy_if_exists("${_icon_src_dir}/settings300.png" "${_dev_shell_dir}/icons/icon_button_${_type}_${_action}.png")
        endforeach()
      endforeach()
    endif()
endif()

if(WIN32)
    # Build-tree assets for direct XMS.bin.exe runs on Windows. The Windows
    # constants look for ./shell beside the executable, so mirror the same
    # minimal runtime assets there during configure.
    set(_win_dev_shell_dir "${CMAKE_CURRENT_BINARY_DIR}/shell")
    set(_win_snd_dir "${CMAKE_CURRENT_SOURCE_DIR}/interfaceFX/AudioServer")
    set(_win_icon_src_dir "${CMAKE_CURRENT_SOURCE_DIR}/interfaceFX/GraphicsServer/HomeMenu")
    file(MAKE_DIRECTORY "${_win_dev_shell_dir}/icons")
    file(MAKE_DIRECTORY "${_win_dev_shell_dir}/sounds")
    openxmb_configure_copy_if_exists(
      "${CMAKE_CURRENT_SOURCE_DIR}/interfaceFX/TypefaceServer/Play-Regular.ttf"
      "${_win_dev_shell_dir}/Play-Regular.ttf"
    )
    openxmb_configure_copy_if_exists(
      "${CMAKE_CURRENT_SOURCE_DIR}/config.json"
      "${CMAKE_CURRENT_BINARY_DIR}/config.json"
    )
    if(EXISTS "${_win_snd_dir}/NSE.clicker.Opt.wav")
      openxmb_configure_copy_if_exists("${_win_snd_dir}/NSE.clicker.Opt.wav" "${_win_dev_shell_dir}/sounds/ok.wav")
    elseif(EXISTS "${_win_snd_dir}/NSE.ui.Confirm.wav")
      openxmb_configure_copy_if_exists("${_win_snd_dir}/NSE.ui.Confirm.wav" "${_win_dev_shell_dir}/sounds/ok.wav")
    endif()
    foreach(_snd IN LISTS OPENXMB_SOUND_ASSETS)
      openxmb_configure_copy_if_exists("${_win_snd_dir}/${_snd}" "${_win_dev_shell_dir}/sounds/${_snd}")
    endforeach()
    openxmb_configure_copy_if_exists("${_win_snd_dir}/NSE.startup.ogg" "${_win_dev_shell_dir}/sounds/startup.ogg")
    openxmb_configure_copy_if_exists("${_win_snd_dir}/NSE.startup.GameBoot.wav" "${_win_dev_shell_dir}/sounds/startup.wav")

    list(LENGTH OPENXMB_ICON_ASSET_MAP _win_icon_map_len)
    math(EXPR _win_icon_map_last "${_win_icon_map_len} - 1")
    foreach(_icon_index RANGE 0 ${_win_icon_map_last} 2)
      math(EXPR _icon_name_index "${_icon_index} + 1")
      list(GET OPENXMB_ICON_ASSET_MAP ${_icon_index} _icon_source)
      list(GET OPENXMB_ICON_ASSET_MAP ${_icon_name_index} _icon_name)
      openxmb_configure_copy_if_exists("${_win_icon_src_dir}/${_icon_source}" "${_win_dev_shell_dir}/icons/${_icon_name}")
    endforeach()
    foreach(_icon_name IN LISTS OPENXMB_PLACEHOLDER_ICON_ASSETS)
      openxmb_configure_copy_if_exists("${_win_icon_src_dir}/settings300.png" "${_win_dev_shell_dir}/icons/${_icon_name}")
    endforeach()
    if(EXISTS "${_win_icon_src_dir}/settings300.png")
      foreach(_type IN LISTS OPENXMB_BUTTON_ICON_TYPES)
        foreach(_action IN LISTS OPENXMB_BUTTON_ICON_ACTIONS)
          openxmb_configure_copy_if_exists("${_win_icon_src_dir}/settings300.png" "${_win_dev_shell_dir}/icons/icon_button_${_type}_${_action}.png")
        endforeach()
      endforeach()
    endif()
endif()

if(WIN32)
    install(TARGETS XMS
        RUNTIME_DEPENDENCY_SET openxmb_runtime_deps
        RUNTIME DESTINATION "."
    )

    set(_openxmb_runtime_dependency_dirs)
    if(DEFINED _VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
      list(APPEND _openxmb_runtime_dependency_dirs "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
    endif()
    set(_openxmb_runtime_dependency_directories_arg)
    if(_openxmb_runtime_dependency_dirs)
      list(APPEND _openxmb_runtime_dependency_directories_arg DIRECTORIES ${_openxmb_runtime_dependency_dirs})
    endif()
    install(RUNTIME_DEPENDENCY_SET openxmb_runtime_deps
        PRE_EXCLUDE_REGEXES "api-ms-" "ext-ms-"
        POST_EXCLUDE_REGEXES ".*[Ww]indows[\\\\/].*" ".*[Ss]ystem32[\\\\/].*"
        ${_openxmb_runtime_dependency_directories_arg}
        RUNTIME DESTINATION "."
    )
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/config.json" DESTINATION ".")
    install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/shell/" DESTINATION "shell")
endif()

# Optional external dependencies when flags are enabled (best-effort)
if(ENABLE_DISC_MEDIA)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(BLURAY libbluray QUIET)
    pkg_check_modules(DVDREAD libdvdread QUIET)
    pkg_check_modules(DVDNAV libdvdnav QUIET)
    if(BLURAY_FOUND)
      target_link_libraries(XMS PRIVATE PkgConfig::BLURAY)
    endif()
    if(DVDREAD_FOUND)
      target_link_libraries(XMS PRIVATE PkgConfig::DVDREAD)
    endif()
    if(DVDNAV_FOUND)
      target_link_libraries(XMS PRIVATE PkgConfig::DVDNAV)
    endif()
  endif()
endif()
# =============================================================================
# --- i18n (gettext) locales ---
# =============================================================================

# Define locale install dir (default to share/locale on non-Windows)
if(WIN32)
  set(CMAKE_INSTALL_LOCALEDIR "locales")
else()
  if(NOT DEFINED CMAKE_INSTALL_LOCALEDIR)
    set(CMAKE_INSTALL_LOCALEDIR "${CMAKE_INSTALL_DATADIR}/locale")
  endif()
endif()

# Read languages and compile .po -> .mo for development and install
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/po/LINGUAS" AND GETTEXT_FOUND)
  file(STRINGS po/LINGUAS LINGUAS)
  foreach(LING ${LINGUAS})
    # Build binary catalog in build dir as <ling>.gmo
    gettext_process_po_files(${LING} ALL PO_FILES po/${LING}.po)
    # Install as "shell.mo" to match textdomain used by the app
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${LING}.gmo
      DESTINATION ${CMAKE_INSTALL_LOCALEDIR}/${LING}/LC_MESSAGES/
      RENAME shell.mo)
    # For running from build tree: create locales/<ling>/LC_MESSAGES/shell.mo.
    # Windows does not always grant symlink privileges, so copy there.
    if(WIN32)
      add_custom_target(openxmb-link-po-${LING} ALL
        COMMAND
          ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/locales/${LING}/LC_MESSAGES
        COMMAND
          ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_BINARY_DIR}/${LING}.gmo ${CMAKE_CURRENT_BINARY_DIR}/locales/${LING}/LC_MESSAGES/shell.mo
        DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/${LING}.gmo"
        BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/locales/${LING}/LC_MESSAGES/shell.mo
        VERBATIM)
    else()
      add_custom_target(openxmb-link-po-${LING} ALL
        COMMAND
          ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/locales/${LING}/LC_MESSAGES
        COMMAND
          ${CMAKE_COMMAND} -E create_symlink ${CMAKE_CURRENT_BINARY_DIR}/${LING}.gmo ${CMAKE_CURRENT_BINARY_DIR}/locales/${LING}/LC_MESSAGES/shell.mo
        DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/${LING}.gmo"
        BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/locales/${LING}/LC_MESSAGES/shell.mo
        VERBATIM)
    endif()
  endforeach()
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/po/LINGUAS")
  message(STATUS "${COLOR_YELLOW}Gettext tools not found; skipping compiled locale catalogs.${COLOR_RESET}")
endif()
