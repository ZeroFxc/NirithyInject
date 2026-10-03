#pragma once
#include "Il2cpp/il2cpp-class.h"
#include "Tool/Tool.h"

namespace Frida
{
    struct TraceData
    {
        MethodInfo *method = nullptr;
        std::string msg;
    };
    void Init();
    bool Trace(MethodInfo *method, HookerData *data);
    bool Untrace(MethodInfo *method);
    bool isTraced(MethodInfo *method);

    // Lua 钩子进入时的上下文（避免把 gumpp 类型暴露给脚本层）
    struct HookContext
    {
        MethodInfo *method = nullptr; // 被调用的方法
        void **args = nullptr;        // 实参值数组（非静态方法的 args[0] 为 this）
        int argCount = 0;             // 实参数目（含 this）
    };

    // 挂钩 Lua 回调：进入 method 时执行脚本引擎中 callbackRef 对应的 Lua 函数
    // 参数：method 目标方法；callbackRef Lua registry 中的回调引用
    // 返回：钩子句柄（>0），失败返回 0
    int HookLua(MethodInfo *method, int callbackRef);

    // 解除 HookLua 注册的钩子，并释放回调引用
    // 参数：id HookLua 返回的钩子句柄
    // 返回：true 表示已成功解除
    bool UnhookLua(int id);
} // namespace Frida
