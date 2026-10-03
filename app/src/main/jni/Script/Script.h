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

    // 关闭引擎并释放 Lua 状态机
    void Shutdown();
} // namespace Script