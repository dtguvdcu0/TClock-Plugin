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

# Stage only shipped language files, never local backups or settings.
foreach(language IN ITEMS en ja)
    add_custom_command(TARGET TCapture POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:TCapture>/tcapture/lang/${language}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/lang/${language}/strings.ini"
            "$<TARGET_FILE_DIR:TCapture>/tcapture/lang/${language}/strings.ini"
        VERBATIM
    )
endforeach()
