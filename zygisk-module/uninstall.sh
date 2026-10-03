#!/system/bin/sh
# 模块卸载回调：KernelSU 在卸载本模块前执行本脚本。
# 删除工具的数据目录（含 targets.txt 注入白名单、inject.log 注入日志及运行时配置），
# 避免卸载后残留。失败不应阻断卸载流程。
rm -rf /data/adb/il2cpp_tool