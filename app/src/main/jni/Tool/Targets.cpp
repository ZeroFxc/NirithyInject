#include "Targets.h"
#include "Includes/Logger.h"
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

    std::vector<std::string> Load()
    {
        std::vector<std::string> out;
        std::ifstream in(kConfigPath);
        if (!in.is_open())
        {
            // Fallback next to the module (KernelSU webui may write here instead)
            in.open(std::string(kModuleDir) + "/targets.txt");
        }
        if (!in.is_open())
            return out;
        std::string line;
        while (std::getline(in, line))
        {
            line = trim(line);
            if (line.empty() || line[0] == '#')
                continue;
            out.push_back(line);
        }
        return out;
    }

    bool ConfigExists()
    {
        return access(kConfigPath, F_OK) == 0 || access((std::string(kModuleDir) + "/targets.txt").c_str(), F_OK) == 0;
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
