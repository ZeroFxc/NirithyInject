#include "Fonts.h"
#include "imgui/imgui.h"
#include "Includes/Roboto-Regular.h"
#include "Includes/Logger.h"
#include <unistd.h>
#include <cstdio>

namespace
{
    struct CjkCandidate
    {
        const char *path;
        int fontNo; // TTC face index; 2 is typically Simplified Chinese in NotoSansCJK
    };

    // Android OEMs ship CJK under different names. First readable file wins.
    const CjkCandidate kCjk[] = {
        {"/system/fonts/NotoSansCJK-Regular.ttc", 2},
        {"/system/fonts/NotoSansCJK-Regular.ttc", 0},
        {"/system/fonts/NotoSansSC-Regular.otf", 0},
        {"/system/fonts/NotoSansCJKsc-Regular.otf", 0},
        {"/system/fonts/NotoSansHans-Regular.otf", 0},
        {"/system/fonts/NotoSansSC-Regular.ttf", 0},
        {"/system/fonts/NotoSansCJK-Regular.otf", 0},
        {"/system/fonts/SourceHanSansCN-Regular.otf", 0},
        {"/system/fonts/SourceHanSansSC-Regular.otf", 0},
        {"/system/fonts/DroidSansFallback.ttf", 0},
        {"/system/fonts/DroidSansChinese.ttf", 0},
        {"/system/fonts/MiSans-Regular.ttf", 0},
        {"/system/fonts/MiSansVF.ttf", 0},
        {"/system/fonts/HarmonyOS_Sans_SC_Regular.ttf", 0},
        {"/system/fonts/HarmonyOS_Sans_SC.ttf", 0},
        {"/system/fonts/NotoSerifCJK-Regular.ttc", 2},
        {"/vendor/fonts/NotoSansCJK-Regular.ttc", 2},
        {"/system/fonts/NotoSansCJK-Regular.ttc", 1},
        {"/system/fonts/NotoSansCJK-Regular.ttc", 3},
    };
} // namespace

void loadOverlayFonts()
{
    ImGuiIO &io = ImGui::GetIO();

    ImFontConfig latin{};
    latin.FontDataOwnedByAtlas = false; // Roboto_Regular is a static array — do not free
    std::snprintf(latin.Name, sizeof(latin.Name), "Roboto");
    io.Fonts->AddFontFromMemoryTTF((void *)Roboto_Regular, (int)sizeof(Roboto_Regular), 40.0f, &latin);

    // Merge a real CJK face. ImGui 1.92 loads missing glyphs on demand from the
    // file, so we do not pre-bake 20k ideographs into the atlas.
    ImFontConfig cjk{};
    cjk.MergeMode = true;
    cjk.PixelSnapH = true;
    cjk.OversampleH = 1;
    cjk.OversampleV = 1;
    std::snprintf(cjk.Name, sizeof(cjk.Name), "CJK");

    for (const auto &cand : kCjk)
    {
        if (access(cand.path, R_OK) != 0)
            continue;
        cjk.FontNo = (ImU32)cand.fontNo;
        ImFont *font = io.Fonts->AddFontFromFileTTF(cand.path, 40.0f, &cjk);
        if (font)
        {
            LOGI("CJK font merged: %s face=%d", cand.path, cand.fontNo);
            return;
        }
    }
    LOGI("No system CJK font found; Chinese will render as missing glyphs");
}
