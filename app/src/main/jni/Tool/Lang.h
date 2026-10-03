#pragma once
#include "imgui/imgui.h"

// Overlay language. 0 = Simplified Chinese (default), 1 = English.
namespace Lang
{
    inline int current = 0;

    inline const char *T(const char *zh, const char *en)
    {
        return current == 0 ? zh : en;
    }

    inline void Toggle()
    {
        current = current == 0 ? 1 : 0;
    }

    inline bool Combo()
    {
        const char *items[] = {"简体中文", "English"};
        int prev = current;
        ImGui::Combo(T("界面语言", "Language"), &current, items, 2);
        return prev != current;
    }
} // namespace Lang
