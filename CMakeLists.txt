cmake_minimum_required(VERSION 3.10)
project(video_player CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)

# --- FFmpeg libraries ---
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVUTIL REQUIRED libavutil)
pkg_check_modules(SWSCALE REQUIRED libswscale)
pkg_check_modules(SWRESAMPLE QUIET libswresample)

# Try using pkg-config first
if (PkgConfig_FOUND)
    pkg_check_modules(FFMPEG libavformat libavcodec libavutil libswscale libswresample)
endif()

# If pkg-config failed, fall back to manual linking
if (NOT FFMPEG_FOUND)
    message(WARNING "pkg-config could not find FFmpeg; falling back to manual linking")
    set(FFMPEG_LIBRARIES avformat avcodec avutil swscale swresample)
    set(FFMPEG_INCLUDE_DIRS /usr/local/include /usr/include/x86_64-linux-gnu)
    set(FFMPEG_LIBRARY_DIRS /usr/local/lib /usr/lib/x86_64-linux-gnu)
endif()

# --- GLFW, GLEW, SDL2 ---
pkg_check_modules(GLFW REQUIRED glfw3)
pkg_check_modules(GLEW REQUIRED glew)
pkg_check_modules(SDL2 REQUIRED sdl2)

include_directories(
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
    ${SWRESAMPLE_INCLUDE_DIRS}
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

# --- Link all necessary libraries ---
target_link_libraries(video_player
    ${AVFORMAT_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${SWSCALE_LIBRARIES}
    ${SWRESAMPLE_LIBRARIES}
    ${GLFW_LIBRARIES}
    ${GLEW_LIBRARIES}
    ${SDL2_LIBRARIES}
    GL
    m
    pthread
)
