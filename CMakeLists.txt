cmake_minimum_required(VERSION 3.10)
project(video_player CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)

# --- FFmpeg ---
pkg_check_modules(FFMPEG REQUIRED libavformat libavcodec libavutil libswscale libswresample)

# --- Other dependencies ---
pkg_check_modules(GLFW REQUIRED glfw3)
pkg_check_modules(GLEW REQUIRED glew)
pkg_check_modules(SDL2 REQUIRED sdl2)

include_directories(
    ${FFMPEG_INCLUDE_DIRS}
    ${GLFW_INCLUDE_DIRS}
    ${GLEW_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIRS}
    imgui
    imgui/backends
)

add_executable(video_player
    video_player.cpp
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/backends/imgui_impl_glfw.cpp
    imgui/backends/imgui_impl_opengl3.cpp
)

# Use the library directories and libraries from pkg-config
link_directories(
    ${FFMPEG_LIBRARY_DIRS}
)

# Proper linking order for FFmpeg and other dependencies
target_link_libraries(video_player
    ${FFMPEG_LIBRARIES}
    ${GLFW_LIBRARIES}
    ${GLEW_LIBRARIES}
    ${SDL2_LIBRARIES}
    GL
    m
    pthread
    dl
)
