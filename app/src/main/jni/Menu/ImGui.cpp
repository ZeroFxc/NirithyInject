
#include "ImGui.h"
#include "VulkanHook.h"
#include "KittyMemory/KittyMemory.h"
#include "dobby.h"
#include "Includes/Utils.h"
#include "Includes/obfuscate.h"
#include "Includes/Logger.h"
#include "imgui/imgui.h"
#include "Includes/Roboto-Regular.h"
#include "imgui/backends/imgui_impl_opengl3.h"
#include "imgui/backends/imgui_impl_android.h"
#include <GLES3/gl3.h>
#include <unistd.h>
#include <chrono>
#include <thread>
#include <cmath>
#include "EGL/egl.h"

#ifndef GL_FRAMEBUFFER_SRGB
#define GL_FRAMEBUFFER_SRGB 0x8DB9
#endif
#ifndef GL_FRAMEBUFFER_SRGB_EXT
#define GL_FRAMEBUFFER_SRGB_EXT 0x8DB9
#endif

using swapbuffers_orig = EGLBoolean (*)(EGLDisplay dpy, EGLSurface surf);
EGLBoolean swapbuffers_hook(EGLDisplay dpy, EGLSurface surf);
swapbuffers_orig o_swapbuffers = nullptr;

void (*menuAddress)();
void (*onInitAddr)();

bool isInitialized = false;
int glWidth = 0;
int glHeight = 0;

// 由 Main.cpp 定义的窗口重排标志：置位后菜单会按新的显示尺寸重新计算窗口位置
extern bool resetWindow;

int getGlWidth()
{
    return glWidth;
}
int getGlHeight()
{
    return glHeight;
}

#define HOOKINPUT(ret, func, ...)                                                                                      \
    ret (*orig##func)(__VA_ARGS__);                                                                                    \
    ret my##func(__VA_ARGS__)

HOOKINPUT(void, Input, void *thiz, void *ex_ab, void *ex_ac)
{
    origInput(thiz, ex_ab, ex_ac);
    if (isInitialized)
        ImGui_ImplAndroid_HandleInputEvent((AInputEvent *)thiz);
    return;
}

ImVec2 initialScreenSize;
bool needClear = true;

// 0 = 不限制帧率；>0 时限制每秒最大绘制帧数
int g_fpsLimit = 0;

static void applyFpsLimit()
{
    if (g_fpsLimit <= 0)
        return;
    static auto lastTime = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    auto frameTime = now - lastTime;
    lastTime = now;

    auto targetFrameTime = std::chrono::microseconds(1000000 / g_fpsLimit);
    if (frameTime < targetFrameTime)
    {
        std::this_thread::sleep_for(targetFrameTime - frameTime);
    }
}

void *initModMenu(void *menu_addr, void *on_init_addr, bool isJni)
{
    menuAddress = (void (*)())menu_addr;
    onInitAddr = (void (*)())on_init_addr;
    initVulkanHooks();
    if (!isJni)
    {
        needClear = false;
        while (!isLibraryLoaded(OBFUSCATE("libEGL.so")))
        {
            sleep(1);
        }
        auto swapBuffers = ((uintptr_t)DobbySymbolResolver(OBFUSCATE("libEGL.so"), OBFUSCATE("eglSwapBuffers")));
        if (swapBuffers)
        {
            KittyMemory::ProtectAddr((void *)swapBuffers, sizeof(swapBuffers), PROT_READ | PROT_WRITE | PROT_EXEC);
            DobbyHook((void *)swapBuffers, (void *)swapbuffers_hook, (void **)&o_swapbuffers);
        }

#ifdef LIB_INPUT
        void *sym_input = DobbySymbolResolver(
            OBFUSCATE("/system/lib/libinput.so"),
            OBFUSCATE("_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE"));
        if (sym_input != nullptr)
        {
            DobbyHook((void *)sym_input, (void *)myInput, (void **)&origInput);
        }
#endif
    }
    LOGI("%s", (char *)OBFUSCATE("ImGUI Hooks initialized"));
    return nullptr;
}

void menuStyle(int theme)
{
    ImGuiStyle &style = ImGui::GetStyle();
    switch (theme)
    {
    case 1: // Light
        style.Colors[ImGuiCol_WindowBg] = ImVec4(0.94f, 0.94f, 0.95f, 0.97f);
        style.Colors[ImGuiCol_ChildBg] = ImVec4(0.90f, 0.90f, 0.92f, 0.95f);
        style.Colors[ImGuiCol_PopupBg] = ImVec4(0.96f, 0.96f, 0.97f, 0.98f);
        style.Colors[ImGuiCol_FrameBg] = ImVec4(0.85f, 0.85f, 0.88f, 0.95f);
        style.Colors[ImGuiCol_Text] = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
        style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.45f, 0.45f, 0.50f, 1.00f);
        style.Colors[ImGuiCol_Button] = ImVec4(0.80f, 0.80f, 0.85f, 0.60f);
        style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.70f, 0.70f, 0.80f, 1.00f);
        style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.60f, 0.60f, 0.75f, 1.00f);
        style.Colors[ImGuiCol_Header] = ImVec4(0.80f, 0.80f, 0.88f, 0.80f);
        style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.70f, 0.75f, 0.90f, 1.00f);
        style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.65f, 0.70f, 0.85f, 1.00f);
        style.Colors[ImGuiCol_Separator] = ImVec4(0.60f, 0.60f, 0.65f, 0.80f);
        style.Colors[ImGuiCol_Border] = ImVec4(0.50f, 0.50f, 0.55f, 0.80f);
        style.Colors[ImGuiCol_TitleBg] = ImVec4(0.85f, 0.85f, 0.90f, 1.00f);
        style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.80f, 0.80f, 0.88f, 1.00f);
        style.Colors[ImGuiCol_Tab] = ImVec4(0.85f, 0.85f, 0.90f, 0.90f);
        style.Colors[ImGuiCol_TabHovered] = ImVec4(0.75f, 0.80f, 0.95f, 1.00f);
        style.Colors[ImGuiCol_TabActive] = ImVec4(0.90f, 0.90f, 0.95f, 1.00f);
        style.Colors[ImGuiCol_CheckMark] = ImVec4(0.20f, 0.45f, 0.90f, 1.00f);
        style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.30f, 0.50f, 0.90f, 1.00f);
        style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.20f, 0.40f, 0.80f, 1.00f);
        style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.88f, 0.88f, 0.92f, 0.90f);
        style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.70f, 0.70f, 0.78f, 0.90f);
        style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.60f, 0.60f, 0.70f, 1.00f);
        style.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.50f, 0.50f, 0.60f, 1.00f);
        break;
    case 2: // Custom (蓝绿暗色)
        style.Colors[ImGuiCol_WindowBg] = ImVec4(0.05f, 0.08f, 0.10f, 0.96f);
        style.Colors[ImGuiCol_ChildBg] = ImVec4(0.04f, 0.07f, 0.09f, 0.92f);
        style.Colors[ImGuiCol_PopupBg] = ImVec4(0.06f, 0.10f, 0.12f, 0.97f);
        style.Colors[ImGuiCol_FrameBg] = ImVec4(0.10f, 0.16f, 0.20f, 0.92f);
        style.Colors[ImGuiCol_Text] = ImVec4(0.85f, 0.95f, 1.00f, 1.00f);
        style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
        style.Colors[ImGuiCol_Button] = ImVec4(0.10f, 0.35f, 0.45f, 0.70f);
        style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.15f, 0.50f, 0.65f, 1.00f);
        style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.20f, 0.60f, 0.80f, 1.00f);
        style.Colors[ImGuiCol_Header] = ImVec4(0.10f, 0.30f, 0.40f, 0.80f);
        style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.15f, 0.45f, 0.60f, 1.00f);
        style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.20f, 0.55f, 0.70f, 1.00f);
        style.Colors[ImGuiCol_Separator] = ImVec4(0.20f, 0.40f, 0.50f, 0.80f);
        style.Colors[ImGuiCol_Border] = ImVec4(0.20f, 0.45f, 0.55f, 0.80f);
        style.Colors[ImGuiCol_TitleBg] = ImVec4(0.08f, 0.20f, 0.25f, 1.00f);
        style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.35f, 0.45f, 1.00f);
        style.Colors[ImGuiCol_Tab] = ImVec4(0.08f, 0.22f, 0.28f, 0.90f);
        style.Colors[ImGuiCol_TabHovered] = ImVec4(0.15f, 0.45f, 0.60f, 1.00f);
        style.Colors[ImGuiCol_TabActive] = ImVec4(0.12f, 0.40f, 0.55f, 1.00f);
        style.Colors[ImGuiCol_CheckMark] = ImVec4(0.20f, 0.90f, 0.80f, 1.00f);
        style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.20f, 0.70f, 0.80f, 1.00f);
        style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.30f, 0.90f, 1.00f, 1.00f);
        style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.08f, 0.15f, 0.18f, 0.90f);
        style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.20f, 0.45f, 0.55f, 0.90f);
        style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.25f, 0.60f, 0.70f, 1.00f);
        style.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.30f, 0.75f, 0.90f, 1.00f);
        break;
    default: // Dark (默认)
        style.Colors[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.07f, 0.08f, 0.96f);
        style.Colors[ImGuiCol_ChildBg] = ImVec4(0.06f, 0.06f, 0.07f, 0.90f);
        style.Colors[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.08f, 0.09f, 0.96f);
        style.Colors[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.14f, 0.16f, 0.90f);
        style.Colors[ImGuiCol_Text] = ImVec4(0.90f, 0.90f, 0.92f, 1.00f);
        style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.50f, 0.55f, 1.00f);
        style.Colors[ImGuiCol_Button] = ImVec4(0.25f, 0.25f, 0.30f, 0.70f);
        style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.35f, 0.35f, 0.45f, 1.00f);
        style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.45f, 0.45f, 0.55f, 1.00f);
        style.Colors[ImGuiCol_Header] = ImVec4(0.20f, 0.20f, 0.25f, 0.80f);
        style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.30f, 0.30f, 0.40f, 1.00f);
        style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.40f, 0.40f, 0.50f, 1.00f);
        style.Colors[ImGuiCol_Separator] = ImVec4(0.30f, 0.30f, 0.35f, 0.80f);
        style.Colors[ImGuiCol_Border] = ImVec4(0.25f, 0.25f, 0.30f, 0.80f);
        style.Colors[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
        style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.15f, 0.15f, 0.18f, 1.00f);
        style.Colors[ImGuiCol_Tab] = ImVec4(0.12f, 0.12f, 0.15f, 0.90f);
        style.Colors[ImGuiCol_TabHovered] = ImVec4(0.25f, 0.25f, 0.35f, 1.00f);
        style.Colors[ImGuiCol_TabActive] = ImVec4(0.18f, 0.18f, 0.22f, 1.00f);
        style.Colors[ImGuiCol_CheckMark] = ImVec4(0.30f, 0.80f, 0.40f, 1.00f);
        style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.30f, 0.60f, 0.40f, 1.00f);
        style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.40f, 0.80f, 0.50f, 1.00f);
        style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.12f, 0.12f, 0.14f, 0.90f);
        style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.30f, 0.30f, 0.35f, 0.90f);
        style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.40f, 0.40f, 0.50f, 1.00f);
        style.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.50f, 0.50f, 0.60f, 1.00f);
        break;
    }
}

void setupMenu()
{
    if (isInitialized)
        return;

    auto ctx = ImGui::CreateContext();
    if (!ctx)
    {
        LOGI("%s", (char *)OBFUSCATE("Failed to create context"));
        return;
    }

    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)glWidth, (float)glHeight);
    io.IniFilename = nullptr;

    ImGui_ImplAndroid_Init(nullptr);
    ImGui_ImplOpenGL3_Init("#version 300 es");

    io.Fonts->AddFontFromMemoryTTF(Roboto_Regular, sizeof(Roboto_Regular), 40.0f);

    ImGui::GetStyle().ScaleAllSizes(2);
    ImGui::GetStyle().ScrollbarSize *= 2.5f;
    // 默认暗色主题；用户选择通过 on_init 的 ConfigGet("theme") 在 doChangeScale 中应用
    menuStyle(0);

    if (onInitAddr)
        onInitAddr();

    isInitialized = true;
    LOGI("setup done.");
}

static void applyOverlayGlState()
{
    glDisable(GL_FRAMEBUFFER_SRGB);
    glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

void internalDrawMenu(int width, int height)
{
    if (!isInitialized)
        return;

    ImGui::GetIO().DisplaySize = ImVec2((float)width, (float)height);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowSize(ImVec2((float)width / 2, (float)height / 2), ImGuiCond_Once);
    menuAddress();

    ImGui::Render();

    GLboolean srgb = GL_FALSE;
    glGetBooleanv(GL_FRAMEBUFFER_SRGB, &srgb);
    if (!srgb)
        glGetBooleanv(GL_FRAMEBUFFER_SRGB_EXT, &srgb);

    applyOverlayGlState();

    if (needClear)
    {
        glViewport(0, 0, width, height);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    if (srgb)
    {
        glEnable(GL_FRAMEBUFFER_SRGB);
        glEnable(GL_FRAMEBUFFER_SRGB_EXT);
    }

    applyFpsLimit();
}

// 上一次绘制使用的叠加层尺寸（仅由 swapbuffers_hook 维护，避免与 Unity 逻辑分辨率混淆）
static int g_overlayWidth = 0;
static int g_overlayHeight = 0;

// 判断叠加层尺寸是否发生显著变化（相对上次绘制尺寸偏差超过 20%）
// 功能：Waydroid / WSA 等环境下 EGL 缓冲尺寸会随窗口密度修正而变化，
//       若仍沿用旧尺寸布局，菜单会停留在旧坐标上导致叠加层错位。
// 参数：width/height 为本帧查询到的 EGL 表面尺寸
// 返回值：true 表示尺寸变化显著，需要重新记录基准并重排窗口
static bool isOverlaySizeChanged(int width, int height)
{
    if (g_overlayWidth <= 0 || g_overlayHeight <= 0)
        return false;
    const float dx = std::fabs(static_cast<float>(width) - static_cast<float>(g_overlayWidth)) /
                     static_cast<float>(g_overlayWidth);
    const float dy = std::fabs(static_cast<float>(height) - static_cast<float>(g_overlayHeight)) /
                     static_cast<float>(g_overlayHeight);
    return dx > 0.2f || dy > 0.2f;
}

EGLBoolean swapbuffers_hook(EGLDisplay dpy, EGLSurface surf)
{
    EGLint w, h;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    glWidth = w;
    glHeight = h;

    if (g_overlayWidth <= 0 || g_overlayHeight <= 0)
    {
        // 首帧只记录基准尺寸，不改动窗口布局
        g_overlayWidth = w;
        g_overlayHeight = h;
        initialScreenSize.x = (float)w;
        initialScreenSize.y = (float)h;
    }
    else if (isOverlaySizeChanged(w, h))
    {
        // 显示尺寸显著变化（分辨率切换或密度修正后），更新基准并通知主线程重排窗口
        g_overlayWidth = w;
        g_overlayHeight = h;
        initialScreenSize.x = (float)w;
        initialScreenSize.y = (float)h;
        LOGI("Overlay size changed to %dx%d, request window layout reset", w, h);
        resetWindow = true;
    }

    setupMenu();
    internalDrawMenu(w, h);

    return o_swapbuffers(dpy, surf);
}
