#include "ClassesTab.h"
#include "Il2cpp/Il2cpp.h"
#include "Il2cpp/il2cpp-tabledefs.h"
#include "Tool/Clipboard.h"
#include "Tool/Keyboard.h"
#include "Tool/Lang.h"
#include "Tool/Patcher.h"
#include "Tool/TextureCache.h"
#include "Tool/Tool.h"
#include "Tool/Util.h"
#include "KittyMemory/KittyMemory.h"
#include "imgui/imgui.h"
#include <mutex>
#include <thread>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include "sstream"

extern std::vector<Il2CppImage *> g_Images;
extern Il2CppImage *g_Image;
extern bool saferInspector;
extern bool sideSplit;

constexpr int MAX_CLASSES = 500;

int maxLine{5};
std::unordered_map<void *, HookerData> hookerMap;
std::mutex hookerMtx;

// 为值类型创建一个零初始化的装箱实例（用于没有构造函数的结构体）
// klass: 目标值类型类；返回装箱后的对象指针，类尺寸无效时返回 nullptr
static Il2CppObject *CreateZeroInitValueType(Il2CppClass *klass)
{
    if (!klass)
        return nullptr;
    size_t size = klass->getSize();
    if (size == 0)
        return nullptr;
    std::vector<uint8_t> zero(size, 0);
    return Il2cpp::GetBoxedValue(klass, zero.data());
}

// 将文本解析为跟踪器参数替换值（原始 ABI 值，可直接写入寄存器）
// type: 参数类型；text: 用户输入文本；keepAlive: 引用类型（字符串）保活对象输出
// 返回值可直接用于 Gum 的 replace_nth_argument；解析失败或类型不支持时返回 nullptr
static void *ParseHookerArgValue(Il2CppType *type, const std::string &text, Il2CppObject **keepAlive)
{
    if (keepAlive)
        *keepAlive = nullptr;
    if (!type || text.empty())
        return nullptr;

    const char *typeName = type->getName();
    try
    {
        // 字符串：创建托管字符串并交给调用方保活
        if (strcmp(typeName, "System.String") == 0)
        {
            auto str = Il2cpp::NewString(text.c_str());
            if (keepAlive)
                *keepAlive = str;
            return str;
        }

        // 基元类型：按原始位模式写入寄存器
        if (type->isPrimitive())
        {
            uint64_t bits = 0;
            if (strcmp(typeName, "System.Boolean") == 0)
            {
                bits = (text == "True" || text == "true" || text == "1") ? 1 : 0;
            }
            else if (strcmp(typeName, "System.Single") == 0)
            {
                float value = std::stof(text);
                memcpy(&bits, &value, sizeof(value));
            }
            else if (strcmp(typeName, "System.Double") == 0)
            {
                // double 需要 64 位寄存器才能承载完整位模式
                if (sizeof(void *) < sizeof(double))
                {
                    LOGE("Double arg override requires 64-bit, skipped: %s", typeName);
                    return nullptr;
                }
                double value = std::stod(text);
                memcpy(&bits, &value, sizeof(value));
            }
            else
            {
                bits = static_cast<uint64_t>(std::stoll(text));
            }
            return reinterpret_cast<void *>(static_cast<uintptr_t>(bits));
        }

        // 枚举按底层整数传递
        if (type->isEnum())
            return reinterpret_cast<void *>(static_cast<uintptr_t>(std::stoll(text)));

        // IntPtr/UIntPtr 与普通引用类型：按地址解析（支持 0x 十六进制）
        if (strcmp(typeName, "System.IntPtr") == 0 || strcmp(typeName, "System.UIntPtr") == 0)
            return reinterpret_cast<void *>(static_cast<uintptr_t>(std::stoull(text, nullptr, 0)));

        if (!type->isValueType())
            return reinterpret_cast<void *>(static_cast<uintptr_t>(std::stoull(text, nullptr, 0)));

        // 结构体等复杂值类型暂不支持直接覆写
        LOGE("Unsupported hooker arg type: %s", typeName);
    }
    catch (const std::exception &e)
    {
        LOGE("Failed to parse hooker arg (%s): %s", typeName, e.what());
    }
    return nullptr;
}

#ifndef USE_FRIDA
void hookerHandler(void *address, DobbyRegisterContext *ctx)
{
    std::lock_guard guard(hookerMtx);
    if (hookerMap.find(address) != hookerMap.end())
    {
        auto &hookerData = hookerMap[address];
        hookerData.hitCount++;
        hookerData.time = 1.f;

        auto name = hookerData.method->getName();
        char buffer[128]{0};
        sprintf(buffer, "%p | %s", hookerData.method->getAbsAddress(), name);
        if (!HookerData::visited.empty())
        {
            // auto &back = HookerData::visited.back();
            // if (back.name == name)
            // {
            //     back.goneTime = 10.f;
            //     back.time = 2.f;
            //     back.hitCount++;
            //     return;
            // }
            int i = 0;
            for (auto it = HookerData::visited.rbegin(); it != HookerData::visited.rend(); ++it)
            {
                if (i >= maxLine)
                {
                    break;
                }
                if (it->name == buffer)
                {
                    it->goneTime = 10.f;
                    it->time = 2.f;
                    it->hitCount++;
                    return;
                }
                i++;
            }
        }
        HookerData::visited.push_back({buffer, 2.f, 10.f, 0});
        // LOGD("%s", hookerData.method->getName());
    }
}
#endif

ClassesTab::MethodList &ClassesTab::buildMethodMap(Il2CppClass *klass)
{
    static Il2CppClass *lastClass = nullptr;
    static MethodList methodList;

    if (lastClass != klass)
    {
        methodList.clear();
        auto methods = klass->getMethods();
        LOGD("Rebuilding %s | %zu methods", klass->getName(), methods.size());
        for (auto method : methods)
        {
            auto paramsInfo = method->getParamsInfo();
            methodList.push_back({method, paramsInfo});
        }
        LOGD("Rebuilt %zu methods", methodList.size());
        lastClass = klass;
    }
    return methodList;
}

ClassesTab::ClassesTab()
{
    selectedImage = g_Image;

    for (int i = 0; i < g_Images.size(); i++)
    {
        if (g_Images[i] == selectedImage)
        {
            selectedImageIndex = i;
            break;
        }
    }
    if (selectedImage)
    {
        classes = selectedImage->getClasses();
        filteredClasses = classes;
    }
}

ClassesTab::Paths &ClassesTab::getJsonPaths(Il2CppObject *object)
{
    return dataMap[object].second;
}

void ClassesTab::setJsonObject(Il2CppObject *object)
{
    // std::vector<uintptr_t> visited{};
    // dataMap[object].first = object->dump(visited, 9999);
    dataMap[object].first = object->dump({});

    tabMap.emplace(object, true);
}

ClassesTab::Json &ClassesTab::getJsonObject(Il2CppObject *object)
{
    return dataMap[object].first.second;
}

void ClassesTab::ImGuiObjectSelector(int id, Il2CppClass *klass, const char *prefix,
                                     std::function<void(Il2CppObject *)> onSelect, bool canNew)
{
    ImGui::PushID(id);
    if (!klass)
    {
        ImGui::Text("%s", Lang::T("无效类", "Invalid class"));
        ImGui::PopID();
        return;
    }
    if (Il2cpp::GetClassIsValueType(klass) || Il2cpp::GetClassIsEnum(klass))
    {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 180, 80, 255));
        ImGui::TextWrapped("%s", Lang::T("值类型/枚举不能作为 GC 对象收集，已跳过以免崩溃。",
                                         "Value types / enums cannot be collected as GC objects (skipped to avoid a crash)."));
        ImGui::PopStyleColor();
        ImGui::PopID();
        return;
    }
    static std::unordered_map<void *, bool> scanState;
    auto &scanning = scanState[klass];
    if (ImGui::Button(Lang::T("查找对象", "Find Objects")))
    {
        scanning = true;
        std::thread(
            [&scanning](Il2CppClass *klass)
            {
                objectMap[klass] = Il2cpp::GC::FindObjects(klass);
                scanning = false;
            },
            klass)
            .detach();
    }
    ImGui::PopID();
    ImGuiIO &io = ImGui::GetIO();
    float width = io.DisplaySize.x;
    float height = io.DisplaySize.y;
    // FIXME: DRY!!
    {
        auto &objects = objectMap[klass];
        ImGui::SetNextWindowSizeConstraints(ImVec2(-1, 0), ImVec2(-1, height / 3));
        ImGui::BeginChild("##ScrollingObjects", ImVec2(width / 1.4f, 0), ImGuiChildFlags_AutoResizeY);
        {
            ImGui::SeparatorText("Result Object");
            if (objects.empty())
            {
                if (scanning)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(50, 255, 50, 255));
                }
                else
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 50, 50, 255));
                }
                if (scanning)
                {
                    ImGui::Text("Scanning...");
                }
                else
                {
                    ImGui::Text("Nothing...");
                }
                ImGui::PopStyleColor();
            }
            else
            {
                if (objects.size() > 100)
                {
                    ImGui::Text("Showing 100 of %zu objects", objects.size());
                }
                for (auto it = objects.begin(); it != (objects.size() > 100 ? objects.begin() + 100 : objects.end());)
                {
                    auto object = *it;
                    if (!object)
                    {
                        ++it;
                        continue;
                    }
                    char buff[64];
                    sprintf(buff, "%s [%p]", prefix, object);
                    auto size = ImGui::GetWindowSize();
                    if (ImGui::Button(buff, ImVec2(size.x / 1.5, 0)))
                    {
                        onSelect(object);
                    }
                    ImGui::SetItemTooltip("%s", object->klass->getName());
                    ImGui::SameLine();
                    ImGui::PushID(buff);
                    if (ImGui::Button("Remove"))
                    {
                        it = objects.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                    ImGui::PopID();
                }
            }
        }
        ImGui::EndChild();
    }
    ImGui::Separator();

    // ImGui::Text("Inherited from %s", klass->getName());
    // for (auto [setKlass, _] : savedSet)
    // {
    //     if (klass != setKlass && Il2cpp::IsClassParentOf(setKlass, klass))
    //     {
    //         ImGui::SetNextWindowSizeConstraints(ImVec2(-1, 0), ImVec2(-1, height / 3));
    //         ImGui::BeginChild("##ScrollingInheritObjects", ImVec2(width / 2.f, 0), ImGuiChildFlags_AutoResizeY);
    //         for (auto object : savedSet[setKlass])
    //         {
    //             char buff[64];
    //             sprintf(buff, "%s [%p]", setKlass->getName(), object);
    //             if (ImGui::Button(buff))
    //             {
    //                 onSelect(object);
    //             }
    //         }
    //         ImGui::EndChild();
    //     }
    // }
    // ImGui::Separator();

    {
        char buffer[128];
        sprintf(buffer, "Inherited from %s", klass->getName());
        if (ImGui::CollapsingHeader(buffer))
        {
            ImGui::SetNextWindowSizeConstraints(ImVec2(-1, 0), ImVec2(-1, height / 3));
            ImGui::BeginChild("##ScrollingInheritedObjects", ImVec2(width / 1.4f, 0), ImGuiChildFlags_AutoResizeY);
            {
                // {
                //     char buffer[128];
                //     sprintf(buffer, "Inherited from %s", klass->getName());
                //     ImGui::SeparatorText(buffer);
                // }
                bool empty = true;
                for (auto &[setKlass, _] : savedSet)
                {
                    if (klass != setKlass && Il2cpp::IsClassParentOf(setKlass, klass))
                    {
                        auto &objects = savedSet[setKlass];
                        for (auto it = objects.begin(); it != objects.end();)
                        {
                            empty = false;
                            auto object = *it;
                            char buff[64];
                            sprintf(buff, "%s [%p]", setKlass->getName(), object);
                            auto size = ImGui::GetWindowSize();
                            if (ImGui::Button(buff, ImVec2(size.x / 1.5, 0)))
                            {
                                onSelect(object);
                            }
                            ImGui::SetItemTooltip("%s", object->klass->getName());
                            ImGui::SameLine();
                            ImGui::PushID(buff);
                            if (ImGui::Button("Remove"))
                            {
                                it = objects.erase(it);
                            }
                            else
                            {
                                ++it;
                            }
                            ImGui::PopID();
                        }
                    }
                }

                if (empty)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 50, 50, 255));
                    ImGui::Text("Nothing...");
                    ImGui::PopStyleColor();
                }
            }
            ImGui::EndChild();
        }
    }
    // ImGui::Text("Saved objects");
    // if (savedSet[klass].empty())
    // {
    //     ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 50, 50, 255));
    //     ImGui::Text("No objects");
    //     ImGui::PopStyleColor();
    // }
    // else
    // {
    //     ImGui::SetNextWindowSizeConstraints(ImVec2(-1, 0), ImVec2(-1, height / 3));
    //     ImGui::BeginChild("##ScrollingSavedObjects", ImVec2(width / 2.f, 0), ImGuiChildFlags_AutoResizeY);
    //     for (auto object : savedSet[klass])
    //     {
    //         char buff[64];
    //         sprintf(buff, "%s [%p]", klass->getName(), object);
    //         if (ImGui::Button(buff))
    //         {
    //             onSelect(object);
    //         }
    //     }
    //     ImGui::EndChild();
    // }
    // ImGui::Separator();

    {
        if (ImGui::CollapsingHeader("Saved Objects"))
        {
            auto &objects = savedSet[klass];
            ImGui::SetNextWindowSizeConstraints(ImVec2(-1, 0), ImVec2(-1, height / 3));
            ImGui::BeginChild("##ScrollingSavedObjects", ImVec2(width / 1.4f, 0), ImGuiChildFlags_AutoResizeY);
            {
                // ImGui::SeparatorText("Saved Objects");
                if (objects.empty())
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 50, 50, 255));
                    ImGui::Text("Nothing...");
                    ImGui::PopStyleColor();
                }
                else
                {
                    for (auto it = objects.begin(); it != objects.end();)
                    {
                        auto object = *it;
                        char buff[64];
                        sprintf(buff, "%s [%p]", klass->getName(), object);
                        auto size = ImGui::GetWindowSize();
                        if (ImGui::Button(buff, ImVec2(size.x / 1.5, 0)))
                        {
                            onSelect(object);
                        }
                        ImGui::SetItemTooltip("%s", object->klass->getName());
                        ImGui::SameLine();
                        ImGui::PushID(buff);
                        if (ImGui::Button("Remove"))
                        {
                            it = objects.erase(it);
                        }
                        else
                        {
                            ++it;
                        }
                        ImGui::PopID();
                    }
                }
            }
            ImGui::EndChild();
        }
    }
    // ImGui::Text("Collected objects");
    // if (HookerData::collectSet.find(klass) == HookerData::collectSet.end())
    // {
    //     ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 50, 50, 255));
    //     ImGui::Text("No objects");
    //     ImGui::PopStyleColor();
    // }
    // else
    // {
    //     ImGui::SetNextWindowSizeConstraints(ImVec2(-1, 0), ImVec2(-1, height / 3));
    //     ImGui::BeginChild("##ScrollingCollectedObjects", ImVec2(width / 2.f, 0), ImGuiChildFlags_AutoResizeY);
    //     for (auto object : HookerData::collectSet[klass])
    //     {
    //         char buff[64];
    //         sprintf(buff, "%s [%p]", klass->getName(), object);
    //         if (ImGui::Button(buff))
    //         {
    //             onSelect(object);
    //         }
    //     }
    //     ImGui::EndChild();
    // }
    // ImGui::Separator();

    {
        if (ImGui::CollapsingHeader("Collected Objects"))
        {
            auto &objects = HookerData::collectSet[klass];
            ImGui::SetNextWindowSizeConstraints(ImVec2(-1, 0), ImVec2(-1, height / 3));
            ImGui::BeginChild("##ScrollingCollectedObjects", ImVec2(width / 1.4f, 0), ImGuiChildFlags_AutoResizeY);
            {
                // ImGui::SeparatorText("Collected Objects");
                if (objects.empty())
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 50, 50, 255));
                    ImGui::Text("Nothing...");
                    ImGui::PopStyleColor();
                }
                else
                {
                    for (auto it = objects.begin(); it != objects.end();)
                    {
                        auto object = *it;
                        char buff[64];
                        sprintf(buff, "%s [%p]", klass->getName(), object);
                        auto size = ImGui::GetWindowSize();
                        if (ImGui::Button(buff, ImVec2(size.x / 1.5, 0)))
                        {
                            onSelect(object);
                        }
                        ImGui::SetItemTooltip("%s", object->klass->getName());
                        ImGui::SameLine();
                        ImGui::PushID(buff);
                        if (ImGui::Button("Remove"))
                        {
                            it = objects.erase(it);
                        }
                        else
                        {
                            ++it;
                        }
                        ImGui::PopID();
                    }
                }
            }
            ImGui::EndChild();
        }
    }
    // if (canNew)
    // {
    //     if (ImGui::Button("New"))
    //     {
    //         auto newObject = klass->New();
    //         newObjectMap[klass].push_back(newObject);
    //         if (Il2cpp::GetClassType(klass)->isValueType())
    //         {
    //             newObject = (Il2CppObject *)Il2cpp::GetUnboxedValue(newObject);
    //         }
    //         onSelect(newObject);
    //     }
    // }
    // else
    // {
    //     ImGui::Text("Created objects");
    // }

    // ImGui::SetNextWindowSizeConstraints(ImVec2(-1, 0), ImVec2(-1, height / 3));
    // ImGui::BeginChild("##ScrollingNewObjects", ImVec2(width / 2.f, 0), ImGuiChildFlags_AutoResizeY);
    // for (auto createObject : newObjectMap[klass])
    // {
    //     char buff[64];
    //     sprintf(buff, "%s [%p]", prefix, createObject);
    //     if (ImGui::Button(buff))
    //     {
    //         onSelect(createObject);
    //     }
    // }
    // ImGui::EndChild();

    {
        if (ImGui::CollapsingHeader("Created Objects"))
        {
            auto &objects = newObjectMap[klass];
            ImGui::SetNextWindowSizeConstraints(ImVec2(-1, 0), ImVec2(-1, height / 3));
            ImGui::BeginChild("##ScrollingNewObjects", ImVec2(width / 1.4f, 0), ImGuiChildFlags_AutoResizeY);
            {
                // ImGui::SeparatorText("Created Objects");
                if (objects.empty())
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 50, 50, 255));
                    ImGui::Text("Nothing...");
                    ImGui::PopStyleColor();
                }
                else
                {
                    for (auto it = objects.begin(); it != objects.end();)
                    {
                        auto object = *it;
                        char buff[64];
                        sprintf(buff, "%s [%p]", prefix, object);
                        auto size = ImGui::GetWindowSize();
                        if (ImGui::Button(buff, ImVec2(size.x / 1.5, 0)))
                        {
                            onSelect(object);
                        }
                        ImGui::SetItemTooltip("%s", object->klass->getName());
                        ImGui::SameLine();
                        ImGui::PushID(buff);
                        if (ImGui::Button("Remove"))
                        {
                            it = objects.erase(it);
                        }
                        else
                        {
                            ++it;
                        }
                        ImGui::PopID();
                    }
                }
                if (canNew)
                {
                    if (ImGui::Button("New"))
                    {
                        auto newObject = klass->New();
                        newObjectMap[klass].push_back(newObject);
                        if (Il2cpp::GetClassType(klass)->isValueType())
                        {
                            // Il2cpp::GC::KeepAlive(newObject);
                            newObject = (Il2CppObject *)Il2cpp::GetUnboxedValue(newObject);
                        }
                        onSelect(newObject);
                    }
                }
            }
            ImGui::EndChild();
        }
    }
    // if (!objectMap.empty())
    // {
    //     ImGui::Separator();
    //     static Il2CppObject *selected = nullptr;
    //     if (ImGui::Button("Force Select Existing"))
    //     {
    //         ImGui::OpenPopup("ForceObjectSelector");
    //         selected = nullptr;
    //     }
    //     if (ImGui::BeginPopup("ForceObjectSelector"))
    //     {
    //         for (auto it = objectMap.begin(); it != objectMap.end() && selected == nullptr; it++)
    //         {
    //             auto [objKlass, objects] = *it;
    //             char klassName[256]{0};
    //             for (auto object : objects)
    //             {
    //                 sprintf(klassName, "%s [%p]", objKlass->getName(), object);
    //                 if (ImGui::Button(klassName))
    //                 {
    //                     selected = object;
    //                     break;
    //                 }
    //             }
    //         }
    //         ImGui::EndPopup();
    //     }
    //     if (selected)
    //     {
    //         onSelect(selected);
    //         selected = nullptr;
    //     }
    // }
}

void ClassesTab::CallerView(Il2CppClass *klass, MethodInfo *method, const MethodParamList &paramsInfo,
                            Il2CppObject *thiz)
{
    static ImGuiIO &io = ImGui::GetIO();
    bool methodIsStatic = Il2cpp::GetIsMethodStatic(method);
    auto &params = paramMap[method];
    if (!methodIsStatic && !thiz)
    {
        auto &thisParam = params["this"];
        char thisLabel[128]{0};
        sprintf(thisLabel, "%s this", Il2cpp::GetClassType(klass)->getName());
        if (!thisParam.value.empty())
        {
            sprintf(thisLabel, "%s = %s", thisLabel, thisParam.value.c_str());
        }
        if (ImGui::Button(thisLabel))
        {
            ImGui::OpenPopup("ThisObjectSelector");
        }
        if (ImGui::BeginPopup("ThisObjectSelector"))
        {
            if (Il2cpp::GetClassIsValueType(klass))
            {
                // 结构体实例方法：值类型无法作为 GC 对象收集，提供零初始化实例创建
                if (ImGui::Button(Lang::T("创建零初始化实例", "Create Zero-Init Instance")))
                {
                    auto boxed = CreateZeroInitValueType(klass);
                    if (boxed)
                    {
                        char objStr[16]{0};
                        sprintf(objStr, "%p", boxed);
                        thisParam.value = objStr;
                        thisParam.object = boxed;
                    }
                    ImGui::CloseCurrentPopup();
                }
            }
            else
            {
                ImGuiObjectSelector(
                    ImGui::GetID("ThisObjectSelector"), klass, "this",
                    [&thisParam](Il2CppObject *object)
                    {
                        char objStr[16]{0};
                        sprintf(objStr, "%p", object);
                        thisParam.value = objStr;
                        thisParam.object = object;
                        ImGui::CloseCurrentPopup();
                    },
                    strcmp(method->getName(), ".ctor") == 0);
            }
            ImGui::EndPopup();
        }
    }
    for (int k = 0; k < paramsInfo.size(); k++)
    {
        auto &[name, type] = paramsInfo[k];

        char paramKey[64]{0};
        sprintf(paramKey, "%p%s%d", method, name, k);
        auto &param = params[paramKey];

        char buttonLabel[128]{0};
        sprintf(buttonLabel, "%s %s", type->getName(), name);
        if (!param.value.empty())
        {
            sprintf(buttonLabel, "%s = %s", buttonLabel, param.value.c_str());
        }
        ImGui::PushID(k);
        if (ImGui::Button(buttonLabel))
        {
            bool isString = strcmp(type->getName(), "System.String") == 0;
            // IntPtr/UIntPtr 不是 primitive，但同样按数值输入处理
            bool isPointer = strcmp(type->getName(), "System.IntPtr") == 0 ||
                             strcmp(type->getName(), "System.UIntPtr") == 0;
            if (type->isPrimitive() || isString || isPointer)
            {
                if (strcmp(type->getName(), "System.Boolean") == 0)
                {
                    // ImGui::OpenPopup("BooleanSelector");
                    poper.Open("BooleanSelector", [&param](const std::string &result) { param.value = result; });
                }
                else
                {
                    Keyboard::Open(
                        [&param, &isString](const std::string &text)
                        {
                            if (isString)
                            {
                                param.object = Il2cpp::NewString(text.c_str());
                            }
                            param.value = text;
                        });
                }
            }
            else if (type->isEnum())
            {
                poper.Open(
                    "EnumSelector", [&param](const std::string &result) { param.value = result; }, type);
            }
            // else if (!type->isValueType() && !(type->isArray() || type->isList()
            // ||
            //                                    strstr(type->getName(),
            //                                    "System.Object")))
            // else if (!strstr(type->getName(), "System.Object"))
            else
            {
                ImGui::OpenPopup("ParamObjectSelector");
            }
        }
        if (ImGui::BeginPopup("ParamObjectSelector"))
        {
            if (type->isValueType() && !type->isEnum())
            {
                // 值类型（结构体）不能作为 GC 对象收集，这里提供零初始化实例创建
                if (ImGui::Button(Lang::T("创建零初始化实例", "Create Zero-Init Instance")))
                {
                    auto boxed = CreateZeroInitValueType(type->getClass());
                    if (boxed)
                    {
                        char objStr[16]{0};
                        sprintf(objStr, "%p", boxed);
                        param.value = objStr;
                        param.object = boxed;
                    }
                    ImGui::CloseCurrentPopup();
                }
            }
            else
            {
                ImGuiObjectSelector(ImGui::GetID("ParamObjectSelector"), type->getClass(), name,
                                    [&param](Il2CppObject *object)
                                    {
                                        char objStr[16]{0};
                                        sprintf(objStr, "%p", object);
                                        param.value = objStr;
                                        param.object = object;
                                        ImGui::CloseCurrentPopup();
                                    });
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(30, 200, 25, 128));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(30, 200, 25, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(30, 200, 25, 255));
    // 调用逻辑抽成 lambda，供 Call 与 Recall 复用
    auto doCall = [&]()
    {
        auto paramsInfo = method->getParamsInfo();
        auto params = paramMap[method];
        auto arrayParams = (paramsInfo.size() > 0) ? new Il2CppObject *[paramsInfo.size()] : nullptr;

        bool hasParams = true;
        Il2CppObject *thisParam = nullptr;
        if (!methodIsStatic && !thiz)
        {
            if (params["this"].value.empty())
            {
                hasParams = false;
            }
            else
            {
                thisParam = params["this"].object;
                LOGD("this = %s", params["this"].value.c_str());
            }
        }
        else if (thiz)
        {
            thisParam = thiz;
        }

        for (int k = 0; k < paramsInfo.size(); k++)
        {
            auto &[name, type] = paramsInfo[k];

            char paramKey[64]{0};
            sprintf(paramKey, "%p%s%d", method, name, k);
            auto &param = params[paramKey];
            LOGD("%s %s = %s", type->getName(), name, param.value.c_str());
            if (!param.value.empty())
            {
                if (strcmp(type->getName(), "System.Int32") == 0)
                {
                    ValueType<int> value{std::stoi(param.value)};
                    auto boxedValue = value.box(type->getClass());
                    arrayParams[k] = boxedValue;
                }
                else if (strcmp(type->getName(), "System.Int64") == 0)
                {
                    ValueType<long> value{std::stol(param.value)};
                    auto boxedValue = value.box(type->getClass());
                    arrayParams[k] = boxedValue;
                }
                else if (strcmp(type->getName(), "System.UInt32") == 0)
                {
                    ValueType<unsigned int> value{static_cast<unsigned int>(std::stoul(param.value))};
                    auto boxedValue = value.box(type->getClass());
                    arrayParams[k] = boxedValue;
                }
                else if (strcmp(type->getName(), "System.UInt64") == 0)
                {
                    ValueType<unsigned long> value{std::stoul(param.value)};
                    auto boxedValue = value.box(type->getClass());
                    arrayParams[k] = boxedValue;
                }
                else if (strcmp(type->getName(), "System.Single") == 0)
                {
                    ValueType<float> value{std::stof(param.value)};
                    auto boxedValue = value.box(type->getClass());
                    arrayParams[k] = boxedValue;
                }
                else if (strcmp(type->getName(), "System.Double") == 0)
                {
                    ValueType<double> value{std::stod(param.value)};
                    auto boxedValue = value.box(type->getClass());
                    arrayParams[k] = boxedValue;
                }
                else if (strcmp(type->getName(), "System.Int16") == 0)
                {
                    ValueType<int16_t> value{static_cast<int16_t>(std::stoi(param.value))};
                    auto boxedValue = value.box(type->getClass());
                    arrayParams[k] = boxedValue;
                }
                else if (strcmp(type->getName(), "System.UInt16") == 0)
                {
                    ValueType<uint16_t> value{static_cast<uint16_t>(std::stoul(param.value))};
                    auto boxedValue = value.box(type->getClass());
                    arrayParams[k] = boxedValue;
                }
                else if (strcmp(type->getName(), "System.SByte") == 0)
                {
                    ValueType<int8_t> value{static_cast<int8_t>(std::stoi(param.value))};
                    auto boxedValue = value.box(type->getClass());
                    arrayParams[k] = boxedValue;
                }
                else if (strcmp(type->getName(), "System.Byte") == 0)
                {
                    ValueType<uint8_t> value{static_cast<uint8_t>(std::stoul(param.value))};
                    auto boxedValue = value.box(type->getClass());
                    arrayParams[k] = boxedValue;
                }
                else if (strcmp(type->getName(), "System.Char") == 0)
                {
                    // 支持直接输入单个字符（取其码点）或输入十进制码点
                    uint16_t code = param.value.size() == 1
                                        ? static_cast<uint16_t>(static_cast<unsigned char>(param.value[0]))
                                        : static_cast<uint16_t>(std::stoul(param.value));
                    ValueType<uint16_t> value{code};
                    auto boxedValue = value.box(type->getClass());
                    arrayParams[k] = boxedValue;
                }
                else if (strcmp(type->getName(), "System.IntPtr") == 0)
                {
                    ValueType<intptr_t> value{static_cast<intptr_t>(std::stoull(param.value))};
                    auto boxedValue = value.box(type->getClass());
                    arrayParams[k] = boxedValue;
                }
                else if (strcmp(type->getName(), "System.UIntPtr") == 0)
                {
                    ValueType<uintptr_t> value{static_cast<uintptr_t>(std::stoull(param.value))};
                    auto boxedValue = value.box(type->getClass());
                    arrayParams[k] = boxedValue;
                }
                else if (strcmp(type->getName(), "System.Boolean") == 0)
                {
                    ValueType<int> value{param.value == "True" ? 1 : 0}; // using true/false sometimes causing
                                                                         // crash for me, don't know why
                    auto boxedValue = value.box(type->getClass());
                    arrayParams[k] = boxedValue;
                }
                else if (type->isEnum())
                {
                    arrayParams[k] = type->getClass()
                                         ->getField(param.value.c_str())
                                         ->getStaticValue<ValueType<int>>()
                                         .box(type->getClass());
                }
                else if (strcmp(type->getName(), "System.String") == 0)
                {
                    arrayParams[k] = Il2cpp::NewString(param.value.c_str());
                }
                else if (param.object)
                {
                    arrayParams[k] = param.object;
                }
                else
                {
                    LOGD("Unhandled type: %s %s", type->getName(), name);
                }
            }
            else
            {
                hasParams = false;
            }
        }
        if (hasParams)
        {
            Il2CppObject *result = nullptr;
            bool thisIsValue = false;
            void *valueThis = nullptr;
            if (strcmp(method->getName(), ".ctor") != 0 && thisParam && thisParam->klass &&
                Il2cpp::GetClassType(thisParam->klass)->isValueType())
            {
                thisIsValue = true;
                valueThis = Il2cpp::GetUnboxedValue(thisParam);
                result = Il2cpp::RuntimeInvokeConvertArgs(method, valueThis, arrayParams, paramsInfo.size());
            }
            else
            {
                result = Il2cpp::RuntimeInvokeConvertArgs(method, thisParam, arrayParams, paramsInfo.size());
            }
            LOGPTR(result);
            auto returnType = method->getReturnType();
            bool isVoid = returnType && strcmp(returnType->getName(), "System.Void") == 0;
            if (thisIsValue && thisParam)
            {
                // Write the mutated struct back so the inspector shows the real object.
                Il2cpp::GC::KeepAlive(thisParam);
                if (tabMap.count(thisParam))
                    setJsonObject(thisParam);
                for (auto &[obj, vis] : tabMap)
                {
                    if (obj == thisParam)
                        setJsonObject(obj);
                }
            }
            if (isVoid)
            {
                callResults.at(method).push_back({"System.Void", Lang::T("调用完成 (void)", "call completed (void)"), 0, thisParam});
            }
            else if (result && strcmp(method->getName(), ".ctor") != 0)
            {
                auto resultType = Il2cpp::GetClassType(result->klass);
                if (resultType->isPrimitive())
                {
                    std::vector<uintptr_t> visited;
                    auto j = result->dump(visited, 1);
                    callResults.at(method).push_back({resultType->getName(), j.begin().value().dump(), 0, nullptr});
                }
                else if (strcmp(resultType->getName(), "System.String") == 0)
                {
                    callResults.at(method).push_back(
                        {"System.String", ((Il2CppString *)result)->to_string(), 0, nullptr});
                }
                else if (resultType->isEnum())
                {
                    callResults.at(method).push_back(
                        {resultType->getName(), result->invoke_method<Il2CppString *>("ToString")->to_string(), 0,
                         nullptr});
                }
                else
                {
                    if (resultType->isValueType())
                    {
                        Il2cpp::GC::KeepAlive(result); // does this actually work?
                    }
                    auto toString = result->klass->getMethod("ToString", 0);
                    if (toString)
                    {
                        Il2CppString *str = nullptr;
                        if (resultType->isValueType())
                        {
                            auto thizz = Il2cpp::GetUnboxedValue(result);
                            // str = toString->invoke_static<Il2CppString *>(thizz);
                            str = (Il2CppString *)Il2cpp::RuntimeInvokeConvertArgs(toString, thizz, nullptr, 0);
                        }
                        else
                        {
                            str = toString->invoke_static<Il2CppString *>(result);
                        }
                        if (str)
                        {
                            callResults.at(method).push_back(
                                {resultType->getName(), str->to_string(), result->klass->getFields(true).size(), result});
                        }
                        else
                        {
                            callResults.at(method).push_back(
                                {resultType->getName(), "the call returned null",
                                 result->klass->getFields(true).size(), result});
                        }
                    }
                    else
                    {
                        char resultStr[16]{0};
                        sprintf(resultStr, "%p", result);
                        callResults.at(method).push_back(
                            {resultType->getName(), resultStr, result->klass->getFields(true).size(), result});
                    }
                }
                savedSet[resultType->getClass()].insert(result);
                // setJsonObject(result);
            }
            else
            {
                callResults.at(method).push_back({returnType ? returnType->getName() : "", "the call returned null", 0, nullptr});
            }
        }
        else
        {
            LOGE("Not all params are set!");
        }
        if (arrayParams)
            delete[] arrayParams;
    };
    float callBtnWidth = io.DisplaySize.x / 3;
    if (ImGui::Button("Call", ImVec2(callBtnWidth, 0)))
    {
        doCall();
    }
    ImGui::SameLine();
    if (ImGui::Button(Lang::T("再次调用", "Recall"), ImVec2(callBtnWidth, 0)))
    {
        doCall();
    }
    ImGui::PopStyleColor(3);
    if (!callResults.at(method).empty())
    {
        ImGui::Separator();
        ImGui::Text("%s", Lang::T("调用结果:", "Call Results:"));
        int ri = 0;
        for (auto &callResult : callResults.at(method))
        {
            ImGui::PushID(ri++);
            // 结构化面板：第一行显示返回类型名
            if (!callResult.typeName.empty())
            {
                ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), "%s", callResult.typeName.c_str());
            }
            if (callResult.object && callResult.fieldCount > 0)
            {
                // 引用类型：点击展开内联 ImGuiJson 树
                char label[512];
                snprintf(label, sizeof(label), "%s  (%zu fields)###cr%p%d", callResult.value.c_str(),
                         callResult.fieldCount, callResult.object, ri);
                if (ImGui::TreeNode(label))
                {
                    if (dataMap.find(callResult.object) == dataMap.end() ||
                        !dataMap[callResult.object].first.first)
                    {
                        setJsonObject(callResult.object);
                    }
                    ImGuiJson(callResult.object);
                    ImGui::TreePop();
                }
            }
            else if (callResult.object)
            {
                char btn[512];
                snprintf(btn, sizeof(btn), "%s###cr%p%d", callResult.value.c_str(), callResult.object, ri);
                if (ImGui::Button(btn))
                {
                    setJsonObject(callResult.object);
                }
            }
            else
            {
                ImGui::Text("%s", callResult.value.c_str());
            }
            ImGui::PopID();
            ImGui::Separator();
        }
        // ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(200, 30, 25, 128));
        // ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(200, 30, 25,
        // 255)); ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(200, 30, 25,
        // 255)); if (ImGui::Button("Clear##CallResults",
        //                   ImVec2(ImGui::GetIO().DisplaySize.x / 3.f, 0)))
        // {
        //     callResults.at(method).clear();
        // }
        // ImGui::PopStyleColor(3);
    }
}

bool ClassesTab::isMethodHooked(MethodInfo *method)
{
    return hookerMap.find(method->methodPointer) != hookerMap.end();
}

void ClassesTab::PatcherView(Il2CppClass *klass, MethodInfo *method, const MethodParamList &paramsInfo,
                             Il2CppObject *thiz)
{
    {
        if (isMethodHooked(method))
        {
            ImGui::TextColored(ImVec4(1, 0, 0, 1), "Can't patch while hooked!");
            return;
        }
    }

    auto &o = oMap[method];
    auto type = method->getReturnType();

    // 复制已修补字节：读取方法入口当前写入的字节并转为十六进制串，写入系统剪贴板
    auto copyPatchedBytes = [method, &o]()
    {
        if (o.bytes.empty())
            return;
        std::string hex = KittyMemory::read2HexStr(method->methodPointer, o.bytes.size());
        if (Clipboard::Set(hex))
            LOGD("Copied patched bytes: %s", hex.c_str());
        else
            LOGE("Failed to copy patched bytes for %s", method->getName());
    };

    if (strcmp(type->getName(), "System.Int16") == 0 || strcmp(type->getName(), "System.Int32") == 0 ||
        strcmp(type->getName(), "System.Int64") == 0 || strcmp(type->getName(), "System.UInt16") == 0 ||
        strcmp(type->getName(), "System.UInt32") == 0 || strcmp(type->getName(), "System.UInt64") == 0 ||
        strcmp(type->getName(), "System.Single") == 0 || strcmp(type->getName(), "System.Boolean") == 0 ||
        strcmp(type->getName(), "System.String") == 0 || type->isEnum())
    {
        if (ImGui::Button("Patch return value"))
        {
            ImGui::OpenPopup("HookReturnValuePopup");
        }
        if (!o.text.empty())
        {
            ImGui::SameLine();
            ImGui::Text("-> %s", o.text.c_str());
            ImGui::SameLine();
            if (ImGui::Button(Lang::T("复制已修补字节", "Copy patched bytes")))
                copyPatchedBytes();
        }
    }
    else if (strcmp(type->getName(), "System.Void") == 0)
    {
        if (o.bytes.empty())
        {
            if (ImGui::Button("NOP"))
            {
                Patcher p{method};
                p.ret();
                o.bytes = p.patch();
            }
        }
        else
        {
            if (ImGui::Button("Restore"))
            {
                memcpy(method->methodPointer, o.bytes.data(), o.bytes.size());
                o.bytes.clear();
                o.text.clear();
            }
            ImGui::SameLine();
            if (ImGui::Button(Lang::T("复制已修补字节", "Copy patched bytes")))
                copyPatchedBytes();
        }
    }
    else
    {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 50, 50, 255));
        ImGui::Text("Not supported!");
        ImGui::PopStyleColor();
    }
    if (ImGui::BeginPopup("HookReturnValuePopup"))
    {
        ImGui::Text("Change return value");
        ImGui::PushID(type);
        char label[64]{0};
        if (!o.bytes.empty())
        {
            sprintf(label, "%s", "Restore");
        }
        else
        {
            sprintf(label, "%s", type->getName());
        }
        if (ImGui::Button(label))
        {
            if (!o.bytes.empty())
            {
                memcpy(method->methodPointer, o.bytes.data(), o.bytes.size());
                o.bytes.clear();
                o.text.clear();
            }
            else
            {
                if (strcmp(type->getName(), "System.Int16") == 0 || strcmp(type->getName(), "System.Int32") == 0 ||
                    strcmp(type->getName(), "System.Int64") == 0 || strcmp(type->getName(), "System.UInt16") == 0 ||
                    strcmp(type->getName(), "System.UInt32") == 0 || strcmp(type->getName(), "System.UInt64") == 0 ||
                    strcmp(type->getName(), "System.Single") == 0 || strcmp(type->getName(), "System.Boolean") == 0 ||
                    strcmp(type->getName(), "System.String") == 0)
                {
                    if (strcmp(type->getName(), "System.Boolean") == 0)
                    {
                        poper.Open("BooleanSelector",
                                   [method](const std::string &b)
                                   {
                                       using namespace asmjit;
                                       Patcher p{method};
                                       bool value = b == "True";
                                       p.movBool(value);
                                       p.ret();

                                       if (oMap[method].bytes.empty())
                                       {
                                           oMap[method].bytes = p.patch();
                                           oMap[method].text = b;
                                       }
                                       else
                                       {
                                           LOGE("oMap is not empty for %s", method->getName());
                                       }
                                   });
                    }
                    else
                    {
                        auto typ = type;
                        auto m = method;
                        Keyboard::Open(
                            [typ, method = m](const std::string &text)
                            {
                                auto isString = strcmp(typ->getName(), "System.String") == 0;
                                if (text.empty())
                                    return;

                                auto type = typ;
                                Patcher p{method};
                                // auto &assembler = p.assembler;
                                if (strcmp(type->getName(), "System.Int16") == 0)
                                {
                                    int16_t value = std::stoi(text);
                                    p.movInt16(value);
                                }
                                else if (strcmp(type->getName(), "System.UInt16") == 0)
                                {
                                    unsigned short value = std::stoi(text);
                                    p.movUInt16(value);
                                }
                                else if (strcmp(type->getName(), "System.Int32") == 0)
                                {
                                    int value{std::stoi(text)};
                                    p.movInt32(value);
                                }
                                else if (strcmp(type->getName(), "System.UInt32") == 0)
                                {
                                    unsigned int value{static_cast<unsigned int>(std::stoul(text))};
                                    p.movUInt32(value);
                                }
                                else if (strcmp(type->getName(), "System.Int64") == 0)
                                {
                                    long value{std::stol(text)};
                                    p.movInt64(value);
                                }
                                else if (strcmp(type->getName(), "System.UInt64") == 0)
                                {
                                    unsigned long value{std::stoul(text)};
                                    p.movUInt64(value);
                                }
                                else if (strcmp(type->getName(), "System.Single") == 0)
                                {
                                    float value = std::stof(text);
                                    p.movFloat(value);
                                }
                                else if (isString)
                                {
                                    p.movPtr(Il2cpp::NewString(text.c_str()));
                                }

                                p.ret();

                                if (oMap[method].bytes.empty())
                                {
                                    oMap[method].bytes = p.patch();
                                    oMap[method].text = text;
                                }
                                else
                                {
                                    LOGE("oMap is not empty for %s", method->getName());
                                }
                            });
                    }
                }
                else if (type->isEnum())
                {
                    poper.Open(
                        "EnumSelector",
                        [method, type](const std::string &result)
                        {
                            int value = type->getClass()->getField(result.c_str())->getStaticValue<int>();

                            using namespace asmjit;
                            Patcher p{method};
                            p.movInt16(value);
                            p.ret();

                            if (oMap[method].bytes.empty())
                            {
                                oMap[method].bytes = p.patch();
                                oMap[method].text = result;
                            }
                            else
                            {
                                LOGE("oMap is not empty for %s", method->getName());
                            }
                        },
                        type);
                }
            }
        }
        // if (ImGui::BeginPopup("BooleanSelector"))
        // {
        //     if (ImGui::Button("True"))
        //     {
        //         using namespace asmjit;
        //         Patcher p{method};
        //         p.movBool(true);
        //         p.ret();

        //         if (oMap[method].bytes.empty())
        //         {
        //             oMap[method].bytes = p.patch();
        //             oMap[method].text = "True";
        //         }
        //         else
        //         {
        //             LOGE("oMap is not empty for %s", method->getName());
        //         }

        //         ImGui::CloseCurrentPopup();
        //     }
        //     if (ImGui::Button("False"))
        //     {
        //         using namespace asmjit;
        //         Patcher p{method};
        //         p.movBool(false);
        //         p.ret();

        //         if (oMap[method].bytes.empty())
        //         {
        //             oMap[method].bytes = p.patch();
        //             oMap[method].text = "False";
        //         }
        //         else
        //         {
        //             LOGE("oMap is not empty for %s", method->getName());
        //         }
        //         ImGui::CloseCurrentPopup();
        //     }
        //     ImGui::EndPopup();
        // }
        // // ImGui::SetNextWindowSize(ImVec2(0, io.DisplaySize.y / 3.f));
        // ImGui::SetNextWindowSizeConstraints(ImVec2(-1, 0.f), ImVec2(-1, io.DisplaySize.y / 3.f));
        // if (ImGui::BeginPopup("EnumSelector")) // assume the current type is enum
        // {
        //     auto klass = type->getClass();
        //     for (auto field : klass->getFields())
        //     {
        //         auto fieldType = field->getType();
        //         if (Il2cpp::GetTypeIsStatic(fieldType) ||
        //             Il2cpp::GetFieldFlags(field) & FIELD_ATTRIBUTE_STATIC)
        //         {
        //             auto fieldName = field->getName();
        //             if (ImGui::Button(fieldName))
        //             {
        //                 int value = type->getClass()->getField(fieldName)->getStaticValue<int>();

        //                 using namespace asmjit;
        //                 Patcher p{method};
        //                 p.movInt16(value);
        //                 p.ret();

        //                 if (oMap[method].bytes.empty())
        //                 {
        //                     oMap[method].bytes = p.patch();
        //                     oMap[method].text = fieldName;
        //                 }
        //                 else
        //                 {
        //                     LOGE("oMap is not empty for %s", method->getName());
        //                 }
        //                 ImGui::CloseCurrentPopup();
        //             }
        //         }
        //     }
        //     ImGui::EndPopup();
        // }
        ImGui::Separator();
        // 手动十六进制补丁：输入形如 "1F 20 03 D5" 的字节串，直接写入方法入口
        ImGui::Text("%s", Lang::T("手动十六进制 :", "Manual hex :"));
        static char hexBuffer[256]{0};
        if (ImGui::Button(Lang::T("输入十六进制并应用", "Enter hex and apply")))
        {
            Keyboard::Open(
                hexBuffer,
                [method](const std::string &text)
                {
                    snprintf(hexBuffer, sizeof(hexBuffer), "%s", text.c_str());

                    // 解析十六进制字节，忽略空格与逗号等分隔符
                    std::vector<uint8_t> newBytes;
                    for (size_t i = 0; i < text.size();)
                    {
                        if (std::isxdigit(static_cast<unsigned char>(text[i])))
                        {
                            if (i + 1 >= text.size() || !std::isxdigit(static_cast<unsigned char>(text[i + 1])))
                            {
                                LOGE("Invalid hex input: %s", text.c_str());
                                return;
                            }
                            newBytes.push_back(static_cast<uint8_t>(std::stoul(text.substr(i, 2), nullptr, 16)));
                            i += 2;
                        }
                        else
                        {
                            i++;
                        }
                    }
                    if (newBytes.empty())
                    {
                        LOGE("Empty hex patch for %s", method->getName());
                        return;
                    }

                    // 先备份原始字节以便 Restore
                    if (oMap[method].bytes.empty())
                    {
                        oMap[method].bytes.assign(reinterpret_cast<uint8_t *>(method->methodPointer),
                                                  reinterpret_cast<uint8_t *>(method->methodPointer) + newBytes.size());
                    }
                    auto protect = KittyMemory::ProtectAddr(method->methodPointer, newBytes.size(),
                                                            PROT_READ | PROT_WRITE | PROT_EXEC);
                    LOGINT(protect);
                    memcpy(method->methodPointer, newBytes.data(), newBytes.size());
                    oMap[method].text = text;
                });
        }
        ImGui::PopID();
        ImGui::EndPopup();
    }
    // if (ImGui::Button("Patch"))
    // {
    //     using namespace asmjit;

    //     CodeHolder code;
    //     code.init(Environment(Arch::kAArch64));

    //     a64::Assembler assembler(&code);
    //     int a = 999;
    //     assembler.movz(a64::w0, 191);
    //     assembler.ret(a64::x30);

    //     std::vector<char> bytes;
    //     for (auto s : code.sections())
    //     {
    //         for (auto c : s->buffer())
    //         {
    //             LOGD("0x%X", c);
    //             bytes.push_back(c);
    //         }
    //     }
    //     auto rawBytes = bytes.data();
    //     auto protect = KittyMemory::ProtectAddr((void *)method->methodPointer,
    //                                             sizeof(method->methodPointer),
    //                                             PROT_READ | PROT_WRITE |
    //                                             PROT_EXEC);
    //     LOGINT(protect);
    //     auto src = method->methodPointer;
    //     memcpy((void *)src, (void *)rawBytes, sizeof(method->methodPointer));
    // }

    ImGui::Separator();
    // 方法重定向：输入目标地址（支持 0x 前缀），将方法入口改写为跳转指令
    ImGui::Text("%s", Lang::T("重定向跳转 :", "Redirect to :"));
    static char redirectBuffer[64]{0};
    if (ImGui::Button(Lang::T("输入目标地址并应用", "Enter target and apply")))
    {
        Keyboard::Open(
            redirectBuffer,
            [method](const std::string &text)
            {
                snprintf(redirectBuffer, sizeof(redirectBuffer), "%s", text.c_str());
                if (text.empty())
                    return;

                uintptr_t address = 0;
                try
                {
                    address = std::stoull(text, nullptr, 0);
                }
                catch (...)
                {
                    LOGE("Invalid redirect address: %s", text.c_str());
                    return;
                }
                if (!address)
                {
                    LOGE("Invalid redirect address: %s", text.c_str());
                    return;
                }
                if (!oMap[method].bytes.empty())
                {
                    LOGE("oMap is not empty for %s", method->getName());
                    return;
                }

                Patcher p{method};
                if (p.redirect(reinterpret_cast<void *>(address)) != asmjit::kErrorOk)
                {
                    LOGE("Failed to assemble redirect for %s", method->getName());
                    return;
                }
                oMap[method].bytes = p.patch();
                oMap[method].text = text;
            });
    }
    if (!o.text.empty())
    {
        ImGui::SameLine();
        ImGui::Text("-> %s", o.text.c_str());
        ImGui::SameLine();
        if (ImGui::Button(Lang::T("复制已修补字节", "Copy patched bytes")))
            copyPatchedBytes();
    }
}

void ClassesTab::HookerView(Il2CppClass *klass, MethodInfo *method, const MethodParamList &paramsInfo,
                            Il2CppObject *thiz)
{
    {
        bool patched = oMap[method].bytes.empty() == false;
        if (patched)
        {
            ImGui::TextColored(ImVec4(1, 0, 0, 1), "Can't hook while patched!");
            return;
        }
    }
    auto it = hookerMap.find(method->methodPointer);
    bool hooked = it != hookerMap.end();
    char label[16];
    if (!hooked)
    {
        sprintf(label, "Trace");
    }
    else
    {
        sprintf(label, "Restore");
        auto value = it->second.hitCount;
        ImGui::Text("Hit Count %d", value);
        ImGui::Separator();
    }
    if (ImGui::Button(label))
    {
        Tool::ToggleHooker(method);
        it = hookerMap.find(method->methodPointer);
        hooked = it != hookerMap.end();
    }
    ImGui::Separator();
    if (hooked)
    {
        // 调用参数修改：输入后在方法被调用时替换对应参数（static 从 idx0，非 static 从 idx1）
        if (!paramsInfo.empty())
        {
            ImGui::Text("%s", Lang::T("修改调用参数 :", "Override call arguments :"));
            auto &mods = hookerArgMods[method];
            for (int k = 0; k < (int)paramsInfo.size(); k++)
            {
                auto &[name, type] = paramsInfo[k];
                ImGui::PushID(k);

                auto modIt = mods.find(k);
                bool overridden = modIt != mods.end() && !modIt->second.text.empty();
                const char *shownText = overridden ? modIt->second.text.c_str() : Lang::T("未修改", "not overridden");
                char btnLabel[192];
                snprintf(btnLabel, sizeof(btnLabel), "%s %s = %s", type->getName(), name, shownText);

                if (ImGui::Button(btnLabel))
                {
                    Keyboard::Open(
                        [method, k, type](const std::string &text)
                        {
                            auto &mod = hookerArgMods[method][k];
                            mod.text = text;
                            mod.value = ParseHookerArgValue(type, text, &mod.keepAlive);
                        });
                }
                ImGui::SameLine();
                if (ImGui::Button(Lang::T("清除", "Clear")))
                {
                    mods.erase(k);
                }
                ImGui::PopID();
            }
            ImGui::Separator();
        }

        auto &backtraced = it->second.backtraced;
        if (!it->second.backtracing)
        {
            if (ImGui::Button("Backtrace"))
            {
                it->second.backtracing = true;
            }
        }

        if (backtraced.empty())
        {
            ImGui::Text("%s", Lang::T("方法尚未被调用", "Method has not been called"));
        }
        else
        {
            ImGui::Text("%s", Lang::T("回溯方法 :", "Backtraced methods :"));

            for (auto &result : backtraced)
            {
                for (auto &r : result)
                {
                    ImGui::Text("%s", r.c_str());
                }
                ImGui::PushStyleColor(ImGuiCol_Separator, IM_COL32(50, 255, 100, 255));
                ImGui::Separator();
                ImGui::PopStyleColor();
            }
        }
        StaticFieldViewer(klass);
        HookerCollectedFieldEditor(klass);
    }
}

bool ClassesTab::MethodViewer(Il2CppClass *klass, MethodInfo *method, const MethodParamList &paramsInfo,
                              Il2CppObject *thiz, bool includeInflated)
{
    bool zeroPointer = method->methodPointer == nullptr;

    if (callResults.find(method) == callResults.end())
    {
        callResults.emplace(method, (size_t)5);
    }

    bool methodIsStatic = Il2cpp::GetIsMethodStatic(method);

    char treeLabel[512]{0};
    sprintf(treeLabel, "%s %s(%zu)###", method->getReturnType()->getName(), method->getName(), paramsInfo.size());
    int pushedColor = 0;
    if (methodIsStatic)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 200, 100, 255));
        pushedColor++;
        Util::prependStringToBuffer(treeLabel, "static ");
    }
    bool patched = oMap[method].bytes.empty() == false;
    bool hooked = hookerMap.find(method->methodPointer) != hookerMap.end();
    // sprintf(treeLabel, "%s##%p", treeLabel, method + j);
    if (zeroPointer)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 100, 100, 255));
        pushedColor++;
    }
    if (patched || hooked)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(25, 255, 125, 255));
        pushedColor++;
        if (hooked)
        {
            int hitCount = hookerMap[method->methodPointer].hitCount;
            char hitLabel[64]{0};
            sprintf(hitLabel, "Hit Count %d | ", hitCount);
            Util::prependStringToBuffer(treeLabel, hitLabel);
        }
        else if (patched)
        {
            auto text = oMap[method].text;
            if (!text.empty())
            {
                char buff[64]{0};
                sprintf(buff, "Returns %s | ", text.c_str());
                Util::prependStringToBuffer(treeLabel, buff);
            }
        }
        // 状态图标前缀：已挂钩 [H]、已打补丁 [P]（保留原有 Hit Count / Returns 文本与颜色）
        if (hooked)
            Util::prependStringToBuffer(treeLabel, "[H] ");
        if (patched)
            Util::prependStringToBuffer(treeLabel, "[P] ");
    }
    bool state = ImGui::TreeNode(treeLabel);
    if (state)
    {
        if (pushedColor)
        {
            ImGui::PopStyleColor(pushedColor);
            pushedColor = 0;
        }

        if (ImGui::BeginTabBar("##methoder"))
        {
            if (ImGui::BeginTabItem(Lang::T("调用", "Caller")))
            {
                CallerView(klass, method, paramsInfo, thiz);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(Lang::T("补丁", "Patcher")))
            {
                PatcherView(klass, method, paramsInfo, thiz);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(Lang::T("跟踪", "Tracer")))
            {
                HookerView(klass, method, paramsInfo, thiz);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::TreePop();
    }
    poper.Update();
    ImGui::PopStyleColor(pushedColor);
    return state;
}

const ClassesTab::MethodParamList &ClassesTab::getCachedParams(MethodInfo *method)
{
    static std::unordered_map<MethodInfo *, MethodParamList> params;
    if (params.find(method) == params.end())
    {
        params[method] = method->getParamsInfo();
    }
    return params[method];
}

// static std::unordered_map<Il2CppClass *, bool> states;
void ClassesTab::ClassViewer(Il2CppClass *klass)
{
    if (!klass)
        return;
    if (ImGui::Button(Lang::T("检查对象", "Inspect Objects")))
    {
        ImGui::OpenPopup("DumpPopup");
    }
    {
        ImGui::SameLine();
        bool &state = states[klass];
        char label[12]{0};
        if (!state)
            sprintf(label, "Trace all");
        else
            sprintf(label, "Restore");
        if (ImGui::Button(label))
        {
            if (!state)
            {
                ImGui::OpenPopup("ConfirmPopup");
            }
            else
            {
                state = false;
                for (auto &[method, paramsInfo] : methodMap[klass])
                {
                    if (!method->methodPointer && !Il2cpp::GetIsMethodInflated(method))
                        continue;

                    Tool::ToggleHooker(method, 0);
                    auto it = std::find(tracedMethods.begin(), tracedMethods.end(), method);
                    if (it != tracedMethods.end())
                        tracedMethods.erase(it);
                }
                if (tracedMethods.empty())
                    traceState = false;
            }
        }
        if (ImGui::BeginPopup("ConfirmPopup"))
        {
            ImGui::TextColored(ImVec4(0.8, 0.8, 0, 1), "WARNING: There's a high-risk of crash");
            if (ImGui::Button("Continue?"))
            {
                state = true;
                std::thread hookThread(
                    [this, klass]
                    {
                        for (auto &[method, paramsInfo] : methodMap[klass])
                        {
                            if (!method->methodPointer && !Il2cpp::GetIsMethodInflated(method))
                                continue;

                            Tool::ToggleHooker(method, 1);
                        }
                    });
                hookThread.detach();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
    // ImGui::SameLine();
    // if (ImGui::Button("Add to Tracer"))
    // {
    //     tracer.push_back(klass);
    // }
    if (ImGui::BeginPopup("DumpPopup"))
    {
        ImGuiObjectSelector(ImGui::GetID("ObjectSelector"), klass, "Inspect",
                            [this](Il2CppObject *object) { setJsonObject(object); });
        ImGui::EndPopup();
    }
    ImGui::PopID();
    ImGui::Separator();

    StaticFieldViewer(klass);

    // 方法视图显示开关：关闭时跳过后面的方法列表渲染
    ImGui::Checkbox(Lang::T("显示方法", "Show Methods"), &showMethods);
    ImGui::Separator();

    if (showMethods)
    {
        int j = 0;
        for (auto &[method, paramsInfo] : methodMap[klass])
        {
            ImGui::PushID(method + j++);
            MethodViewer(klass, method, paramsInfo);
            ImGui::Separator();
            ImGui::PopID();
        }
    }
}

void ClassesTab::Draw(int index, bool closeable)
{
    static ImGuiIO &io = ImGui::GetIO();
    char tabLabel[256];
    if (!title.empty())
    {
        snprintf(tabLabel, sizeof(tabLabel), "%s", title.c_str());
    }
    else if (filter.empty())
    {
        sprintf(tabLabel, "Classes");
        if (index >= 0)
            sprintf(tabLabel, "Classes [%d]", index + 1);
    }
    else
    {
        sprintf(tabLabel, "%s", filter.c_str());
    }

    if ((currentlyOpened = ImGui::BeginTabItem(tabLabel, closeable ? &opened : nullptr,
                                               setOpenedTab ? ImGuiTabItemFlags_SetSelected : 0)))
    {
        setOpenedTab = false;
        ImGui::BeginDisabled(includeAllImages);
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(-1, io.DisplaySize.y / 1.5f));
        if (ImGui::BeginCombo("Image##ImageSelector", selectedImage->getName()))
        {

            for (int i = 0; i < g_Images.size(); i++)
            {
                bool selected = selectedImageIndex == i;
                if (ImGui::Selectable(g_Images[i]->getName(), selected))
                {
                    selectedImage = g_Images[i];
                    selectedImageIndex = i;
                    FilterClasses(filter);
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Checkbox("All", &includeAllImages))
        {
            FilterClasses(filter);
        }

        char filterBuffer[256];
        sprintf(filterBuffer, "Filter : %s | %zu of %zu", filter.empty() ? "(none)" : filter.c_str(),
                filteredClasses.size(), classes.size());
        if (ImGui::Button(filterBuffer, ImVec2(ImGui::GetContentRegionAvail().x, 0.0f)) && !Keyboard::IsOpen())
        {
            Keyboard::Open(
                [this](const std::string &text)
                {
                    filter = text;
                    FilterClasses(filter);
                });
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s", Lang::T("支持 :方法名 直接按方法筛选", "Prefix with ':' to filter by method name"));
        }
        if (!Keyboard::IsOpen() && ImGui::IsItemHeld())
        {
            Keyboard::Open(filter.c_str(),
                           [this](const std::string &text)
                           {
                               filter = text;
                               FilterClasses(filter);
                           });
        }
        if (ImGui::Button("Filter Options"))
        {
            ImGui::OpenPopup("FilterOptions");
        }

        {
            static bool processing = false;
            ImGui::SameLine();
            char label[12]{0};
            if (!traceState)
                sprintf(label, "Trace all");
            else
                sprintf(label, "Restore");

            bool disabled = false;
            if (processing)
            {
                disabled = true; // creating variable because `processing` could be set to false and we don't call
                                 // `EndDisabled`
                ImGui::BeginDisabled();
            }
            if (ImGui::Button(label))
            {
                {
                    if (!traceState)
                    {
                        ImGui::OpenPopup("ConfirmPopup");
                    }
                    else
                    {
                        traceState = false;
                        std::thread hookThread(
                            [this]
                            {
                                LOGD("Restoring all methods...");
                                processing = true;
                                maxProgress = tracedMethods.size();
                                for (auto method : tracedMethods)
                                {
                                    Tool::ToggleHooker(method, 0);
                                    progress++;
                                }
                                LOGD("Restored %zu methods", tracedMethods.size());
                                tracedMethods.clear();
                                for (auto &kv : states)
                                    kv.second = false;
                                processing = false;
                                maxProgress = 0;
                                progress = 0;
                                LOGD("Done");
                            });
                        hookThread.detach();
                    }
                }
            }

            if (disabled)
                ImGui::EndDisabled();

            if (ImGui::BeginPopup("ConfirmPopup"))
            {
                ImGui::TextColored(ImVec4(0.8, 0.8, 0, 1), "WARNING: There's a high-risk of crash");
                if (ImGui::Button("Continue?"))
                {
                    traceState = true;
                    std::thread hookThread(
                        [this]
                        {
                            LOGD("Tracing all methods...");
                            for (auto &klass : filteredClasses)
                            {
                                for (auto &[method, paramsInfo] : methodMap[klass])
                                {
                                    if (!method->methodPointer && !Il2cpp::GetIsMethodInflated(method))
                                        continue;

                                    maxProgress++;
                                }
                            }
                            processing = true;
                            for (auto &klass : filteredClasses)
                            {
                                states[klass] = true;
                                for (auto &[method, paramsInfo] : methodMap[klass])
                                {
                                    if (!method->methodPointer && !Il2cpp::GetIsMethodInflated(method))
                                        continue;

                                    if (Tool::ToggleHooker(method, 1))
                                    {
                                        tracedMethods.push_back(method);
                                    }
                                    progress++;
                                }
                            }
                            LOGD("Traced %zu methods", tracedMethods.size());

                            processing = false;
                            maxProgress = 0;
                            progress = 0;
                            LOGD("Done");
                        });
                    hookThread.detach();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            if (processing)
            {
                ImGui::SameLine();
                ImGui::Text("Processing %d of %d ...", progress, maxProgress);
            }
        }
        if (ImGui::BeginPopup("FilterOptions"))
        {
            if (ImGui::Checkbox(Lang::T("区分大小写", "Case-Sensitive"), &caseSensitive))
            {
                FilterClasses(filter);
            }
            if (ImGui::Checkbox(Lang::T("精确匹配", "Exact match"), &exactMatch))
            {
                FilterClasses(filter);
            }
            ImGui::Text("%s", Lang::T("按以下筛选", "Filter by "));
            ImGui::SameLine();
            if (ImGui::RadioButton(Lang::T("类", "Class"), filterByClass == true))
            {
                filterByClass = true;
                filterByMethod = false;
                filterByField = false;
                FilterClasses(filter);
            }
            ImGui::SameLine();
            if (ImGui::RadioButton(Lang::T("方法", "Method"), filterByMethod == true))
            {
                filterByClass = false;
                filterByMethod = true;
                filterByField = false;
                FilterClasses(filter);
            }
            ImGui::SameLine();
            if (ImGui::RadioButton(Lang::T("字段", "Field"), filterByField == true))
            {
                filterByClass = false;
                filterByMethod = false;
                filterByField = true;
                FilterClasses(filter);
            }
            if (filterByMethod)
            {
                ImGui::Separator();
                ImGui::Text("%s", Lang::T("方法筛选维度", "Method filter by"));
                if (ImGui::Checkbox(Lang::T("名称", "Name"), &filterMethodByName))
                    FilterClasses(filter);
                ImGui::SameLine();
                if (ImGui::Checkbox(Lang::T("返回类型", "Return type"), &filterMethodByReturn))
                    FilterClasses(filter);
                ImGui::SameLine();
                if (ImGui::Checkbox(Lang::T("参数", "Parameters"), &filterMethodByParams))
                    FilterClasses(filter);
            }
            if (ImGui::Checkbox(Lang::T("显示全部类", "Show All Classes"), &showAllClasses))
            {
                FilterClasses(filter);
            }
            ImGui::EndPopup();
        }
        ImGui::Separator();
        if (!filteredClasses.empty())
        {
            auto avail = ImGui::GetContentRegionAvail();
            float leftWidth = sideSplit ? avail.x * 0.35f : 0.f;
            ImGui::BeginChild("Child", sideSplit ? ImVec2(leftWidth, 0) : ImVec2(0, 0), false,
                              ImGuiWindowFlags_HorizontalScrollbar);
            for (int i = 0; i < filteredClasses.size(); i++)
            {
                auto klass = filteredClasses[i];

                bool isValueType = Il2cpp::GetClassType(klass)->isValueType();
                int pushedColor = 0;
                if (isValueType)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(222, 222, 222, 255));
                    pushedColor++;
                }
                bool collapsingHeader = ImGui::CollapsingHeader(klass->getFullName().c_str());
                if (ImGui::IsItemClicked())
                    selectedClass = klass;
                if (filterByMethod && ImGui::IsItemHeld(0.7f))
                {
                    auto name = Util::extractClassNameFromTypename(klass->getFullName().c_str());
                    auto &tab = Tool::OpenNewTab();
                    tab.filter = name;
                    tab.selectedImage = klass->getImage();
                    tab.FilterClasses(tab.filter);
                }
                if (collapsingHeader && !sideSplit)
                {
                    if (pushedColor)
                    {
                        ImGui::PopStyleColor(pushedColor);
                        pushedColor = 0;
                    }
                    ImGui::PushID(i);
                    // TODO: Rename Dump (DumpPopup and other related functions used) to Inspect
                    ClassViewer(klass);
                }
                if (pushedColor)
                {
                    ImGui::PopStyleColor(pushedColor);
                    pushedColor = 0;
                }
            }
            ImGui::ScrollWhenDraggingOnVoid();
            ImGui::EndChild();
            if (sideSplit)
            {
                ImGui::SameLine();
                ImGui::BeginChild("##SideRight", ImVec2(0, 0));
                if (selectedClass)
                {
                    ClassViewer(selectedClass);
                }
                else
                {
                    ImGui::TextDisabled("%s", Lang::T("从左侧选择一个类", "Select a class on the left"));
                }
                ImGui::EndChild();
            }
        }
        ImGui::EndTabItem();
    }
}
void ClassesTab::DrawTabMap()
{
    for (auto it = tabMap.begin(); it != tabMap.end();)
    {
        auto &[object, visible] = *it;
        char buff[32]{0};
        sprintf(buff, "[%p]", object);

        if (!visible)
        {
            it = tabMap.erase(it);
            dataMap.erase(object);
        }
        else
        {
            if (ImGui::BeginTabItem(buff, &visible))
            {
                ImGuiJson(object);
                ImGui::EndTabItem();
            }
            ++it;
        }
    }
}

void ensureIfValueType(Il2CppObject *currentObj, std::vector<std::string> &paths, Il2CppObject *rootObj)
{
    if (currentObj == rootObj)
        return;
    // 更安全模式：跳过值类型写回，避免潜在的崩溃
    if (saferInspector)
        return;
    bool isValueType = Il2cpp::GetClassType(currentObj->klass)->isValueType();
    if (isValueType)
    {
        if (paths.size() > 1)
        {
            auto pathsButLast = std::vector(paths.begin(), paths.end() - 1);
            auto [beforeObject, j] = rootObj->dump(pathsButLast, true);

            auto path = paths.rbegin();
            std::istringstream iss(path->c_str());
            std::string _, val;
            iss >> _ >> val;
            void *unboxed = Il2cpp::GetUnboxedValue(currentObj);
            // object->setField(val.c_str(), unboxed);
            auto f = beforeObject->klass->getField(val.c_str());
            Il2cpp::SetFieldValue(beforeObject, f, unboxed);
            ensureIfValueType(beforeObject, pathsButLast, rootObj);
        }
        else
        {
            auto path = paths.begin();

            std::istringstream iss(path->c_str());
            std::string _, val;
            iss >> _ >> val;
            void *unboxed = Il2cpp::GetUnboxedValue(currentObj);
            // object->setField(val.c_str(), unboxed);
            auto f = rootObj->klass->getField(val.c_str());
            Il2cpp::SetFieldValue(rootObj, f, unboxed);
        }
    }
}

void ClassesTab::ImGuiJson(Il2CppObject *rootObj)
{
    // auto &paths = tool.dataMap[object].second;
    auto &paths = getJsonPaths(rootObj);

    int indentCounter = 0;
    auto currentObj = dataMap[rootObj].first.first;
    if (!currentObj || !currentObj->klass)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 80, 80, 255));
        ImGui::Text("%s", Lang::T("对象无效或已被回收（旧版 Unity 上更常见）",
                                  "Object is invalid or was collected (more common on old Unity)."));
        ImGui::PopStyleColor();
        return;
    }
    for (auto it = paths.begin() + (paths.size() > 3 ? paths.size() - 4 : 0); it != paths.end(); ++it)
    {
        const bool isLast = std::next(it) == paths.end();
        auto key = it->c_str();
        ImGui::PushID(indentCounter);

        bool buttonPressed = false;
        bool isValueType = Il2cpp::GetClassType(currentObj->klass)->isValueType();
        if (isLast && isValueType)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(222, 222, 222, 255));
        }
        if (isLast && currentObj && savedSet[currentObj->klass].count(currentObj) == 0)
        {
            auto width = ImGui::CalcTextSize("Save").x + ImGui::GetStyle().FramePadding.x * 5.f;
            buttonPressed = ImGui::Button(key, ImVec2(ImGui::GetContentRegionAvail().x - width, 0));
            if (ImGui::IsItemHeld())
            {
                Tool::OpenNewTabFromClass(currentObj->klass);
                LOGD("OpenNewTabFromObject %p", currentObj);
            }
            ImGui::SameLine();
            char buttonLabel[32]{0};
            sprintf(buttonLabel, "Save");
            if (ImGui::Button(buttonLabel,
                              ImVec2(ImGui::GetContentRegionAvail().x - ImGui::GetStyle().FramePadding.x, 0)))
            {
                savedSet[currentObj->klass].insert(currentObj);
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%p", currentObj);
            }
        }
        else
        {
            buttonPressed =
                ImGui::Button(key, ImVec2(ImGui::GetContentRegionAvail().x - ImGui::GetStyle().FramePadding.x, 0));
            if (ImGui::IsItemHeld())
            {
                Tool::OpenNewTabFromClass(currentObj->klass);
                LOGD("OpenNewTabFromObject %p", currentObj);
            }
        }
        if (isLast && isValueType)
        {
            ImGui::PopStyleColor();
        }
        ImGui::Indent(10.f);
        indentCounter++;
        ImGui::PopID();

        if (buttonPressed)
        {
            paths.erase(it, paths.end());
            dataMap[rootObj].first = rootObj->dump(paths);
            break;
        }
    }
    if (indentCounter)
        ImGui::Unindent(10.f * indentCounter);

    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(100, 200, 20, 128));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(100, 200, 20, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(100, 200, 20, 255));
    static bool doRefresh = false;
    if (ImGui::Button(Lang::T("刷新", "Refresh"), ImVec2(ImGui::GetContentRegionAvail().x * 0.65f, 0.0f)))
    {
        doRefresh = true;
    }
    ImGui::SameLine();
    ImGui::Checkbox(Lang::T("始终刷新", "Always refresh"), &alwaysRefreshInspector);
    // 更安全模式：禁用自动刷新，避免深度遍历导致卡顿/崩溃
    if (alwaysRefreshInspector && !saferInspector)
        doRefresh = true;
    if (doRefresh)
    {
        doRefresh = false;
        dataMap[rootObj].first = rootObj->dump(paths);
    }
    ImGui::PopStyleColor(3);

    ImGui::PushStyleColor(ImGuiCol_Separator, IM_COL32(0, 255, 100, 255));
    ImGui::Separator();
    ImGui::PopStyleColor();
    static PopUpSelector poper;
    if (ImGui::BeginTable("sometable", 1))
    {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::BeginChild("ChildJson",
                          ImVec2(0, ImGui::GetContentRegionAvail().y - (ImGui::GetFontSize() * 1.8f) * 2.f), 0,
                          ImGuiWindowFlags_HorizontalScrollbar);

        const nlohmann::ordered_json &current = getJsonObject(rootObj);
        if (current.empty())
        {
            ImGui::Text("Empty");
        }

        for (auto &[key, value] : current.items())
        {
            // int count = 0;
            if (value.is_object() || value.is_array())
            {
                if (value.is_array() && value.size() == 0)
                {
                    ImGui::Text("%s = [Empty]", key.c_str());
                }
                else if (ImGui::Button(key.c_str(), ImVec2(key.length() <= 3 ? ImGui::GetContentRegionAvail().x -
                                                                                   ImGui::GetStyle().FramePadding.x
                                                                             : 0,
                                                           0)))
                {
                    try
                    {
                        paths.push_back(key);
                        // LOGD("%s", object->dump(paths).dump().c_str());
                        dataMap[rootObj].first = rootObj->dump(paths);
                        break;
                    }
                    catch (nlohmann::json::exception &e)
                    {
                        LOGE("Json exception %s", e.what());
                    }
                    catch (std::exception &e)
                    {
                        LOGE("Exception %s", e.what());
                    }
                }
            }
            else if (value.is_string())
            {
                auto text = value.get<std::string>();
                ImGui::Text("%s = %s", key.c_str(), text.c_str());
                if (ImGui::IsItemClicked())
                {
                    std::istringstream iss(key);
                    std::string type, val;
                    iss >> type >> val;
                    if (strcmp(type.c_str(), "String") == 0)
                    {
                        Keyboard::Open(
                            text.c_str(),
                            [type = std::move(type), val = std::move(val), currentObj](const std::string &value)
                            {
                                LOGD("%s", value.c_str());
                                auto f = currentObj->klass->getField(val.c_str());
                                auto newStr = Il2cpp::NewString(value.c_str());
                                // Il2cpp::SetFieldValueObject(currentObj, f, newStr);
                                Il2cpp::SetFieldValue(currentObj, f, newStr);
                                doRefresh = true;
                            });
                    }
                    else
                    {
                        auto field = currentObj->klass->getField(val.c_str());
                        auto fieldType = field->getType();
                        if (fieldType->isEnum())
                        {
                            poper.Open(
                                "EnumSelector",
                                [fieldType, currentObj, field](const std::string &result)
                                {
                                    int value = fieldType->getClass()->getField(result.c_str())->getStaticValue<int>();
                                    Il2cpp::SetFieldValue(currentObj, field, &value);
                                    doRefresh = true;
                                },
                                fieldType);
                        }
                    }
                }
            }
            else if (value.is_boolean())
            {
                ImGui::Text("%s = %s", key.c_str(), value.get<bool>() ? "True" : "False");
                if (ImGui::IsItemClicked())
                {
                    std::istringstream iss(key);
                    std::string _, val;
                    iss >> _ >> val;
                    poper.Open("BooleanSelector",
                               [currentObj, val, &paths, rootObj](const std::string &value)
                               {
                                   bool b = value == "True";
                                   // split key by space
                                   currentObj->setField(val.c_str(), (int)b);
                                   ensureIfValueType(currentObj, paths, rootObj);
                                   doRefresh = true;
                               });
                }
            }
            else if (value.is_number_float())
            {
                ImGui::Text("%s = %f", key.c_str(), value.get<float>());

                if (ImGui::IsItemClicked())
                {
                    std::istringstream iss(key);
                    std::string type, val;
                    iss >> type >> val;
                    Keyboard::Open(std::to_string(value.get<float>()).c_str(),
                                   [type, currentObj, val, &paths, rootObj](const std::string &text)
                                   {
                                       if (strcmp(type.c_str(), "Single") == 0)
                                       {
                                           float value = std::stof(text);
                                           currentObj->setField(val.c_str(), value);
                                       }
                                       else if (strcmp(type.c_str(), "Double") == 0)
                                       {
                                           double value = std::stod(text);
                                           currentObj->setField(val.c_str(), value);
                                       }
                                       ensureIfValueType(currentObj, paths, rootObj);
                                       doRefresh = true;
                                   });
                }
            }
            else if (value.is_number())
            {
                ImGui::Text("%s = %d", key.c_str(), value.get<int>());
                if (ImGui::IsItemClicked())
                {
                    std::istringstream iss(key);
                    std::string type, val;
                    iss >> type >> val;
                    Keyboard::Open(std::to_string(value.get<int>()).c_str(),
                                   [type, currentObj, val, &paths, rootObj](const std::string &text)
                                   {
                                       if (strcmp(type.c_str(), "Int16") == 0)
                                       {
                                           int16_t value = std::stoi(text);
                                           currentObj->setField(val.c_str(), value);
                                       }
                                       else if (strcmp(type.c_str(), "UInt16") == 0)
                                       {
                                           uint16_t value = std::stoi(text);
                                           currentObj->setField(val.c_str(), value);
                                       }
                                       else if (strcmp(type.c_str(), "Int32") == 0)
                                       {
                                           int32_t value = std::stoi(text);
                                           currentObj->setField(val.c_str(), value);
                                       }
                                       else if (strcmp(type.c_str(), "UInt32") == 0)
                                       {
                                           uint32_t value = std::stoul(text);
                                           currentObj->setField(val.c_str(), value);
                                       }
                                       else if (strcmp(type.c_str(), "Int64") == 0)
                                       {
                                           int64_t value = std::stoll(text);
                                           currentObj->setField(val.c_str(), value);
                                       }
                                       else if (strcmp(type.c_str(), "UInt64") == 0)
                                       {
                                           uint64_t value = std::stoull(text);
                                           currentObj->setField(val.c_str(), value);
                                       }
                                       ensureIfValueType(currentObj, paths, rootObj);
                                       doRefresh = true;
                                   });
                }
            }
            else
            {
                ImGui::Text("Unk %s %s", key.c_str(), value.type_name());
            }
            // constexpr ImU32 colors[8] = {
            //     IM_COL32(255,50,50,255),     IM_COL32(0, 255, 0, 255),   IM_COL32(0, 0, 255, 255),
            //     IM_COL32(255, 255, 0, 255),   IM_COL32(255, 0, 255, 255), IM_COL32(0, 255, 255, 255),
            //     IM_COL32(255, 255, 255, 255), IM_COL32(0, 0, 0, 255),
            // };
            // ImGui::PushStyleColor(ImGuiCol_Separator, colors[paths.size() % 8]);
            ImGui::Separator();
            // ImGui::PopStyleColor();
        }

        ImGui::ScrollWhenDraggingOnVoid();
        ImGui::EndChild();

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::BeginChild("bottom");
        if (ImGui::Button("Methods", ImVec2(ImGui::GetContentRegionAvail().x - ImGui::GetStyle().FramePadding.x, 0)))
        {
            ImGui::OpenPopup("MethodPopup");
        }

        static ImGuiIO &io = ImGui::GetIO();
        ImGui::SetNextWindowSizeConstraints(ImVec2(io.DisplaySize.x / 1.2f, 0),
                                            ImVec2(io.DisplaySize.x / 1.2f, io.DisplaySize.y / 2));
        if (ImGui::BeginPopup("MethodPopup", ImGuiWindowFlags_HorizontalScrollbar))
        {
            auto text = currentObj->klass->getName();
            auto windowWidth = ImGui::GetWindowSize().x;
            auto textWidth = ImGui::CalcTextSize(text).x;
            ImGui::SetCursorPosX((windowWidth - textWidth) * 0.5f);
            ImGui::Text("%s", text);

            ImGui::Separator();

            auto &methods = buildMethodMap(currentObj->klass);
            if (methods.empty())
            {
                ImGui::Text("No methods for class %s", currentObj->klass->getName());
            }
            else
            {
                int j = 0;
                for (auto &[method, paramsInfo] : methods)
                {
                    ImGui::PushID(method + j++);
                    MethodViewer(currentObj->klass, method, paramsInfo, currentObj, true);
                    ImGui::Separator();
                    ImGui::PopID();
                }
            }
            ImGui::EndPopup();
        }

        if (ImGui::Button("Dump to file",
                          ImVec2(ImGui::GetContentRegionAvail().x - ImGui::GetStyle().FramePadding.x, 0)))
        {
            ImGui::OpenPopup("ProceedPopUp");
        }
        if (ImGui::BeginPopup("ProceedPopUp"))
        {
            char fileName[256]{0};
            snprintf(fileName, sizeof(fileName), "dump_%s (%p).json", currentObj->klass->getName(), currentObj);
            ImGui::Text("File will be saved as %s", fileName);
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 50, 255));
            ImGui::Text("Note: This may take a while depending on the size of the object");
            ImGui::Text("Do not touch the screen if it's freezing!");
            ImGui::PopStyleColor();
            if (ImGui::Button("Proceed", ImVec2(ImGui::GetContentRegionAvail().x, 0)))
            {
                ChangeMaxListArraySize(9999,
                                       [&object = currentObj]()
                                       {
                                           char fileName[256]{0};
                                           snprintf(fileName, sizeof(fileName), "dump_%s (%p).json",
                                                    object->klass->getName(), object);
                                           Util::FileWriter file(fileName);
                                           std::vector<uintptr_t> visited{};
                                           nlohmann::ordered_json j = object->dump(visited, 9999);
                                           file.write(j.dump(2, ' ').c_str());
                                           LOGD("Done save");
                                           ImGui::CloseCurrentPopup();
                                       });
            }
            ImGui::EndPopup();
        }
        poper.Update();
        ImGui::EndChild();
        ImGui::EndTable();
    }
}

static bool isPrimitiveTypeName(const char *n)
{
    return n && (!strcmp(n, "System.Boolean") || !strcmp(n, "System.Int16") || !strcmp(n, "System.Int32") ||
                 !strcmp(n, "System.Int64") || !strcmp(n, "System.UInt16") || !strcmp(n, "System.UInt32") ||
                 !strcmp(n, "System.UInt64") || !strcmp(n, "System.Single") || !strcmp(n, "System.Double") ||
                 !strcmp(n, "System.String") || !strcmp(n, "System.Byte") || !strcmp(n, "System.SByte"));
}

static std::string staticFieldToString(FieldInfo *field)
{
    auto type = field->getType();
    const char *n = type ? type->getName() : "";
    char buf[128]{0};
    try
    {
        if (!strcmp(n, "System.Boolean"))
            snprintf(buf, sizeof(buf), "%s", field->getStaticValue<bool>() ? "True" : "False");
        else if (!strcmp(n, "System.Int32") || !strcmp(n, "System.Int16") || !strcmp(n, "System.Byte") ||
                 !strcmp(n, "System.SByte"))
            snprintf(buf, sizeof(buf), "%d", field->getStaticValue<int>());
        else if (!strcmp(n, "System.Int64"))
            snprintf(buf, sizeof(buf), "%lld", (long long)field->getStaticValue<int64_t>());
        else if (!strcmp(n, "System.UInt32") || !strcmp(n, "System.UInt16"))
            snprintf(buf, sizeof(buf), "%u", field->getStaticValue<uint32_t>());
        else if (!strcmp(n, "System.UInt64"))
            snprintf(buf, sizeof(buf), "%llu", (unsigned long long)field->getStaticValue<uint64_t>());
        else if (!strcmp(n, "System.Single"))
            snprintf(buf, sizeof(buf), "%g", field->getStaticValue<float>());
        else if (!strcmp(n, "System.Double"))
            snprintf(buf, sizeof(buf), "%g", field->getStaticValue<double>());
        else if (!strcmp(n, "System.String"))
        {
            auto s = field->getStaticValue<Il2CppString *>();
            return s ? s->to_string() : "null";
        }
        else
            snprintf(buf, sizeof(buf), "%p", field->getStaticValue<void *>());
    }
    catch (...)
    {
        snprintf(buf, sizeof(buf), "?");
    }
    return buf;
}

static void applyStaticFieldString(FieldInfo *field, const std::string &text)
{
    auto type = field->getType();
    const char *n = type ? type->getName() : "";
    try
    {
        if (!strcmp(n, "System.Boolean"))
            field->setStaticValue<bool>(text == "True" || text == "true" || text == "1");
        else if (!strcmp(n, "System.Int32") || !strcmp(n, "System.Int16") || !strcmp(n, "System.Byte") ||
                 !strcmp(n, "System.SByte"))
            field->setStaticValue<int>(std::stoi(text));
        else if (!strcmp(n, "System.Int64"))
            field->setStaticValue<int64_t>(std::stoll(text));
        else if (!strcmp(n, "System.UInt32") || !strcmp(n, "System.UInt16"))
            field->setStaticValue<uint32_t>((uint32_t)std::stoul(text));
        else if (!strcmp(n, "System.UInt64"))
            field->setStaticValue<uint64_t>(std::stoull(text));
        else if (!strcmp(n, "System.Single"))
            field->setStaticValue<float>(std::stof(text));
        else if (!strcmp(n, "System.Double"))
            field->setStaticValue<double>(std::stod(text));
        else if (!strcmp(n, "System.String"))
            field->setStaticValue<Il2CppString *>(Il2cpp::NewString(text.c_str()));
    }
    catch (...)
    {
        LOGE("Failed to set static field %s", field->getName());
    }
}

void ClassesTab::StaticFieldViewer(Il2CppClass *klass)
{
    if (!klass)
        return;
    auto fields = klass->getFields(true);
    bool any = false;
    for (auto f : fields)
    {
        if (Il2cpp::GetFieldFlags(f) & FIELD_ATTRIBUTE_STATIC)
        {
            any = true;
            break;
        }
    }
    if (!any)
        return;
    if (!ImGui::CollapsingHeader(Lang::T("静态字段", "Static Fields")))
        return;
    for (auto field : fields)
    {
        int flags = Il2cpp::GetFieldFlags(field);
        if (!(flags & FIELD_ATTRIBUTE_STATIC))
            continue;
        bool readonly = (flags & FIELD_ATTRIBUTE_LITERAL) || (flags & FIELD_ATTRIBUTE_INIT_ONLY);
        auto type = field->getType();
        const char *tn = type ? type->getName() : "?";
        auto value = staticFieldToString(field);
        ImGui::PushID(field);
        ImGui::Text("%s%s %s = %s", readonly ? "[const] " : "", tn, field->getName(), value.c_str());
        if (!readonly && isPrimitiveTypeName(tn))
        {
            ImGui::SameLine();
            if (ImGui::SmallButton(Lang::T("改", "Edit")))
            {
                Keyboard::Open(value.c_str(), [field](const std::string &text) { applyStaticFieldString(field, text); });
            }
        }
        else if (readonly)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", Lang::T("只读", "read-only"));
        }
        ImGui::PopID();
    }
}

static std::string instanceFieldToString(Il2CppObject *obj, FieldInfo *field)
{
    auto type = field->getType();
    const char *n = type ? type->getName() : "";
    char buf[128]{0};
    try
    {
        if (!strcmp(n, "System.Boolean"))
            snprintf(buf, sizeof(buf), "%s", field->getValue<bool>(obj) ? "True" : "False");
        else if (!strcmp(n, "System.Int32") || !strcmp(n, "System.Int16") || !strcmp(n, "System.Byte"))
            snprintf(buf, sizeof(buf), "%d", field->getValue<int>(obj));
        else if (!strcmp(n, "System.Int64"))
            snprintf(buf, sizeof(buf), "%lld", (long long)field->getValue<int64_t>(obj));
        else if (!strcmp(n, "System.Single"))
            snprintf(buf, sizeof(buf), "%g", field->getValue<float>(obj));
        else if (!strcmp(n, "System.Double"))
            snprintf(buf, sizeof(buf), "%g", field->getValue<double>(obj));
        else if (!strcmp(n, "System.String"))
        {
            auto s = field->getValue<Il2CppString *>(obj);
            return s ? s->to_string() : "null";
        }
        else
            snprintf(buf, sizeof(buf), "%p", field->getValue<void *>(obj));
    }
    catch (...)
    {
        snprintf(buf, sizeof(buf), "?");
    }
    return buf;
}

static void applyInstanceFieldString(Il2CppObject *obj, FieldInfo *field, const std::string &text)
{
    auto type = field->getType();
    const char *n = type ? type->getName() : "";
    try
    {
        if (!strcmp(n, "System.Boolean"))
            field->setValue<bool>(obj, text == "True" || text == "true" || text == "1");
        else if (!strcmp(n, "System.Int32") || !strcmp(n, "System.Int16") || !strcmp(n, "System.Byte"))
            field->setValue<int>(obj, std::stoi(text));
        else if (!strcmp(n, "System.Int64"))
            field->setValue<int64_t>(obj, std::stoll(text));
        else if (!strcmp(n, "System.Single"))
            field->setValue<float>(obj, std::stof(text));
        else if (!strcmp(n, "System.Double"))
            field->setValue<double>(obj, std::stod(text));
        else if (!strcmp(n, "System.String"))
            field->setValue<Il2CppString *>(obj, Il2cpp::NewString(text.c_str()));
    }
    catch (...)
    {
        LOGE("Failed to set field %s", field->getName());
    }
}

void ClassesTab::HookerCollectedFieldEditor(Il2CppClass *klass)
{
    if (!klass)
        return;
    auto it = HookerData::collectSet.find(klass);
    if (it == HookerData::collectSet.end() || it->second.empty())
        return;
    if (!ImGui::CollapsingHeader(Lang::T("已收集对象字段", "Collected object fields")))
        return;
    int shown = 0;
    for (auto obj : it->second)
    {
        if (!obj || !obj->klass)
            continue;
        if (shown++ > 20)
            break;
        ImGui::PushID(obj);
        ImGui::SeparatorText(obj->klass->getName());
        ImGui::Text("%p", obj);
        for (auto field : obj->klass->getFields())
        {
            int flags = Il2cpp::GetFieldFlags(field);
            if (flags & FIELD_ATTRIBUTE_STATIC)
                continue;
            bool readonly = (flags & FIELD_ATTRIBUTE_LITERAL) || (flags & FIELD_ATTRIBUTE_INIT_ONLY);
            auto type = field->getType();
            const char *tn = type ? type->getName() : "?";
            auto value = instanceFieldToString(obj, field);
            ImGui::PushID(field);
            ImGui::Text("%s = %s", field->getName(), value.c_str());
            if (!readonly && isPrimitiveTypeName(tn))
            {
                ImGui::SameLine();
                if (ImGui::SmallButton(Lang::T("改", "Edit")))
                {
                    Keyboard::Open(value.c_str(), [obj, field](const std::string &text) {
                        applyInstanceFieldString(obj, field, text);
                    });
                }
            }
            ImGui::PopID();
        }
        ImGui::PopID();
    }
}

void ClassesTab::FilterClasses(const std::string &filter)
{
    filteredClasses.clear();
    classes.clear();
    methodMap.clear(); // is this necessary?

    // 冒号前缀：":MethodName" 表示按方法名筛选
    std::string effectiveFilter = filter;
    if (!filter.empty() && filter[0] == ':')
    {
        effectiveFilter = filter.substr(1);
        filterByClass = false;
        filterByMethod = true;
        filterByField = false;
        filterMethodByName = true;
        filterMethodByReturn = false;
        filterMethodByParams = false;
    }
    if (includeAllImages)
    {
        for (auto image : g_Images)
        {
            auto imageClasses = image->getClasses();
            classes.insert(classes.end(), imageClasses.begin(), imageClasses.end());
        }
    }
    else
    {
        classes = selectedImage->getClasses();
    }

    auto equalsCaseInsensitive = [](const std::string &a, const std::string &b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); i++)
            if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
                return false;
        return true;
    };

    auto finderCaseSensitive = [](const std::string &a, const std::string &b)
    { return a.find(b) != std::string::npos; };

    auto finderCaseInsensitive = [](const std::string &a, const std::string &b)
    {
        auto newA = a;
        auto newB = b;
        std::transform(newA.begin(), newA.end(), newA.begin(), ::tolower);
        std::transform(newB.begin(), newB.end(), newB.begin(), ::tolower);
        return newA.find(newB) != std::string::npos;
    };

    auto finder = [&](const std::string &hay, const std::string &needle) -> bool
    {
        if (needle.empty())
            return true;
        if (exactMatch)
            return caseSensitive ? hay == needle : equalsCaseInsensitive(hay, needle);
        return caseSensitive ? finderCaseSensitive(hay, needle) : finderCaseInsensitive(hay, needle);
    };

    for (int i = 0; i < classes.size() && filteredClasses.size() < (showAllClasses ? classes.size() : MAX_CLASSES); i++)
    {
        auto klass = classes[i];

        if (/* Il2cpp::GetClassIsStatic(klass) || */ Il2cpp::GetClassIsEnum(klass))
            continue;

        if (filterByClass)
        {
            // if (klass->getFullName().find(effectiveFilter) != std::string::npos)
            if (finder(klass->getFullName(), effectiveFilter))
            {
                filteredClasses.push_back(klass);
                for (auto m : klass->getMethods())
                {
                    auto paramsInfo = m->getParamsInfo();
                    methodMap[klass].push_back({m, paramsInfo});
                }
            }
        }
        else if (filterByMethod)
        {
            bool found = false;
            for (auto m : klass->getMethods())
            {
                bool hit = false;
                if (filterMethodByName && finder(m->getName(), effectiveFilter))
                    hit = true;
                if (!hit && filterMethodByReturn)
                {
                    auto rt = m->getReturnType();
                    if (rt && finder(rt->getName(), effectiveFilter))
                        hit = true;
                }
                if (!hit && filterMethodByParams)
                {
                    for (auto &[pname, ptype] : m->getParamsInfo())
                    {
                        if ((pname && finder(pname, effectiveFilter)) || (ptype && finder(ptype->getName(), effectiveFilter)))
                        {
                            hit = true;
                            break;
                        }
                    }
                }
                if (hit)
                {
                    found = true;
                    auto paramsInfo = m->getParamsInfo();
                    methodMap[klass].push_back({m, paramsInfo});
                }
            }
            if (found)
                filteredClasses.push_back(klass);
        }
        else if (filterByField)
        {
            bool found = false;
            for (auto f : klass->getFields())
            {
                // if (std::string(f->getName()).find(effectiveFilter) != std::string::npos)
                if (finder(f->getName(), effectiveFilter))
                {
                    found = true;
                }
            }
            if (found)
            {
                filteredClasses.push_back(klass);
                for (auto m : klass->getMethods())
                {
                    auto paramsInfo = m->getParamsInfo();
                    methodMap[klass].push_back({m, paramsInfo});
                }
            }
        }
    }
    Tool::ConfigSave();
}

void to_json(nlohmann::ordered_json &j, const ClassesTab &p)
{
    j["filter"] = p.filter;
    j["filterByClass"] = p.filterByClass;
    j["filterByField"] = p.filterByField;
    j["filterByMethod"] = p.filterByMethod;
    j["filterMethodByName"] = p.filterMethodByName;
    j["filterMethodByReturn"] = p.filterMethodByReturn;
    j["filterMethodByParams"] = p.filterMethodByParams;
    j["showAllClasses"] = p.showAllClasses;
    j["includeAllImages"] = p.includeAllImages;
    j["caseSensitive"] = p.caseSensitive;
    j["exactMatch"] = p.exactMatch;
    j["alwaysRefreshInspector"] = p.alwaysRefreshInspector;
    j["showMethods"] = p.showMethods;
    j["selectedImage"] = p.selectedImage ? p.selectedImage->getName() : "";
}

void from_json(const nlohmann::ordered_json &j, ClassesTab &p)
{
    j.at("filter").get_to(p.filter);
    j.at("filterByClass").get_to(p.filterByClass);
    j.at("filterByField").get_to(p.filterByField);
    j.at("filterByMethod").get_to(p.filterByMethod);
    j.at("showAllClasses").get_to(p.showAllClasses);
    j.at("includeAllImages").get_to(p.includeAllImages);
    j.at("caseSensitive").get_to(p.caseSensitive);
    if (j.contains("exactMatch"))
        j.at("exactMatch").get_to(p.exactMatch);
    if (j.contains("filterMethodByName"))
        j.at("filterMethodByName").get_to(p.filterMethodByName);
    if (j.contains("filterMethodByReturn"))
        j.at("filterMethodByReturn").get_to(p.filterMethodByReturn);
    if (j.contains("filterMethodByParams"))
        j.at("filterMethodByParams").get_to(p.filterMethodByParams);
    if (j.contains("alwaysRefreshInspector"))
        j.at("alwaysRefreshInspector").get_to(p.alwaysRefreshInspector);
    if (j.contains("showMethods"))
        j.at("showMethods").get_to(p.showMethods);
    std::string selectedImage = j.at("selectedImage").get<std::string>();
    if (selectedImage.ends_with(".dll"))
    {
        selectedImage.erase(selectedImage.size() - 4);
    }
    auto assembly = Il2cpp::GetAssembly(selectedImage.c_str());
    if (assembly)
    {
        auto image = assembly->getImage();
        if (image)
        {
            p.selectedImage = image;
        }
    }
}

std::unordered_map<Il2CppClass *, std::vector<Il2CppObject *>> ClassesTab::objectMap;
std::unordered_map<Il2CppClass *, std::vector<Il2CppObject *>> ClassesTab::newObjectMap;
std::unordered_map<Il2CppClass *, std::set<Il2CppObject *>> ClassesTab::savedSet;
std::unordered_map<MethodInfo *, ClassesTab::OriginalMethodBytes> ClassesTab::oMap;
std::unordered_map<MethodInfo *, std::unordered_map<int, ClassesTab::HookerArgMod>> ClassesTab::hookerArgMods;
std::unordered_map<Il2CppClass *, bool> ClassesTab::states;
PopUpSelector ClassesTab::poper;
