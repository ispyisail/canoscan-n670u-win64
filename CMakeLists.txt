cmake_minimum_required(VERSION 3.20)
project(n670u C CXX)

# Static CRT: the WIA service loads our DLL, so avoid any VC++ redist dependency.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Build for x64 (the WIA service on 64-bit Windows is 64-bit).")
endif()

# ---------------------------------------------------------------------------
# SANE plustek backend + Win32 sanei layer
# ---------------------------------------------------------------------------
set(SANE_SRC
  src/sane/backend/plustek.c          # #includes all plustek-usb*.c
  src/sane/sanei/sanei_lm983x.c
  src/sane/sanei/sanei_constrain_value.c)

add_library(plustek STATIC ${SANE_SRC} src/win32/sanei_win32.c src/host/n670_scan.c)
target_include_directories(plustek PUBLIC
  ${CMAKE_SOURCE_DIR}/src/win32
  ${CMAKE_SOURCE_DIR}/src/sane/include
  ${CMAKE_SOURCE_DIR}/src/host)
target_compile_definitions(plustek PRIVATE
  _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS WIN32_LEAN_AND_MEAN)
# SANE sources get the POSIX shim force-included
set_source_files_properties(${SANE_SRC} PROPERTIES
  COMPILE_OPTIONS "/FIposix_compat.h")
# /UNDEBUG: SANE compiles DBG() away under NDEBUG; keep it (runtime-gated)
target_compile_options(plustek PRIVATE
  /UNDEBUG /W3 /wd4244 /wd4267 /wd4018 /wd4305 /wd4996 /wd4101 /wd4102 /wd4146)
target_link_libraries(plustek PUBLIC setupapi)

# ---------------------------------------------------------------------------
# n670scan.exe - command-line test tool
# ---------------------------------------------------------------------------
add_executable(n670scan tools/n670scan.c)
target_link_libraries(n670scan PRIVATE plustek)

# ---------------------------------------------------------------------------
# n670mdrv.dll - WIA microdriver loaded by wiafbdrv.dll
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_SOURCE_DIR}/wia/n670mdrv.cpp)
  add_library(n670mdrv SHARED
    wia/n670mdrv.cpp wia/n670mdrv.def wia/n670mdrv.rc)
  target_link_libraries(n670mdrv PRIVATE plustek ole32 advapi32)
  target_compile_definitions(n670mdrv PRIVATE UNICODE _UNICODE)
endif()

# usbprobe.exe - usbscan.sys pipe/register diagnostics
add_executable(usbprobe tools/usbprobe.c)
target_link_libraries(usbprobe PRIVATE setupapi)
