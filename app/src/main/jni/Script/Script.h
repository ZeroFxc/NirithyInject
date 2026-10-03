#pragma once
#include <string>

struct lua_State;

// Lua 脚本引擎：管理 Lua 状态机生命周期，并向脚本暴露宿主 C API
namespace Script
{
    // 初始化 Lua 引擎并注册 C API（幂等，重复调用直接返回）
    void Init();

    // 引擎是否可用（已成功创建状态机）
    bool IsReady();

    // 返回底层 Lua 状态机；未初始化时返回 nullptr
    lua_State *State();

    // 执行一段 Lua 代码
    // 参数：code 脚本源码；name 代码块名称（用于错误定位）
    // 参数：error 输出参数，执行失败时写入错误信息
    // 返回：true 表示编译并执行成功
    bool Run(const std::string &code, const std::string &name, std::string &error);

    // 绘制脚本选项卡内容：多行编辑框、执行按钮与日志区
    void Draw();

    // 供钩子监听器调用：执行 registry 中 callbackRef 对应的 Lua 回调
    // 参数：callbackRef Lua registry 回调引用；context Frida::HookContext*
    // 返回：true 表示回调执行成功
    bool InvokeHookCallback(int callbackRef, void *context);

    // 释放 InvokeHookCallback 使用的 registry 回调引用
    void ReleaseHookCallback(int callbackRef);

    // 关闭引擎并释放 Lua 状态机
    void Shutdown();
} // namespace Script