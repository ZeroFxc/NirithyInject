#pragma once

// Native Vulkan present-queue overlay. Games that never call eglSwapBuffers
// (Unity + Vulkan, etc.) still get the ImGui menu.
// Java SurfaceView overlays are intentionally NOT used (Zygisk-incompatible).
void initVulkanHooks();
bool isVulkanOverlayActive();
