#include "Targets.h"
#include "Includes/Logger.h"
#include <algorithm>
#include <fstream>
#include <unistd.h>

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

    // 目标列表的候选路径：WebUI 写入路径 + 模块目录回退路径
    static std::vector<std::string> configPaths()
    {
        return {kConfigPath, std::string(kModuleDir) + "/targets.txt"};
    }

    // 读取单个文件中的包名，按出现顺序追加并去重
    // path: 文件路径；out: 结果集合（同时作为去重依据）
    // 返回 true 表示文件成功打开
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
            if (std::find(out.begin(), out.end(), line) == out.end())
                out.push_back(line);
        }
        return true;
    }

    std::vector<std::string> Load()
    {
        std::vector<std::string> out;
        // 文件可能在模块挂载/WebUI 写入后稍晚才可见，最多重试 3 次；两处路径合并去重
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
        // 仅判断可读性：文件存在但无读权限时与不存在区分开，便于上层排查权限问题
        for (const auto &path : configPaths())
        {
            if (access(path.c_str(), R_OK) == 0)
                return true;
        }
        return false;
    }

    bool Contains(const std::string &package)
    {
        if (package.empty())
            return false;
        auto list = Load();
        for (auto &p : list)
        {
            if (p == package)
                return true;
        }
        return false;
    }

    std::string CurrentProcessPackage()
    {
        // cmdline is NUL-separated; first token is the process name / package.
        std::ifstream in("/proc/self/cmdline", std::ios::binary);
        if (!in.is_open())
            return "";
        std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (data.empty())
            return "";
        auto n = data.find('\0');
        if (n != std::string::npos)
            data.resize(n);
        // Strip trailing :process suffix (e.g. com.game:unity)
        auto colon = data.find(':');
        if (colon != std::string::npos)
            data.resize(colon);
        return data;
    }
} // namespace Targets