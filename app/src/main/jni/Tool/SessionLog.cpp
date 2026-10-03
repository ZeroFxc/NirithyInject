#include "SessionLog.h"

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace SessionLog
{
    static std::mutex g_mtx;
    static int g_chanFd = -1; // 与 root companion 之间的日志通道
    static int g_fileFd = -1; // 回退：应用私有目录日志文件

    // 循环写满 n 字节，避免短写丢日志
    static bool writeAll(int fd, const void *data, size_t n)
    {
        auto p = static_cast<const uint8_t *>(data);
        size_t off = 0;
        while (off < n)
        {
            ssize_t w = write(fd, p + off, n - off);
            if (w <= 0)
                return false;
            off += static_cast<size_t>(w);
        }
        return true;
    }

    // 逐级创建目录（等价于 mkdir -p）
    static void mkdirs(const std::string &path)
    {
        for (size_t i = 1; i < path.size(); i++)
        {
            if (path[i] == '/')
                mkdir(path.substr(0, i).c_str(), 0755);
        }
        mkdir(path.c_str(), 0755);
    }

    // 当天日期，格式 YYYY-MM-DD
    static std::string today()
    {
        time_t now = time(nullptr);
        struct tm tm{};
        localtime_r(&now, &tm);
        char b[32];
        snprintf(b, sizeof(b), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        return b;
    }

    void Init(int fd, const std::string &pkg)
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_chanFd = fd;
        if (g_chanFd >= 0)
            return; // 由 root companion 写入 /data/adb/NirithyInject/<包名>/<日期>.log
        if (pkg.empty())
            return;

        // 回退：应用私有目录（与 app 同 uid，可直接写）
        std::string dir = "/data/data/" + pkg + "/files/il2Cpp";
        mkdirs(dir);
        std::string file = dir + "/" + today() + ".log";
        g_fileFd = open(file.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    }

    void Append(const char *line)
    {
        if (!line)
            return;

        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_chanFd < 0 && g_fileFd < 0)
            return;

        std::string rec = line;
        if (rec.empty() || rec.back() != '\n')
            rec.push_back('\n');

        if (g_chanFd >= 0)
        {
            // 帧格式：uint32 长度 + 内容（与 companion_handler 的读取协议一致）
            uint32_t n = static_cast<uint32_t>(rec.size());
            if (!writeAll(g_chanFd, &n, sizeof(n)) || !writeAll(g_chanFd, rec.data(), n))
                g_chanFd = -1; // 通道断开后不再尝试，避免每次写日志都失败
        }
        else
        {
            (void)writeAll(g_fileFd, rec.data(), rec.size());
        }
    }

    bool Ready()
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        return g_chanFd >= 0 || g_fileFd >= 0;
    }
} // namespace SessionLog
