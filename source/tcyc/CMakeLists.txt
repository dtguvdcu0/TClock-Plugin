cmake_minimum_required(VERSION 3.20)
project(TCycle LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

add_executable(TCycle WIN32
    main.cpp
    config.cpp
    state.cpp
    scheduler.cpp
    runner.cpp
    watchdog.cpp
    hotkey.cpp
    log.cpp
    gui_main.cpp
    tcycle.rc
    ini_utf8_util.cpp
)

target_link_libraries(TCycle PRIVATE
    User32
    Advapi32
    Shell32
)

if(MSVC)
    target_compile_options(TCycle PRIVATE /W4 /utf-8)
    target_compile_definitions(TCycle PRIVATE UNICODE _UNICODE)
endif()

# Stage only shipped language files, never local backups or settings.
foreach(language IN ITEMS en ja)
    add_custom_command(TARGET TCycle POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:TCycle>/tcyc/lang/${language}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/lang/${language}/strings.ini"
            "$<TARGET_FILE_DIR:TCycle>/tcyc/lang/${language}/strings.ini"
        VERBATIM
    )
endforeach()
