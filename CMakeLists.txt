# Faders OBS plugin — one codebase for macOS and Windows. Only the WebSocket transport differs
# (src/ws-apple.m vs src/ws-win.c); capture, encoding, protocol and UI are shared C.
#
# Dependencies live in .deps/ (scripts/fetch-deps.sh on macOS, scripts/fetch-deps-windows.ps1 on
# Windows, scripts/cross-windows.sh to cross-compile the Windows DLL from macOS).
cmake_minimum_required(VERSION 3.24)
project(faders VERSION 0.3.1 LANGUAGES C)
# Plugin Manager metadata (OBS 32+ reads data/manifest.json): written from the project version so
# it can't drift; committed so the installers, which package data/, always include it.
configure_file("${CMAKE_SOURCE_DIR}/cmake/manifest.json.in" "${CMAKE_SOURCE_DIR}/data/manifest.json" @ONLY NEWLINE_STYLE UNIX)

set(OBS_VERSION "32.2.2" CACHE STRING "OBS Studio version to build against")
set(OBS_SRC "${CMAKE_SOURCE_DIR}/.deps/obs-studio-src" CACHE PATH "obs-studio source (headers)")
# libobs headers use SIMDe on non-x86 (header-only).
set(SIMDE_INCLUDE "${CMAKE_SOURCE_DIR}/.deps/simde" CACHE PATH "SIMDe headers")
set(CMAKE_C_STANDARD 17)

# libobs headers include obsconfig.h, which OBS generates at build time.
set(GEN "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GEN}")
file(WRITE "${GEN}/obsconfig.h" "#pragma once\n#define OBS_RELEASE_CANDIDATE 0\n#define OBS_BETA 0\n")

set(FADERS_SOURCES src/plugin-main.c src/uplink.c)
set(OBS_INCLUDES "${GEN}" "${OBS_SRC}/libobs" "${OBS_SRC}/frontend/api" "${SIMDE_INCLUDE}")

if(APPLE)
  enable_language(OBJC)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0") # OBS 32 requires macOS 13
  if(NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64")
  endif()
  set(OBS_APP "${CMAKE_SOURCE_DIR}/.deps/OBS.app" CACHE PATH "OBS.app providing libobs")
  set(OPUS_ROOT "${CMAKE_SOURCE_DIR}/.deps/opus" CACHE PATH "Static libopus (macOS 13 target)")
  set(FRAMEWORKS "${OBS_APP}/Contents/Frameworks")

  add_library(faders MODULE ${FADERS_SOURCES} src/ws-apple.m)
  set_source_files_properties(src/ws-apple.m PROPERTIES COMPILE_FLAGS "-fobjc-arc")
  target_link_options(faders PRIVATE "-F${FRAMEWORKS}")
  target_link_libraries(faders PRIVATE "-framework libobs" "${FRAMEWORKS}/obs-frontend-api.dylib" "${OPUS_ROOT}/lib/libopus.a" "-framework Foundation")
  set_target_properties(faders PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION plugin
    MACOSX_BUNDLE_GUI_IDENTIFIER tv.faders.obs-plugin
    MACOSX_BUNDLE_BUNDLE_NAME faders
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION})
  # Locale data lives in the bundle's Resources, where OBS looks for module data on macOS.
  add_custom_command(TARGET faders POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/data" "$<TARGET_BUNDLE_CONTENT_DIR:faders>/Resources"
    COMMAND codesign --force --sign - "$<TARGET_BUNDLE_DIR:faders>"
    COMMENT "Copying data and ad-hoc signing faders.plugin")
  set(PLUGIN_BIN "$<TARGET_BUNDLE_DIR:faders>/Contents/MacOS/faders")
  set(PLUGIN_DATA "$<TARGET_BUNDLE_DIR:faders>/Contents/Resources")
  set(GRAPHICS_MODULE "${FRAMEWORKS}/libobs-opengl.dylib")

elseif(WIN32)
  # Import libraries generated from the official OBS release DLLs (no libobs build needed).
  set(OBS_WIN_LIBDIR "${CMAKE_SOURCE_DIR}/.deps/win/lib" CACHE PATH "obs/obs-frontend-api/w32-pthreads import libs")
  set(OPUS_ROOT "${CMAKE_SOURCE_DIR}/.deps/win/opus" CACHE PATH "Static libopus for Windows")
  list(APPEND OBS_INCLUDES "${OBS_SRC}/deps/w32-pthreads")

  add_library(faders MODULE ${FADERS_SOURCES} src/ws-win.c)
  target_compile_definitions(faders PRIVATE _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN)
  if(MSVC)
    set(OBS_IMPORT_LIBS "${OBS_WIN_LIBDIR}/obs.lib" "${OBS_WIN_LIBDIR}/obs-frontend-api.lib" "${OBS_WIN_LIBDIR}/w32-pthreads.lib")
    set(OPUS_LIB "${OPUS_ROOT}/lib/opus.lib")
  else()
    set(OBS_IMPORT_LIBS "${OBS_WIN_LIBDIR}/libobs.dll.a" "${OBS_WIN_LIBDIR}/libobs-frontend-api.dll.a" "${OBS_WIN_LIBDIR}/libw32-pthreads.dll.a")
    set(OPUS_LIB "${OPUS_ROOT}/lib/libopus.a")
    # No libgcc / libwinpthread DLLs next to the plugin: OBS doesn't ship them.
    target_link_options(faders PRIVATE -static-libgcc -static)
    target_sources(faders PRIVATE src/faders.def)
    set(MINGW_SHIM "-include${CMAKE_SOURCE_DIR}/compat/mingw-msvc-intrinsics.h")
    target_compile_options(faders PRIVATE ${MINGW_SHIM})
  endif()
  target_link_libraries(faders PRIVATE ${OBS_IMPORT_LIBS} "${OPUS_LIB}" winhttp)
  # OBS layout: obs-plugins/64bit/faders.dll + data/obs-plugins/faders/locale/*.ini
  set_target_properties(faders PROPERTIES PREFIX "" OUTPUT_NAME faders)
  add_custom_command(TARGET faders POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/data" "${CMAKE_BINARY_DIR}/data/obs-plugins/faders"
    COMMENT "Staging plugin data")
  set(PLUGIN_BIN "$<TARGET_FILE:faders>")
  set(PLUGIN_DATA "${CMAKE_BINARY_DIR}/data/obs-plugins/faders")
  set(GRAPHICS_MODULE "libobs-d3d11")
else()
  message(FATAL_ERROR "Linux is not supported yet (needs a libcurl WebSocket transport in src/ws-curl.c)")
endif()

target_compile_definitions(faders PRIVATE FDR_VERSION_STR="${PROJECT_VERSION}")
target_include_directories(faders PRIVATE ${OBS_INCLUDES} "${OPUS_ROOT}/include")
if(NOT MSVC)
  target_compile_options(faders PRIVATE -Wall -Wno-unused-parameter $<$<COMPILE_LANGUAGE:C>:-Wno-deprecated-declarations>)
endif()

# Headless test host: real libobs, no GUI; plays WAVs into tracks 2/3/4 and loads the plugin.
add_executable(faders-headless test/headless.c)
target_include_directories(faders-headless PRIVATE ${OBS_INCLUDES})
target_compile_definitions(faders-headless PRIVATE FADERS_PLUGIN_BIN="${PLUGIN_BIN}" FADERS_PLUGIN_DATA="${PLUGIN_DATA}" FADERS_GRAPHICS_MODULE="${GRAPHICS_MODULE}")
if(APPLE)
  target_link_options(faders-headless PRIVATE "-F${FRAMEWORKS}" "-Wl,-rpath,${FRAMEWORKS}")
  target_link_libraries(faders-headless PRIVATE "-framework libobs")
else()
  target_link_libraries(faders-headless PRIVATE ${OBS_IMPORT_LIBS})
  if(NOT MSVC)
    target_link_options(faders-headless PRIVATE -static-libgcc -static)
    target_compile_options(faders-headless PRIVATE ${MINGW_SHIM})
  endif()
endif()
add_dependencies(faders-headless faders)
