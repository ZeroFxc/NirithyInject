# 修复叠加层输入钩子并增强注入诊断 Spec

## Why
「KernelSU 加白后打开应用看不到注入窗口」这一核心问题仍未闭环，目前只能靠猜。原因有二：一是叠加层自身还存在两处已确认的底层缺陷（32 位输入钩子 ABI 错位、交换缓冲同帧双重绘制），二是 WebUI 看不到 `/data/adb/il2cpp_tool/inject.log`，注入到底有没有发生、走到哪一步无从判断。

## What Changes
- 修复 `InputConsumer::consume` 钩子在 32 位下的参数 ABI：`nsecs_t` 是 `int64_t`，当前误声明为 `long`（armeabi-v7a 上仅 32 位，导致后续参数整体错位）
- 消除 `eglSwapBuffers` 与 `eglSwapBuffersWithDamage`/`WithDamageEXT` 同时命中时的同帧双重绘制
- WebUI 新增「注入诊断」视图，读取并展示 `/data/adb/il2cpp_tool/inject.log`
- 新增模块卸载脚本，清理 `/data/adb/il2cpp_tool`，避免残留
- 补齐 zh-TW 关键文案，消除繁体回退为简体的现象

## Impact
- Affected specs: 叠加层渲染与输入、Zygisk 注入诊断、KernelSU WebUI、多语言
- Affected code:
  - `app/src/main/jni/Menu/ImGui.cpp`（钩子 ABI、绘制去重）
  - `webui/src/main.ts` + `zygisk-module/webroot/index.html`（诊断视图）
  - `zygisk-module/uninstall.sh`（新增）
  - `app/src/main/jni/Tool/Lang.cpp`（zh-TW 条目）

## ADDED Requirements

### Requirement: WebUI 注入诊断
系统 SHALL 在 KernelSU WebUI 提供读取 `/data/adb/il2cpp_tool/inject.log` 的视图，并支持手动刷新。

#### Scenario: 正常读取
- **WHEN** 用户打开诊断视图并点击刷新，且 `inject.log` 存在
- **THEN** 按行展示日志内容，长内容可滚动查看

#### Scenario: 文件不存在或环境不支持
- **WHEN** `inject.log` 不存在，或运行环境未注入全局 `ksu` 对象
- **THEN** 给出明确提示（如"暂无注入日志" / "需在 KernelSU WebUI 中打开"），不报错中断

### Requirement: 模块卸载清理
模块被卸载时 SHALL 清理其数据目录 `/data/adb/il2cpp_tool`。

#### Scenario: 卸载模块
- **WHEN** 用户卸载本模块
- **THEN** `uninstall.sh` 删除 `/data/adb/il2cpp_tool`（含 `targets.txt`、`inject.log`、配置），不留残留

## MODIFIED Requirements

### Requirement: 输入钩子 ABI 正确性
`consume` 钩子 SHALL 以 `int64_t` 声明 `nsecs_t` 参数，保证 32 位与 64 位下参数均不错位；其余输入钩子签名 SHALL 与 `android::InputConsumer` 的真实 ABI 一致。

#### Scenario: 32 位设备触摸交互
- **WHEN** 在 armeabi-v7a 设备上产生触摸事件
- **THEN** 参数不发生错位，叠加层的点击/拖动判定正常

### Requirement: 同帧单次绘制
叠加层 SHALL 保证每帧最多绘制一次，即使 `eglSwapBuffers` 与 `eglSwapBuffersWithDamage*` 被同时命中（驱动可能内部互相调用）。

#### Scenario: 两个交换缓冲符号同时命中
- **WHEN** 一帧内两个被勾取的交换缓冲入口都被调用
- **THEN** `internalDrawMenu` 每帧至多执行一次，视觉无重影、性能无额外开销

### Requirement: 多语言覆盖
所有用户可见文案 SHALL 在三种语言下均有对应值；繁体（zh-TW）下 SHALL NOT 回退为简体。

#### Scenario: 切换繁体
- **WHEN** 语言切换为 zh-TW
- **THEN** 诸如"最大帧率"等经 `Lang::T(中文, English)` 输出的文案显示为繁体

## REMOVED Requirements
无。