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
        // --- 主窗口 / 设置 ---
        {"main.fullscreen", "全屏", "Fullscreen", "全螢幕"},
        {"main.tracedCount", "已跟踪方法数 :", "Traced method count :", "已追蹤方法數 :"},
        {"main.quickRestore", "快速恢复", "Quick Restore", "快速復原"},
        {"main.noTracedCall", "还没有被调用的跟踪方法", "No traced method has been called yet", "還沒有被呼叫的追蹤方法"},
        {"main.display", "显示", "Display", "顯示"},
        {"main.tabs", "选项卡:", "Tabs:", "分頁:"},
        {"main.twoFingerScroll", "双指滚动", "Two-finger scroll", "雙指捲動"},
        {"main.touchPassthrough", "触摸穿透", "Touch Passthrough", "觸控穿透"},
        {"main.touchPassthroughHint", "临时方案：触摸事件未传递到游戏时启用", "Temporary fix if touches are not reaching the game", "臨時方案：觸控事件未傳遞到遊戲時啟用"},
        {"main.saferInspector", "更安全的检查器（较慢）", "Safer Inspector (slower)", "更安全的檢查器（較慢）"},
        {"main.sideSplit", "分栏布局", "Side Split", "分欄佈局"},
        {"main.scale", "缩放##Global", "Scale##Global", "縮放##Global"},
        {"main.theme", "主题##Global", "Theme##Global", "主題##Global"},
        {"main.maxFps", "最大帧率", "Max FPS", "最大幀率"},
        {"main.limitFpsHint", "限制菜单帧率会同时限制游戏帧率", "Limiting menu FPS also limits game FPS", "限制選單幀率會同時限制遊戲幀率"},
        {"main.keyboardFallback", "键盘收不到输入时启用备用方案", "Enable if keyboard input is not being received", "鍵盤收不到輸入時啟用備用方案"},
        {"main.resetImageCache", "重置图片缓存", "Reset image cache", "重設圖片快取"},
        {"main.viewport", "视口", "Viewport", "視口"},
        {"main.resetWindow", "重置窗口", "Reset Window", "重設視窗"},
        {"main.center", "居中", "Center", "置中"},
        {"main.topLeft", "左上", "Top-Left", "左上"},
        {"main.topRight", "右上", "Top-Right", "右上"},
        {"main.bottomLeft", "左下", "Bottom-Left", "左下"},
        {"main.bottomRight", "右下", "Bottom-Right", "右下"},
        {"main.package", "包名", "Package", "套件名稱"},
        {"main.version", "版本", "Version", "版本"},
        {"main.renderer", "渲染", "Renderer", "渲染器"},
        // --- 工具选项卡 ---
        {"tool.autoDump", "自动转储", "Auto dump", "自動轉儲"},
        {"tool.selectAssemblies", "选择要转储的程序集（可多选）", "Select assemblies to dump (multi-select)", "選擇要轉儲的組件（可多選）"},
        {"tool.selectAll", "全选", "Select all", "全選"},
        {"tool.clearSelection", "清空", "Clear", "清空"},
        {"tool.selected", "已选:", "Selected:", "已選:"},
        {"tool.dumpAllHint", "未选择程序集，将转储全部", "No assembly selected, dumping all", "未選擇組件，將轉儲全部"},
        {"tool.dumping", "正在转储", "Dumping", "正在轉儲"},
        {"tool.dumpBtn", "转储", "DUMP", "轉儲"},
        {"tool.dumpInProgress", "转储进行中或已完成！", "Dump in progress or finished!", "轉儲進行中或已完成！"},
        {"tool.dumpDone", "完成", "Done", "完成"},
        {"tool.copyPath", "复制路径", "Copy path", "複製路徑"},
        {"tool.outputPath", "输出文件路径", "Output file path", "輸出檔案路徑"},
        // --- 内存选项卡 ---
        {"memory.hint", "输入地址后读取内存（最多 256 字节）", "Enter an address to read memory (up to 256 bytes)", "輸入位址後讀取記憶體（最多 256 位元組）"},
        {"memory.enterAddress", "输入地址", "Enter address", "輸入位址"},
        {"memory.tryLoad", "尝试加载", "Try load", "嘗試載入"},
        {"common.refresh", "刷新", "Refresh", "重新整理"},
        {"memory.length", "长度 :", "Length :", "長度 :"},
        // --- 软键盘 ---
        {"keyboard.title", "键盘输入", "Keyboard Input", "鍵盤輸入"},
        {"keyboard.opened", "系统键盘已打开...", "System keyboard opened...", "系統鍵盤已開啟..."},
        {"keyboard.cancel", "取消", "Cancel", "取消"},
        // --- 类 / 检查器选项卡 ---
        {"cls.copyAddress", "复制地址", "Copy address", "複製位址"},
        {"cls.invalidClass", "无效类", "Invalid class", "無效類別"},
        {"cls.valueTypeSkip", "值类型/枚举不能作为 GC 对象收集，已跳过以免崩溃。", "Value types / enums cannot be collected as GC objects (skipped to avoid a crash).", "值類型/列舉不能作為 GC 物件收集，已略過以免當機。"},
        {"cls.enterObjectAddress", "输入对象地址", "Enter object address", "輸入物件位址"},
        {"cls.findObjects", "查找对象", "Find Objects", "尋找物件"},
        {"cls.createZeroInit", "创建零初始化实例", "Create Zero-Init Instance", "建立零初始化實例"},
        {"cls.callCompletedVoid", "调用完成 (void)", "call completed (void)", "呼叫完成 (void)"},
        {"cls.recall", "再次调用", "Recall", "再次呼叫"},
        {"cls.callResults", "调用结果:", "Call Results:", "呼叫結果:"},
        {"cls.call", "调用", "Call", "呼叫"},
        {"cls.patchReturnValue", "修改返回值", "Patch return value", "修改返回值"},
        {"cls.nop", "NOP", "NOP", "NOP"},
        {"cls.restore", "恢复", "Restore", "恢復"},
        {"cls.notSupported", "不支持！", "Not supported!", "不支援！"},
        {"cls.changeReturnValue", "修改返回值", "Change return value", "修改返回值"},
        {"obj.scanning", "扫描中...", "Scanning...", "掃描中..."},
        {"obj.nothing", "无...", "Nothing...", "無..."},
        {"obj.showing100", "显示 %zu 个对象中的 100 个", "Showing 100 of %zu objects", "顯示 %zu 個物件中的 100 個"},
        {"obj.remove", "移除", "Remove", "移除"},
        {"obj.new", "新建", "New", "新建"},
        {"cls.copyPatched", "复制已修补字节", "Copy patched bytes", "複製已修補位元組"},
        {"cls.manualHex", "手动十六进制 :", "Manual hex :", "手動十六進位 :"},
        {"cls.enterHexApply", "输入十六进制并应用", "Enter hex and apply", "輸入十六進位並套用"},
        {"cls.redirect", "重定向跳转 :", "Redirect to :", "重新導向跳轉 :"},
        {"cls.enterTargetApply", "输入目标地址并应用", "Enter target and apply", "輸入目標位址並套用"},
        {"cls.overrideArgs", "修改调用参数 :", "Override call arguments :", "修改呼叫參數 :"},
        {"cls.notOverridden", "未修改", "not overridden", "未修改"},
        {"cls.clear", "清除", "Clear", "清除"},
        {"cls.methodNotCalled", "方法尚未被调用", "Method has not been called", "方法尚未被呼叫"},
        {"cls.backtraced", "回溯方法 :", "Backtraced methods :", "回溯方法 :"},
        {"cls.class", "类", "Class", "類別"},
        {"cls.return", "返回", "Return", "傳回"},
        {"cls.kind", "种类", "Kind", "種類"},
        {"cls.parameters", "参数", "Parameters", "參數"},
        {"cls.inspectObjects", "检查对象", "Inspect Objects", "檢查物件"},
        {"cls.showMethods", "显示方法", "Show Methods", "顯示方法"},
        {"cls.searchBox", "搜索类或方法（:方法名）", "Search class or method (:name)", "搜尋類別或方法（:方法名）"},
        {"cls.filterHint", "支持 :方法名 直接按方法筛选", "Prefix with ':' to filter by method name", "支援 :方法名 直接依方法篩選"},
        {"cls.caseSensitive", "区分大小写", "Case-Sensitive", "區分大小寫"},
        {"cls.exactMatch", "精确匹配", "Exact match", "精確匹配"},
        {"cls.filterBy", "按以下筛选", "Filter by ", "依下列篩選"},
        {"cls.method", "方法", "Method", "方法"},
        {"cls.field", "字段", "Field", "欄位"},
        {"cls.methodFilterBy", "方法筛选维度", "Method filter by", "方法篩選維度"},
        {"cls.name", "名称", "Name", "名稱"},
        {"cls.returnType", "返回类型", "Return type", "傳回類型"},
        {"cls.showAllClasses", "显示全部类", "Show All Classes", "顯示全部類別"},
        {"cls.selectClass", "从左侧选择一个类", "Select a class on the left", "從左側選擇一個類別"},
        {"cls.objectInvalid", "对象无效或已被回收（旧版 Unity 上更常见）", "Object is invalid or was collected (more common on old Unity).", "物件無效或已被回收（舊版 Unity 上更常見）"},
        {"cls.alwaysRefresh", "始终刷新", "Always refresh", "永遠重新整理"},
        {"cls.staticFields", "静态字段", "Static Fields", "靜態欄位"},
        {"cls.edit", "改", "Edit", "編輯"},
        {"cls.readOnly", "只读", "read-only", "唯讀"},
        {"cls.collectedFields", "已收集对象字段", "Collected object fields", "已收集物件欄位"},
        // --- 脚本选项卡 ---
        {"script.notInitialized", "脚本引擎未初始化", "Script engine is not initialized", "腳本引擎未初始化"},
        {"script.run", "执行", "Run", "執行"},
        {"script.runCompleted", "[脚本] 执行完成", "[script] run completed", "[腳本] 執行完成"},
        {"script.editViaKeyboard", "软键盘编辑", "Edit via keyboard", "以軟體鍵盤編輯"},
        {"script.clearScript", "清空脚本", "Clear script", "清空腳本"},
        {"script.clearLog", "清空日志", "Clear log", "清空日誌"},
        {"script.log", "日志", "Log", "日誌"},
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
