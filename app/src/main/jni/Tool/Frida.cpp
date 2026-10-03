#include "Frida.h"
#include "Frida/arm64-v8a/frida-gum.h"
#include "Frida/gumpp/gumpp.hpp"
#include "Il2cpp/Il2cpp.h"
#include "Il2cpp/il2cpp-class.h"
#include "Script/Script.h"
#include "Tool/Tool.h"

// extern std::unordered_map<void *, HookerData> hookerMap;
extern int maxLine;
extern std::vector<MethodInfo *> g_Methods;

MethodInfo *binarySearchClosest(const uintptr_t addr)
{
    int left = 0;
    int right = g_Methods.size() - 1;

    while (left <= right)
    {
        int pivot = (left + right) / 2;
        int comparison = (uintptr_t)g_Methods[pivot]->methodPointer - addr;

        if (comparison == 0)
        {
            return g_Methods[pivot];
        }
        else if (comparison > 0)
        {
            right = pivot - 1;
        }
        else
        {
            left = pivot + 1;
        }
    }
    return g_Methods[right];
}
namespace Frida
{
    class TraceListener : public Gum::InvocationListener
    {
      private:
        Gum::RefPtr<Gum::Backtracer> backtracer;

      public:
        TraceListener() : backtracer(Gum::Backtracer_make_accurate())
        {
            if (!backtracer)
            {
                LOGE("Failed to create backtracer");
            }
        }

        void Backtracer(Gum::InvocationContext *context)
        {
            auto hookerData = context->get_listener_function_data<HookerData>();
            // hookerData->backtraced.clear();
            // hookerData->backtracing = false;
            
            {
                Gum::ReturnAddressArray return_addresses;
                backtracer->generate(context->get_cpu_context(), return_addresses);
                LOGD("========================================");
                std::vector<std::string> result;
                for (int i = 0; i < return_addresses.len; i++)
                {
                    auto addr = return_addresses.items[i];
                    // auto result =
                    //     std::min_element(g_Methods.begin(), g_Methods.end(),
                    //                      [&addr](const auto &a, const auto &b)
                    //                      {
                    //                          // Calculate absolute differences
                    //                          auto diffA = std::abs(static_cast<int>((uintptr_t)a->methodPointer -
                    //                                                                 reinterpret_cast<uintptr_t>(addr)));
                    //                          auto diffB = std::abs(static_cast<int>((uintptr_t)b->methodPointer -
                    //                                                                 reinterpret_cast<uintptr_t>(addr)));

                    //                          // Ensure addr is greater than the address
                    //                          if (addr <= a->methodPointer)
                    //                          {
                    //                              return diffA < diffB;
                    //                          }
                    //                          else
                    //                          {
                    //                              return diffB < diffA;
                    //                          }
                    //                      });
                    auto closestMethod = binarySearchClosest((uintptr_t)addr);
                    if (closestMethod)
                    {
                        intptr_t offset = (intptr_t)addr - (intptr_t)closestMethod->methodPointer;
                        if (offset < 0)
                        {
                            offset = -offset;
                        }
                        if (offset <= 0x1000)
                        {
                            char buffer[265];
                            sprintf(buffer, "%s::%s+0x%" PRIxPTR, closestMethod->getClass()->getFullName().c_str(),
                                    closestMethod->getName(), offset);
                            // LOGD("%s::%s+0x%lx => %p", closestPtr->second->getClass()->getFullName().c_str(),
                            //      closestPtr->second->getName(), gap, (void *)closestPtr->first);

                            result.push_back(buffer);
                        }
                        else
                        {
                            LOGE("Offset too big: %" PRIxPTR, offset);
                            LOGD("%s => %p", gum_symbol_name_from_address(addr), addr);
                        }
                    }
                    else
                    {
                        LOGE("Not found: %p", (void *)addr);
                        LOGD("%s => %p", gum_symbol_name_from_address(addr), addr);
                    }

                    // LOGD("%s => %p", gum_symbol_name_from_address(addr), addr);
                }
                if (!result.empty())
                    hookerData->backtraced.push_back(result);
            }
        }

        virtual void on_enter(Gum::InvocationContext *context)
        {
            auto hookerData = context->get_listener_function_data<HookerData>();
            if (hookerData->backtracing)
            {
                Backtracer(context);
            }

            hookerData->hitCount++;
            hookerData->time = 1.f;

            // 应用调用参数修改：static 方法参数从 idx0 开始，非 static 的 this 占 idx0
            {
                auto modIt = ClassesTab::hookerArgMods.find(hookerData->method);
                if (modIt != ClassesTab::hookerArgMods.end())
                {
                    int argBase = Il2cpp::GetIsMethodStatic(hookerData->method) ? 0 : 1;
                    for (auto &[paramIndex, mod] : modIt->second)
                    {
                        if (!mod.value)
                            continue;
                        context->replace_nth_argument((unsigned int)(argBase + paramIndex), mod.value);
                    }
                }
            }

            if (!Il2cpp::GetIsMethodStatic(hookerData->method))
            {
                auto klass = hookerData->method->getClass();
                if (klass && !Il2cpp::GetClassIsValueType(klass) && !Il2cpp::GetClassIsEnum(klass))
                {
                    auto thiz = context->get_nth_argument<Il2CppObject *>(0);
                    if (thiz && thiz->klass)
                        HookerData::collectSet[klass].emplace(thiz);
                }
            }
                // auto paramsInfo = hookerData->method->getParamsInfo();
                // for (int i = 0; i < paramsInfo.size(); i++)
                // {
                //     auto [name, type] = paramsInfo[i];
                //     auto param = context->get_nth_argument_ptr(idx);
                //     LOGD("%s %s => %p", type->getName(), name, param);
                //     if (strcmp(type->getName(), "System.String") == 0)
                //     {
                //         if (param)
                //         {
                //             auto obj = (Il2CppString *)param;
                //             auto str = obj->to_string();
                //             LOGD("%s %s => %s", type->getName(), name, str.c_str());
                //         }
                //         else
                //         {
                //             LOGE("%s %s => null", type->getName(), name);
                //         }
                //     }
                //     else
                //     {
                //         auto ToString = type->getClass()->getMethod("ToString", 0);
                //         if (ToString)
                //         {
                //             if (type->isValueType() || type->isEnum())
                //             {
                //                 // if (type->isEnum())
                //                 // {
                //                 auto boxed = Il2cpp::GetBoxedValue(type->getClass(), &param);
                //                 auto obj = ToString->invoke_static<Il2CppString *>(boxed);
                //                 auto str = obj->to_string();
                //                 LOGD("%s %s => %s", type->getName(), name, str.c_str());
                //                 // }
                //                 // else
                //                 // {
                //                 //     auto obj = ToString->invoke_static<Il2CppString *>(&param);
                //                 //     auto str = obj->to_string();
                //                 //     LOGD("%s %s => %s", type->getName(), name, str.c_str());
                //                 // }
                //             }
                //             else
                //             {
                //                 auto obj = ToString->invoke_static<Il2CppString *>(param);
                //                 auto str = obj->to_string();
                //                 LOGD("%s %s => %s", type->getName(), name, str.c_str());
                //             }
                //         }
                //         else
                //         {
                //             LOGE("%s %s => %p", type->getName(), name, param);
                //         }
                //     }
                //     idx++;
                // }

            auto name = hookerData->method->getName();
            char buffer[256]{0};
            sprintf(buffer, "%p | %s::%s", (void *)hookerData->method->getAbsAddress(),
                    hookerData->method->getClass()->getName(), name);
            if (!HookerData::visited.empty())
            {
                for (auto it = HookerData::visited.rbegin(); it != HookerData::visited.rend(); ++it)
                {
                    if (it->name == buffer)
                    {
                        it->goneTime = 10.f;
                        it->time = 2.f;
                        it->hitCount++;
                        // std::rotate(HookerData::visited.rbegin(), it + 1, HookerData::visited.rend());
                        return;
                    }
                }
                // LOGD("%s", hookerData->method->getName());
            }
            HookerData::visited.push_back({buffer, 2.f, 10.f, 0});
        }

        virtual void on_leave(Gum::InvocationContext *context)
        {
        }
    };

    // Lua 钩子数据：被挂钩方法与其回调在 Lua registry 中的引用
    struct LuaHookData
    {
        MethodInfo *method = nullptr;
        int callbackRef = -1;
    };

    // Lua 钩子监听器：进入方法时整理实参并交由脚本引擎执行 Lua 回调
    class LuaHookListener : public Gum::InvocationListener
    {
      public:
        virtual void on_enter(Gum::InvocationContext *context)
        {
            auto data = context->get_listener_function_data<LuaHookData>();
            if (!data || !data->method)
                return;

            const bool isStatic = Il2cpp::GetIsMethodStatic(data->method);
            const int argCount = (int)Il2cpp::GetMethodParamCount(data->method) + (isStatic ? 0 : 1);
            std::vector<void *> args;
            args.reserve(argCount);
            for (int i = 0; i < argCount; i++)
                args.push_back(context->get_nth_argument_ptr(i));

            HookContext ctx{data->method, args.empty() ? nullptr : args.data(), argCount};
            Script::InvokeHookCallback(data->callbackRef, &ctx);
        }

        virtual void on_leave(Gum::InvocationContext *context)
        {
        }
    };

    Gum::RefPtr<Gum::Interceptor> interceptor;
    TraceListener *traceListener;
    std::unordered_map<void *, std::unique_ptr<TraceListener>> traceListeners;
    void Init()
    {
        interceptor = Gum::Interceptor_obtain();
        traceListener = new TraceListener();
    }

    bool Trace(MethodInfo *method, HookerData *data)
    {
        auto it = traceListeners.find(data->method->methodPointer);
        if (it != traceListeners.end())
        {
            LOGE("Already hooked %s", data->method->getName());
            return false;
        }
        traceListeners[method->methodPointer] = std::make_unique<TraceListener>();
        bool result = interceptor->attach(method->methodPointer, traceListeners[method->methodPointer].get(), data);
        if (!result)
        {
            traceListeners.erase(method->methodPointer);
        }
        return result;
    }

    bool Untrace(MethodInfo *method)
    {
        auto it = traceListeners.find(method->methodPointer);
        if (it == traceListeners.end())
            return false;

        interceptor->detach(it->second.get());
        traceListeners.erase(it);
        return true;
    }

    bool isTraced(MethodInfo *method)
    {
        return traceListeners.find(method->methodPointer) != traceListeners.end();
    }

    // 已注册的 Lua 钩子：句柄 -> 条目
    struct LuaHookEntry
    {
        MethodInfo *method = nullptr;
        int callbackRef = -1;
        std::unique_ptr<LuaHookListener> listener;
        std::unique_ptr<LuaHookData> data;
    };

    std::unordered_map<int, LuaHookEntry> luaHooks;
    int luaHookNextId = 1;

    int HookLua(MethodInfo *method, int callbackRef)
    {
        if (!method || !method->methodPointer || callbackRef < 0)
            return 0;

        if (!interceptor)
            Init();

        // 同一方法重复挂钩时先解除旧钩子，避免重复插桩
        std::vector<int> stale;
        for (const auto &[id, entry] : luaHooks)
        {
            if (entry.method == method)
                stale.push_back(id);
        }
        for (int id : stale)
            UnhookLua(id);

        LuaHookEntry entry;
        entry.method = method;
        entry.callbackRef = callbackRef;
        entry.listener = std::make_unique<LuaHookListener>();
        entry.data = std::make_unique<LuaHookData>();
        entry.data->method = method;
        entry.data->callbackRef = callbackRef;

        if (!interceptor->attach(method->methodPointer, entry.listener.get(), entry.data.get()))
        {
            LOGE("Failed to hook lua callback: %s::%s", method->getClass()->getName(), method->getName());
            return 0;
        }

        const int id = luaHookNextId++;
        luaHooks.emplace(id, std::move(entry));
        LOGI("Lua hook #%d attached to %s::%s", id, method->getClass()->getName(), method->getName());
        return id;
    }

    bool UnhookLua(int id)
    {
        auto it = luaHooks.find(id);
        if (it == luaHooks.end())
            return false;

        if (interceptor && it->second.listener)
            interceptor->detach(it->second.listener.get());
        // 释放 Lua registry 引用，避免回调函数被永久持有
        Script::ReleaseHookCallback(it->second.callbackRef);
        luaHooks.erase(it);
        return true;
    }
} // namespace Frida
