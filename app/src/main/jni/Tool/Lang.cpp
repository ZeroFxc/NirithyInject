#include "Lang.h"
#include <cstring>
#include <string>
#include <unordered_map>

namespace Lang
{
    struct Entry
    {
        const char *key;
        const char *zh_cn;
        const char *en;
        const char *zh_tw;
    };

    // Single table: add rows for new strings, add a column + Code enum for a new language.
    static const Entry kDict[] = {
        {"lang.name", "简体中文", "English", "繁體中文"},
        {"lang.label", "界面语言", "Language", "介面語言"},
        {"tab.tools", "工具", "Tools", "工具"},
        {"tab.tracer", "跟踪器", "Tracer", "追蹤器"},
        {"tab.dumper", "转储", "Dumper", "轉儲"},
        {"tab.script", "脚本", "Script", "腳本"},
        {"tab.memory", "内存", "Memory", "記憶體"},
        {"tab.settings", "设置", "Settings", "設定"},
        {"tab.call", "调用", "Call", "呼叫"},
        {"tab.patch", "补丁", "Patch", "補丁"},
        {"tab.trace", "追踪", "Trace", "追蹤"},
        {"tab.info", "信息", "Info", "資訊"},
        {"method.card", "方法详情", "Method", "方法詳情"},
        {"method.address", "方法地址", "Method address", "方法位址"},
        {"method.signature", "签名", "Signature", "簽章"},
        {"method.static", "静态", "static", "靜態"},
        {"method.instance", "实例", "instance", "實例"},
        {"method.open", "打开详情", "Details", "開啟詳情"},
        {"method.close", "关闭", "Close", "關閉"},
        {"theme.label", "主题", "Theme", "主題"},
        {"theme.dark", "暗色", "Dark", "暗色"},
        {"theme.light", "亮色", "Light", "亮色"},
        {"theme.teal", "青绿", "Teal", "青綠"},
        {"theme.md3dark", "MD3 暗色", "MD3 Dark", "MD3 暗色"},
        {"theme.md3light", "MD3 亮色", "MD3 Light", "MD3 亮色"},
        {"theme.amoled", "AMOLED", "AMOLED", "AMOLED"},
        {"theme.dracula", "Dracula", "Dracula", "Dracula"},
        {"theme.nord", "Nord", "Nord", "Nord"},
        {"il2cpp.error", "IL2CPP 错误", "IL2CPP Error", "IL2CPP 錯誤"},
        {"il2cpp.notready",
         "游戏可能未使用 IL2CPP，或 libil2cpp.so 被混淆/加密/重命名。菜单仍可显示，但检查器不可用。",
         "This game may not use IL2CPP, or libil2cpp.so is obfuscated/encrypted/renamed. The overlay still shows, but the inspector is disabled.",
         "遊戲可能未使用 IL2CPP，或 libil2cpp.so 被混淆/加密/重新命名。選單仍可顯示，但檢查器不可用。"},
    };

    static std::unordered_map<std::string, const Entry *> gByKey;
    static std::unordered_map<std::string, const Entry *> gByZh;
    static bool gReady = false;

    static void ensure()
    {
        if (gReady)
            return;
        gReady = true;
        for (const auto &e : kDict)
        {
            gByKey.emplace(e.key, &e);
            if (e.zh_cn && e.zh_cn[0])
                gByZh.emplace(e.zh_cn, &e);
        }
    }

    static const char *pick(const Entry *e)
    {
        if (!e)
            return "";
        switch (current)
        {
        case EN:
            return e->en ? e->en : e->zh_cn;
        case ZH_TW:
            return e->zh_tw ? e->zh_tw : e->zh_cn;
        case ZH_CN:
        default:
            return e->zh_cn ? e->zh_cn : e->en;
        }
    }

    const char *tr(const char *key)
    {
        if (!key)
            return "";
        ensure();
        auto it = gByKey.find(key);
        if (it == gByKey.end())
            return key;
        return pick(it->second);
    }

    const char *T(const char *zh, const char *en)
    {
        if (current == EN)
            return en ? en : (zh ? zh : "");
        ensure();
        if (zh)
        {
            auto it = gByZh.find(zh);
            if (it != gByZh.end())
                return pick(it->second);
        }
        return zh ? zh : (en ? en : "");
    }

    const char *codeName()
    {
        switch (current)
        {
        case EN:
            return "en";
        case ZH_TW:
            return "zh-TW";
        default:
            return "zh-CN";
        }
    }

    const char *displayName(int lang)
    {
        switch (lang)
        {
        case EN:
            return "English";
        case ZH_TW:
            return "繁體中文";
        default:
            return "简体中文";
        }
    }

    bool Combo()
    {
        const char *items[] = {displayName(ZH_CN), displayName(EN), displayName(ZH_TW)};
        int prev = current;
        ImGui::Combo(tr("lang.label"), &current, items, kCount);
        if (current < 0 || current >= kCount)
            current = ZH_CN;
        return prev != current;
    }
} // namespace Lang
