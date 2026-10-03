cmake_minimum_required(VERSION 3.10)

project(Tool)

# Set the C++ standard to C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Add flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -s -Wno-error=format-security -fvisibility=hidden -fpermissive -fexceptions -Wno-error=c++11-narrowing -Wall")
set(LINKER_FLAGS "-ffixed-x18 -Wl,--hash-style=both")
set(C_FLAGS "-Werror=format -fdata-sections -ffunction-sections")

# print out compiler information
message("CMAKE_CXX_COMPILER_ID: ${CMAKE_CXX_COMPILER_ID}")
message("CMAKE_CXX_COMPILER_VERSION: ${CMAKE_CXX_COMPILER_VERSION}")
message("${CMAKE_BINARY_DIR}")
message("${CMAKE_CURRENT_LIST_DIR}")

set(C_FLAGS "${C_FLAGS} -O3 -fvisibility=hidden -fvisibility-inlines-hidden")
set(LINKER_FLAGS "${LINKER_FLAGS} -Wl,-exclude-libs,ALL -Wl,--gc-sections -Wl,--strip-all")

if (NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    # set(C_FLAGS "${C_FLAGS} -O2 -fvisibility=hidden -fvisibility-inlines-hidden")
    # set(LINKER_FLAGS "${LINKER_FLAGS} -Wl,-exclude-libs,ALL -Wl,--gc-sections -Wl,--strip-all")
    # set(DOBBY_DEBUG OFF)
    message("Release Mode")
else ()
    # set(C_FLAGS "${C_FLAGS} -O2")
    # set(LINKER_FLAGS "${LINKER_FLAGS}")
    message("Debug Mode")
    add_compile_definitions(__DEBUG__)
endif ()
set(USE_FRIDA ON)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${C_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${C_FLAGS}")

set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${LINKER_FLAGS}")

set(DOBBY_GENERATE_SHARED OFF)

include(FetchContent)

set(ASMJIT_STATIC TRUE)
# Fetch AsmJit
# FetchContent_Declare(
#     asmjit
#     GIT_REPOSITORY https://github.com/asmjit/asmjit.git
#     GIT_TAG a32_port  # You can specify a release tag or commit hash here
# )

# FetchContent_GetProperties(asmjit)
# if (NOT asmjit_POPULATED)
#     FetchContent_Populate(asmjit)
#     add_subdirectory(${asmjit_SOURCE_DIR} ${asmjit_BINARY_DIR})
# endif()

# set_target_properties(asmjit PROPERTIES COMPILE_OPTIONS "-w")

# Set(ASMJIT_EMBED TRUE)
# add_definitions(-DASMJIT_STATIC)

# Add the source files
set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(LOCAL_SRC_FILES
        Main.cpp
        Menu/ImGui.cpp
        Tool/Keyboard.cpp
        Tool/Tool.cpp
        Tool/Util.cpp
        Tool/Patcher.cpp
        Tool/PopUpSelector.cpp
        Tool/ClassesTab.cpp
        Tool/Unity.cpp
        Includes/Utils.cpp
        Includes/Logger.cpp
        KittyMemory/KittyMemory.cpp
        KittyMemory/MemoryPatch.cpp
        KittyMemory/MemoryBackup.cpp
        KittyMemory/KittyUtils.cpp
        Il2cpp/Il2cpp.cpp
        Il2cpp/il2cpp-class.cpp
        Il2cpp/xdl/xdl.c
        Il2cpp/xdl/xdl_iterate.c
        Il2cpp/xdl/xdl_linker.c
        Il2cpp/xdl/xdl_lzma.c
        Il2cpp/xdl/xdl_util.c
        # imgui/backends/imgui_impl_opengl3.cpp
        # imgui/backends/imgui_impl_android.cpp
        # imgui/imgui.cpp
        # imgui/imgui_draw.cpp
        # imgui/imgui_demo.cpp
        # imgui/imgui_tables.cpp
        # imgui/imgui_widgets.cpp
)

# Add include directories
include_directories(${SRC_DIR})

add_subdirectory(asmjit)
# target_compile_options(dobby_static PRIVATE -w)
add_subdirectory(Dobby)
# set_target_properties(dobby_static PROPERTIES COMPILE_OPTIONS "-w")
add_subdirectory(imgui)
# set_target_properties(imgui PROPERTIES COMPILE_OPTIONS "-w")


set(JSON_BuildTests OFF CACHE INTERNAL "")

add_subdirectory(json)

function(add_my_library LIB_NAME)
    set(LOCAL_SRC_FILES ${ARGN})

    if(USE_FRIDA)
        add_definitions(-DUSE_FRIDA)
        include_directories(${CMAKE_SOURCE_DIR}/Frida/${CMAKE_ANDROID_ARCH_ABI})
        include_directories(${CMAKE_SOURCE_DIR}/Frida/gumpp)
        set(LOCAL_SRC_FILES ${LOCAL_SRC_FILES}
            Frida/gumpp/runtime.cpp
            Frida/gumpp/backtracer.cpp
            Frida/gumpp/interceptor.cpp
            Frida/gumpp/invocationlistener.cpp
            Frida/gumpp/returnaddress.cpp
            Tool/Frida.cpp
        )
        add_library(${LIB_NAME} SHARED ${LOCAL_SRC_FILES})
        target_link_libraries(${LIB_NAME}
            log
            android
            EGL
            GLESv3
            dobby_static
            nlohmann_json::nlohmann_json
            asmjit::asmjit
            imgui
            ${CMAKE_SOURCE_DIR}/Frida/${CMAKE_ANDROID_ARCH_ABI}/libfrida-gum.a
        )
    else()
        add_library(${LIB_NAME} SHARED ${LOCAL_SRC_FILES})
        target_link_libraries(${LIB_NAME}
            log
            android
            EGL
            GLESv3
            dobby_static
            nlohmann_json::nlohmann_json
            asmjit::asmjit
            imgui
        )
    endif()
endfunction()

add_my_library(${CMAKE_PROJECT_NAME} ${LOCAL_SRC_FILES})

if (NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_my_library(${CMAKE_PROJECT_NAME}_fix_input ${LOCAL_SRC_FILES})
    target_compile_definitions(${CMAKE_PROJECT_NAME}_fix_input PRIVATE LIB_INPUT)
endif()


if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
  # Windows platform detected, perform a copy operation
  file(COPY "${CMAKE_BINARY_DIR}/compile_commands.json" DESTINATION "${CMAKE_SOURCE_DIR}")
else()
  # For non-Windows platforms, create a symbolic link
  file(CREATE_LINK "${CMAKE_BINARY_DIR}/compile_commands.json" "${CMAKE_SOURCE_DIR}/compile_commands.json" SYMBOLIC)
endif()
