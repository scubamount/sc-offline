# sc-offline: dinput8.dll (the mod, src/) and sc-offline.exe (the launcher, launcher/).
#
#   cmake -S . -B build -G "Visual Studio 18 2026" -A x64
#   cmake --build build --config Release --parallel
#
# Outputs land in build/bin/<config>/ (build/bin/ with a single-config generator such as Ninja).
#
# Layout:
#   imgui       static   src/third_party/imgui (vendored, its own warnings off)
#   dinput8     shared   src/*.cpp, src/builtins/*.cpp -> dinput8.dll
#   sc-offline  exe      launcher/launcher.cpp -> sc-offline.exe
#   sco-core    static   external/sco-core (submodule): scanners, signatures, host kit, plugin
#                        loader and sco-lua; dinput8 links sco_app and sco_lua
#
# Every setting below carries over from the former src/sc-offline-dll.vcxproj and
# launcher/sc-offline.vcxproj (Release|x64 and Debug|x64).

cmake_minimum_required(VERSION 3.30)

# The .vcxproj files had Debug and Release only.
if(NOT DEFINED CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_CONFIGURATION_TYPES Debug Release CACHE STRING "Configurations")
endif()

project(sc-offline LANGUAGES C CXX)

if(NOT MSVC)
  message(FATAL_ERROR "sc-offline builds with MSVC only (tools/check.sh parses it elsewhere)")
endif()
if(NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
  message(FATAL_ERROR "sc-offline is x64 only (got '${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}'); configure with -A x64")
endif()

# Static C runtime (RuntimeLibrary MultiThreaded): the DLL and the launcher run without the
# Visual C++ redistributable, in Wine too.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# LanguageStandard stdcpp20.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# CMake's default MSVC flags define WIN32 and _WINDOWS; the x64 configurations of the
# .vcxproj files defined neither.
foreach(lang C CXX)
  string(REGEX REPLACE "[/-]D *(WIN32|_WINDOWS)( |$)" "" CMAKE_${lang}_FLAGS "${CMAKE_${lang}_FLAGS}")
endforeach()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# --- sco-core (external/sco-core, a git submodule) -----------------------------------------------
# Added after CMAKE_MSVC_RUNTIME_LIBRARY above, so its libraries get the static CRT too: they link
# into dinput8.dll, and CI's verify step checks the DLL imports no dynamic CRT.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/sco-core/CMakeLists.txt")
  message(FATAL_ERROR "external/sco-core is empty: the sco-core submodule isn't checked out. "
                      "Run `git submodule update --init` (or clone with --recurse-submodules).")
endif()
# sco-core's tests run in sco-core's own CI, not here.
set(SCO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
# sco-core's own CI builds it with /WX. Here a warning that only a newer toolset emits must not
# break the product build, so sco-core's warnings stay warnings.
set(SCO_WERROR OFF CACHE BOOL "" FORCE)
# EXCLUDE_FROM_ALL: only the libraries dinput8 links are built, not sco-core's host tools
# (sco-sigcheck, sco-host-sim).
add_subdirectory(external/sco-core EXCLUDE_FROM_ALL)

# Settings every target had: CharacterSet Unicode, ConformanceMode (/permissive-), SDLCheck (/sdl).
# NDEBUG in Release comes from CMake's default Release flags (/O2 /Ob2 /DNDEBUG).
set(SCO_COMMON_DEFINES UNICODE _UNICODE _CONSOLE)
set(SCO_COMMON_OPTIONS /permissive- /sdl)

# --- dinput8.dll and the ImGui it draws with --------------------------------------------------
# The .vcxproj compiled ImGui inside the DLL project, so it gets the DLL's settings except warnings.
set(SCO_DLL_DEFINES ${SCO_COMMON_DEFINES} _WINDLL $<$<CONFIG:Debug>:_DEBUG>)
# Release: FunctionLevelLinking (/Gy), IntrinsicFunctions (/Oi), StringPooling (/GF),
# FavorSizeOrSpeed Speed (/Ot); Optimization MaxSpeed is CMake's Release /O2.
set(SCO_DLL_OPTIONS ${SCO_COMMON_OPTIONS} "$<$<CONFIG:Release>:/Gy;/Oi;/GF;/Ot>")

add_library(imgui STATIC
  src/third_party/imgui/imgui.cpp
  src/third_party/imgui/imgui_draw.cpp
  src/third_party/imgui/imgui_tables.cpp
  src/third_party/imgui/imgui_widgets.cpp
  src/third_party/imgui/imgui_impl_win32.cpp
  src/third_party/imgui/imgui_impl_dx11.cpp
)
target_compile_definitions(imgui PRIVATE ${SCO_DLL_DEFINES})
target_compile_options(imgui PRIVATE ${SCO_DLL_OPTIONS} /W0)
# What imgui's own #pragma comment(lib) lines pull in.
target_link_libraries(imgui INTERFACE d3d11 d3dcompiler dwmapi imm32)

add_library(dinput8 SHARED
  src/dllmain.cpp
  src/common.cpp
  src/patches.cpp
  src/hooks.cpp
  src/teleport.cpp
  src/spawner.cpp
  src/loadout.cpp
  src/npc.cpp
  src/build.cpp
  src/cvars.cpp
  src/missions.cpp
  src/contracts.cpp
  src/quantum.cpp
  src/ammo.cpp
  src/services.cpp
  src/outfits.cpp
  src/menu.cpp
  src/travel.cpp
  # Built-in plugins (src/builtins/builtins.h).
  src/builtins/teleport_plugin.cpp
  # Manifest EnableSegmentHeap.
  src/dinput8.manifest
)
# TargetName dinput8. No dinput8_EXPORTS define: the .vcxproj had none, and the only export is
# the DirectInput8Create forwarder in dllmain.cpp (#pragma comment(linker, "/export:...")).
set_target_properties(dinput8 PROPERTIES OUTPUT_NAME dinput8 DEFINE_SYMBOL "")
target_compile_definitions(dinput8 PRIVATE ${SCO_DLL_DEFINES})
target_compile_options(dinput8 PRIVATE ${SCO_DLL_OPTIONS} /W3)
# SubSystem Windows; Release: OptimizeReferences, EnableCOMDATFolding.
target_link_options(dinput8 PRIVATE /SUBSYSTEM:WINDOWS "$<$<CONFIG:Release>:/OPT:REF;/OPT:ICF>")
# Also named by #pragma comment(lib) in dllmain.cpp and menu.cpp.
target_link_libraries(dinput8 PRIVATE imgui d3d11 shlwapi advapi32 windowscodecs ole32)
# The host kit (sco/app.h, with sco/scan.h, signatures, caps, the plugin loader) and sco-lua.
target_link_libraries(dinput8 PRIVATE sco_app sco_lua)

# WholeProgramOptimization + LinkTimeCodeGeneration in Release (/GL, /LTCG).
set_target_properties(imgui dinput8 PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)

# --- sc-offline.exe -----------------------------------------------------------------------------
# Console subsystem (CMake's default for an executable). No UAC setting: the manifest keeps the
# linker default asInvoker; the launcher asks for administrator rights at run time
# (ShellExecuteEx "runas") only for the parts that need them.
add_executable(sc-offline launcher/launcher.cpp)
target_compile_definitions(sc-offline PRIVATE ${SCO_COMMON_DEFINES})
target_compile_options(sc-offline PRIVATE ${SCO_COMMON_OPTIONS} /W3)
# Also named by #pragma comment(lib) in launcher.cpp.
target_link_libraries(sc-offline PRIVATE bcrypt shell32 shlwapi advapi32 ole32 winhttp user32 gdi32 comctl32)
