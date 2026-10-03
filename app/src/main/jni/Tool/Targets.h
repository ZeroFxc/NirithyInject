#pragma once
#include <string>
#include <vector>

// Zygisk / KernelSU target package allow-list.
// WebUI writes `/data/adb/il2cpp_tool/targets.txt` (one package per line).
// The Zygisk companion (root) reads it in preAppSpecialize — zygote cannot
// reliably open /data/adb due to SELinux.
namespace Targets
{
    constexpr const char *kConfigPath = "/data/adb/il2cpp_tool/targets.txt";
    constexpr const char *kModuleDir = "/data/adb/modules/il2cpp_tool";
    constexpr const char *kInjectLog = "/data/adb/il2cpp_tool/inject.log";

    std::vector<std::string> Load();
    bool Contains(const std::string &package);
    bool ConfigExists();
    std::string CurrentProcessPackage();
    std::string NormalizePackage(std::string pkg);
    bool Matches(const std::string &package, const std::vector<std::string> &list);
    std::string ParsePackageFromDataDir(const std::string &dir);
    bool IsZygoteProcess();
    void AppendInjectLog(const char *line);
} // namespace Targets
