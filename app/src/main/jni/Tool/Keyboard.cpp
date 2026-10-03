#include "Keyboard.h"
#include "Il2cpp/Il2cpp.h"
#include "Tool/Lang.h"
#include "imgui/imgui.h"
#include "string"

namespace Keyboard
{
    Il2CppClass *TouchScreenKeyboard = nullptr;
    Il2CppObject *openedKeyboard = nullptr;
    std::function<void(const std::string &)> lastCallback = nullptr;

    void Init()
    {
        TouchScreenKeyboard = Il2cpp::FindClass("UnityEngine.TouchScreenKeyboard");
        LOGPTR(TouchScreenKeyboard);
    }

    void Open(const std::function<void(const std::string &)> &callback)
    {
        Open("", callback);
    }
    void Open(const char *text, const std::function<void(const std::string &)> &callback)
    {
        LOGD("Keyboard Open");
        if (!TouchScreenKeyboard)
            Init();
        if (!TouchScreenKeyboard)
        {
            LOGE("TouchScreenKeyboard class missing");
            return;
        }
        auto open = TouchScreenKeyboard->getMethod("Open");
        if (!open)
        {
            LOGE("TouchScreenKeyboard.Open missing");
            return;
        }
        openedKeyboard = TouchScreenKeyboard->invoke_static_method<Il2CppObject *>("Open", Il2cpp::NewString(text ? text : ""),
                                                                                   0, 0, 0, 0, Il2cpp::NewString(""), 0);
        lastCallback = callback;
    }

    void Reset()
    {
        lastCallback = nullptr;
        if (!openedKeyboard)
            return;

        // Instance methods — never invoke_static. The "backup" keyboard path used to crash here.
        auto klass = openedKeyboard->klass;
        if (klass)
        {
            auto setActive = klass->getMethod("set_active", 1);
            if (setActive)
            {
                bool off = false;
                void *args[1] = {&off};
                Il2cpp::RuntimeInvoke(setActive, openedKeyboard, args, nullptr);
            }
        }
        openedKeyboard = nullptr;
    }

    static bool readStatus(TouchScreenKeyboardStatus &status)
    {
        if (!openedKeyboard || !TouchScreenKeyboard)
            return false;
        auto get_statusMethod = TouchScreenKeyboard->getMethod("get_status");
        if (!get_statusMethod)
            return false;

        // Both the default RuntimeInvoke path and the "backup" invoke_method path
        // go through boxed enum unbox. The old backup path crashed because it
        // treated Destroy/Finalize as static and because get_status was invoked
        // with the wrong this ABI on some Unity versions.
        Il2CppException *exc = nullptr;
        auto result = Il2cpp::RuntimeInvoke(get_statusMethod, openedKeyboard, nullptr, &exc);
        if (exc || !result)
        {
            if (check)
            {
                // Backup: try instance invoke via invoker_method
                try
                {
                    status = openedKeyboard->invoke_method<TouchScreenKeyboardStatus>("get_status");
                    return true;
                }
                catch (...)
                {
                    return false;
                }
            }
            return false;
        }
        status = Il2cpp::GetUnboxedValue<TouchScreenKeyboardStatus>(result);
        return true;
    }

    void Update()
    {
        // 可移动输入状态弹窗：系统键盘弹出时显示，可拖动位置
        if (openedKeyboard)
        {
            ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(260, 0), ImGuiCond_FirstUseEver);
            ImGui::Begin(Lang::T("键盘输入", "Keyboard Input"), nullptr,
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);
            ImGui::Text("%s", Lang::T("系统键盘已打开...", "System keyboard opened..."));
            if (ImGui::Button(Lang::T("取消", "Cancel")))
            {
                Reset();
            }
            ImGui::End();
        }

        if (!openedKeyboard)
            return;

        TouchScreenKeyboardStatus status = Canceled;
        if (!readStatus(status))
        {
            LOGE("Failed to get keyboard status");
            Reset();
            return;
        }
        if (status == Done)
        {
            Il2CppString *text = nullptr;
            auto get_textMethod = TouchScreenKeyboard->getMethod("get_text");
            if (get_textMethod)
            {
                Il2CppException *exc = nullptr;
                auto result = Il2cpp::RuntimeInvoke(get_textMethod, openedKeyboard, nullptr, &exc);
                text = (Il2CppString *)result;
            }
            if (!text && check && openedKeyboard)
            {
                text = openedKeyboard->invoke_method<Il2CppString *>("get_text");
            }

            std::string resultStr = text ? text->to_string() : "";
            auto cb = lastCallback;
            Reset();
            if (cb)
                cb(resultStr);
            LOGD("Keyboard Done");
        }
        else if (status != Visible)
        {
            Reset();
            LOGD("Keyboard Canceled");
        }
    }

    bool IsOpen()
    {
        return openedKeyboard != nullptr;
    }
} // namespace Keyboard
