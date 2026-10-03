# Tasks

- [x] Task 1: 修复 32 位输入钩子 ABI 错位
  - [x] SubTask 1.1: 将 `Menu/ImGui.cpp` 中 `HOOKINPUT(int32_t, Consume, void*, void*, bool, long, uint32_t*, AInputEvent**)` 的 `long arg3` 改为 `int64_t`（`nsecs_t`）
  - [x] SubTask 1.2: 复核同文件 `Input` 钩子（`initializeMotionEvent`）与 `android::InputConsumer` 相关方法签名一致
  - [x] SubTask 1.3: 分别编译 arm64-v8a 与 armeabi-v7a 的 Debug，确认无错误

- [x] Task 2: 消除交换缓冲同帧双重绘制
  - [x] SubTask 2.1: 加线程局部重入保护（`static thread_local bool g_inSwap`），两个交换缓冲钩子在嵌套调用链中只绘制一次（实现采用重入保护，非帧序号/时间戳）
  - [x] SubTask 2.2: 确认只命中单个交换缓冲入口时仍能正常绘制（不影响原有路径）
  - [x] SubTask 2.3: 编译验证（arm64-v8a 与 armeabi-v7a）

- [x] Task 3: WebUI 新增注入诊断视图
  - [x] SubTask 3.1: 在 `webui/src/main.ts` 新增读取 `/data/adb/il2cpp_tool/inject.log` 的视图与刷新按钮（经全局 `ksu.exec`）
  - [x] SubTask 3.2: 处理 `inject.log` 不存在、`ksu` 不可用等情况，给出明确提示
  - [x] SubTask 3.3: `npm run build` 产出单文件并同步到 `zygisk-module/webroot/index.html`，再跑 `:app:assembleZygiskModule`

- [x] Task 4: 新增模块卸载清理脚本
  - [x] SubTask 4.1: 新增 `zygisk-module/uninstall.sh`，删除 `/data/adb/il2cpp_tool`
  - [x] SubTask 4.2: 确认该脚本被打进 zygisk 模块 zip

- [x] Task 5: 补齐 zh-TW 关键文案
  - [x] SubTask 5.1: 在 `Tool/Lang.cpp` 的 `kDict` 中为当前经 `Lang::T(中文, English)` 输出的中文键补齐 `zh_tw` 条目
  - [x] SubTask 5.2: 编译验证

# Task Dependencies
- Task 2 依赖 Task 1（同处 `Menu/ImGui.cpp`，串行修改以免冲突）
- Task 3、Task 4、Task 5 相互独立，可并行执行
- 所有 Task 完成后统一跑一次 `:app:assembleDebug` 与 `:app:assembleZygiskModule` 作为收口验证