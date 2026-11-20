# [BUILD] RendDX9.dll and TextureAtlasBuilder.dll as Ninja targets: the same images
# Code/Build/renddx9.cmd and Code/Build/textureatlasbuilder.cmd build (TU manifest, flags, defines, link
# inputs and ABI checks), compiled incrementally and in parallel with compiler-reported header
# dependencies. The scripts remain for the G2w seam flavour (BF2_GFX_SEAM), the editor's own RendDX9
# (Code/Tools/BF2Editor) and the parity harnesses, and read the same manifests,
# Code/Build/renddx9_sources.txt and Code/Build/textureatlasbuilder_sources.txt.
#
# Included by the root CMakeLists.txt. Defines bf2_textureatlas_native (OUTPUT_NAME TextureAtlasBuilder)
# in ${CMAKE_BINARY_DIR}/textureatlasbuilder and, unless BF2_GFX_SEAM, bf2_renddx9_native (OUTPUT_NAME
# RendDX9) in ${CMAKE_BINARY_DIR}/renddx9.

include("${PROJECT_SOURCE_DIR}/Code/Build/plugin_common.cmake")
set(_abi "${_code}/RendDX9Abi")
set(_out "${CMAKE_BINARY_DIR}/renddx9")

# ---- D3DX: the April 2005 DirectX SDK, or the .def fallback (Code/Build/dxsdk.cmd, same rules) -------
# BF2_D3DX (environment): auto (default) takes the SDK when present, else the fallback; sdk requires
# it; def forces the fallback.
set(_d3dx_mode "$ENV{BF2_D3DX}")
if(NOT _d3dx_mode)
    set(_d3dx_mode auto)
endif()
if(NOT _d3dx_mode MATCHES "^(auto|sdk|def)$")
    message(FATAL_ERROR "RendDX9: BF2_D3DX must be auto, sdk or def (is \"${_d3dx_mode}\")")
endif()
set(_dxsdk "")
if(NOT _d3dx_mode STREQUAL "def")
    set(_candidates)
    if(DEFINED ENV{DXSDK_DIR})
        file(TO_CMAKE_PATH "$ENV{DXSDK_DIR}" _dxsdk_env)
        list(APPEND _candidates "${_dxsdk_env}")
    endif()
    set(_pf86 "ProgramFiles(x86)")
    file(TO_CMAKE_PATH "$ENV{${_pf86}}" _pf86_path)
    file(TO_CMAKE_PATH "$ENV{ProgramFiles}" _pf_path)
    list(APPEND _candidates
        "${_pf86_path}/Microsoft DirectX 9.0 SDK (April 2005)"
        "${_pf_path}/Microsoft DirectX 9.0 SDK (April 2005)")
    foreach(_try IN LISTS _candidates)
        string(REGEX REPLACE "/+$" "" _try "${_try}")
        if(_dxsdk OR NOT EXISTS "${_try}/Include/d3dx9core.h")
            continue()
        endif()
        file(STRINGS "${_try}/Include/d3dx9core.h" _ver REGEX "#define D3DX_SDK_VERSION 25")
        if(_ver AND EXISTS "${_try}/Lib/${_machine}/d3dx9.lib" AND EXISTS "${_try}/Lib/${_machine}/DxErr9.lib")
            set(_dxsdk "${_try}")
        endif()
    endforeach()
    if(NOT _dxsdk AND _d3dx_mode STREQUAL "sdk")
        message(FATAL_ERROR "RendDX9: BF2_D3DX=sdk but no April 2005 DirectX SDK was found (docs/archive/building.md, \"DirectX SDK\")")
    endif()
endif()
if(_dxsdk)
    message(STATUS "RendDX9: DirectX SDK ${_dxsdk} (D3DX 25, ${_machine})")
else()
    message(STATUS "RendDX9: no DirectX SDK - D3DX from Code/RendDX9Abi/D3dx9_25.def and D3dxFallbackInclude")
endif()

# ---- shared by both images ----------------------------------------------------------------------------
if(_dxsdk)
    set(_fallback OFF)
else()
    set(_fallback ON)
endif()
# Import libraries generated from .def files, built by bf2_plugin_implibs.
set(_libdir "${CMAKE_BINARY_DIR}/plugin_implibs")
set(_implibs)
function(_bf2_implib name def)
    add_custom_command(
        OUTPUT "${_libdir}/${name}.lib"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_libdir}"
        COMMAND "${CMAKE_AR}" /nologo "/def:${def}" "/machine:${_machine}" "/out:${_libdir}/${name}.lib"
        DEPENDS "${def}"
        COMMENT "${name}.lib from ${def}"
        VERBATIM)
    set(_implibs ${_implibs} "${_libdir}/${name}.lib" PARENT_SCOPE)
endfunction()
set(_renddx9_implibs)
if(BF2_BUILD_X64)
    _bf2_implib(TextureAtlasBuilder "${_abi}/TextureAtlasBuilder_x64.def")
    list(APPEND _renddx9_implibs "${_libdir}/TextureAtlasBuilder.lib")
else()
    _bf2_implib(TextureAtlasBuilder "${_abi}/TextureAtlasBuilder.def")
    # binkw32.dll exists only as a 32-bit DLL; x64 links the FFmpeg-based port below instead.
    _bf2_implib(binkw32 "${_abi}/binkw32.def")
    list(APPEND _renddx9_implibs "${_libdir}/TextureAtlasBuilder.lib" "${_libdir}/binkw32.lib")
endif()
if(_dxsdk)
    set(_d3dx_lib "${_dxsdk}/Lib/${_machine}/d3dx9.lib")
else()
    _bf2_implib(d3dx9_25 "${_abi}/D3dx9_25.def")
    set(_d3dx_lib "${_libdir}/d3dx9_25.lib")
endif()
add_custom_target(bf2_plugin_implibs DEPENDS ${_implibs})

# D3DX headers for C++: with the SDK, dxsdk.cmd appends it after the Windows SDK (INCLUDE), so the
# Windows SDK's d3d9.h wins; /I directories precede INCLUDE, so name the toolchain's directories first,
# then the SDK's. Without it, the fallback headers.
set(_d3dx_includes)
if(_dxsdk)
    set(_env_include "$ENV{INCLUDE}")
    foreach(_dir IN LISTS _env_include)
        if(_dir)
            file(TO_CMAKE_PATH "${_dir}" _dir)
            list(APPEND _d3dx_includes "${_dir}")
        endif()
    endforeach()
    list(APPEND _d3dx_includes "${_dxsdk}/Include")
else()
    list(APPEND _d3dx_includes "${_abi}/D3dxFallbackInclude")
endif()

# ---- TextureAtlasBuilder.dll: textureatlasbuilder.cmd -------------------------------------------------
# The atlas packer RendDX9's editor commands call through two exported creates. It links the CRT and STL
# statically (/MT), unlike the other plugin DLLs: nothing on its interface crosses a heap or a CRT, and
# the .tai writer's float formatting must keep VC7.1's round-half-up (legacy_stdio_float_rounding.obj),
# which a shared msvcp140 would not. See the script's header.
set(_tab_out "${CMAKE_BINARY_DIR}/textureatlasbuilder")
bf2_read_plugin_manifest(_tab_sources "${PROJECT_SOURCE_DIR}/Code/Build/textureatlasbuilder_sources.txt")
if(NOT _dxsdk)
    list(APPEND _tab_sources "${_abi}/DxErr9StandIn.cpp")
endif()
add_library(bf2_textureatlas_native SHARED ${_tab_sources})
add_dependencies(bf2_textureatlas_native bf2_plugin_implibs)
bf2_plugin_dll_properties(bf2_textureatlas_native TextureAtlasBuilder "${_tab_out}" MultiThreaded)
# /DBF2_IMAGE_TEXTUREATLASBUILDER admits the TUs and makes ITextureAtlas.h dllexport; /DNDEBUG: the
# shipped image is a release build.
target_compile_options(bf2_textureatlas_native PRIVATE
    /O2 /std:c++14 /permissive /EHsc /W4 /wd4100 /wd4127 "/FI${_bf2}/Core/BuildConfig.h")
if(BF2_BUILD_X64)
    target_compile_options(bf2_textureatlas_native PRIVATE /we4311 /we4312)
endif()
target_compile_definitions(bf2_textureatlas_native PRIVATE
    BF2_IMAGE_TEXTUREATLASBUILDER DEDICATED=0 WIN32 _WINDOWS NDEBUG
    _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
target_include_directories(bf2_textureatlas_native PRIVATE ${_d3dx_includes})
# No d3d9.lib: the device arrives through create(). d3dx9_25.dll is delay-loaded, as in RendDX9.
target_link_libraries(bf2_textureatlas_native PRIVATE "${_d3dx_lib}" kernel32.lib delayimp.lib)
if(_dxsdk)
    # DxErr9.lib is a VC7.1 object: its DXTrace needs __vsnprintf and MessageBox, which /OPT:REF drops.
    target_link_libraries(bf2_textureatlas_native PRIVATE
        "${_dxsdk}/Lib/${_machine}/DxErr9.lib" legacy_stdio_definitions.lib user32.lib)
endif()
target_link_options(bf2_textureatlas_native PRIVATE
    /OPT:REF /OPT:ICF /DELAYLOAD:d3dx9_25.dll legacy_stdio_float_rounding.obj)
if(NOT BF2_BUILD_X64)
    # x86: the shipped base, and the .def that pins the twelve exports to the shipped ordinals. x64 exports
    # the same twelve names through the dllexport classes in ITextureAtlas.h.
    target_link_options(bf2_textureatlas_native PRIVATE /BASE:0x10000000
        "/DEF:${_abi}/TextureAtlasBuilderExports.def")
endif()
bf2_plugin_abicheck(bf2_textureatlas_native textureatlasbuilder "${_tab_out}"
    "-DFALLBACK=${_fallback}" "-DABI=${_abi}")

# ---- RendDX9.dll: renddx9.cmd ----------------------------------------------------------------------
# The G2w seam flavour stays with the script (root CMakeLists.txt).
if(BF2_GFX_SEAM)
    return()
endif()

bf2_read_plugin_manifest(_sources "${PROJECT_SOURCE_DIR}/Code/Build/renddx9_sources.txt")
if(NOT _dxsdk)
    # DXGetErrorDescription9A without the SDK's DxErr9.lib.
    list(APPEND _sources "${_abi}/DxErr9StandIn.cpp")
endif()
set(_ffbink_sources)
if(BF2_BUILD_X64)
    # The Bink stand-in (Code/BF2/External/ffmpeg-bink/README.md): the C++ port with the image flags,
    # FFmpeg 7.1.1's Bink decoders (LGPL-2.1+) as optimised C11.
    list(APPEND _sources "${_abi}/BinkFFmpeg.cpp" "${_abi}/BinkAudioDirectSound.cpp")
    set(_ffb "${_bf2}/External/ffmpeg-bink")
    set(_ffbink_sources
        "${_ffb}/libavcodec/bink.c" "${_ffb}/libavcodec/binkdsp.c" "${_ffb}/libavcodec/binkaudio.c"
        "${_ffb}/libavcodec/wma_freqs.c" "${_ffb}/shim/ffbink_shim.c")
endif()

add_library(bf2_renddx9_native SHARED ${_sources} ${_ffbink_sources} "${_abi}/RendDX9.def")
add_dependencies(bf2_renddx9_native bf2_plugin_implibs)
bf2_plugin_dll_properties(bf2_renddx9_native RendDX9 "${_out}" MultiThreadedDLL)

# C++: the script's FLAGS. /DBF2_IMAGE_RENDDX9 admits the module TUs under Code/BF2; /DDEDICATED=0 with
# /FI Core/BuildConfig.h names the client image (AGENTS.md).
set(_cxx_options /std:c++14 /permissive /EHsc /W4 /wd4100 /wd4127 "/FI${_bf2}/Core/BuildConfig.h")
if(BF2_BUILD_X64)
    # A pointer truncated into a 32-bit integer is an error.
    list(APPEND _cxx_options /we4311 /we4312)
endif()
set(_cxx_defines BF2_IMAGE_RENDDX9 BF2_PLUGIN_ABI_NATIVE=1 _ITERATOR_DEBUG_LEVEL=0 DEDICATED=0
    WIN32 _WINDOWS _CRT_SECURE_NO_WARNINGS)
if(BF2_FRONTIER_DEFAULT_CONTINUE)
    list(APPEND _cxx_defines BF2_FRONTIER_DEFAULT_CONTINUE=1)
endif()
# [UI] ImGui's backends include imgui.h by its bare name (docs/lanes/ui.md).
set(_cxx_includes ${_d3dx_includes} "${_bf2}/External/imgui")
if(_dxsdk)
    target_link_directories(bf2_renddx9_native PRIVATE "${_dxsdk}/Lib/${_machine}")
endif()
target_compile_options(bf2_renddx9_native PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${_cxx_options}>")
target_compile_definitions(bf2_renddx9_native PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${_cxx_defines}>")
target_include_directories(bf2_renddx9_native PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${_cxx_includes}>")

# C: the script's FFBINK_FLAGS.
target_compile_options(bf2_renddx9_native PRIVATE
    "$<$<COMPILE_LANGUAGE:C>:/std:c11;/O2;/W3;/wd4244;/wd4267;/wd4018;/wd4146>")
target_compile_definitions(bf2_renddx9_native PRIVATE "$<$<COMPILE_LANGUAGE:C>:_CRT_SECURE_NO_WARNINGS>")
if(BF2_BUILD_X64)
    target_include_directories(bf2_renddx9_native PRIVATE "$<$<COMPILE_LANGUAGE:C>:${_bf2}/External/ffmpeg-bink/shim>")
endif()

# ---- link: the script's STEP 3 ---------------------------------------------------------------------
# d3dx9_25.dll is delay-loaded, so a missing (or 32-bit only) D3DX reaches BF2Render::init's message box
# instead of failing this DLL's load. TextureAtlasBuilder.dll is a static import, as in retail.
target_link_libraries(bf2_renddx9_native PRIVATE
    "${_d3dx_lib}" ${_renddx9_implibs} kernel32.lib user32.lib gdi32.lib d3d9.lib delayimp.lib)
target_link_options(bf2_renddx9_native PRIVATE
    /DELAYLOAD:d3dx9_25.dll)
if(NOT _dxsdk)
    # Effect.cpp asks for the SDK's dxerr9.lib by #pragma; the fallback compiles DxErr9StandIn.cpp.
    target_link_options(bf2_renddx9_native PRIVATE /NODEFAULTLIB:dxerr9.lib)
endif()
if(NOT BF2_BUILD_X64)
    # The shipped x86 image base, kept for side-by-side dumpbin comparison.
    target_link_options(bf2_renddx9_native PRIVATE /BASE:0x10000000)
endif()

# ---- the script's STEP 4: the image's ABI invariants, checked on every link ------------------------
bf2_plugin_abicheck(bf2_renddx9_native renddx9 "${_out}" "-DFALLBACK=${_fallback}" "-DABI=${_abi}")
