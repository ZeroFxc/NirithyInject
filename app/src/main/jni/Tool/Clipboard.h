#pragma once

#include <string>

// 剪贴板后端：通过 IL2CPP 反射 UnityEngine.GUIUtility.systemCopyBuffer 读写系统剪贴板
namespace Clipboard
{
    // 读取系统剪贴板文本；不可用时返回空字符串
    std::string Get();

    // 写入文本到系统剪贴板；成功返回 true
    bool Set(const std::string &text);
}