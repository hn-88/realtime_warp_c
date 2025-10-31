cmake_minimum_required(VERSION 3.16)
project(VideoPlayer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- Compiler warnings & optimization ----
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-O2 -Wall)

# ---- ImGui sources ----
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

# ---- Find system packages ----
find_package(PkgConfig REQUIRED)

# FFmpeg (via pkg-config)
pkg_check_modules(FFMPEG REQUIRED
    libavformat
    libavcodec
    libavutil
    libswscale
    libswresample
)

# GLFW, GLEW, SDL2 (via system find or pkg-config)
find_package(OpenGL REQUIRED)
pkg_check_modules(GLFW REQUIRED glfw3)
pkg_check_modules(GLEW REQUIRED glew)
pkg_check_modules(SDL2 REQUIRED sdl2)

# ---- Target ----
add_executable(video_player
    video_player.cpp
    ${IMGUI_SOURCES}
)

# ---- Include directories ----
target_include_directories(video_player PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${FFMPEG_INCLUDE_DIRS}
    ${GLFW_INCLUDE_DIRS}
    ${GLEW_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIRS}
    ${OPENGL_INCLUDE_DIR}
)

# ---- Link libraries ----
target_link_libraries(video_player PRIVATE
    ${FFMPEG_LIBRARIES}
    ${GLFW_LIBRARIES}
    ${GLEW_LIBRARIES}
    ${SDL2_LIBRARIES}
    OpenGL::GL
    m
)

# ---- Ensure linker can find shared libs ----
link_directories(
    ${FFMPEG_LIBRARY_DIRS}
    ${GLFW_LIBRARY_DIRS}
    ${GLEW_LIBRARY_DIRS}
    ${SDL2_LIBRARY_DIRS}
)
