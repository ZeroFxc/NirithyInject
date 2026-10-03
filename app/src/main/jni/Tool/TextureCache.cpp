#include "TextureCache.h"
#include <GLES3/gl3.h>

namespace TextureCache
{
    static std::mutex mtx;
    static std::unordered_map<void *, unsigned int> cache;

    void Store(void *key, unsigned int glTex)
    {
        std::lock_guard<std::mutex> g(mtx);
        auto it = cache.find(key);
        if (it != cache.end() && it->second != glTex && it->second != 0)
        {
            GLuint old = it->second;
            glDeleteTextures(1, &old);
        }
        cache[key] = glTex;
    }

    void Reset()
    {
        std::lock_guard<std::mutex> g(mtx);
        for (auto &[_, tex] : cache)
        {
            if (tex)
            {
                GLuint id = tex;
                glDeleteTextures(1, &id);
            }
        }
        cache.clear();
    }

    size_t Count()
    {
        std::lock_guard<std::mutex> g(mtx);
        return cache.size();
    }
} // namespace TextureCache
