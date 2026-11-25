# Faders OBS plugin — one codebase for macOS and Windows. Only the WebSocket transport differs
# (src/ws-apple.m vs src/ws-win.c); capture, encoding, protocol and UI are shared C.
#
# Dependencies live in .deps/ (scripts/fetch-deps.sh on macOS, scripts/fetch-deps-windows.ps1 on
# Windows, scripts/cross-windows.sh to cross-compile the Windows DLL from macOS).
cmake_minimum_required(VERSION 3.24)
project(faders VERSION 0.4.1 LANGUAGES C)
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

set(FADERS_SOURCES src/plugin-main.c src/uplink.c src/mixer.c src/placement.c src/logring.c src/redact.c src/report.c src/sysinfo.c)
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
  set(OBS_TRANSITIONS_BIN "${OBS_APP}/Contents/PlugIns/obs-transitions.plugin/Contents/MacOS/obs-transitions")
  set(OBS_TRANSITIONS_DATA "${OBS_APP}/Contents/PlugIns/obs-transitions.plugin/Contents/Resources")

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
  target_link_libraries(faders PRIVATE ${OBS_IMPORT_LIBS} "${OPUS_LIB}" winhttp advapi32)
  # OBS layout: obs-plugins/64bit/faders.dll + data/obs-plugins/faders/locale/*.ini
  set_target_properties(faders PROPERTIES PREFIX "" OUTPUT_NAME faders)
  add_custom_command(TARGET faders POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/data" "${CMAKE_BINARY_DIR}/data/obs-plugins/faders"
    COMMENT "Staging plugin data")
  set(PLUGIN_BIN "$<TARGET_FILE:faders>")
  set(PLUGIN_DATA "${CMAKE_BINARY_DIR}/data/obs-plugins/faders")
  set(GRAPHICS_MODULE "libobs-d3d11")
  set(OBS_TRANSITIONS_BIN "")
  set(OBS_TRANSITIONS_DATA "")
else()
  message(FATAL_ERROR "Linux is not supported yet (needs a libcurl WebSocket transport in src/ws-curl.c)")
endif()

target_compile_definitions(faders PRIVATE FDR_VERSION_STR="${PROJECT_VERSION}")
target_include_directories(faders PRIVATE ${OBS_INCLUDES} "${OPUS_ROOT}/include")
if(NOT MSVC)
  target_compile_options(faders PRIVATE -Wall -Wno-unused-parameter $<$<COMPILE_LANGUAGE:C>:-Wno-deprecated-declarations>)
endif()

# Headless test host: real libobs, no GUI; plays WAVs as sources on the streamed track and loads the plugin.
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

# Transport shutdown: no callback after fdr_ws_destroy() returns (test/wstest.c, with test/ws-flood.mjs).
if(APPLE)
  add_executable(faders-wstest test/wstest.c src/ws-apple.m)
  target_compile_definitions(faders-wstest PRIVATE FDR_VERSION_STR="${PROJECT_VERSION}")
  target_link_libraries(faders-wstest PRIVATE "-framework Foundation")
endif()

# Mixer check: proves the libobs behaviour the virtual mixer relies on (test/mixcheck.c), no plugin.
add_executable(faders-mixcheck test/mixcheck.c)
target_include_directories(faders-mixcheck PRIVATE ${OBS_INCLUDES})
target_compile_definitions(faders-mixcheck PRIVATE FADERS_GRAPHICS_MODULE="${GRAPHICS_MODULE}" FADERS_OBS_TRANSITIONS_BIN="${OBS_TRANSITIONS_BIN}" FADERS_OBS_TRANSITIONS_DATA="${OBS_TRANSITIONS_DATA}")
if(APPLE)
  target_link_options(faders-mixcheck PRIVATE "-F${FRAMEWORKS}" "-Wl,-rpath,${FRAMEWORKS}")
  target_link_libraries(faders-mixcheck PRIVATE "-framework libobs")
else()
  target_link_libraries(faders-mixcheck PRIVATE ${OBS_IMPORT_LIBS})
  if(NOT MSVC)
    target_link_options(faders-mixcheck PRIVATE -static-libgcc -static)
    target_compile_options(faders-mixcheck PRIVATE ${MINGW_SHIM})
  endif()
endif()

# Mixer test: the virtual mixer through the real plugin (test/mixtest.c).
add_executable(faders-mixtest test/mixtest.c test/panel-enumeration.c test/panel-migration.c)
target_include_directories(faders-mixtest PRIVATE ${OBS_INCLUDES})
target_compile_definitions(faders-mixtest PRIVATE FADERS_PLUGIN_BIN="${PLUGIN_BIN}" FADERS_PLUGIN_DATA="${PLUGIN_DATA}" FADERS_GRAPHICS_MODULE="${GRAPHICS_MODULE}" FADERS_OBS_TRANSITIONS_BIN="${OBS_TRANSITIONS_BIN}" FADERS_OBS_TRANSITIONS_DATA="${OBS_TRANSITIONS_DATA}")
if(APPLE)
  target_link_options(faders-mixtest PRIVATE "-F${FRAMEWORKS}" "-Wl,-rpath,${FRAMEWORKS}")
  target_link_libraries(faders-mixtest PRIVATE "-framework libobs")
else()
  target_link_libraries(faders-mixtest PRIVATE ${OBS_IMPORT_LIBS})
  if(NOT MSVC)
    target_link_options(faders-mixtest PRIVATE -static-libgcc -static)
    target_compile_options(faders-mixtest PRIVATE ${MINGW_SHIM})
  endif()
endif()
add_dependencies(faders-mixtest faders)

# Panel listing tests: "What's on each fader" when a fader has 2+ sources, one executable per
# test/panel-listing-*.c (shared harness: test/panel-listing.h).
file(GLOB PANEL_LISTING_TESTS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/test/panel-listing-*.c")
foreach(test_src ${PANEL_LISTING_TESTS})
  get_filename_component(test_name "${test_src}" NAME_WE)
  add_executable(faders-${test_name} "${test_src}")
  target_include_directories(faders-${test_name} PRIVATE ${OBS_INCLUDES})
  target_compile_definitions(faders-${test_name} PRIVATE FADERS_PLUGIN_BIN="${PLUGIN_BIN}" FADERS_PLUGIN_DATA="${PLUGIN_DATA}" FADERS_GRAPHICS_MODULE="${GRAPHICS_MODULE}")
  if(APPLE)
    target_link_options(faders-${test_name} PRIVATE "-F${FRAMEWORKS}" "-Wl,-rpath,${FRAMEWORKS}")
    target_link_libraries(faders-${test_name} PRIVATE "-framework libobs")
  else()
    target_link_libraries(faders-${test_name} PRIVATE ${OBS_IMPORT_LIBS})
    if(NOT MSVC)
      target_link_options(faders-${test_name} PRIVATE -static-libgcc -static)
      target_compile_options(faders-${test_name} PRIVATE ${MINGW_SHIM})
    endif()
  endif()
  add_dependencies(faders-${test_name} faders)
endforeach()

enable_testing()
add_test(NAME panel-enumeration COMMAND faders-mixtest enumeration)
foreach(scenario IN ITEMS legacy track-only map-and-track)
  add_test(NAME panel-migration-${scenario}
    COMMAND faders-mixtest --panel-migration "${CMAKE_CURRENT_BINARY_DIR}/panel-migration-${scenario}" "${scenario}")
endforeach()

# Source identity: kinds, renames, duplicate/unicode/long names; dialog and mixer agree (test/identity.c).
add_executable(faders-identity test/identity.c)
target_include_directories(faders-identity PRIVATE ${OBS_INCLUDES})
target_compile_definitions(faders-identity PRIVATE FADERS_PLUGIN_BIN="${PLUGIN_BIN}" FADERS_PLUGIN_DATA="${PLUGIN_DATA}" FADERS_GRAPHICS_MODULE="${GRAPHICS_MODULE}")
if(APPLE)
  target_link_options(faders-identity PRIVATE "-F${FRAMEWORKS}" "-Wl,-rpath,${FRAMEWORKS}")
  target_link_libraries(faders-identity PRIVATE "-framework libobs")
else()
  target_link_libraries(faders-identity PRIVATE ${OBS_IMPORT_LIBS})
  if(NOT MSVC)
    target_link_options(faders-identity PRIVATE -static-libgcc -static)
    target_compile_options(faders-identity PRIVATE ${MINGW_SHIM})
  endif()
endif()
add_dependencies(faders-identity faders)

# Placement model on its own (src/placement.c, no OBS): value codec, saved vs effective, checks.
add_executable(faders-placement-unit test/placement-unit.c)
add_test(NAME placement-unit COMMAND faders-placement-unit)
add_test(NAME identity COMMAND faders-identity)

# Placement through the real plugin: nothing is placed for you, moves are checked, the window
# follows the sources (test/placement.c). One process per scenario.
add_executable(faders-placement test/placement.c)
target_include_directories(faders-placement PRIVATE ${OBS_INCLUDES})
target_compile_definitions(faders-placement PRIVATE FADERS_PLUGIN_BIN="${PLUGIN_BIN}" FADERS_PLUGIN_DATA="${PLUGIN_DATA}" FADERS_GRAPHICS_MODULE="${GRAPHICS_MODULE}")
if(APPLE)
  target_link_options(faders-placement PRIVATE "-F${FRAMEWORKS}" "-Wl,-rpath,${FRAMEWORKS}")
  target_link_libraries(faders-placement PRIVATE "-framework libobs")
else()
  target_link_libraries(faders-placement PRIVATE ${OBS_IMPORT_LIBS})
  if(NOT MSVC)
    target_link_options(faders-placement PRIVATE -static-libgcc -static)
    target_compile_options(faders-placement PRIVATE ${MINGW_SHIM})
  endif()
endif()
add_dependencies(faders-placement faders)
foreach(scenario model legacy duplicate validate capacity sharkey2 refresh cancel firstrun firstrun-placed)
  add_test(NAME placement-${scenario} COMMAND faders-placement ${scenario})
endforeach()

# Debug report core, no OBS (src/redact.c, src/logring.c, src/report.c, src/sysinfo.c): the
# redaction table, the log ring under concurrent writers, the report against golden files.
find_package(Threads REQUIRED)
add_executable(faders-redact-test test/redact-test.c)
add_executable(faders-logring-test test/logring-test.c)
target_link_libraries(faders-logring-test PRIVATE Threads::Threads)
add_executable(faders-report-test test/report-test.c)
target_link_libraries(faders-report-test PRIVATE Threads::Threads)
if(WIN32)
  target_link_libraries(faders-report-test PRIVATE advapi32)
endif()
add_test(NAME debug-redact COMMAND faders-redact-test)
add_test(NAME debug-logring COMMAND faders-logring-test)
add_test(NAME debug-report-golden COMMAND faders-report-test "${CMAKE_SOURCE_DIR}/test/report")

# The debug report through the real plugin (test/debug-report.c).
add_executable(faders-debug-report test/debug-report.c)
target_include_directories(faders-debug-report PRIVATE ${OBS_INCLUDES})
target_compile_definitions(faders-debug-report PRIVATE FADERS_PLUGIN_BIN="${PLUGIN_BIN}" FADERS_PLUGIN_DATA="${PLUGIN_DATA}" FADERS_GRAPHICS_MODULE="${GRAPHICS_MODULE}")
if(APPLE)
  target_link_options(faders-debug-report PRIVATE "-F${FRAMEWORKS}" "-Wl,-rpath,${FRAMEWORKS}")
  target_link_libraries(faders-debug-report PRIVATE "-framework libobs")
else()
  target_link_libraries(faders-debug-report PRIVATE ${OBS_IMPORT_LIBS})
  if(NOT MSVC)
    target_link_options(faders-debug-report PRIVATE -static-libgcc -static)
    target_compile_options(faders-debug-report PRIVATE ${MINGW_SHIM})
  endif()
endif()
add_dependencies(faders-debug-report faders)
add_test(NAME debug-report-plugin COMMAND faders-debug-report)
