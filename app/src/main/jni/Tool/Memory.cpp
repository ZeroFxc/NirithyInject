#include "Memory.h"
#include "Includes/Logger.h"
#include "Tool/Keyboard.h"
#include "Tool/Lang.h"
#include "imgui/imgui.h"
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

namespace Memory
{
    namespace
    {
        // 每行展示的字节数
        constexpr size_t kBytesPerLine = 16;
        // 单次最多查看的字节数
        constexpr size_t kMaxDumpSize = 256;

        // 已加载的内存数据与状态
        uintptr_t g_address = 0;
        size_t g_dumpSize = 0;
        bool g_loaded = false;
        uint8_t g_buffer[kMaxDumpSize]{0};
        // 地址输入框当前文本（保留以便重新编辑）
        char g_addressInput[64]{0};

        // 读取指定地址的内存到缓冲区
        // address: 起始地址；size: 读取长度
        // 返回 true 表示读取成功；地址未映射或不可访问时返回 false（不会崩溃）
        bool LoadAddress(uintptr_t address, size_t size)
        {
            if (!address || size == 0)
                return false;

            const long pageSize = sysconf(_SC_PAGESIZE);
            if (pageSize <= 0)
                return false;

            // 目标内存可能没有读权限，先为覆盖范围补上 PROT_READ；
            // 若地址根本未映射，mprotect 会失败，从而避免直接读取导致段错误
            const uintptr_t pageStart = address & ~(uintptr_t)(pageSize - 1);
            const size_t span = (address - pageStart) + size;
            if (mprotect(reinterpret_cast<void *>(pageStart), span, PROT_READ) != 0)
            {
                LOGE("mprotect failed for %p: %s", reinterpret_cast<void *>(address), strerror(errno));
                return false;
            }

            memcpy(g_buffer, reinterpret_cast<void *>(address), size);
            g_address = address;
            g_dumpSize = size;
            g_loaded = true;
            return true;
        }
    } // namespace

    void Draw()
    {
        ImGui::TextWrapped("%s", Lang::T("输入地址后读取内存（最多 256 字节）",
                                         "Enter an address to read memory (up to 256 bytes)"));

        if (ImGui::Button(Lang::T("输入地址", "Enter address")))
        {
            Keyboard::Open(
                g_addressInput,
                [](const std::string &text)
                {
                    snprintf(g_addressInput, sizeof(g_addressInput), "%s", text.c_str());
                    if (text.empty())
                        return;

                    uintptr_t address = 0;
                    try
                    {
                        address = std::stoull(text, nullptr, 0);
                    }
                    catch (...)
                    {
                        LOGE("Invalid memory address: %s", text.c_str());
                        return;
                    }
                    if (!LoadAddress(address, kMaxDumpSize))
                        LOGE("Failed to read memory at %s", text.c_str());
                });
        }
        if (g_addressInput[0])
        {
            ImGui::SameLine();
            ImGui::Text("%s", g_addressInput);
        }

        if (!g_loaded)
            return;

        ImGui::SameLine();
        if (ImGui::Button(Lang::T("刷新", "Refresh")))
        {
            if (!LoadAddress(g_address, g_dumpSize))
                LOGE("Failed to refresh memory at %p", reinterpret_cast<void *>(g_address));
        }
        ImGui::TextDisabled("%s %zu", Lang::T("长度 :", "Length :"), g_dumpSize);
        ImGui::Separator();

        char line[128];
        for (size_t offset = 0; offset < g_dumpSize; offset += kBytesPerLine)
        {
            int pos = snprintf(line, sizeof(line), "%p  ", reinterpret_cast<void *>(g_address + offset));
            for (size_t i = 0; i < kBytesPerLine; i++)
            {
                if (offset + i < g_dumpSize)
                    pos += snprintf(line + pos, sizeof(line) - pos, "%02X ", g_buffer[offset + i]);
                else
                    pos += snprintf(line + pos, sizeof(line) - pos, "   ");
            }
            line[pos++] = ' ';
            for (size_t i = 0; i < kBytesPerLine && offset + i < g_dumpSize; i++)
            {
                const uint8_t c = g_buffer[offset + i];
                line[pos++] = (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '.';
            }
            line[pos] = '\0';
            ImGui::TextUnformatted(line);
        }
    }
} // namespace Memory