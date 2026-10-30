cmake_minimum_required(VERSION 3.25)

# SkyRecomp UWP runtime - Xbox Series X|S, Developer Mode.
#
# Mirrors the structure of the two shipping Developer Mode ports studied in
# ../PORT-BLUEPRINT.md (Cosmo/gow3_xbox, JYRS20/WWHD-UWP): CMake driving clang-cl with
# CMAKE_SYSTEM_NAME=WindowsStore, the Store CRT import libraries first on the link line,
# /APPCONTAINER, fixed-address VA, and a 16 MB stack.
#
# STATUS: never configured or compiled. Nothing in this sandbox can do that. Prerequisites
# below are hard errors on purpose, so a half-configured build stops immediately instead of
# producing a package that fails at activation (which is exactly how the v0.1.x smoke package
# wasted a console round trip).

set(CMAKE_SYSTEM_NAME WindowsStore)
set(CMAKE_SYSTEM_VERSION 10.0)
project(skyrecomp_uwp LANGUAGES C CXX)

# ---- toolchain gate --------------------------------------------------------------------
# The recompiled guest code uses the System V x64 ABI and the runtime relies on clang
# extensions; WWHD additionally needs `musttail`. MSVC is not an option.
if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "Use clang-cl: the guest code requires the System V x64 ABI (see PORT-BLUEPRINT.md section 2).")
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)   # /MD is mandatory with C++/CX

set(SKY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH "SkyRecomp checkout root")
get_filename_component(SKY_ROOT "${SKY_ROOT}" ABSOLUTE)

# ---- dependency gates ------------------------------------------------------------------
# Every one of these has to be built for UWP before this project can link. They are not
# optional: the renderer is Vulkan (via Dozen), input/windowing is SDL3, video is FFmpeg and
# shader compilation is shaderc.
set(SKY_DOZEN_DLL   "" CACHE FILEPATH "vulkan_dzn.dll built for UWP (Mesa Dozen)")
set(SKY_DXC_ROOT    "" CACHE PATH     "DirectX Shader Compiler release (bin/x64/dxil.dll)")
set(SKY_SDL3_BUILD  "" CACHE PATH     "MSVC-built SDL3 for UWP (contains SDL3.lib/SDL3.dll)")
set(SKY_SDL3_SOURCE "" CACHE PATH     "Pinned SDL3 source (release-3.4.18)")
set(SKY_FFMPEG_ROOT "" CACHE PATH     "LGPL FFmpeg 9.0.2 built for UWP (static libs + headers)")
set(SKY_SHADERC_ROOT "" CACHE PATH    "shaderc built for UWP (static)")

foreach(required
        "${SKY_DOZEN_DLL}"
        "${SKY_DXC_ROOT}/bin/x64/dxil.dll"
        "${SKY_SDL3_BUILD}/SDL3.lib"
        "${SKY_SDL3_SOURCE}/include/SDL3/SDL.h")
    if(NOT EXISTS "${required}")
        message(FATAL_ERROR
            "Missing ${required}\n"
            "Build the UWP dependencies first - see ../port/BUILD-PORT.md. "
            "The smoke-test pipeline proves packaging; this project needs real UWP builds of "
            "Dozen, SDL3 and FFmpeg before it can link.")
    endif()
endforeach()

# ---- shared compile settings -----------------------------------------------------------
add_compile_definitions(
    WINAPI_FAMILY=WINAPI_FAMILY_APP
    _CRT_APP
    SKYRECOMP_UWP=1
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
    _FILE_OFFSET_BITS=64
    GAMELIB_ID="5e5bafbb450f894b")     # must match the Makefile, see runtime/gamebuild.c

# Source list mirrors the Makefile's RT_SRC/BACKEND_OBJ/UI_OBJ, with the UWP platform layer
# swapped in for windows.c. runtime/ui/*.mm is plain C++ outside __APPLE__ blocks, exactly as
# the Linux build treats it (-x c++ in the Makefile).
file(GLOB SKY_RUNTIME_SRC   CONFIGURE_DEPENDS "${SKY_ROOT}/runtime/*.c")
file(GLOB SKY_HLE_SRC       CONFIGURE_DEPENDS "${SKY_ROOT}/runtime/hle/*.c")
file(GLOB SKY_NVN_SRC       CONFIGURE_DEPENDS "${SKY_ROOT}/runtime/nvn/*.c")
file(GLOB SKY_MAXWELL_SRC   CONFIGURE_DEPENDS "${SKY_ROOT}/runtime/nvn/maxwell/*.c")
file(GLOB SKY_VULKAN_SRC    CONFIGURE_DEPENDS "${SKY_ROOT}/runtime/nvn/vulkan/*.c" "${SKY_ROOT}/runtime/nvn/vulkan/*.cpp")
file(GLOB SKY_UI_SRC        CONFIGURE_DEPENDS "${SKY_ROOT}/runtime/ui/*.mm")

# windows.c/macos.c/linux.c must not be compiled here; uwp.c replaces them.
list(FILTER SKY_RUNTIME_SRC EXCLUDE REGEX "/platform/(windows|macos|linux)\\.c$")

# The C++/CX activation host. Required: a UWP app is activated through the class the manifest's
# EntryPoint names, and that class is what hands LocalState and the install directory to uwp.c
# before plat_init() runs. It is not written yet - see PORT-INVENTORY.md section 8, step 2 - so
# this fails loudly rather than producing an executable nothing can activate.
set(SKY_UWP_ENTRY "${CMAKE_CURRENT_LIST_DIR}/runtime/platform/uwp_entry.cpp")
if(NOT EXISTS "${SKY_UWP_ENTRY}")
    message(FATAL_ERROR
        "Missing ${SKY_UWP_ENTRY}\n"
        "This is the C++/CX IFrameworkView the manifest's EntryPoint names; see PORT-INVENTORY.md "
        "section 8 step 2. Nothing can activate a package built without it.")
endif()

add_executable(skyrecomp_uwp WIN32
    ${SKY_RUNTIME_SRC} ${SKY_HLE_SRC} ${SKY_NVN_SRC} ${SKY_MAXWELL_SRC}
    ${SKY_VULKAN_SRC} ${SKY_UI_SRC}
    "${CMAKE_CURRENT_LIST_DIR}/runtime/platform/uwp.c"
    "${SKY_UWP_ENTRY}")

set_source_files_properties(${SKY_UI_SRC} PROPERTIES LANGUAGE CXX)

target_include_directories(skyrecomp_uwp PRIVATE
    "${SKY_ROOT}/runtime" "${SKY_ROOT}/runtime/nvn"
    "${SKY_ROOT}/third_party/imgui" "${SKY_ROOT}/third_party/imgui/backends"
    "${SKY_SDL3_SOURCE}/include"
    "${SKY_FFMPEG_ROOT}/include")

# ---- link ------------------------------------------------------------------------------
# Store CRT import libraries FIRST: the UWP variants of the CRT are what windowsapp.lib
# expects (WWHD and gow3 both do this).
target_link_directories(skyrecomp_uwp BEFORE PRIVATE "$ENV{VCToolsInstallDir}/lib/x64/store")

target_link_options(skyrecomp_uwp PRIVATE
    /APPCONTAINER
    /HIGHENTROPYVA:NO          # the guest window at 0x70_0000_0000 must not be randomised
    /STACK:16777216,16777216   # same 16 MB the Makefile requests
    /OPT:REF)

# Dozen (Vulkan-on-D3D12) and the SDL3 UWP build are both DLLs.
add_library(dozen SHARED IMPORTED)
set_target_properties(dozen PROPERTIES IMPORTED_LOCATION "${SKY_DOZEN_DLL}")

add_library(SDL3_UWP SHARED IMPORTED)
set_target_properties(SDL3_UWP PROPERTIES
    IMPORTED_IMPLIB "${SKY_SDL3_BUILD}/SDL3.lib"
    IMPORTED_LOCATION "${SKY_SDL3_BUILD}/SDL3.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${SKY_SDL3_SOURCE}/include")

# FFmpeg: static, LGPL. The movie decoder only needs VP9/VP8/Vorbis/Opus/H.264/AAC in
# Matroska/MP4 - the same configure line as tools/build_deps_windows.sh, retargeted to UWP.
foreach(lib avformat avcodec avutil swscale)
    add_library(${lib} STATIC IMPORTED)
    set_target_properties(${lib} PROPERTIES IMPORTED_LOCATION "${SKY_FFMPEG_ROOT}/lib/${lib}.lib")
endforeach()

target_link_libraries(skyrecomp_uwp PRIVATE
    dozen SDL3_UWP avformat avcodec avutil swscale
    d3d12 dxgi dxguid
    windowsapp
    # Deliberately absent, all unavailable or unnecessary on console:
    #   vulkan-1  -> Dozen is loaded directly and presents through D3D12
    #   winmm     -> timeBeginPeriod is replaced by high-resolution waitable timers
    #   shell32   -> no ShellExecute; folder opening is a UI action in the app
    #   comdlg32  -> FileOpenPicker (WinRT) replaces the Win32 common dialogs
    #   ole32/uuid-> COM apartment setup moves into the C++/CX activation host
    )

# ---- package layout --------------------------------------------------------------------
# Everything the app reads at runtime ships inside the package (read-only). Writable state
# goes to LocalState. The user's game files never enter the package (PORT-BLUEPRINT.md 5).
set(SKY_LAYOUT "${CMAKE_BINARY_DIR}/layout")
add_custom_command(TARGET skyrecomp_uwp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SKY_LAYOUT}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:skyrecomp_uwp>" "${SKY_LAYOUT}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SKY_DOZEN_DLL}" "${SKY_LAYOUT}/vulkan_dzn.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SKY_DXC_ROOT}/bin/x64/dxil.dll" "${SKY_LAYOUT}/dxil.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SKY_SDL3_BUILD}/SDL3.dll" "${SKY_LAYOUT}/"
    COMMENT "Staging package layout")

message(STATUS "SkyRecomp UWP configured. Layout: ${SKY_LAYOUT}")
message(STATUS "NOTE: manifest and packaging are driven by ../packaging/Package-Uwp.ps1 + manifest/Package.appxmanifest.port.in")
