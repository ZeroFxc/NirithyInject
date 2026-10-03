#include "Unity.h"
#include "Il2cpp/Il2cpp.h"
#include "Includes/Macros.h"
#include "Includes/obfuscate.h"
#include "imgui/imgui.h"

int (*o_get_touchCount)() = nullptr;
int get_touchCount();
bool (*oInput_GetMouseButton)(int n) = nullptr;
bool Input_GetMouseButton(int n);

static Il2CppClass *Input = nullptr;
static bool g_inputHooked = false;

extern bool collapsed;
extern bool fullScreen;

bool Input_GetMouseButton(int n)
{
    ImGuiIO &io = ImGui::GetIO();

    ImVec2 size{ImGui::GetFrameHeight() * 2.f, ImGui::GetFrameHeight() * 2.f};
    if (io.WantCaptureMouse && !(collapsed && fullScreen && (io.MousePos.x > size.x && io.MousePos.y > size.y)))
        return false;
    if (oInput_GetMouseButton)
        return oInput_GetMouseButton(n);
    return false;
}
int get_touchCount()
{
    ImGuiIO &io = ImGui::GetIO();

    auto count = o_get_touchCount ? o_get_touchCount() : 0;
    if (count > 0 && Input)
    {
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        auto touch = Input->invoke_static_method<UnityEngine_Touch>("GetTouch", 0);
        float x = touch.m_Position.x;
        float y = io.DisplaySize.y - touch.m_Position.y;

        if (touch.m_Phase == UnityEngine_TouchPhase::Began)
        {
            io.AddMousePosEvent(x, y);
            io.AddMouseButtonEvent(0, true);
        }
        else if (touch.m_Phase == UnityEngine_TouchPhase::Ended)
        {
            io.AddMousePosEvent(x, y);
            io.AddMouseButtonEvent(0, false);
            io.AddMousePosEvent(-1, -1);
        }
        else if (touch.m_Phase == UnityEngine_TouchPhase::Moved)
        {
            io.AddMousePosEvent(x, y);
        }
    }

    ImVec2 size{ImGui::GetFrameHeight() * 2.f, ImGui::GetFrameHeight() * 2.f};
    if (io.WantCaptureMouse && !(collapsed && fullScreen && (io.MousePos.x > size.x && io.MousePos.y > size.y)))
    {
        return 0;
    }

    return count;
}

namespace Unity
{
    static Il2CppImage *g_Image;
    static bool tryHookLegacy()
    {
        auto img = Il2cpp::GetImage("UnityEngine.InputLegacyModule");
        if (!img)
            img = Il2cpp::GetImage("UnityEngine.CoreModule");
        if (!img)
            return false;
        auto klass = img->getClass("UnityEngine.Input");
        if (!klass)
            klass = Il2cpp::FindClass("UnityEngine.Input");
        if (!klass)
            return false;
        auto touchCount = klass->getMethod("get_touchCount", 0);
        auto mouseBtn = klass->getMethod("GetMouseButton", 1);
        if (!touchCount || !touchCount->methodPointer)
        {
            LOGI("Legacy Input.get_touchCount missing — skip (new Input System?)");
            return false;
        }
        g_Image = img;
        Input = klass;
        REPLACE_NAME_KLASS_ORIG(klass, "get_touchCount", get_touchCount, o_get_touchCount);
        if (mouseBtn && mouseBtn->methodPointer)
            REPLACE_NAME_KLASS_ORIG(klass, "GetMouseButton", Input_GetMouseButton, oInput_GetMouseButton);
        return true;
    }

    static bool tryHookNewInputSystem()
    {
        // Unity.InputSystem — EnhancedTouch / Touchscreen. Don't hook EnhancedTouchUpdate
        // on versions that crash; just keep overlay from stealing mouse via Mouse.current.
        auto klass = Il2cpp::FindClass("UnityEngine.InputSystem.EnhancedTouch.Touch");
        if (!klass)
            klass = Il2cpp::FindClass("UnityEngine.InputSystem.Touchscreen");
        if (!klass)
        {
            LOGI("New Input System types not found");
            return false;
        }
        LOGI("New Input System detected — leaving it unhooked to avoid crash; overlay uses AInputEvent / WantCaptureMouse");
        return true;
    }

    void HookInput()
    {
        if (g_inputHooked)
            return;
        if (tryHookLegacy())
        {
            g_inputHooked = true;
            return;
        }
        tryHookNewInputSystem();
        g_inputHooked = true;
    }
} // namespace Unity
