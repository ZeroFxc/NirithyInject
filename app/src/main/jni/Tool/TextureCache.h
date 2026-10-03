#pragma once
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

// Cached GL/Vulkan textures used by the inspector (Unity Texture2D previews, icons).
namespace TextureCache
{
    void Store(void *key, unsigned int glTex);
    void Reset();
    size_t Count();
} // namespace TextureCache
