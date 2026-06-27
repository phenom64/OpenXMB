if(NOT CMAKE_HOST_WIN32)
  message(FATAL_ERROR "windows-clang-vcpkg.cmake can only configure on a Windows host")
endif()

set(_openxmb_llvm_hints
  "$ENV{ProgramFiles}/LLVM/bin"
  "$ENV{ProgramFiles}/Microsoft Visual Studio/2022/Community/VC/Tools/Llvm/x64/bin"
  "$ENV{ProgramFiles}/Microsoft Visual Studio/2022/BuildTools/VC/Tools/Llvm/x64/bin"
)

if(NOT CMAKE_CXX_COMPILER)
  find_program(_openxmb_clangxx
    NAMES clang++.exe clang++
    HINTS ${_openxmb_llvm_hints}
    NO_CACHE
  )
  if(NOT _openxmb_clangxx)
    message(FATAL_ERROR
      "LLVM clang++ was not found. Install LLVM with clang-scan-deps or set "
      "CMAKE_CXX_COMPILER to an absolute clang++.exe path."
    )
  endif()
  set(CMAKE_CXX_COMPILER "${_openxmb_clangxx}" CACHE FILEPATH "LLVM clang++" FORCE)
endif()

get_filename_component(_openxmb_clang_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
list(PREPEND CMAKE_PROGRAM_PATH "${_openxmb_clang_bin}")

if(NOT CLANG_SCAN_DEPS_EXECUTABLE)
  find_program(_openxmb_clang_scan_deps
    NAMES clang-scan-deps.exe clang-scan-deps
    HINTS "${_openxmb_clang_bin}" ${_openxmb_llvm_hints}
    NO_CACHE
  )
  if(NOT _openxmb_clang_scan_deps)
    message(FATAL_ERROR
      "clang-scan-deps was not found beside ${CMAKE_CXX_COMPILER}. "
      "Install a complete LLVM toolchain or set CLANG_SCAN_DEPS_EXECUTABLE."
    )
  endif()
  set(CLANG_SCAN_DEPS_EXECUTABLE "${_openxmb_clang_scan_deps}" CACHE FILEPATH
      "Path to clang-scan-deps" FORCE)
endif()

if(NOT CMAKE_RC_COMPILER)
  file(GLOB _openxmb_rc_candidates
    "$ENV{SystemDrive}/Program Files (x86)/Windows Kits/10/bin/*/x64/rc.exe"
  )
  if(NOT _openxmb_rc_candidates)
    message(FATAL_ERROR
      "Windows SDK rc.exe was not found. Install the Windows 10/11 SDK or set "
      "CMAKE_RC_COMPILER to an absolute rc.exe path."
    )
  endif()
  list(SORT _openxmb_rc_candidates COMPARE NATURAL ORDER DESCENDING)
  list(GET _openxmb_rc_candidates 0 _openxmb_rc)
  file(TO_CMAKE_PATH "${_openxmb_rc}" _openxmb_rc)
  set(CMAKE_RC_COMPILER "${_openxmb_rc}" CACHE FILEPATH "Windows resource compiler" FORCE)
  get_filename_component(_openxmb_rc_bin "${_openxmb_rc}" DIRECTORY)
  list(PREPEND CMAKE_PROGRAM_PATH "${_openxmb_rc_bin}")
endif()

set(_openxmb_vcpkg_candidates)
if(OPENXMB_VCPKG_ROOT)
  list(APPEND _openxmb_vcpkg_candidates "${OPENXMB_VCPKG_ROOT}")
endif()
if(DEFINED ENV{VCPKG_ROOT})
  list(APPEND _openxmb_vcpkg_candidates "$ENV{VCPKG_ROOT}")
endif()
list(APPEND _openxmb_vcpkg_candidates
  "$ENV{USERPROFILE}/Developer/vcpkg"
  "$ENV{USERPROFILE}/vcpkg"
  "C:/vcpkg"
  "C:/src/vcpkg"
)

set(_openxmb_vcpkg_toolchain)
foreach(_openxmb_vcpkg_root IN LISTS _openxmb_vcpkg_candidates)
  file(TO_CMAKE_PATH "${_openxmb_vcpkg_root}" _openxmb_vcpkg_root)
  if(EXISTS "${_openxmb_vcpkg_root}/scripts/buildsystems/vcpkg.cmake")
    set(_openxmb_vcpkg_toolchain
      "${_openxmb_vcpkg_root}/scripts/buildsystems/vcpkg.cmake")
    set(OPENXMB_VCPKG_ROOT "${_openxmb_vcpkg_root}" CACHE PATH "vcpkg checkout" FORCE)
    break()
  endif()
endforeach()

if(NOT _openxmb_vcpkg_toolchain)
  message(FATAL_ERROR
    "vcpkg was not found. Set OPENXMB_VCPKG_ROOT or VCPKG_ROOT to a checkout "
    "containing scripts/buildsystems/vcpkg.cmake."
  )
endif()

set(VCPKG_TARGET_TRIPLET "x64-windows" CACHE STRING "vcpkg target triplet")
include("${_openxmb_vcpkg_toolchain}")
