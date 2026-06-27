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

# Static CRT so the DLL has no runtime DLL dependencies inside the game process.
set_property(TARGET bo1bridge PROPERTY
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

target_compile_definitions(bo1bridge PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  _CRT_SECURE_NO_WARNINGS
)
