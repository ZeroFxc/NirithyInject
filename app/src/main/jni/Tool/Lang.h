#pragma once
#include "imgui/imgui.h"

// Dictionary-based overlay i18n.
// Add a language: extend Code, add a column to kDict in Lang.cpp, bump kLangCount.
namespace Lang
{
    enum Code
    {
        ZH_CN = 0,
        EN = 1,
        ZH_TW = 2,
        kCount
    };

    inline int current = ZH_CN;

    // Look up a stable key ("tab.tools"). Falls back to the key itself.
    const char *tr(const char *key);

    // Legacy two-string helper used across the codebase. Other languages resolve
    // via the dictionary using the Chinese text as an alternate key.
    const char *T(const char *zh, const char *en);

    inline void Toggle()
    {
        current = (current + 1) % kCount;
    }

    const char *codeName();
    const char *displayName(int lang);
    bool Combo();
} // namespace Lang
