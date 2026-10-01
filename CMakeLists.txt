cmake_minimum_required(VERSION 3.25)
project(DayZVR LANGUAGES C CXX)
# GetProcAddress returns FARPROC; these Windows API boundaries must cast it to
# the actual exported signature. Clang's type-mismatch warning cannot model it.
set_source_files_properties(dxgi/dxgi_proxy.cpp common/openxr_host.cpp tests/dxgi_smoke.cpp
  PROPERTIES COMPILE_OPTIONS "/clang:-Wno-cast-function-type-mismatch")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
set(OPENXR_SDK "" CACHE PATH "OpenXR SDK checkout with generated include/ headers")
set(OPENXR_LOADER "" CACHE FILEPATH "Windows x64 openxr_loader.dll")
if(NOT EXISTS "${OPENXR_SDK}/include/openxr/openxr.h" OR NOT EXISTS "${OPENXR_LOADER}")
  message(FATAL_ERROR "Set OPENXR_SDK and OPENXR_LOADER to existing files")
endif()
find_program(LLVM_DLLTOOL NAMES llvm-dlltool-19 REQUIRED)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/openxr_loader.lib"
  COMMAND "${LLVM_DLLTOOL}" -m i386:x86-64
    -d "${OPENXR_SDK}/src/loader/openxr-loader.def"
    -l "${CMAKE_CURRENT_BINARY_DIR}/openxr_loader.lib"
  DEPENDS "${OPENXR_SDK}/src/loader/openxr-loader.def" VERBATIM)
add_custom_target(openxr_import DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/openxr_loader.lib")
add_library(minhook STATIC third_party/minhook/src/buffer.c third_party/minhook/src/hook.c
  third_party/minhook/src/trampoline.c third_party/minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC third_party/minhook/include)
add_library(vr_common STATIC common/logging.cpp common/stereo_state.cpp
  common/openxr_host.cpp common/debug_frame_source.cpp common/dayz_frame_source.cpp
  common/dayz_runtime_probe.cpp common/debug_bridge.cpp)
target_include_directories(vr_common PUBLIC common "${OPENXR_SDK}/include")
target_compile_definitions(vr_common PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX
  XR_USE_PLATFORM_WIN32 XR_USE_GRAPHICS_API_D3D11)
target_link_libraries(vr_common PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/openxr_loader.lib"
  d3d11 d3dcompiler dxguid user32 minhook)
add_dependencies(vr_common openxr_import)
add_library(dxgi SHARED dxgi/dllmain.cpp dxgi/dxgi_proxy.cpp dxgi/swapchain_hooks.cpp
  dxgi/dxgi.def)
target_link_libraries(dxgi PRIVATE vr_common minhook)
set_target_properties(dxgi PROPERTIES PREFIX "")
add_executable(xr_probe xr_probe/main.cpp)
target_link_libraries(xr_probe PRIVATE vr_common dxgi.lib)
add_executable(dxgi_smoke WIN32 tests/dxgi_smoke.cpp)
target_link_libraries(dxgi_smoke PRIVATE d3d11 dxguid user32)
# Optional runtime control plugin. Deliberately not linked to vr_common: it only
# shares the C header and is loaded by dxgi.dll when [debug] enabled=true.
add_library(dayz_openxr_debug SHARED debug_plugin/dayz_openxr_debug.cpp)
target_compile_definitions(dayz_openxr_debug PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(dayz_openxr_debug PRIVATE ws2_32)
set_target_properties(dayz_openxr_debug PROPERTIES PREFIX "")
foreach(target vr_common dxgi xr_probe dxgi_smoke dayz_openxr_debug)
  target_compile_options(${target} PRIVATE /W4 /WX /EHsc /Zi)
  target_link_options(${target} PRIVATE /DEBUG)
endforeach()
configure_file("${OPENXR_LOADER}" openxr_loader.dll COPYONLY)
configure_file(dayz_openxr.ini dayz_openxr.ini COPYONLY)
