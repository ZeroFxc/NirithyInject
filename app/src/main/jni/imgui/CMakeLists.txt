# CMakeLists.txt for building ImGui as a library

cmake_minimum_required(VERSION 3.0)

project(imgui)

set(IMGUI_SOURCES
    imgui_widgets.cpp
    imgui_draw.cpp
    imgui_demo.cpp
    imgui.cpp
    imgui_tables.cpp
    imstb_textedit.h
    imstb_truetype.h
    imstb_rectpack.h
    backends/imgui_impl_opengl3.cpp
    backends/imgui_impl_android.cpp
    backends/imgui_impl_vulkan.cpp
)

add_library(${PROJECT_NAME} STATIC ${IMGUI_SOURCES})

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(${PROJECT_NAME} PRIVATE "-w")
target_compile_definitions(${PROJECT_NAME} PUBLIC IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
