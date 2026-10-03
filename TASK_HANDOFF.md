# IL2CPP Tool 任务交接清单

## 一、项目与工作流规则

- **项目路径**：`E:\il2CppTool\.claude\worktrees\silly-lamarr-d8612e`
- **当前分支**：`claude/silly-lamarr-d8612e`
- **规则**：
  - 一步一提交（每个功能一个 git commit）
  - 提交信息中文 + 末尾附 `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`
  - 每步提交前必须编译验证：`ANDROID_HOME="$LOCALAPPDATA/Android/Sdk" ./gradlew.bat :app:assembleDebug --console=plain`
  - 提交前检查 `git status` / `git diff`，防止文件串步
- **重要提示**：新 agent 开工第一步先运行 `git status --short` 和 `git log --oneline -15`，确认工作区干净、HEAD 为 `b716f84`（第 18 步）。`showMethods` 已落地并提交，无需再处理。

## 二、已完成提交（19 个）

```
b716f84 feat: 调用结果改为结构化面板
3e785af feat: 调用结果支持一键重复调用
7345a5f feat: 支持创建无构造函数的结构体
799bdfb feat: 扩展调用参数类型支持
fb3a129 feat: 补全方法状态指示图标
11e2e6c feat: 方法视图添加显示开关
4129287 feat: 冒号前缀按方法名筛选
fadd289 feat: 窗口位置记忆与浮动控制
a43b31a feat: 类窗口支持左右分栏布局
6120948 feat: 类窗口标题显示类名
661e147 feat: 输入弹窗支持拖动
f3ecfaa feat: 新增更安全的检查器选项
536f325 feat: 新增视口位置操作按钮
f24ed61 feat: 新增触摸穿透选项
8003cb7 feat: 新增双指滚动设置
6f150d5 feat: 主选项卡显示/隐藏开关
10e13d2 feat: 新增最大帧率限制设置
9cbe9d7 feat: 新增主题样式切换设置
3ce61d3 fix: 修复项目基线编译错误（NDK30 兼容）
```

## 三、剩余任务列表（第 19–38 步，共 20 步）

> C 阶段（13–18）已完成：showMethods 开关（随选项卡配置持久化）、[P]/[H] 状态图标、
> 参数装箱扩展（Int16/UInt16/SByte/Byte/Char/IntPtr/UIntPtr）、零初始化结构体
> （ClassesTab.cpp 静态辅助函数 `CreateZeroInitValueType`，参数与结构体 this 均适用）、
> doCall lambda + Recall 按钮、CallResult 结构化面板（ClassesTab.h 新增 `struct CallResult`）。

### C 阶段：方法视图与调用（已完成）

| 步 | 提交信息 | 文件 | 要点 |
|---|---|---|---|
| 13 | `feat: 方法视图添加显示开关` | ClassesTab.h / ClassesTab.cpp | `bool showMethods = true;`；`ClassViewer` 的 `StaticFieldViewer` 后加 checkbox，false 时跳过方法循环 |
| 14 | `feat: 补全方法状态指示图标` | ClassesTab.cpp | MethodViewer（1323-1361）：patched 加前缀 `[P]`、hooked 加 `[H]`，保留 Hit Count/Returns 文本与现有颜色 |
| 15 | `feat: 扩展调用参数类型支持` | ClassesTab.cpp | CallerView 装箱（706-778）补 Int16/UInt16/SByte/Byte/Char/IntPtr/UIntPtr |
| 16 | `feat: 支持创建无构造函数的结构体` | ClassesTab.cpp | 值类型参数用 `Il2cpp::GetBoxedValue(klass, &zero)` 建零初始化实例（Il2cpp.h:125，实现 Il2cpp.cpp:1308） |
| 17 | `feat: 调用结果支持一键重复调用` | ClassesTab.cpp | Call 逻辑抽成 `doCall()` lambda，加 Recall 按钮 |
| 18 | `feat: 调用结果改为结构化面板` | ClassesTab.h / ClassesTab.cpp | callResults 扩展为类型名+值+字段数；引用类型点击展开 ImGuiJson |
| 19 | `feat: 新增剪贴板后端` | 新增 `Tool/Clipboard.h/.cpp` + CMakeLists | `Clipboard::Set/Get`，主路径反射 `UnityEngine.GUIUtility.systemCopyBuffer`；**是 22/26 步前置** |
| 20 | `feat: 跟踪器支持修改调用参数` | Frida.cpp / ClassesTab.cpp / ClassesTab.h | `hookerArgMods` 存储；`TraceListener::on_enter` 用 `replace_nth_argument`（gumpp.hpp:85）；static 从 idx0，非 static 从 idx1 |

### D 阶段：Patcher

| 步 | 提交信息 | 文件 | 要点 |
|---|---|---|---|
| 21 | `feat: 补丁支持手动十六进制输入` | ClassesTab.cpp | HookReturnValuePopup 加 hex 输入框，解析后 ProtectAddr+memcpy，先备份到 `oMap[method].bytes` |
| 22 | `feat: 支持复制已修补字节` | ClassesTab.cpp | `oMap` 非空显示 hex + 复制按钮调 `Clipboard::Set`（依赖 19） |
| 23 | `feat: 支持方法重定向跳转` | Patcher.h/.cpp + ClassesTab.cpp | 新增 `redirect(void *target)`；aarch64 `b(imm)` 或 `ldr x16; br x16`；arm32 `b(imm)` |
| 24 | `fix: 修正布尔/枚举补丁返回值宽度` | ClassesTab.cpp | 枚举路径 `movInt16` 改 `movInt32`（1105-1128） |

### E 阶段：内存与对象

| 步 | 提交信息 | 文件 | 要点 |
|---|---|---|---|
| 25 | `feat: 支持手动输入对象地址` | ClassesTab.cpp | ImGuiObjectSelector 顶部加地址输入框，校验 klass 匹配 |
| 26 | `feat: 支持复制对象/方法地址` | ClassesTab.cpp | 地址旁加复制按钮调 `Clipboard::Set`（依赖 19） |
| 27 | `feat: 新增内存查看选项卡` | 新增 `Tool/Memory.h/.cpp` + CMakeLists + Main.cpp | "内存"Tab 默认隐藏（复用 tabShowMemory 开关）；hex dump 256 字节；`mprotect(PROT_READ)` 防段错误 |
| 28 | `feat: 内存地址手动输入并尝试加载` | Tool/Memory.cpp | "尝试加载"按钮读取输入地址 |

### F 阶段：转储与修复

| 步 | 提交信息 | 文件 | 要点 |
|---|---|---|---|
| 29 | `feat: 转储器支持自动转储` | Tool.cpp / Main.cpp | Dumper 加自动转储 checkbox（`ConfigGet<bool>("autoDump", false)`），进入页面自动启动 il2cpp_dump |
| 30 | `fix: 修复 Zygisk 目标文件读取时序与权限` | Targets.cpp / ZygiskModule.cpp | `Load()` 加 3 次重试 + usleep(50ms)，两处路径合并去重 |
| 31 | `fix: 修复 Waydroid/WSA 叠加层位置错误` | ImGui.cpp / MainActivity.java | EGL 尺寸与实际 DisplaySize 差异 >20% 时重设；Java 侧 densityDpi 修正 |
| 32 | `fix: 根据 API 等级补全崩溃防护` | get_device_api_level_inlines.h / Il2cpp.cpp / ImGui.cpp | `api_level()` 改 `static inline`（当前是普通函数定义，多 TU 会重复定义）；API<26 跳过 Vulkan |
| 33 | `feat: 放宽 il2cpp 符号解析为可选` | il2cpp-api-functions.h / Il2cpp.cpp | 新增 `DO_API_OPTIONAL` 宏，非核心符号失败不置 g_ready=false |
| 34 | `fix: 增加 il2cpp 初始化重试` | Il2cpp.cpp | openIl2cppHandle 失败每 1s 重试 3 次 |

### G 阶段：LuaJIT 脚本（最复杂）

| 步 | 提交信息 | 文件 | 要点 |
|---|---|---|---|
| 35 | `feat: 嵌入 LuaJIT 脚本引擎并提供 C API` | 新增 `jni/luajit/` + `Script/Script.h/.cpp` + CMakeLists | **构建策略按优先级**：① 本机 make+NDK 交叉编译 libluajit.a；② 预生成头文件+CMake 直接编静态库（复刻 Frida 先例）；③ 降级嵌入 Lua 5.4 源码（纯 C 无生成步骤，API 层不变）。注册 `il2cpp_call/log/config_get/set/read_mem/write_mem/toggle_hooker`，剔除 os.execute |
| 36 | `feat: 新增 Lua 脚本选项卡` | Main.cpp / Script.cpp | "脚本"Tab 默认隐藏；多行编辑框+执行按钮+日志区 |
| 37 | `feat: Lua 脚本支持注册钩子` | Script.cpp / Frida.cpp | `hook_method(klass, method, onEnterLuaFunc)`，on_enter 用 lua_pcall 回调 |

### H 阶段：WebUI

| 步 | 提交信息 | 文件 | 要点 |
|---|---|---|---|
| 38 | `feat: 新增最小 WebUI 管理目标包` | 新增 `zygisk-module/webroot/index.html` | 通过 KernelSU WebUI API 读写 `/data/adb/il2cpp_tool/targets.txt`，增删目标包 |

## 四、关键依赖与注意事项

- **依赖链**：第 19 步（剪贴板）→ 第 22、26 步；第 27/28 步（内存）→ 第 3 步的选项卡开关机制；第 35 步（LuaJIT）→ 第 36/37 步
- 新增 `.cpp` 的步骤（19/27/35/36）必须同步更新 `CMakeLists.txt` 的 `LOCAL_SRC_FILES`
- 每步提交前编译验证；涉及新源文件的步骤额外跑一次 `assembleRelease`（验证 `libTool_fix_input.so` 双构建）
- LuaJIT 若降级为 Lua 5.4，需在提交信息中注明
- **已建立的代码模式（后续步骤可复用）**：
  - `ClassesTab.h` 的 `struct CallResult`（typeName/value/fieldCount/object），`callResults` 现为 `CircularBuffer<CallResult>`
  - `ClassesTab.cpp` 静态辅助 `CreateZeroInitValueType(Il2CppClass*)`：按 `klass->getSize()` 清零后 `Il2cpp::GetBoxedValue`
  - CallerView 的调用逻辑已是 `doCall` lambda（Call / Recall 共用），第 20 步改参数可在其内部或 `on_enter` 处理
  - 值类型（结构体）不能走 `ImGuiObjectSelector`（会被早退跳过），需用上述零初始化辅助