#include "Tool/Clipboard.h"
#include "Il2cpp/Il2cpp.h"
#include "Includes/Logger.h"

namespace Clipboard
{
    // 查找并缓存 UnityEngine.GUIUtility 类；未就绪时不缓存空结果以便后续重试
    static Il2CppClass *FindGUIUtility()
    {
        static Il2CppClass *cached = nullptr;
        if (!cached)
            cached = Il2cpp::FindClass("UnityEngine.GUIUtility");
        return cached;
    }

    std::string Get()
    {
        auto klass = FindGUIUtility();
        if (!klass)
            return "";
        auto getter = klass->getMethod("get_systemCopyBuffer");
        if (!getter || !getter->methodPointer)
            return "";
        auto str = getter->invoke_static<Il2CppString *>();
        if (!str)
            return "";
        return str->to_string();
    }

    bool Set(const std::string &text)
    {
        auto klass = FindGUIUtility();
        if (!klass)
            return false;
        auto setter = klass->getMethod("set_systemCopyBuffer");
        if (!setter || !setter->methodPointer)
            return false;
        auto str = Il2cpp::NewString(text.c_str());
        if (!str)
            return false;
        setter->invoke_static<void, Il2CppString *>(str);
        return true;
    }
}