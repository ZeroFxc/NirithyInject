#pragma once
#include <string>
#include <vector>

// Zygisk / KernelSU target package allow-list.
// WebUI writes `/data/adb/il2cpp_tool/targets.txt` (one package per line).
// The Zygisk module reads it in preAppSpecialize (unsandboxed).
namespace Targets
{
    constexpr const char *kConfigPath = "/data/adb/il2cpp_tool/targets.txt";
    constexpr const char *kModuleDir = "/data/adb/modules/il2cpp_tool";

    std::vector<std::string> Load();
    bool Contains(const std::string &package);
    bool ConfigExists();
    std::string CurrentProcessPackage();
} // namespace Targets
