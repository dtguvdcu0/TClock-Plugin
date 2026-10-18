cmake_minimum_required(VERSION 3.20)
project(TCalendar LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(WEBVIEW2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2")
set(WEBVIEW2_INCLUDE_DIR "${WEBVIEW2_ROOT}/include")
set(WEBVIEW2_LOADER_STATIC_LIB "${WEBVIEW2_ROOT}/lib/x64/WebView2LoaderStatic.lib")
set(WEBVIEW2_LOADER_DLL "${WEBVIEW2_ROOT}/bin/x64/WebView2Loader.dll")
set(WEB_ASSETS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/web")
set(PROVIDER_ASSETS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/providers")

if(NOT EXISTS "${WEBVIEW2_INCLUDE_DIR}/WebView2.h")
  message(FATAL_ERROR "WebView2 header not found: ${WEBVIEW2_INCLUDE_DIR}/WebView2.h")
endif()
if(NOT EXISTS "${WEBVIEW2_LOADER_STATIC_LIB}")
  message(FATAL_ERROR "WebView2 loader static lib not found: ${WEBVIEW2_LOADER_STATIC_LIB}")
endif()
if(NOT EXISTS "${WEBVIEW2_LOADER_DLL}")
  message(FATAL_ERROR "WebView2 loader DLL not found: ${WEBVIEW2_LOADER_DLL}")
endif()
if(NOT EXISTS "${WEB_ASSETS_DIR}/default/index.html")
  message(FATAL_ERROR "Web assets directory not found: ${WEB_ASSETS_DIR}/default/index.html")
endif()
if(NOT EXISTS "${PROVIDER_ASSETS_DIR}/jp-public-holiday.ini")
  message(FATAL_ERROR "Provider assets not found: ${PROVIDER_ASSETS_DIR}/jp-public-holiday.ini")
endif()
if(NOT EXISTS "${PROVIDER_ASSETS_DIR}/jp-public-holiday.js")
  message(FATAL_ERROR "Provider assets not found: ${PROVIDER_ASSETS_DIR}/jp-public-holiday.js")
endif()

add_executable(TCalendar WIN32
  host/main.cpp
  host/tcalendar_host.cpp
  host/bridge_contract.cpp
  host/task_store.cpp
  host/json_strict.cpp
  resources.rc
)

target_include_directories(TCalendar PRIVATE
  host
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${WEBVIEW2_INCLUDE_DIR}
)

target_link_libraries(TCalendar PRIVATE
  "${WEBVIEW2_LOADER_STATIC_LIB}"
  User32
  Gdi32
  Ole32
  OleAut32
  windowsapp
  winsqlite3
)

add_custom_command(TARGET TCalendar POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${WEBVIEW2_LOADER_DLL}"
          "$<TARGET_FILE_DIR:TCalendar>/WebView2Loader.dll"
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:TCalendar>/tcalendar/template"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${WEB_ASSETS_DIR}"
          "$<TARGET_FILE_DIR:TCalendar>/tcalendar/template"
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:TCalendar>/tcalendar/providers"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${PROVIDER_ASSETS_DIR}"
          "$<TARGET_FILE_DIR:TCalendar>/tcalendar/providers"
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:TCalendar>/tcalendar/data"
)

if (MSVC)
  target_compile_options(TCalendar PRIVATE /W4 /utf-8)
  target_compile_definitions(TCalendar PRIVATE UNICODE _UNICODE)
endif()
