cmake_minimum_required(VERSION 3.10)

project(Tool)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -s -Wno-error=format-security -fvisibility=hidden -fpermissive -fexceptions -Wno-error=c++11-narrowing -Wall")
set(LINKER_FLAGS "-ffixed-x18 -Wl,--hash-style=both")
set(C_FLAGS "-Werror=format -fdata-sections -ffunction-sections")

message("CMAKE_CXX_COMPILER_ID: ${CMAKE_CXX_COMPILER_ID}")
message("CMAKE_CXX_COMPILER_VERSION: ${CMAKE_CXX_COMPILER_VERSION}")
message("${CMAKE_BINARY_DIR}")
message("${CMAKE_CURRENT_LIST_DIR}")

set(C_FLAGS "${C_FLAGS} -O3 -fvisibility=hidden -fvisibility-inlines-hidden")
set(LINKER_FLAGS "${LINKER_FLAGS} -Wl,-exclude-libs,ALL -Wl,--gc-sections -Wl,--strip-all")

if (NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    message("Release Mode")
else ()
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

set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(LOCAL_SRC_FILES
        Main.cpp
        Menu/ImGui.cpp
        Menu/Fonts.cpp
        Menu/VulkanHook.cpp
        Tool/Keyboard.cpp
        Tool/Tool.cpp
        Tool/Util.cpp
        Tool/Patcher.cpp
        Tool/PopUpSelector.cpp
        Tool/ClassesTab.cpp
        Tool/Clipboard.cpp
        Tool/Memory.cpp
        Tool/Unity.cpp
        Tool/TextureCache.cpp
        Tool/Targets.cpp
        Tool/Lang.cpp
        Script/Script.cpp
        Zygisk/ZygiskModule.cpp
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
        lua/lapi.c
        lua/lauxlib.c
        lua/lbaselib.c
        lua/lcode.c
        lua/lcorolib.c
        lua/lctype.c
        lua/ldblib.c
        lua/ldebug.c
        lua/ldo.c
        lua/ldump.c
        lua/lfunc.c
        lua/lgc.c
        lua/linit.c
        lua/liolib.c
        lua/llex.c
        lua/lmathlib.c
        lua/lmem.c
        lua/loadlib.c
        lua/lobject.c
        lua/lopcodes.c
        lua/loslib.c
        lua/lparser.c
        lua/lstate.c
        lua/lstring.c
        lua/lstrlib.c
        lua/ltable.c
        lua/ltablib.c
        lua/ltm.c
        lua/lundump.c
        lua/lutf8lib.c
        lua/lvm.c
        lua/lzio.c
)

include_directories(${SRC_DIR})

add_subdirectory(asmjit)
add_subdirectory(Dobby)
add_subdirectory(imgui)

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
  if(EXISTS "${CMAKE_BINARY_DIR}/compile_commands.json")
    file(COPY "${CMAKE_BINARY_DIR}/compile_commands.json" DESTINATION "${CMAKE_SOURCE_DIR}")
  endif()
else()
  if(EXISTS "${CMAKE_BINARY_DIR}/compile_commands.json")
    file(CREATE_LINK "${CMAKE_BINARY_DIR}/compile_commands.json" "${CMAKE_SOURCE_DIR}/compile_commands.json" SYMBOLIC)
  endif()
endif()
