# Tasks

> 约定：每个 Task 完成后必须 `assembleDebug` 编译通过，并**单独 git 提交一次**（一步一 git）。

- [ ] Task 1: 锁定技术路线并记录布局基线（仅调研 + 文档）
  - [ ] SubTask 1.1: 核实 ImGui 停靠能力：`IMGUI_HAS_DOCK` 是否定义、`imgui.h` 是否声明 `DockSpace`
  - [ ] SubTask 1.2: 记录现有构造成本点：顶部 `BeginTabBar`、`sideSplit` 双列开关、方法卡片独立窗口
  - [ ] SubTask 1.3: 结论：不替换 ImGui 源，采用"预设锚点布局"路线（写入 spec.md）
  - 验证：本任务无功能代码；以 git 提交记录调研结论

- [ ] Task 2: 类/方法层级树就地展开
  - [ ] SubTask 2.1: 类节点改用可折叠树渲染，类名前带展开/折叠箭头（`OpenOnArrow`，避免整行点击误触）
  - [ ] SubTask 2.2: 方法作为类节点子项渲染，方法名前同样有箭头，可二级展开显示参数/返回类型
  - [ ] SubTask 2.3: 移除"点击类即新开独立窗口"的路径，改为就地展开
  - [ ] SubTask 2.4: 搜索过滤结果与树展开状态保持一致
  - 验证：`assembleDebug` 通过；真机点击箭头就地展开、无新窗口产生

- [ ] Task 3: 惯性平滑滚动
  - [ ] SubTask 3.1: 在触摸拖拽时记录滚动速度，松手后逐帧衰减调用 `SetScrollY`
  - [ ] SubTask 3.2: 覆盖类列表、方法列表、侧栏等主要列表；到边界时钳制不越界
  - [ ] SubTask 3.3: 保留 `ImGuiListClipper` 大数据量裁剪，不因惯性回退性能
  - 验证：`assembleDebug` 通过；真机滑动顺滑、松手后平滑减速

- [ ] Task 4: 展开项钉选为顶部选项卡标签（可收回）
  - [ ] SubTask 4.1: 类节点旁增加钉选按钮，点击后把该类作为标签加入顶部 TabBar（可重排）
  - [ ] SubTask 4.2: 再次点击取消钉选并从 TabBar 移除
  - [ ] SubTask 4.3: 抽出 `DrawClassTree(className, pinned)` 复用渲染逻辑，Tab 内容与树内展开共用
  - [ ] SubTask 4.4: 树中已钉选类显示钉选态高亮
  - 验证：`assembleDebug` 通过；真机钉选与取消可往返、标签内容正确

- [ ] Task 5: 标签卡脱离为浮动窗口与回归
  - [ ] SubTask 5.1: 标签卡增加"弹出"操作，改为独立窗口渲染并从 TabBar 移除
  - [ ] SubTask 5.2: 浮动窗口支持拖动与缩放，并提供"回归"操作重新加入 TabBar
  - [ ] SubTask 5.3: 记录浮动窗口位置/尺寸，随布局一起持久化
  - 验证：`assembleDebug` 通过；真机脱离/回归往返正常，窗口位置可保持

- [ ] Task 6: 预设锚点停靠布局
  - [ ] SubTask 6.1: 定义并实现 4 种布局：单列 / 双列（左列表右内容）/ 双列（左右内容）/ 三列
  - [ ] SubTask 6.2: 拖拽标签卡时检测最近锚点并高亮预览，离开取消
  - [ ] SubTask 6.3: 松手后切换布局并把目标放置到锚点区域
  - [ ] SubTask 6.4: 布局选择与锚点分配持久化，重开菜单恢复
  - 验证：`assembleDebug` 通过；真机拖到左/右锚点可成双列并正确落位

- [ ] Task 7: 层级视觉与文案收尾
  - [ ] SubTask 7.1: 增加缩进引导线、层级配色、hover/选中态区分
  - [ ] SubTask 7.2: 折叠/钉选/布局状态记忆（与现有 Config 机制对接）
  - [ ] SubTask 7.3: 新增 UI 文案改为多语言字典项（含 zh-TW）
  - 验证：`assembleDebug` 通过；真机观感与文案确认无误

# Task Dependencies
- Task 2 是 Task 4 / 5 / 6 的前置（无层级树则无展开项可钉选/脱离/停靠）
- Task 5 是 Task 6 的前置（锚点拖拽基于"可脱离为窗口"的机制）
- Task 3 与 Task 2 可并行（滚动与树渲染互不阻塞）
- Task 7 依赖 Task 2~6 全部完成
