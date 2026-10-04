#include <jni.h>
#include <pthread.h>
#include <thread>
#include <unistd.h>
#include "Il2cpp/Il2cpp.h"
#include "Il2cpp/il2cpp-class.h"
#include "Includes/Logger.h"
#include "Includes/Utils.h"
#include "Includes/obfuscate.h"
#include "Menu/ImGui.h"
#include "Menu/VulkanHook.h"
#include "Script/Script.h"
#include "Tool/Keyboard.h"
#include "Tool/Lang.h"
#include "Tool/Memory.h"
#include "Tool/SessionLog.h"
#include "Tool/TextureCache.h"
#include "Tool/Targets.h"
#include "Tool/Tool.h"
#include "Tool/Util.h"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "sstream"
#include "jni.h"
#include "Tool/Unity.h"
#include <atomic>
#include <cstdint>

// OverlayLoader: 用 InMemoryDexClassLoader 加载嵌入的 dex，反射启动 Java SurfaceView overlay
extern bool OverlayLoader_start();

void logcatJson(nlohmann::ordered_json &json)
{
    auto str = json.dump(4, '#');
    std::istringstream iss(str);
    std::string line;
    while (std::getline(iss, line))
    {
        usleep(100);
        LOGD("%s", line.c_str());
    }
}

template <typename T>
void ConfigSet(const char *key, T value);

template <typename T>
T ConfigGet(const char *key, T defaultValue);

#define targetLibName OBFUSCATE("libil2cpp.so")

Il2CppImage *g_Image = nullptr;
std::vector<MethodInfo *> g_Methods;
extern std::unordered_map<void *, HookerData> hookerMap;
extern int maxLine;

extern ImVec2 initialScreenSize;

bool collapsed = false;
bool fullScreen = false;
bool resetWindow = false;
bool centerWindow = false;
int snapWindow = 0; // 0=无, 1=左上, 2=右上, 3=左下, 4=右下
int selectedScale = 3;
int selectedTheme = 0;
bool neonBorder = false;
float neonBorderColor[3] = {0.0f, 0.95f, 1.0f};
int fpsLimit = 0;

extern int g_fpsLimit;

bool doChangeScale = false;

// 主选项卡显示开关（脚本/资源管理器等预留项默认隐藏）
bool tabShowTools = true;
bool tabShowTracer = true;
bool tabShowDumper = true;
bool tabShowSettings = true;
// 内存查看选项卡（默认隐藏）
bool tabShowMemory = false;
// 脚本选项卡（默认隐藏）
bool tabShowScript = false;

// 自动转储：进入转储页面后自动开始一次转储
bool autoDump = false;

// 双指滚动开关
bool twoFingerScroll = true;

// Touch Passthrough：开启后不拦截触摸事件，直接传递给游戏
bool touchPassthrough = false;

// Safer Inspector：更安全的检查模式（较慢）
bool saferInspector = false;

// Side Split：类窗口左右分栏布局
bool sideSplit = false;

// 窗口位置记忆（浮动/贴边控制的持久化）
ImVec2 g_lastWindowPos = ImVec2(0, 0);
ImVec2 g_lastWindowSize = ImVec2(0, 0);
bool g_windowPosLoaded = false;

constexpr std::array<const char *, 7> possibleScale = {
    "Smallest", "Smaller", "Small", "Default", "Large", "Larger", "Largest",
};
constexpr std::array<float, 7> scaleFactors = {0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f};

ImGuiStyle initialStyle;

const char *title = OBFUSCATE("NirithyTool");
static std::atomic<bool> g_hackStarted{false};

void draw_thread()
{
    // 启动时从配置恢复窗口位置/尺寸（仅一次）
    if (!g_windowPosLoaded)
    {
        g_windowPosLoaded = true;
        g_lastWindowPos.x = (float)ConfigGet<double>("winPosX", 20.0);
        g_lastWindowPos.y = (float)ConfigGet<double>("winPosY", 80.0);
        g_lastWindowSize.x = (float)ConfigGet<double>("winSizeX", 0.0);
        g_lastWindowSize.y = (float)ConfigGet<double>("winSizeY", 0.0);
    }

    if (!Il2cpp::IsReady())
    {
        ImGui::SetNextWindowPos(ImVec2(20, 80), ImGuiCond_Once);
        ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x * 0.7f, 0), ImGuiCond_Once);
        ImGui::Begin(Lang::T("IL2CPP 错误", "IL2CPP Error"), nullptr);
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 80, 80, 255));
        ImGui::TextWrapped("%s", Il2cpp::GetStatusMessage());
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::TextWrapped("%s",
                           Lang::T("游戏可能未使用 IL2CPP，或 libil2cpp.so 被混淆/加密/重命名。菜单仍可显示，但检查器不可用。",
                                   "This game may not use IL2CPP, or libil2cpp.so is obfuscated/encrypted/renamed. The overlay still shows, but the inspector is disabled."));
        Lang::Combo();
        ImGui::End();
        return;
    }

    if (resetWindow)
    {
        resetWindow = false;
        if (fullScreen)
        {
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            auto screenSize = ImGui::GetIO().DisplaySize;
            ImGui::SetNextWindowSize(screenSize);
        }
        else
        {
            ImGui::SetNextWindowPos(g_lastWindowPos);
            ImGui::SetNextWindowSize(g_lastWindowSize);
        }
    }
    if (centerWindow || snapWindow)
    {
        auto screenSize = ImGui::GetIO().DisplaySize;
        ImVec2 winSize = g_lastWindowSize;
        if (winSize.x <= 0.f || winSize.y <= 0.f)
        {
            winSize = ImVec2(screenSize.x * 0.5f, screenSize.y * 0.5f);
        }
        if (centerWindow)
        {
            centerWindow = false;
            ImGui::SetNextWindowPos(ImVec2((screenSize.x - winSize.x) * 0.5f, (screenSize.y - winSize.y) * 0.5f));
        }
        else if (snapWindow)
        {
            const float margin = 0.f;
            switch (snapWindow)
            {
            case 1: // 左上
                ImGui::SetNextWindowPos(ImVec2(margin, margin));
                break;
            case 2: // 右上
                ImGui::SetNextWindowPos(ImVec2(screenSize.x - winSize.x - margin, margin));
                break;
            case 3: // 左下
                ImGui::SetNextWindowPos(ImVec2(margin, screenSize.y - winSize.y - margin));
                break;
            case 4: // 右下
                ImGui::SetNextWindowPos(ImVec2(screenSize.x - winSize.x - margin, screenSize.y - winSize.y - margin));
                break;
            default:
                break;
            }
            snapWindow = 0;
        }
        ImGui::SetNextWindowSize(winSize);
    }
    if (fullScreen)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, ImGui::GetFrameHeight()));
    }
    int i = 0;
    auto drawList = ImGui::GetBackgroundDrawList();

    for (auto &v : HookerData::visited)
    {
        if (v.name.empty())
            continue;
        char label[256]{0};
        sprintf(label, "%s", v.name.c_str());
        if (v.hitCount > 0)
        {
            sprintf(label, "%s (%dx)", label, v.hitCount);
        }
        auto labelSize = ImGui::CalcTextSize(label);
        ImVec2 labellPos{20, 150 + (labelSize.y * i)};

        auto dt = ImGui::GetIO().DeltaTime;
        constexpr ImVec4 GREEN = {0.f, 1.f, 0.f, 1.f};
        ImColor color = ImColor(1.f, 1.f, 1.f, 1.f);
        if (v.time > 0.f)
        {
            v.time -= dt;
            auto t = v.time;
            color = ImColor(ImLerp(color.Value.x, GREEN.x, t), ImLerp(color.Value.y, GREEN.y, t),
                            ImLerp(color.Value.z, GREEN.z, t), 1.f);
        }
        v.goneTime -= dt;
        if (v.goneTime > 0.f && v.goneTime <= 1.f)
        {
            auto t = v.goneTime;
            color.Value.w = ImLerp(0.f, color.Value.w, t);
        }
        if (v.goneTime <= 0.f)
        {
            v.name = "";
        }

        drawList->AddRectFilled(labellPos, {labellPos.x + labelSize.x, labellPos.y + labelSize.y},
                                IM_COL32(0, 0, 0, 100));
        drawList->AddText(labellPos, color, label);
        i++;
    }
    collapsed = !ImGui::Begin(title, nullptr, (fullScreen ? ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove : 0));
    if (fullScreen)
    {
        ImGui::PopStyleVar();
    }

#ifdef __DEBUG__
    static bool showDemoWindow = false;
#endif
    Keyboard::Update();
    static bool changeToToolsTab = false;
    if (ImGui::BeginTabBar("mainTabber"))
    {
        if (tabShowTools)
        {
            if (ImGui::BeginTabItem(Lang::T("工具", "Tools"), nullptr, changeToToolsTab ? ImGuiTabItemFlags_SetSelected : 0))
            {
                changeToToolsTab = false;
                if (ImGui::Checkbox(Lang::T("全屏", "Fullscreen"), &fullScreen))
                {
                    if (fullScreen)
                    {
                        g_lastWindowSize = ImGui::GetWindowSize();
                        g_lastWindowPos = ImGui::GetWindowPos();
                        ConfigSet("winPosX", (double)g_lastWindowPos.x);
                        ConfigSet("winPosY", (double)g_lastWindowPos.y);
                        ConfigSet("winSizeX", (double)g_lastWindowSize.x);
                        ConfigSet("winSizeY", (double)g_lastWindowSize.y);
                    }
                    resetWindow = true;
                }
                Tool::Draw();
                ImGui::EndTabItem();
            }
        }

        if (tabShowTracer && !hookerMap.empty())
        {
            if (ImGui::BeginTabItem(Lang::T("跟踪器", "Tracer")))
            {
                ImGui::Text("%s %zu", Lang::T("已跟踪方法数 :", "Traced method count :"), hookerMap.size());
                std::vector<HookerData *> sortedHooker;
                for (auto &[name, data] : hookerMap)
                {
                    if (data.hitCount > 0)
                        sortedHooker.push_back(&data);
                }
                if (!sortedHooker.empty())
                {
                    if (ImGui::Button(Lang::T("快速恢复", "Quick Restore")))
                    {
                        ImGui::OpenPopup("QuickRestorePopup");
                    }
                    std::sort(sortedHooker.begin(), sortedHooker.end(),
                              [](const HookerData *a, const HookerData *b) { return a->hitCount > b->hitCount; });
                    ImGui::BeginChild("TracerList", ImVec2(0, 0), ImGuiChildFlags_None,
                                      ImGuiWindowFlags_HorizontalScrollbar);
                    auto &tab = Tool::GetFirstTab();
                    for (auto v : sortedHooker)
                    {
                        if (!v->method)
                            continue;
                        ImGui::PushID(v->method);
                        bool opened =
                            tab.MethodViewer(v->method->getClass(), v->method, tab.getCachedParams(v->method));
                        // Field editor in the tracer / hooker list
                        if (opened && v->method->getClass())
                        {
                            tab.StaticFieldViewer(v->method->getClass());
                            tab.HookerCollectedFieldEditor(v->method->getClass());
                        }
                        if (!opened && !changeToToolsTab && ImGui::IsItemHeld())
                        {
                            LOGD("IsItemHeld %s", v->method->getName());
                            changeToToolsTab = true;
                            Tool::OpenNewTabFromClass(v->method->getClass()).setOpenedTab = true;
                            ImGui::PopID();
                            break;
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndChild();
                    static auto &io = ImGui::GetIO();
                    ImGui::SetNextWindowSizeConstraints(ImVec2(io.DisplaySize.x / 1.2f, 0),
                                                        ImVec2(io.DisplaySize.x / 1.2f, io.DisplaySize.y / 2));
                    if (ImGui::BeginPopup("QuickRestorePopup",
                                          ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_MenuBar))
                    {
                        HookerData *toBeErased = nullptr;
                        for (auto v : sortedHooker)
                        {
                            char label[256]{0};
                            sprintf(label, "%s::%s (%dx)###%p", v->method->getClass()->getName(), v->method->getName(),
                                    v->hitCount, v->method);
                            if (ImGui::Button(label, ImVec2(ImGui::GetContentRegionAvail().x, 0)))
                            {
                                toBeErased = v;
                            }
                        }
                        if (toBeErased)
                        {
                            Tool::ToggleHooker(toBeErased->method, 0);
                        }
                        ImGui::EndPopup();
                    }
                }
                else
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 0, 0, 255));
                    ImGui::Text("%s", Lang::T("还没有被调用的跟踪方法", "No traced method has been called yet"));
                    ImGui::PopStyleColor();
                }
                ImGui::EndTabItem();
            }
        }
        if (tabShowDumper)
        {
            if (ImGui::BeginTabItem(Lang::T("转储", "Dumper")))
            {
                Tool::Dumper();
                ImGui::EndTabItem();
            }
        }
        if (tabShowScript)
        {
            if (ImGui::BeginTabItem(Lang::T("脚本", "Script")))
            {
                Script::Draw();
                ImGui::EndTabItem();
            }
        }
        if (tabShowMemory)
        {
            if (ImGui::BeginTabItem(Lang::T("内存", "Memory")))
            {
                Memory::Draw();
                ImGui::EndTabItem();
            }
        }
        if (tabShowSettings)
        {
            if (ImGui::BeginTabItem(Lang::T("设置", "Settings")))
            {
            ImGui::Separator();
            ImGui::Text("%s", Lang::T("显示", "Display"));
            ImGui::TextDisabled("%s", Lang::T("选项卡:", "Tabs:"));
            // 自适应网格：根据可用宽度自动计算每行列数
            {
                float avail = ImGui::GetContentRegionAvail().x;
                int perRow = avail > 180.0f ? (int)(avail / 180.0f) : 1;
                int col = 0;
                auto check = [&](const char *label, bool *var, const char *key)
                {
                    if (ImGui::Checkbox(label, var))
                        ConfigSet(key, *var);
                    col++;
                    if (col % perRow != 0)
                        ImGui::SameLine();
                };
                check(Lang::T("工具", "Tools"), &tabShowTools, "tabShowTools");
                check(Lang::T("跟踪器", "Tracer"), &tabShowTracer, "tabShowTracer");
                check(Lang::T("转储", "Dumper"), &tabShowDumper, "tabShowDumper");
                check(Lang::T("内存", "Memory"), &tabShowMemory, "tabShowMemory");
                check(Lang::T("脚本", "Script"), &tabShowScript, "tabShowScript");
                check(Lang::T("设置", "Settings"), &tabShowSettings, "tabShowSettings");
                check(Lang::T("双指滚动", "Two-finger scroll"), &twoFingerScroll, "twoFingerScroll");
                check(Lang::T("触摸穿透", "Touch Passthrough"), &touchPassthrough, "touchPassthrough");
                check(Lang::T("更安全的检查器", "Safer Inspector"), &saferInspector, "saferInspector");
            }
            if (touchPassthrough)
            {
                ImGui::TextDisabled("%s", Lang::T("临时方案：触摸事件未传递到游戏时启用", "Temporary fix if touches are not reaching the game"));
            }
            ImGui::Separator();
            auto preview = possibleScale[selectedScale];
            if (ImGui::BeginCombo(Lang::T("缩放##Global", "Scale##Global"), preview))
            {
                for (int i = 0; i < possibleScale.size(); i++)
                {
                    bool selected = i == selectedScale;
                    if (ImGui::Selectable(possibleScale[i], selected))
                    {
                        selectedScale = i;
                        ConfigSet("selectedScale", selectedScale);
                        doChangeScale = true;
                    }
                    if (selected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            if (Lang::Combo())
            {
                ConfigSet("lang", Lang::current);
            }
            ImGui::Separator();
            ImGui::Text("%s", Lang::tr("theme.label"));
            const char *themeLabel = Lang::tr(themeName(selectedTheme));
            if (ImGui::BeginCombo(Lang::T("主题##Global", "Theme##Global"), themeLabel))
            {
                for (int i = 0; i < kThemeCount; i++)
                {
                    bool selected = i == selectedTheme;
                    if (ImGui::Selectable(Lang::tr(themeName(i)), selected))
                    {
                        selectedTheme = i;
                        ConfigSet("theme", selectedTheme);
                        doChangeScale = true;
                    }
                    if (selected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::Checkbox(Lang::T("霓虹边框", "Neon border"), &neonBorder))
            {
                ConfigSet("neonBorder", neonBorder);
                doChangeScale = true;
            }
            if (neonBorder)
            {
                ImGui::SameLine();
                if (ImGui::ColorEdit3(Lang::T("边框颜色", "Border color"), neonBorderColor))
                {
                    ConfigSet("neonBorderR", neonBorderColor[0]);
                    ConfigSet("neonBorderG", neonBorderColor[1]);
                    ConfigSet("neonBorderB", neonBorderColor[2]);
                    doChangeScale = true;
                }
            }
            ImGui::Text("%s", Lang::T("最大帧率", "Max FPS"));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120);
            static const std::array<const char *, 7> possibleFps = {
                "0 (Unlimited)", "30", "45", "60", "90", "120", "144",
            };
            int fpsIndex = 0;
            for (int i = 0; i < possibleFps.size(); i++)
            {
                if (atoi(possibleFps[i]) == fpsLimit)
                    fpsIndex = i;
            }
            if (ImGui::BeginCombo("##FpsLimit", possibleFps[fpsIndex]))
            {
                for (int i = 0; i < possibleFps.size(); i++)
                {
                    bool selected = i == fpsIndex;
                    if (ImGui::Selectable(possibleFps[i], selected))
                    {
                        fpsLimit = atoi(possibleFps[i]);
                        g_fpsLimit = fpsLimit;
                        ConfigSet("fpsLimit", fpsLimit);
                    }
                    if (selected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::TextDisabled("%s", Lang::T("限制菜单帧率会同时限制游戏帧率", "Limiting menu FPS also limits game FPS"));
            ImGui::Checkbox(Lang::T("键盘收不到输入时启用备用方案", "Enable if keyboard input is not being received"),
                            &Keyboard::check);
            if (ImGui::Button(Lang::T("重置图片缓存", "Reset image cache")))
            {
                TextureCache::Reset();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(%zu)", TextureCache::Count());
            ImGui::Separator();
            ImGui::Text("%s", Lang::T("视口", "Viewport"));
            if (ImGui::Button(Lang::T("重置窗口", "Reset Window")))
            {
                resetWindow = true;
            }
            ImGui::SameLine();
            if (ImGui::Button(Lang::T("居中", "Center")))
            {
                centerWindow = true;
            }
            if (ImGui::Button(Lang::T("左上", "Top-Left")))
            {
                snapWindow = 1;
            }
            ImGui::SameLine();
            if (ImGui::Button(Lang::T("右上", "Top-Right")))
            {
                snapWindow = 2;
            }
            ImGui::SameLine();
            if (ImGui::Button(Lang::T("左下", "Bottom-Left")))
            {
                snapWindow = 3;
            }
            ImGui::SameLine();
            if (ImGui::Button(Lang::T("右下", "Bottom-Right")))
            {
                snapWindow = 4;
            }
#ifdef __DEBUG__
            if (ImGui::Checkbox("Show Demo Window", &showDemoWindow))
            {
            }
#endif
            ImGui::Separator();
            ImGui::Text("%s", Lang::T("信息", "Info"));
            static auto packageName = Il2cpp::getPackageName();
            static auto unityVersion = Il2cpp::getUnityVersion();
            static auto gameVersion = Il2cpp::getGameVersion();
            ImGui::Text("%s: %s", Lang::T("包名", "Package"), packageName.c_str());
            ImGui::Text("%s: %s", Lang::T("版本", "Version"), gameVersion.c_str());
            ImGui::Text("Unity: %s", unityVersion.c_str());
#ifdef __aarch64__
            ImGui::Text("Arch: %s", "arm64-v8a");
#else
            ImGui::Text("Arch: %s", "armeabi-v7a");
#endif
            ImGui::Text("%s: %s", Lang::T("渲染", "Renderer"),
                        isVulkanOverlayActive() ? "Vulkan" : "OpenGL ES");
            ImGui::Separator();

            if (ImGui::Button("Tg : crackhannel", ImVec2(-1, 0)))
            {
                auto Application = Il2cpp::FindClass("UnityEngine.Application");
                auto OpenURL = Application ? Application->getMethod("OpenURL", 1) : nullptr;
                if (OpenURL)
                {
                    OpenURL->invoke_static<void>(Il2cpp::NewString(OBFUSCATE("https://t.me/crackhannel")));
                }
                else
                {
                    Keyboard::Open(OBFUSCATE("https://t.me/crackhannel"), nullptr);
                }
            }
            if (ImGui::Button("Qgroup : ", ImVec2(-1, 0)))
            {
                auto Application = Il2cpp::FindClass("UnityEngine.Application");
                auto OpenURL = Application ? Application->getMethod("OpenURL", 1) : nullptr;
                if (OpenURL)
                {
                    OpenURL->invoke_static<void>(Il2cpp::NewString(OBFUSCATE("https://qm.qq.com/q/f7KWZ9wJtS")));
                }
                else
                {
                    Keyboard::Open(OBFUSCATE("https://qm.qq.com/q/f7KWZ9wJtS"), nullptr);
                }
            }

            ImGui::Separator();
            ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    static bool doCalculate = false;
    if (doCalculate)
    {
        doCalculate = false;
        Tool::CalculateSomething();
    }

    if (doChangeScale)
    {
        doChangeScale = false;
        static auto font = ImGui::GetFont();

        font->Scale = scaleFactors[selectedScale];
        // 从初始风格恢复尺寸，应用主题颜色，再统一缩放（避免反复缩放累积）
        ImGui::GetStyle() = initialStyle;
        menuStyle(selectedTheme);
        auto style = ImGui::GetStyle();
        style.ScaleAllSizes(font->Scale);
        ImGui::GetStyle() = style;
        doCalculate = true;
        if (fullScreen)
            resetWindow = true;
    }

#ifdef __DEBUG__
    if (showDemoWindow)
    {
        ImGui::ShowDemoWindow();
    }
#endif
    ImGui::End();

    // 周期保存窗口位置/尺寸（全屏或缩放时不覆盖已保存值）
    static int saveCounter = 0;
    if (!fullScreen && ++saveCounter % 120 == 0)
    {
        g_lastWindowPos = ImGui::GetWindowPos();
        g_lastWindowSize = ImGui::GetWindowSize();
        ConfigSet("winPosX", (double)g_lastWindowPos.x);
        ConfigSet("winPosY", (double)g_lastWindowPos.y);
        ConfigSet("winSizeX", (double)g_lastWindowSize.x);
        ConfigSet("winSizeY", (double)g_lastWindowSize.y);
    }
}

static nlohmann::ordered_json gConf;

// 将全局配置写回 tool_conf.json（供其它模块修改共享开关后调用）
void SaveGlobalConfig()
{
    LOGD(__FUNCTION__);
    Util::FileWriter fileWriter("tool_conf.json");
    fileWriter.write(gConf.dump(2).c_str());
}

template <typename T>
void ConfigSet(const char *key, T value)
{
    LOGD(__FUNCTION__);
    gConf[key] = value;
    LOGD("ConfigWrite %s = %s", key, gConf[key].dump().c_str());
    SaveGlobalConfig();
}

template <typename T>
T ConfigGet(const char *key, T defaultValue)
{
    LOGD(__FUNCTION__);
    if (gConf.contains(key))
    {
        LOGD("ConfigGet %s = %s", key, gConf[key].dump().c_str());
        return gConf[key].get<T>();
    }
    else
    {
        ConfigSet(key, defaultValue);
    }
    return defaultValue;
}

// 以 JSON 文本形式读取全局配置（供脚本引擎等其它编译单元使用）
// key: 配置键
// 返回：配置值的 JSON 文本，键不存在时返回 "null"
std::string ConfigGetJson(const char *key)
{
    if (gConf.contains(key))
        return gConf[key].dump();
    return "null";
}

// 以 JSON 文本形式写入全局配置并持久化到 tool_conf.json
// key: 配置键；jsonValue: JSON 文本形式的取值
// 解析失败时不修改配置
void ConfigSetJson(const char *key, const char *jsonValue)
{
    try
    {
        gConf[key] = nlohmann::json::parse(jsonValue);
        SaveGlobalConfig();
    }
    catch (nlohmann::json::exception &e)
    {
        LOGE("ConfigSetJson error : %s", e.what());
    }
}

void ConfigInit()
{
    Util::FileReader fileReader("tool_conf.json");
    if (fileReader.exists())
    {
        auto data = fileReader.read();
        try
        {
            gConf = nlohmann::json::parse(data);
            logcatJson(gConf);
        }
        catch (nlohmann::json::exception &e)
        {
            LOGE("ConfigInit error : %s", e.what());
            Util::FileWriter fileWriter("tool_conf.json");
            fileWriter.write("{}");
        }
    }
    else
    {
        Util::FileWriter fileWriter("tool_conf.json");
        fileWriter.write("{}");
    }
}

void on_init()
{
    LOGD(__FUNCTION__);
    int waited = 0;
    while (!isLibraryLoaded(targetLibName) && waited < 30)
    {
        sleep(1);
        waited++;
    }

    Il2cpp::Init();
    if (!Il2cpp::IsReady())
    {
        LOGE("il2cpp init failed: %s", Il2cpp::GetStatusMessage());
        initialStyle = ImGui::GetStyle();
        ConfigInit();
        Lang::current = ConfigGet<int>("lang", 0);
        selectedScale = ConfigGet<int>("selectedScale", selectedScale);
        selectedTheme = ConfigGet<int>("theme", 0);
        neonBorder = ConfigGet<bool>("neonBorder", false);
        neonBorderColor[0] = ConfigGet<float>("neonBorderR", 0.0f);
        neonBorderColor[1] = ConfigGet<float>("neonBorderG", 0.95f);
        neonBorderColor[2] = ConfigGet<float>("neonBorderB", 1.0f);
        doChangeScale = true;
        return;
    }

    LOGI("%s has been loaded", (const char *)targetLibName);

    if (!Il2cpp::EnsureAttached())
    {
        LOGE("il2cpp attach failed");
        return;
    }

    Keyboard::Init();

    initialStyle = ImGui::GetStyle();
    ConfigInit();
    // 初始化 Lua 脚本引擎（注册宿主 C API）
    Script::Init();
    selectedScale = ConfigGet<int>("selectedScale", selectedScale);
    selectedTheme = ConfigGet<int>("theme", 0);
    Lang::current = ConfigGet<int>("lang", 0);
    fpsLimit = ConfigGet<int>("fpsLimit", 0);
    g_fpsLimit = fpsLimit;
    tabShowTools = ConfigGet<bool>("tabShowTools", true);
    tabShowTracer = ConfigGet<bool>("tabShowTracer", true);
    tabShowDumper = ConfigGet<bool>("tabShowDumper", true);
    tabShowSettings = ConfigGet<bool>("tabShowSettings", true);
    tabShowMemory = ConfigGet<bool>("tabShowMemory", false);
    tabShowScript = ConfigGet<bool>("tabShowScript", false);
    autoDump = ConfigGet<bool>("autoDump", false);
    twoFingerScroll = ConfigGet<bool>("twoFingerScroll", true);
    touchPassthrough = ConfigGet<bool>("touchPassthrough", false);
    saferInspector = ConfigGet<bool>("saferInspector", false);
    sideSplit = ConfigGet<bool>("sideSplit", false);
    neonBorder = ConfigGet<bool>("neonBorder", false);
    neonBorderColor[0] = ConfigGet<float>("neonBorderR", 0.0f);
    neonBorderColor[1] = ConfigGet<float>("neonBorderG", 0.95f);
    neonBorderColor[2] = ConfigGet<float>("neonBorderB", 1.0f);
    if (selectedScale < 0 || selectedScale >= scaleFactors.size())
    {
        selectedScale = 3;
        ConfigSet("selectedScale", selectedScale);
    }
    if (selectedTheme < 0 || selectedTheme >= kThemeCount)
    {
        selectedTheme = 0;
        ConfigSet("theme", selectedTheme);
    }
    doChangeScale = true;

    LOGD("HOOKING...");

#ifndef LIB_INPUT
    Unity::HookInput();
#endif

    auto assembly = Il2cpp::GetAssembly("Assembly-CSharp");
    g_Image = assembly ? assembly->getImage() : nullptr;
    auto images = Il2cpp::GetImages();
    if (!g_Image && !images.empty())
        g_Image = images.front();
    Tool::Init(g_Image, images);

    for (auto image : images)
    {
        if (!image)
            continue;
        for (auto klass : image->getClasses())
        {
            if (!klass)
                continue;
            for (auto m : klass->getMethods())
            {
                if (!m || !m->methodPointer)
                    continue;
                g_Methods.emplace_back(m);
            }
        }
    }
    LOGD("%zu methods", g_Methods.size());
    if (!g_Methods.empty())
    {
        LOGD("SORTING");
        std::sort(g_Methods.begin(), g_Methods.end(),
                  [](const auto &a, const auto &b) { return a->methodPointer < b->methodPointer; });
        LOGPTR(g_Methods.front()->methodPointer);
        LOGPTR(g_Methods.back()->methodPointer);
        LOGD("SORTED");
    }
    LOGD("HOOKED!");
}

bool useJava = false;
// arg 为 Zygisk preAppSpecialize 阶段建立的 root companion 日志通道 fd（转成 intptr_t），
// -1 表示无通道（APK 进程内注入），此时会话日志回退到应用私有目录。
void *hack_thread(void *arg)
{
    bool expected = false;
    if (!g_hackStarted.compare_exchange_strong(expected, true))
    {
        LOGI("hack_thread already started");
        return nullptr;
    }
    logger::Clear();
    // 必须在任何 LOGx 之前初始化，否则最早的几行日志无法落盘
    SessionLog::Init((int)(intptr_t)arg, Targets::CurrentProcessPackage());
    LOGI("session log started (channel=%d)", (int)(intptr_t)arg);

    LOGI("pthread created");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 优先尝试 Java SurfaceView overlay（InMemoryDexClassLoader + OverlayHelper）
    // 这种方式不依赖游戏的渲染 API（Vulkan/GLES 都能显示），是 IL2CPP MCP 等成熟工具的方案
    LOGI("OverlayLoader_start begin");
    bool javaOk = OverlayLoader_start();
    LOGI("OverlayLoader_start result=%d", (int)javaOk);

    if (javaOk)
    {
        useJava = true;
        LOGI("java overlay mode: waiting for onSurfaceCreate from GLSurfaceView");
        // on_init 需要在 GL 线程里执行（setupMenu 里调用），这里不需要做更多
        return nullptr;
    }

    // Java overlay 失败，回退到 Vulkan hook 方案
    LOGINT(useJava);
    LOGI("initVulkanHooks begin");
    initVulkanHooks();
    LOGI("initVulkanHooks done");
    if (!useJava)
    {
        LOGI("initModMenu begin");
        initModMenu((void *)draw_thread, (void *)on_init);
        LOGI("initModMenu returned");
    }
    else
        LOGI("java mode: waiting for onSurfaceCreate");
    return nullptr;
}

extern int glWidth;
extern int glHeight;

extern "C"
{
    JNIEXPORT void JNICALL Java_imgui_il2cpp_tool_NativeMethods_onDrawFrame(JNIEnv *env, jclass clazz)
    {
        static std::atomic<uint64_t> drawCallCount{0};
        static auto lastDrawLog = std::chrono::steady_clock::now();
        uint64_t dc = drawCallCount.fetch_add(1);
        if (dc == 0)
            LOGI("onDrawFrame: FIRST CALL glWidth=%d glHeight=%d", glWidth, glHeight);
        auto now = std::chrono::steady_clock::now();
        if (dc % 300 == 0 || std::chrono::duration_cast<std::chrono::seconds>(now - lastDrawLog).count() >= 5)
        {
            lastDrawLog = now;
            LOGI("onDrawFrame: heartbeat call=%llu glSize=%dx%d", (unsigned long long)dc, glWidth, glHeight);
        }
        internalDrawMenu(glWidth, glHeight);
    }

    JNIEXPORT void JNICALL Java_imgui_il2cpp_tool_NativeMethods_onSurfaceChanged(JNIEnv *env, jclass clazz, jint width,
                                                                                 jint height)
    {
        LOGD(__FUNCTION__);
        glWidth = width;
        glHeight = height;
        LOGD("glWidth = %d, glHeight = %d", glWidth, glHeight);
        setupMenu();
    }

    JNIEXPORT void JNICALL Java_imgui_il2cpp_tool_NativeMethods_onSurfaceCreate(JNIEnv *env, jclass clazz)
    {
        LOGD(__FUNCTION__);
        initModMenu((void *)draw_thread, (void *)on_init, useJava);
    }

    // OverlayHelper 调用的 touch 事件入口
    JNIEXPORT void JNICALL Java_imgui_il2cpp_tool_OverlayHelper_nativeOnTouch(
        JNIEnv *env, jclass clazz, jint action, jfloat x, jfloat y, jint pointerId, jlong eventTime)
    {
        // touch 事件统计：每秒汇总一次，诊断输入是否在工作
        static std::atomic<uint64_t> touchCount{0};
        static std::atomic<uint64_t> downCount{0};
        static std::atomic<uint64_t> moveCount{0};
        static std::atomic<uint64_t> upCount{0};
        static auto lastTouchLog = std::chrono::steady_clock::now();
        uint64_t tc = touchCount.fetch_add(1);
        switch (action & 0xFF)
        {
        case 0: downCount.fetch_add(1); break;
        case 2: moveCount.fetch_add(1); break;
        case 1: case 3: upCount.fetch_add(1); break;
        }
        auto now = std::chrono::steady_clock::now();
        if (tc == 0 || std::chrono::duration_cast<std::chrono::seconds>(now - lastTouchLog).count() >= 1)
        {
            lastTouchLog = now;
            LOGI("nativeOnTouch: total=%llu down=%llu move=%llu up=%llu (action=%d x=%.1f y=%.1f)",
                 (unsigned long long)tc, (unsigned long long)downCount.load(),
                 (unsigned long long)moveCount.load(), (unsigned long long)upCount.load(),
                 action, x, y);
        }
        ImGuiIO &io = ImGui::GetIO();
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        switch (action & 0xFF)
        {
        case 0: // ACTION_DOWN
            io.AddMousePosEvent(x, y);
            io.AddMouseButtonEvent(0, true);
            break;
        case 2: // ACTION_MOVE
            io.AddMousePosEvent(x, y);
            break;
        case 1: // ACTION_UP
        case 3: // ACTION_CANCEL
            io.AddMouseButtonEvent(0, false);
            io.AddMousePosEvent(-1e30f, -1e30f);
            break;
        }
    }

    // GlViewWrapper surface 销毁时调用：标记 GL 上下文丢失，下次 onSurfaceCreate 时重新初始化 OpenGL3 backend
    JNIEXPORT void JNICALL Java_imgui_il2cpp_tool_OverlayHelper_nativeOnSurfaceDestroyed(JNIEnv *env, jclass clazz)
    {
        LOGI("nativeOnSurfaceDestroyed: JNI called from GlViewWrapper surfaceDestroyed callback");
        markGlContextLost();
    }
}

// APK / in-process inject path. Zygisk starts hack_thread from postAppSpecialize
// instead; the atomic in hack_thread de-dupes the two.
__attribute__((constructor)) void lib_main()
{
    // 关键诊断点：so 被进程 dlopen 后立即执行，能区分“模块没加载”与“加载了但没进目标名单”
    LOGI("constructor: module lib loaded");
    // If a Zygisk allow-list exists and this process is not on it, skip.
    if (Targets::ConfigExists())
    {
        auto pkg = Targets::CurrentProcessPackage();
        LOGI("constructor: config exists, pkg='%s'", pkg.c_str());
        if (!Targets::Contains(pkg))
        {
            LOGI("constructor: skip non-target %s", pkg.c_str());
            return;
        }
        LOGI("constructor: pkg is a target, starting hack_thread");
    }
    else
    {
        LOGI("constructor: no allow-list, injecting unconditionally");
    }
    pthread_t ptid;
    if (pthread_create(&ptid, nullptr, hack_thread, (void *)(intptr_t)-1) != 0)
        LOGI("constructor: pthread_create failed");
    else
        LOGI("constructor: hack_thread created");
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, [[maybe_unused]] void *reserved)
{
    JNIEnv *env;
    vm->GetEnv((void **)&env, JNI_VERSION_1_6);
    if (env->FindClass("imgui/il2cpp/tool/NativeMethods") == nullptr)
    {
        LOGE("Could not find class imgui/il2cpp/tool/NativeMethods");
        env->ExceptionClear();
    }
    else
    {
        useJava = true;
    }
    LOGI("JNI_OnLoad: useJava=%d", (int)useJava);

    return JNI_VERSION_1_6;
}
