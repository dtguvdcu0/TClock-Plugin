cmake_minimum_required(VERSION 3.20)
project(TCapture LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    # Use the dynamic CRT so deployment can rely on the VC++ redistributable.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

add_executable(TCapture WIN32
    screenshot.cpp
    gui_main.cpp
    settings.cpp
    resources.rc
)

target_link_libraries(TCapture PRIVATE
    Gdi32
    User32
    Shell32
    Comctl32
    Shcore
    Shlwapi
    Ole32
    OleAut32
    Windowscodecs
)
