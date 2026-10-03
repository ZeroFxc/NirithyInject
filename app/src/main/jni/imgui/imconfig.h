//-----------------------------------------------------------------------------
// DEAR IMGUI COMPILE-TIME OPTIONS
//-----------------------------------------------------------------------------
#pragma once

#include "Includes/Logger.h"
#define IM_ASSERT(_EXPR) do { \
    if(!(_EXPR)) {\
        LOGE("Assertion failed: %s, file %s, line %d", #_EXPR, __FILE__, __LINE__);\
        __builtin_trap(); \
    }\
} while(0)

// Load Vulkan functions via our hook layer (dlopen libvulkan.so).
#define IMGUI_IMPL_VULKAN_NO_PROTOTYPES
