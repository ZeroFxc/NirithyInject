#pragma once
#include <string>

// 会话日志：把"注入成功之后"目标进程内的运行日志落盘，不再只依赖 logcat。
// 落盘位置优先为 root 侧目录 /data/adb/NirithyInject/<包名>/<日期>.log：
//   app 进程受 SELinux 限制写不了 /data/adb，因此日志经 preAppSpecialize 阶段建立的
//   root companion socket 通道发送，由 companion 进程写入该目录。
// 通道不可用（例如 APK 内进程内注入、或 companion 连接失败）时回退写入应用私有目录
//   /data/data/<包名>/files/il2Cpp/<日期>.log。
namespace SessionLog
{
    // 初始化会话日志。
    // fd  : 与 root companion 之间的日志通道 fd，-1 表示不可用
    // pkg : 当前进程包名，用于回退路径
    void Init(int fd, const std::string &pkg);

    // 追加一行日志（自动补换行），线程安全；未初始化或通道不可用时为空操作
    void Append(const char *line);

    // 日志通道是否可用（用于诊断输出）
    bool Ready();
} // namespace SessionLog
