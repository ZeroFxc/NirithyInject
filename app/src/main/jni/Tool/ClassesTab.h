#pragma once

#include "Il2cpp/il2cpp-class.h"
#include "Includes/circular_buffer.h"
#include "Tool/PopUpSelector.h"
#include <set>
struct ClassesTab
{
    using Object = Il2CppObject *;
    using Class = Il2CppClass *;
    using Json = nlohmann::ordered_json;
    using Paths = std::vector<std::string>;
    using DataPair = std::pair<std::pair<Il2CppObject *, Json>, Paths>;

    using MethodParamList = std::vector<std::pair<const char *, Il2CppType *>>;
    using MethodList = std::vector<std::pair<MethodInfo *, MethodParamList>>;
    using ClassMethodMap = std::unordered_map<Class, MethodList>;

    struct CallData
    {
        MethodInfo *method;
        Il2CppObject *thiz;
        Il2CppArray<Il2CppObject *> *params;
    };

    struct ParamValue
    {
        std::string value;
        Il2CppObject *object;
    };

    std::unordered_map<Object, DataPair> dataMap{};

    bool caseSensitive = true;
    bool exactMatch = false;
    bool filterByClass = true;
    bool filterByMethod = false;
    bool filterByField = false;
    bool filterMethodByName = true;
    bool filterMethodByReturn = false;
    bool filterMethodByParams = false;
    bool showAllClasses = false;
    bool includeAllImages = false;
    bool alwaysRefreshInspector = false;

    std::map<Il2CppObject *, bool> tabMap{};

    Il2CppImage *selectedImage = nullptr;

    std::vector<Class> classes{};
    std::vector<Class> filteredClasses{};
    std::vector<Il2CppClass *> tracer{};
    std::vector<MethodInfo *> tracedMethods;

    static std::unordered_map<Il2CppClass *, std::vector<Il2CppObject *>> objectMap;
    static std::unordered_map<Il2CppClass *, std::vector<Il2CppObject *>> newObjectMap;
    static std::unordered_map<Il2CppClass *, std::set<Il2CppObject *>> savedSet;
    // 调用结果：类型名 + 值 + 字段数（引用类型可点击展开 ImGuiJson）
    struct CallResult
    {
        std::string typeName;            // 返回类型名
        std::string value;               // 值的文本表示
        size_t fieldCount = 0;           // 返回对象的字段数，0 表示不可展开
        Il2CppObject *object = nullptr;  // 返回对象，可展开时为非空
    };

    // 跟踪器参数修改：方法 -> (参数下标 k -> 覆写信息)
    struct HookerArgMod
    {
        std::string text;                  // 用户输入的覆写文本（仅用于界面显示）
        void *value = nullptr;             // 解析后的原始 ABI 参数值，可直接替换寄存器
        Il2CppObject *keepAlive = nullptr; // 引用类型替换值的保活对象
    };
    static std::unordered_map<MethodInfo *, std::unordered_map<int, HookerArgMod>> hookerArgMods;

    std::unordered_map<MethodInfo *, std::unordered_map<std::string, ParamValue>> paramMap{};
    std::unordered_map<MethodInfo *, CircularBuffer<CallResult>> callResults{};

    MethodList &buildMethodMap(Il2CppClass *klass);

    ClassMethodMap methodMap{};
    ClassesTab();

    Paths &getJsonPaths(Il2CppObject *object);

    void setJsonObject(Il2CppObject *object);

    Json &getJsonObject(Il2CppObject *object);

    void ImGuiObjectSelector(int id, Il2CppClass *klass, const char *prefix,
                             std::function<void(Il2CppObject *)> onSelect, bool canNew = false);

    void CallerView(Il2CppClass *klass, MethodInfo *method, const MethodParamList &paramsInfo,
                    Il2CppObject *thiz = nullptr);

    bool isMethodHooked(MethodInfo *method);

    void PatcherView(Il2CppClass *klass, MethodInfo *method, const MethodParamList &paramsInfo,
                     Il2CppObject *thiz = nullptr);

    void HookerView(Il2CppClass *klass, MethodInfo *method, const MethodParamList &paramsInfo,
                    Il2CppObject *thiz = nullptr);

    const MethodParamList &getCachedParams(MethodInfo *method);

    struct OriginalMethodBytes
    {
        std::vector<uint8_t> bytes;
        std::string text;
    };
    static std::unordered_map<MethodInfo *, OriginalMethodBytes> oMap;
    static PopUpSelector poper; // still a prototype!
    bool MethodViewer(Il2CppClass *klass, MethodInfo *method, const MethodParamList &paramsInfo,
                      Il2CppObject *thiz = nullptr, bool includeInflated = false);

    struct MethodCard
    {
        ClassesTab *owner = nullptr;
        Il2CppClass *klass = nullptr;
        MethodInfo *method = nullptr;
        MethodParamList params;
        Il2CppObject *thiz = nullptr;
        bool open = false;
    };
    static MethodCard methodCard;
    static void DrawMethodDetailCard();

    static std::unordered_map<Il2CppClass *, bool> states;
    void ClassViewer(Il2CppClass *klass);
    void StaticFieldViewer(Il2CppClass *klass);
    void HookerCollectedFieldEditor(Il2CppClass *klass);

    std::string filter = "";
    std::string title = ""; // 标签页标题（类短名），空时回退到 filter
    int selectedImageIndex = -1;
    bool traceState = false;

    // Side Split 分栏：当前选中查看的类
    Il2CppClass *selectedClass = nullptr;

    // 方法视图显示开关（方法列表默认开启）
    bool showMethods = true;

    int maxProgress = 0;
    int progress = 0;
    void Draw(int index = -1, bool closeable = false);

    bool opened = true;
    bool currentlyOpened = false;
    bool setOpenedTab = false;
    void DrawTabMap();

    void ImGuiJson(Il2CppObject *object);

    void FilterClasses(const std::string &filter);
};

void to_json(nlohmann::ordered_json &j, const ClassesTab &p);
void from_json(const nlohmann::ordered_json &j, ClassesTab &p);
