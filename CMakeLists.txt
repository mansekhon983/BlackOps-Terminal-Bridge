cmake_minimum_required(VERSION 3.20)
project(bo1_shell_bridge LANGUAGES CXX)

# -----------------------------------------------------------------------------
# BO1 Terminal -> Real Shell Bridge
#
# Target MUST be 32-bit: BlackOps.exe is i386 / PE32 (verified via PE header,
# Machine 0x014C). Always configure with:  cmake -A Win32 -B build
# -----------------------------------------------------------------------------

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR
    "This DLL must be built for 32-bit (x86) to match BlackOps.exe. "
    "Configure with: cmake -A Win32 -B build")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT, applied to ALL targets (incl. the MinHook subproject). CMP0091 NEW
# is forced as a policy default so MinHook -- whose cmake_minimum_required is too
# old to enable it -- still honors CMAKE_MSVC_RUNTIME_LIBRARY. Without this the
# DLL (/MT) and minhook (/MD) collide at link (__except_handler4_common).
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# --- MinHook (inline hooking) ------------------------------------------------
include(FetchContent)
FetchContent_Declare(
  minhook
  GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
  GIT_TAG        master
)
FetchContent_MakeAvailable(minhook)

# --- The injected DLL --------------------------------------------------------
add_library(bo1bridge SHARED
  src/dllmain.cpp
  src/hooks.cpp
  src/bridge.cpp
  src/queues.cpp
  src/sigscan.cpp
)

target_include_directories(bo1bridge PRIVATE src)
target_link_libraries(bo1bridge PRIVATE minhook)

target_compile_definitions(bo1bridge PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  _CRT_SECURE_NO_WARNINGS
)
