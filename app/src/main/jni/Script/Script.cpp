#include "Script.h"
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/mman.h>
#include <vector>
#include "Il2cpp/Il2cpp.h"
#include "Il2cpp/il2cpp-class.h"
#include "Includes/Logger.h"
#include "KittyMemory/KittyMemory.h"
#include "Tool/Frida.h"
#include "Tool/Keyboard.h"
#include "Tool/Lang.h"
#include "Tool/Tool.h"
#include "imgui/imgui.h"

// Lua 头文件按 C 语言声明，必须用 extern "C" 包裹以免符号被 C++ 名称修饰
extern "C"
{
#include "lua/lauxlib.h"
#include "lua/lua.h"
#include "lua/lualib.h"
}

// 定义于 Main.cpp：以 JSON 文本形式读写全局配置（tool_conf.json）
std::string ConfigGetJson(const char *key);
void ConfigSetJson(const char *key, const char *jsonValue);

namespace
{
    lua_State *g_L = nullptr;
    // 保护 Lua 状态机的互斥锁（钩子回调可能来自任意游戏线程；递归锁允许回调内重入 Lua）
    std::recursive_mutex g_luaMutex;

    // 脚本选项卡 UI 状态
    constexpr size_t kMaxCodeSize = 16384; // 多行编辑框缓冲区大小
    constexpr size_t kMaxLogLines = 200;   // 日志区最多保留的行数
    char g_codeBuf[kMaxCodeSize] = {0};    // 编辑框缓冲区（地址固定，供 ImGui 跨帧持有）
    std::string g_code;                    // 当前脚本内容（与编辑框保持同步）
    bool g_codeLoaded = false;             // 是否已从配置载入脚本
    std::vector<std::string> g_logs;       // 日志区内容

    // 追加一行日志（超过上限时丢弃最旧的记录）
    void AppendLog(const std::string &line)
    {
        g_logs.push_back(line);
        if (g_logs.size() > kMaxLogLines)
            g_logs.erase(g_logs.begin());
    }

    // 将当前脚本内容持久化到全局配置（tool_conf.json 的 luaScript 字段）
    void SaveCode()
    {
        ConfigSetJson("luaScript", nlohmann::json(g_code).dump().c_str());
    }

    // 将 Lua 值（栈索引 idx）转为 JSON 值；仅原生支持 nil/布尔/数字/字符串，其它类型退化为字符串
    nlohmann::json LuaToJson(lua_State *L, int idx)
    {
        switch (lua_type(L, idx))
        {
        case LUA_TNIL:
        case LUA_TNONE:
            return nullptr;
        case LUA_TBOOLEAN:
            return lua_toboolean(L, idx) != 0;
        case LUA_TNUMBER:
            if (lua_isinteger(L, idx))
                return (long long)lua_tointeger(L, idx);
            return lua_tonumber(L, idx);
        case LUA_TSTRING:
            return std::string(lua_tostring(L, idx));
        default:
        {
            size_t len = 0;
            const char *text = luaL_tolstring(L, idx, &len);
            std::string out(text ? text : "", text ? len : 0);
            lua_pop(L, 1); // 弹出 luaL_tolstring 压入的字符串
            return out;
        }
        }
    }

    // 将 JSON 值压入 Lua 栈
    void PushJson(lua_State *L, const nlohmann::json &j)
    {
        if (j.is_null())
            lua_pushnil(L);
        else if (j.is_boolean())
            lua_pushboolean(L, j.get<bool>());
        else if (j.is_number_integer())
            lua_pushinteger(L, (lua_Integer)j.get<long long>());
        else if (j.is_number_unsigned())
            lua_pushinteger(L, (lua_Integer)j.get<unsigned long long>());
        else if (j.is_number())
            lua_pushnumber(L, j.get<double>());
        else if (j.is_string())
            lua_pushstring(L, j.get<std::string>().c_str());
        else
            lua_pushstring(L, j.dump().c_str());
    }

    // host.log(...)：将脚本输出写入日志（logcat + 内置日志面板）
    int l_log(lua_State *L)
    {
        const int argc = lua_gettop(L);
        std::string out;
        for (int i = 1; i <= argc; i++)
        {
            size_t len = 0;
            const char *text = luaL_tolstring(L, i, &len);
            if (i > 1)
                out += '\t';
            out.append(text ? text : "", text ? len : 0);
            lua_pop(L, 1); // 弹出 luaL_tolstring 压入的字符串
        }
        LOGI("[Lua] %s", out.c_str());
        AppendLog(out);
        return 0;
    }

    // config_get(key [, default])：读取全局配置，未配置时返回默认值
    int l_config_get(lua_State *L)
    {
        const char *key = luaL_checkstring(L, 1);
        nlohmann::json j = nlohmann::json::parse(ConfigGetJson(key), nullptr, false);
        if (j.is_discarded() || j.is_null())
        {
            if (lua_gettop(L) >= 2)
                lua_pushvalue(L, 2);
            else
                lua_pushnil(L);
            return 1;
        }
        PushJson(L, j);
        return 1;
    }

    // config_set(key, value)：写入全局配置（持久化到 tool_conf.json）
    int l_config_set(lua_State *L)
    {
        const char *key = luaL_checkstring(L, 1);
        nlohmann::json j = LuaToJson(L, 2);
        ConfigSetJson(key, j.dump().c_str());
        return 0;
    }

    // 按参数声明类型将 Lua 值装箱为 il2cpp 参数对象
    // L: Lua 状态机；idx: 参数栈索引；type: 目标参数类型
    // 返回：装箱后的对象指针；nil 或无法转换时返回 nullptr
    Il2CppObject *BoxLuaArg(lua_State *L, int idx, Il2CppType *type)
    {
        if (!type || lua_isnoneornil(L, idx))
            return nullptr;

        const char *name = type->getName();
        Il2CppClass *klass = type->getClass();

        if (strcmp(name, "System.String") == 0)
            return Il2cpp::NewString(luaL_checkstring(L, idx));

        if (strcmp(name, "System.Boolean") == 0)
        {
            ValueType<int> value{lua_toboolean(L, idx) ? 1 : 0};
            return value.box(klass);
        }

        if (lua_type(L, idx) != LUA_TNUMBER)
        {
            LOGE("[Lua] 参数类型不匹配: 期望 %s", name);
            return nullptr;
        }

        const double num = lua_tonumber(L, idx);
        if (strcmp(name, "System.Int32") == 0)
            return ValueType<int>{(int)num}.box(klass);
        if (strcmp(name, "System.Int64") == 0)
            return ValueType<long>{(long)num}.box(klass);
        if (strcmp(name, "System.UInt32") == 0)
            return ValueType<unsigned int>{(unsigned int)num}.box(klass);
        if (strcmp(name, "System.UInt64") == 0)
            return ValueType<unsigned long>{(unsigned long)num}.box(klass);
        if (strcmp(name, "System.Single") == 0)
            return ValueType<float>{(float)num}.box(klass);
        if (strcmp(name, "System.Double") == 0)
            return ValueType<double>{num}.box(klass);
        if (strcmp(name, "System.Int16") == 0)
            return ValueType<int16_t>{(int16_t)num}.box(klass);
        if (strcmp(name, "System.UInt16") == 0)
            return ValueType<uint16_t>{(uint16_t)num}.box(klass);
        if (strcmp(name, "System.SByte") == 0)
            return ValueType<int8_t>{(int8_t)num}.box(klass);
        if (strcmp(name, "System.Byte") == 0)
            return ValueType<uint8_t>{(uint8_t)num}.box(klass);
        if (strcmp(name, "System.Char") == 0)
            return ValueType<uint16_t>{(uint16_t)num}.box(klass);
        if (strcmp(name, "System.IntPtr") == 0)
            return ValueType<intptr_t>{(intptr_t)num}.box(klass);
        if (strcmp(name, "System.UIntPtr") == 0)
            return ValueType<uintptr_t>{(uintptr_t)num}.box(klass);
        if (type->isEnum())
            return ValueType<int>{(int)num}.box(klass);

        LOGE("[Lua] 暂不支持的参数类型: %s", name);
        return nullptr;
    }

    // 将调用结果压入 Lua 栈：void/nil 压 nil，基元压布尔或数字，字符串压字符串，其它优先 ToString
    void PushCallResult(lua_State *L, MethodInfo *method, Il2CppObject *result)
    {
        auto returnType = method->getReturnType();
        if (!returnType || strcmp(returnType->getName(), "System.Void") == 0 || !result)
        {
            lua_pushnil(L);
            return;
        }

        Il2CppType *resultType = Il2cpp::GetClassType(result->klass);
        const char *name = resultType->getName();

        if (resultType->isPrimitive())
        {
            if (strcmp(name, "System.Boolean") == 0)
                lua_pushboolean(L, Il2cpp::GetUnboxedValue<int>(result) != 0);
            else if (strcmp(name, "System.Int64") == 0)
                lua_pushinteger(L, (lua_Integer)Il2cpp::GetUnboxedValue<long>(result));
            else if (strcmp(name, "System.UInt64") == 0)
                lua_pushinteger(L, (lua_Integer)Il2cpp::GetUnboxedValue<unsigned long>(result));
            else if (strcmp(name, "System.Single") == 0)
                lua_pushnumber(L, Il2cpp::GetUnboxedValue<float>(result));
            else if (strcmp(name, "System.Double") == 0)
                lua_pushnumber(L, Il2cpp::GetUnboxedValue<double>(result));
            else if (strcmp(name, "System.Int32") == 0)
                lua_pushinteger(L, Il2cpp::GetUnboxedValue<int>(result));
            else if (strcmp(name, "System.UInt32") == 0)
                lua_pushinteger(L, Il2cpp::GetUnboxedValue<unsigned int>(result));
            else if (strcmp(name, "System.Int16") == 0)
                lua_pushinteger(L, Il2cpp::GetUnboxedValue<int16_t>(result));
            else if (strcmp(name, "System.UInt16") == 0)
                lua_pushinteger(L, Il2cpp::GetUnboxedValue<uint16_t>(result));
            else if (strcmp(name, "System.SByte") == 0)
                lua_pushinteger(L, Il2cpp::GetUnboxedValue<int8_t>(result));
            else if (strcmp(name, "System.Byte") == 0)
                lua_pushinteger(L, Il2cpp::GetUnboxedValue<uint8_t>(result));
            else
                lua_pushnil(L);
            return;
        }

        if (strcmp(name, "System.String") == 0)
        {
            lua_pushstring(L, ((Il2CppString *)result)->to_string().c_str());
            return;
        }

        // 其它引用/值类型：优先返回 ToString() 文本，取不到则返回对象地址
        auto str = result->invoke_method<Il2CppString *>("ToString");
        if (str)
            lua_pushstring(L, str->to_string().c_str());
        else
            lua_pushinteger(L, (lua_Integer)(uintptr_t)result);
    }

    // 将钩子捕获的单个实参转换为 Lua 值
    // type: 参数声明类型；arg: 实参值（引用类型为对象指针，值类型为原始寄存器值）
    void PushHookArg(lua_State *L, Il2CppType *type, void *arg)
    {
        if (!type)
        {
            lua_pushnil(L);
            return;
        }

        const char *name = type->getName();
        if (strcmp(name, "System.String") == 0)
        {
            auto str = (Il2CppString *)arg;
            if (str)
                lua_pushstring(L, str->to_string().c_str());
            else
                lua_pushnil(L);
            return;
        }

        // 值类型/基元的实参值直接位于寄存器中，按位读取（arm64 小端）
        if (strcmp(name, "System.Boolean") == 0)
            lua_pushboolean(L, (*(int *)&arg) != 0);
        else if (strcmp(name, "System.Int32") == 0 || strcmp(name, "System.Int16") == 0 ||
                 strcmp(name, "System.SByte") == 0 || strcmp(name, "System.Char") == 0 || type->isEnum())
            lua_pushinteger(L, *(int *)&arg);
        else if (strcmp(name, "System.UInt32") == 0 || strcmp(name, "System.UInt16") == 0 ||
                 strcmp(name, "System.Byte") == 0)
            lua_pushinteger(L, *(uint32_t *)&arg);
        else if (strcmp(name, "System.Int64") == 0 || strcmp(name, "System.UInt64") == 0 ||
                 strcmp(name, "System.IntPtr") == 0 || strcmp(name, "System.UIntPtr") == 0)
            lua_pushinteger(L, (lua_Integer)(uintptr_t)arg);
        else if (strcmp(name, "System.Single") == 0)
            lua_pushnumber(L, *(float *)&arg);
        else if (strcmp(name, "System.Double") == 0)
            lua_pushnumber(L, *(double *)&arg);
        else
            lua_pushinteger(L, (lua_Integer)(uintptr_t)arg); // 其它引用类型：返回对象地址
    }

    // hook_method(class, method, callback [, argsCount])：进入方法时执行 Lua 回调
    // 回调签名：function(obj, ...)，obj 为 this 对象地址（静态方法为 nil），后续为各实参
    // 返回：钩子句柄；失败返回 nil + 错误信息
    int l_hook_method(lua_State *L)
    {
        if (!Il2cpp::IsReady())
            return luaL_error(L, "il2cpp 尚未就绪");

        const char *klassName = luaL_checkstring(L, 1);
        const char *methodName = luaL_checkstring(L, 2);
        luaL_checktype(L, 3, LUA_TFUNCTION);
        const size_t argCount = (size_t)luaL_optinteger(L, 4, -1);

        MethodInfo *method = Il2cpp::FindMethod(klassName, methodName, argCount);
        if (!method || !method->methodPointer)
            return luaL_error(L, "未找到方法 %s.%s", klassName, methodName);

        // 回调存入 registry 并取得引用，交由钩子监听器回调
        lua_pushvalue(L, 3);
        const int callbackRef = luaL_ref(L, LUA_REGISTRYINDEX);

        const int id = Frida::HookLua(method, callbackRef);
        if (id <= 0)
        {
            luaL_unref(L, LUA_REGISTRYINDEX, callbackRef);
            lua_pushnil(L);
            lua_pushfstring(L, "挂钩失败: %s.%s", klassName, methodName);
            return 2;
        }
        lua_pushinteger(L, id);
        return 1;
    }

    // unhook_method(id)：解除 hook_method 注册的钩子
    int l_unhook_method(lua_State *L)
    {
        const int id = (int)luaL_checkinteger(L, 1);
        lua_pushboolean(L, Frida::UnhookLua(id) ? 1 : 0);
        return 1;
    }

    // il2cpp_call(class, method, ...)：调用静态方法；参数按声明类型装箱
    int l_il2cpp_call(lua_State *L)
    {
        if (!Il2cpp::IsReady())
            return luaL_error(L, "il2cpp 尚未就绪");

        const char *klassName = luaL_checkstring(L, 1);
        const char *methodName = luaL_checkstring(L, 2);
        const int argCount = lua_gettop(L) - 2;

        MethodInfo *method = Il2cpp::FindMethod(klassName, methodName, (size_t)argCount);
        if (!method)
            return luaL_error(L, "未找到方法 %s.%s/%d", klassName, methodName, argCount);
        if (!Il2cpp::GetIsMethodStatic(method))
            return luaL_error(L, "il2cpp_call 仅支持静态方法：%s.%s", klassName, methodName);

        std::vector<Il2CppObject *> args;
        if (argCount > 0)
        {
            auto paramsInfo = method->getParamsInfo();
            if ((int)paramsInfo.size() != argCount)
                return luaL_error(L, "参数个数不匹配：%s.%s 需要 %zu 个", klassName, methodName, paramsInfo.size());
            args.reserve(argCount);
            for (int i = 0; i < argCount; i++)
                args.push_back(BoxLuaArg(L, i + 3, paramsInfo[i].second));
        }

        Il2CppObject *result = Il2cpp::RuntimeInvokeConvertArgs(method, nullptr, args.empty() ? nullptr : args.data(),
                                                               argCount);
        PushCallResult(L, method, result);
        return 1;
    }

    // read_mem(addr, size)：读取内存并返回大写十六进制字符串，失败返回 nil
    int l_read_mem(lua_State *L)
    {
        const uintptr_t addr = (uintptr_t)luaL_checkinteger(L, 1);
        const lua_Integer size = luaL_optinteger(L, 2, 64);
        if (size <= 0 || size > 4096)
            return luaL_error(L, "读取长度必须在 1..4096 之间");

        // 先通过 mprotect 验证地址已映射，避免直接读取未映射内存导致崩溃
        if (!KittyMemory::ProtectAddr((void *)addr, (size_t)size, PROT_READ))
        {
            lua_pushnil(L);
            lua_pushfstring(L, "无法读取地址 %p", (void *)addr);
            return 2;
        }

        std::string hex = KittyMemory::read2HexStr((void *)addr, (size_t)size);
        if (hex.empty())
        {
            lua_pushnil(L);
            lua_pushstring(L, "读取失败");
            return 2;
        }
        lua_pushstring(L, hex.c_str());
        return 1;
    }

    // 将十六进制字符串解析为字节序列（忽略空白，允许单个 0x 前缀）
    bool ParseHex(const char *text, std::vector<uint8_t> &out)
    {
        if (!text)
            return false;
        const char *p = text;
        while (*p && isspace((unsigned char)*p))
            ++p;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
            p += 2;

        std::string digits;
        for (; *p; ++p)
        {
            if (isspace((unsigned char)*p))
                continue;
            digits.push_back(*p);
        }
        if (digits.empty() || digits.size() % 2 != 0)
            return false;

        auto hexValue = [](char c) -> int
        {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            return -1;
        };

        out.clear();
        out.reserve(digits.size() / 2);
        for (size_t i = 0; i < digits.size(); i += 2)
        {
            const int hi = hexValue(digits[i]);
            const int lo = hexValue(digits[i + 1]);
            if (hi < 0 || lo < 0)
                return false;
            out.push_back((uint8_t)((hi << 4) | lo));
        }
        return true;
    }

    // write_mem(addr, hex)：按十六进制字符串向内存写入字节，返回是否成功
    int l_write_mem(lua_State *L)
    {
        const uintptr_t addr = (uintptr_t)luaL_checkinteger(L, 1);
        const char *hex = luaL_checkstring(L, 2);

        std::vector<uint8_t> bytes;
        if (!ParseHex(hex, bytes))
            return luaL_error(L, "十六进制字符串格式错误");
        if (bytes.empty())
            return luaL_error(L, "写入长度不能为 0");

        bool ok = KittyMemory::memWrite((void *)addr, bytes.data(), bytes.size()) == KittyMemory::SUCCESS;
        lua_pushboolean(L, ok ? 1 : 0);
        return 1;
    }

    // toggle_hooker(class, method [, argsCount])：切换方法挂钩状态，返回当前是否已挂钩
    int l_toggle_hooker(lua_State *L)
    {
        if (!Il2cpp::IsReady())
            return luaL_error(L, "il2cpp 尚未就绪");

        const char *klassName = luaL_checkstring(L, 1);
        const char *methodName = luaL_checkstring(L, 2);
        const size_t argCount = (size_t)luaL_optinteger(L, 3, -1);

        MethodInfo *method = Il2cpp::FindMethod(klassName, methodName, argCount);
        if (!method)
            return luaL_error(L, "未找到方法 %s.%s", klassName, methodName);

        lua_pushboolean(L, Tool::ToggleHooker(method) ? 1 : 0);
        return 1;
    }

    const luaL_Reg kApi[] = {
        {"log", l_log},
        {"config_get", l_config_get},
        {"config_set", l_config_set},
        {"il2cpp_call", l_il2cpp_call},
        {"read_mem", l_read_mem},
        {"write_mem", l_write_mem},
        {"toggle_hooker", l_toggle_hooker},
        {"hook_method", l_hook_method},
        {"unhook_method", l_unhook_method},
        {nullptr, nullptr},
    };
} // namespace

void Script::Init()
{
    if (g_L)
        return;

    g_L = luaL_newstate();
    if (!g_L)
    {
        LOGE("[Script] 创建 Lua 状态机失败");
        return;
    }
    luaL_openlibs(g_L);

    // 注册宿主 C API（同时把 print 重定向到日志，便于脚本调试）
    for (const luaL_Reg *reg = kApi; reg->name; ++reg)
    {
        lua_pushcfunction(g_L, reg->func);
        lua_setglobal(g_L, reg->name);
    }
    lua_pushcfunction(g_L, l_log);
    lua_setglobal(g_L, "print");

    // 剔除 os.execute，避免脚本执行任意系统命令
    lua_getglobal(g_L, "os");
    if (lua_istable(g_L, -1))
    {
        lua_pushnil(g_L);
        lua_setfield(g_L, -2, "execute");
    }
    lua_pop(g_L, 1);

    LOGI("[Script] Lua %s 引擎已就绪", LUA_RELEASE);
}

bool Script::IsReady()
{
    return g_L != nullptr;
}

lua_State *Script::State()
{
    return g_L;
}

bool Script::Run(const std::string &code, const std::string &name, std::string &error)
{
    if (!g_L)
    {
        error = "脚本引擎未初始化";
        return false;
    }

    std::lock_guard<std::recursive_mutex> lock(g_luaMutex);
    if (luaL_loadbuffer(g_L, code.data(), code.size(), name.c_str()) != LUA_OK)
    {
        error = lua_tostring(g_L, -1) ? lua_tostring(g_L, -1) : "脚本编译失败";
        lua_pop(g_L, 1);
        return false;
    }
    if (lua_pcall(g_L, 0, 0, 0) != LUA_OK)
    {
        error = lua_tostring(g_L, -1) ? lua_tostring(g_L, -1) : "脚本执行失败";
        lua_pop(g_L, 1);
        return false;
    }

    error.clear();
    return true;
}

bool Script::InvokeHookCallback(int callbackRef, void *context)
{
    if (!g_L || callbackRef < 0 || !context)
        return false;

    auto *ctx = static_cast<Frida::HookContext *>(context);
    std::lock_guard<std::recursive_mutex> lock(g_luaMutex);

    lua_rawgeti(g_L, LUA_REGISTRYINDEX, callbackRef);
    if (!lua_isfunction(g_L, -1))
    {
        lua_pop(g_L, 1);
        return false;
    }

    const bool isStatic = Il2cpp::GetIsMethodStatic(ctx->method);
    // 回调签名 function(obj, ...)：静态方法 obj 为 nil，非静态方法传 this 对象地址
    if (isStatic || ctx->argCount < 1)
        lua_pushnil(g_L);
    else
        lua_pushinteger(g_L, (lua_Integer)(uintptr_t)ctx->args[0]);

    int nargs = 1;
    auto paramsInfo = ctx->method->getParamsInfo();
    const int argBase = isStatic ? 0 : 1;
    for (int i = 0; i < (int)paramsInfo.size(); i++)
    {
        const int index = argBase + i;
        void *arg = index < ctx->argCount ? ctx->args[index] : nullptr;
        PushHookArg(g_L, paramsInfo[i].second, arg);
        nargs++;
    }

    if (lua_pcall(g_L, nargs, 0, 0) != LUA_OK)
    {
        const char *err = lua_tostring(g_L, -1);
        LOGE("[Script] Lua 钩子回调执行失败: %s", err ? err : "unknown");
        lua_pop(g_L, 1);
        return false;
    }
    return true;
}

void Script::ReleaseHookCallback(int callbackRef)
{
    if (!g_L || callbackRef < 0)
        return;
    std::lock_guard<std::recursive_mutex> lock(g_luaMutex);
    luaL_unref(g_L, LUA_REGISTRYINDEX, callbackRef);
}

void Script::Draw()
{
    // 首次绘制时从配置载入上次保存的脚本
    if (!g_codeLoaded)
    {
        g_codeLoaded = true;
        nlohmann::json j = nlohmann::json::parse(ConfigGetJson("luaScript"), nullptr, false);
        if (j.is_string())
        {
            g_code = j.get<std::string>();
            snprintf(g_codeBuf, sizeof(g_codeBuf), "%s", g_code.c_str());
        }
    }

    if (!IsReady())
    {
        ImGui::TextWrapped("%s", Lang::T("脚本引擎未初始化", "Script engine is not initialized"));
        return;
    }

    if (ImGui::Button(Lang::T("执行", "Run")))
    {
        std::string error;
        if (Run(g_code, "lua_script", error))
            AppendLog(Lang::T("[脚本] 执行完成", "[script] run completed"));
        else
            AppendLog(error);
    }
    ImGui::SameLine();
    if (ImGui::Button(Lang::T("软键盘编辑", "Edit via keyboard")))
    {
        // ImGui 未接入输入法，这里用系统软键盘编辑脚本
        Keyboard::Open(g_code.c_str(),
                       [](const std::string &text)
                       {
                           g_code = text;
                           snprintf(g_codeBuf, sizeof(g_codeBuf), "%s", g_code.c_str());
                           SaveCode();
                       });
    }
    ImGui::SameLine();
    if (ImGui::Button(Lang::T("清空脚本", "Clear script")))
    {
        g_code.clear();
        g_codeBuf[0] = '\0';
        SaveCode();
    }
    ImGui::SameLine();
    if (ImGui::Button(Lang::T("清空日志", "Clear log")))
    {
        g_logs.clear();
    }

    ImGui::Separator();
    if (ImGui::InputTextMultiline("##luaCode", g_codeBuf, sizeof(g_codeBuf), ImVec2(-1, 220)))
    {
        g_code = g_codeBuf;
        SaveCode();
    }

    ImGui::Separator();
    ImGui::Text("%s", Lang::T("日志", "Log"));
    ImGui::BeginChild("##luaLog", ImVec2(0, 160), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    for (const auto &line : g_logs)
        ImGui::TextUnformatted(line.c_str());
    if (!g_logs.empty())
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

void Script::Shutdown()
{
    if (!g_L)
        return;
    lua_close(g_L);
    g_L = nullptr;
}