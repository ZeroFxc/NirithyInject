#include "Fonts.h"
#include "imgui/imgui.h"
#include "Includes/Roboto-Regular.h"
#include "Includes/Logger.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// 私有的 stb_truetype 副本，仅用于「字体预校验」
//
// ImGui 自带的 stb_truetype 是以 STBTT_STATIC 编译的（符号为 static，无法跨
// 编译单元链接），所以这里再编译一份只在本文件可见的副本。
//
// 崩溃根因：ImGui 在 ImFontAtlas::AddFont() 内部会【同步】调用 stbtt_InitFont()，
// 一旦字体解析失败就触发 IM_ASSERT("stbtt_InitFont(): failed to parse FontData")
// 并直接 abort 进程（AddFont 的返回值检查根本来不及生效）。
// 因此这里在把字体交给 ImGui 之前，用完全相同的 stbtt 调用先校验一遍：
//   - stbtt_InitFont 返回 0        -> 不可用
//   - 目标字形不存在               -> 不可用
//   - stbtt 内部断言不通过         -> 不可用（ImGui 的副本会因此 abort）
// 这样可保证：凡是交给 ImGui 的字体都不会再触发它的断言。
// ---------------------------------------------------------------------------
static bool g_stbAssertTripped = false;

#define STBTT_assert(x)                                                                                                \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(x))                                                                                                      \
            g_stbAssertTripped = true;                                                                                 \
    } while (0)
#define STBTT_malloc(x, u) ((void)(u), malloc(x))
#define STBTT_free(x, u) ((void)(u), free(x))
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wmissing-prototypes"
#endif
#include "imgui/imstb_truetype.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace
{
    constexpr float kFontSize = 40.0f;
    constexpr int kProbeCodepoint = 0x4E2D; // '中'：确认字体确实覆盖中文
    constexpr int kMaxTtcFaces = 16;        // 扫描 TTC 时最多探测的字面数
    constexpr size_t kMinScanBytes = 256u * 1024u;         // 过小必然不含 CJK
    constexpr size_t kMaxFontBytes = 128u * 1024u * 1024u; // 防御异常大文件
    constexpr int kMaxScanFiles = 64;

    struct Candidate
    {
        const char *path;
        int fontNo; // TTC 中的字面索引；非 TTC 恒为 0
    };

    // 各 OEM 的中文字体命名差异很大，按可能性排序，命中即止。
    // 同一 TTC 会依次尝试多个 face（NotoSansCJK 中 2 通常为简体中文）。
    const Candidate kCjkCandidates[] = {
        {"/system/fonts/NotoSansCJK-Regular.ttc", 2},
        {"/system/fonts/NotoSansCJK-Regular.ttc", 1},
        {"/system/fonts/NotoSansCJK-Regular.ttc", 0},
        {"/system/fonts/NotoSansCJK-Regular.ttc", 3},
        {"/system/fonts/NotoSansSC-Regular.otf", 0},
        {"/system/fonts/NotoSansCJKsc-Regular.otf", 0},
        {"/system/fonts/NotoSansSC-Regular.ttf", 0},
        {"/system/fonts/NotoSansHans-Regular.otf", 0},
        {"/system/fonts/SourceHanSansSC-Regular.otf", 0},
        {"/system/fonts/SourceHanSansCN-Regular.otf", 0},
        {"/system/fonts/DroidSansFallback.ttf", 0},
        {"/system/fonts/DroidSansChinese.ttf", 0},
        {"/system/fonts/OPPOSans-Regular.ttf", 0},
        {"/system/fonts/OPPOSans-Regular.otf", 0},
        {"/system/fonts/OnePlusSans-Regular.ttf", 0},
        {"/system/fonts/MiSans-Regular.ttf", 0},
        {"/system/fonts/MiSansVF.ttf", 0},
        {"/system/fonts/HarmonyOS_Sans_SC_Regular.ttf", 0},
        {"/system/fonts/HarmonyOS_Sans_SC.ttf", 0},
        {"/system/fonts/NotoSerifCJK-Regular.ttc", 2},
        {"/vendor/fonts/NotoSansCJK-Regular.ttc", 2},
    };

    struct MappedFont
    {
        const unsigned char *data = nullptr;
        size_t size = 0;
        int fd = -1;
    };

    // 已合并字体的映射必须常驻到进程结束：
    // ImGui 1.92.6+ 起 FontDataOwnedByAtlas=false 不再复制数据，只持有指针。
    MappedFont gMergedFont;

    // 只读映射字体文件。相比整份读入内存，stbtt/ImGui 只会触碰实际用到的表，
    // 既避免几十 MB 的无用拷贝，也避免首帧因读文件而卡顿。
    bool mapFile(const char *path, MappedFont &out)
    {
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return false;

        struct stat st{};
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || (size_t)st.st_size > kMaxFontBytes)
        {
            close(fd);
            return false;
        }

        void *p = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p == MAP_FAILED)
        {
            close(fd);
            return false;
        }

        out.data = (const unsigned char *)p;
        out.size = (size_t)st.st_size;
        out.fd = fd;
        return true;
    }

    void unmapFile(MappedFont &m)
    {
        if (m.data != nullptr)
            munmap((void *)m.data, m.size);
        if (m.fd >= 0)
            close(m.fd);
        m = MappedFont{};
    }

    // 用与 ImGui 图集构建完全相同的 stbtt 调用做预校验
    bool probeFont(const MappedFont &m, int fontNo)
    {
        int offset = stbtt_GetFontOffsetForIndex(m.data, fontNo);
        if (offset < 0)
            return false;

        stbtt_fontinfo info;
        g_stbAssertTripped = false;
        if (!stbtt_InitFont(&info, m.data, offset))
            return false;
        if (stbtt_FindGlyphIndex(&info, kProbeCodepoint) == 0)
            return false;
        if (g_stbAssertTripped)
            return false;
        return true;
    }

    // 把校验通过的字面合并进当前图集；成功即长期持有该映射
    bool mergeFont(ImGuiIO &io, MappedFont &m, int fontNo, const char *path)
    {
        ImFontConfig cfg{};
        cfg.MergeMode = true;
        cfg.PixelSnapH = true;
        cfg.OversampleH = 1;
        cfg.OversampleV = 1;
        cfg.FontDataOwnedByAtlas = false; // 数据由 mmap 持有，ImGui 不得释放
        cfg.FontNo = (ImU32)fontNo;
        std::snprintf(cfg.Name, sizeof(cfg.Name), "CJK");

        if (io.Fonts->AddFontFromMemoryTTF((void *)m.data, (int)m.size, kFontSize, &cfg) == nullptr)
            return false;

        gMergedFont = m;
        m = MappedFont{};
        LOGI("CJK font merged: %s face=%d bytes=%zu", path, fontNo, gMergedFont.size);
        return true;
    }

    // 兜底：扫描 /system/fonts，找任意「可解析且含中文字形」的字体
    bool scanSystemFonts(ImGuiIO &io)
    {
        DIR *dir = opendir("/system/fonts");
        if (dir == nullptr)
            return false;

        std::vector<std::string> paths;
        for (struct dirent *e = readdir(dir); e != nullptr && (int)paths.size() < kMaxScanFiles; e = readdir(dir))
        {
            std::string name = e->d_name;
            size_t dot = name.rfind('.');
            if (dot == std::string::npos)
                continue;
            std::string ext = name.substr(dot);
            if (ext != ".ttf" && ext != ".ttc" && ext != ".otf")
                continue;
            paths.push_back(std::string("/system/fonts/") + name);
        }
        closedir(dir);

        for (const std::string &path : paths)
        {
            MappedFont m;
            if (!mapFile(path.c_str(), m))
                continue;
            if (m.size < kMinScanBytes) // 体积过小不可能覆盖 CJK
            {
                unmapFile(m);
                continue;
            }
            for (int face = 0; face < kMaxTtcFaces; face++)
            {
                if (!probeFont(m, face))
                    continue;
                if (mergeFont(io, m, face, path.c_str()))
                    return true;
                break; // mergeFont 失败时映射已归调用方，交由下方统一释放
            }
            unmapFile(m); // mergeFont 成功时 m 已被置空，这里无副作用
        }
        return false;
    }
} // namespace

void loadOverlayFonts()
{
    ImGuiIO &io = ImGui::GetIO();

    // 拉丁基础字体：Roboto_Regular 是编译进 so 的静态数组，不可让 ImGui 释放
    ImFontConfig latin{};
    latin.FontDataOwnedByAtlas = false;
    std::snprintf(latin.Name, sizeof(latin.Name), "Roboto");
    io.Fonts->AddFontFromMemoryTTF((void *)Roboto_Regular, (int)sizeof(Roboto_Regular), kFontSize, &latin);

    // 1) 已知候选表：同一文件只映射一次，依次尝试多个 TTC face
    MappedFont cur;
    const char *curPath = nullptr;
    for (const Candidate &c : kCjkCandidates)
    {
        if (curPath == nullptr || std::strcmp(curPath, c.path) != 0)
        {
            unmapFile(cur);
            curPath = nullptr;
            if (!mapFile(c.path, cur))
                continue;
            curPath = c.path;
        }
        if (!probeFont(cur, c.fontNo))
            continue;
        if (mergeFont(io, cur, c.fontNo, c.path))
            return;
        curPath = nullptr; // 合并失败：映射已被释放，下一轮重新映射
    }
    unmapFile(cur);

    // 2) 兜底扫描
    if (scanSystemFonts(io))
        return;

    LOGI("No usable system CJK font found; CJK text will render as missing glyphs");
}