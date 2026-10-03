#include "Targets.h"
#include "Includes/Logger.h"
#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

namespace Targets
{
    static std::string trim(const std::string &s)
    {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos)
            return "";
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    static std::vector<std::string> configPaths()
    {
        return {
            kConfigPath,
            std::string(kModuleDir) + "/targets.txt",
            "/data/adb/modules_update/il2cpp_tool/targets.txt",
        };
    }

    static bool readFile(const std::string &path, std::vector<std::string> &out)
    {
        std::ifstream in(path);
        if (!in.is_open())
            return false;

        std::string line;
        while (std::getline(in, line))
        {
            line = trim(line);
            if (line.empty() || line[0] == '#')
                continue;
            line = NormalizePackage(line);
            if (line.empty())
                continue;
            if (std::find(out.begin(), out.end(), line) == out.end())
                out.push_back(line);
        }
        return true;
    }

    std::string NormalizePackage(std::string pkg)
    {
        pkg = trim(pkg);
        auto colon = pkg.find(':');
        if (colon != std::string::npos)
            pkg.resize(colon);
        return pkg;
    }

    std::string ParsePackageFromDataDir(const std::string &dir)
    {
        if (dir.empty())
            return "";
        // /data/user/0/com.foo.bar  or  /data/data/com.foo.bar
        // /mnt/expand/.../user/0/com.foo.bar
        int user = 0;
        char package[256] = {0};
        if (sscanf(dir.c_str(), "/data/%*[^/]/%d/%255s", &user, package) == 2)
            return NormalizePackage(package);
        if (sscanf(dir.c_str(), "/data/%*[^/]/%255s", package) == 1)
            return NormalizePackage(package);
        auto pos = dir.rfind('/');
        if (pos == std::string::npos || pos + 1 >= dir.size())
            return NormalizePackage(dir);
        return NormalizePackage(dir.substr(pos + 1));
    }

    bool Matches(const std::string &package, const std::vector<std::string> &list)
    {
        auto pkg = NormalizePackage(package);
        if (pkg.empty())
            return false;
        for (auto t : list)
        {
            t = NormalizePackage(t);
            if (t.empty())
                continue;
            if (t == pkg)
                return true;
            // "com.foo" matches process "com.foo:unity" (already stripped) and
            // isolated "com.foo" prefixes only when equal after normalize.
        }
        return false;
    }

    std::vector<std::string> Load()
    {
        std::vector<std::string> out;
        for (int attempt = 0; attempt < 3; attempt++)
        {
            out.clear();
            for (const auto &path : configPaths())
                readFile(path, out);
            if (!out.empty())
                break;
            usleep(50 * 1000);
        }
        return out;
    }

    bool ConfigExists()
    {
        for (const auto &path : configPaths())
        {
            if (access(path.c_str(), R_OK) == 0)
                return true;
        }
        return false;
    }

    bool Contains(const std::string &package)
    {
        return Matches(package, Load());
    }

    std::string CurrentProcessPackage()
    {
        std::ifstream in("/proc/self/cmdline", std::ios::binary);
        if (!in.is_open())
            return "";
        std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (data.empty())
            return "";
        auto n = data.find('\0');
        if (n != std::string::npos)
            data.resize(n);
        return NormalizePackage(data);
    }

    bool IsZygoteProcess()
    {
        auto cmd = CurrentProcessPackage();
        if (cmd == "zygote" || cmd == "zygote64" || cmd == "zygote32")
            return true;
        if (cmd.rfind("app_process", 0) == 0)
            return true;
        return false;
    }

    // root companion 专用：把一行日志追加到 /data/adb/NirithyInject/<包名>/<日期>.log。
    // 每次 open/close 以保证多进程（多个 app 同时注入）追加时不互相覆盖。
    void AppendPackageLog(const std::string &package, const char *line)
    {
        if (package.empty() || !line)
            return;

        // 过滤包名中的路径分隔符等字符，避免目录穿越
        std::string safe;
        for (char c : package)
        {
            if (isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-')
                safe.push_back(c);
        }
        if (safe.empty())
            return;

        time_t now = time(nullptr);
        struct tm tm{};
        localtime_r(&now, &tm);
        char date[32];
        snprintf(date, sizeof(date), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);

        std::string dir = std::string("/data/adb/NirithyInject/") + safe;
        mkdir("/data/adb/NirithyInject", 0755);
        mkdir(dir.c_str(), 0755);
        std::string file = dir + "/" + date + ".log";

        int fd = open(file.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0)
            return;

        char buf[1152];
        int n = snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d %s\n",
                         tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                         tm.tm_hour, tm.tm_min, tm.tm_sec, line);
        if (n > 0)
            (void)write(fd, buf, (size_t)n);
        close(fd);
    }

    void AppendInjectLog(const char *line)
    {
        if (!line)
            return;
        mkdir("/data/adb/il2cpp_tool", 0755);
        int fd = open(kInjectLog, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0)
            return;
        char buf[512];
        time_t now = time(nullptr);
        struct tm tm{};
        localtime_r(&now, &tm);
        int n = snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d %s\n",
                         tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                         tm.tm_hour, tm.tm_min, tm.tm_sec, line);
        if (n > 0)
            (void)write(fd, buf, (size_t)n);
        close(fd);
    }
} // namespace Targets
