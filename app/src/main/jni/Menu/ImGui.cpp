
#include "ImGui.h"
#include "Fonts.h"
#include "VulkanHook.h"
#include "KittyMemory/KittyMemory.h"
#include "dobby.h"
#include "Includes/Utils.h"
#include "Includes/obfuscate.h"
#include "Includes/Logger.h"
#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_opengl3.h"
#include "imgui/backends/imgui_impl_android.h"
#include <GLES3/gl3.h>
#include <unistd.h>
#include <chrono>
#include <thread>
#include <cmath>
#include <dlfcn.h>
#include <atomic>
#include <cstdint>
#include "EGL/egl.h"
#include <android/input.h>

#ifndef GL_FRAMEBUFFER_SRGB
#define GL_FRAMEBUFFER_SRGB 0x8DB9
#endif
#ifndef GL_FRAMEBUFFER_SRGB_EXT
#define GL_FRAMEBUFFER_SRGB_EXT 0x8DB9
#endif

#ifndef EGL_EGLEXT_PROTOTYPES
#define EGL_EGLEXT_PROTOTYPES
#endif
#include <EGL/eglext.h>

using swapbuffers_orig = EGLBoolean (*)(EGLDisplay dpy, EGLSurface surf);
EGLBoolean swapbuffers_hook(EGLDisplay dpy, EGLSurface surf);
void internalDrawMenu(int width, int height);
swapbuffers_orig o_swapbuffers = nullptr;
static EGLBoolean (*o_swap_damage)(EGLDisplay, EGLSurface, EGLint *, EGLint) = nullptr;

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

// InputConsumer::initializeMotionEvent(MotionEvent *event, const InputMessage *msg)
// 注意：thiz 是 InputConsumer* 本身，第 2 个参数（ex_ab）才是 MotionEvent*，
// 也只有它才是合法的 AInputEvent*；此前误把 thiz 传进去会把垃圾内存当作输入事件解析。
HOOKINPUT(void, Input, void *thiz, void *ex_ab, void *ex_ac)
{
    origInput(thiz, ex_ab, ex_ac);
    if (isInitialized)
        ImGui_ImplAndroid_HandleInputEvent((AInputEvent *)ex_ab);
    return;
}

// input_event 由参数 arg4、input_event 正确传递
// 注意：arg3(frameTime) 真实类型为 nsecs_t(int64_t)，32 位 ABI 下 long 仅 32 位会导致后续参数错位
HOOKINPUT(int32_t, Consume, void *thiz, void *arg1, bool arg2, int64_t arg3, uint32_t *arg4, AInputEvent **input_event)
{
    auto result = origConsume(thiz, arg1, arg2, arg3, arg4, input_event);
    if (result != 0 || input_event == nullptr || *input_event == nullptr)
        return result;
    if (isInitialized)
        ImGui_ImplAndroid_HandleInputEvent(*input_event);
    return result;
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

// 解析指定库中的符号
// 参数 lib：库名或绝对路径；sym：符号名
// 返回值：符号地址，找不到时返回 nullptr
// 说明：这里刻意不使用 DobbySymbolResolver。当符号无法通过 dlsym 找到时，它会遍历
// 并 mmap 进程内所有模块，再逐个按磁盘文件解析 ELF 头；一旦遇到内存镜像与磁盘文件
// 不一致的库（加固 / 解密 / 被截断），e_phoff 会解析成非法值，随后在 elf_ctx_init
// 中解引用野指针直接 SIGSEGV（已在 04_10-02-24-28_907.log 中确认该崩溃栈）。
static void *resolveSym(const char *lib, const char *sym)
{
    // 1) 目标库已加载时，直接在其导出表中查找（RTLD_NOLOAD 不会触发真正的加载）
    if (void *h = dlopen(lib, RTLD_NOW | RTLD_NOLOAD))
    {
        if (void *p = dlsym(h, sym))
            return p;
    }
    // 2) 退化为全局符号域查找，等价于 DobbySymbolResolver 的第一步，但不会扫描全部模块
    void *p = dlsym(RTLD_DEFAULT, sym);
    if (!p)
        LOGI("symbol not found: %s (lib %s)", sym, lib);
    return p;
}

static bool hookAddr(void *addr, void *hook, void **orig, const char *tag)
{
    if (!addr)
        return false;
    static void *hooked[16]{};
    static int nHooked = 0;
    for (int i = 0; i < nHooked; i++)
        if (hooked[i] == addr)
            return true;
    if (DobbyHook(addr, hook, orig) == 0)
    {
        if (nHooked < 16)
            hooked[nHooked++] = addr;
        LOGI("hooked %s at %p", tag, addr);
        return true;
    }
    LOGI("hook failed %s at %p", tag, addr);
    return false;
}

// 线程局部重入标志：若驱动把 eglSwapBuffers 实现为内部调用 WithDamage 变体（或反之），
// 一次交换会嵌套触发两个钩子；用该标志保证同一线程一次交换的调用链中只绘制一次
static thread_local bool g_inSwap = false;

static EGLBoolean swap_damage_hook(EGLDisplay dpy, EGLSurface surf, EGLint *rects, EGLint n)
{
    // 重入保护：内层嵌套调用只转发原始函数，不重复绘制
    if (g_inSwap)
    {
        if (o_swap_damage)
            return o_swap_damage(dpy, surf, rects, n);
        if (o_swapbuffers)
            return o_swapbuffers(dpy, surf);
        return EGL_TRUE;
    }
    g_inSwap = true;

    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    // 首次回调日志：用于区分游戏走的是 WithDamage 变体还是 eglSwapBuffers
    static bool s_firstDamageLogged = false;
    if (!s_firstDamageLogged)
    {
        s_firstDamageLogged = true;
        LOGI("eglSwapBuffersWithDamage called first time (%dx%d)", w, h);
    }
    if (w > 0 && h > 0)
    {
        glWidth = w;
        glHeight = h;
        setupMenu();
        internalDrawMenu(w, h);
    }

    EGLBoolean ret;
    if (o_swap_damage)
        ret = o_swap_damage(dpy, surf, rects, n);
    else if (o_swapbuffers)
        ret = o_swapbuffers(dpy, surf);
    else
        ret = EGL_TRUE;

    g_inSwap = false;
    return ret;
}

void installOverlayHooks()
{
    static const char *kEglLibs[] = {"libEGL.so", "libunity.so", "libGLESv2.so", "libGLESv3.so"};
    for (auto lib : kEglLibs)
    {
        if (!o_swapbuffers)
            hookAddr(resolveSym(lib, "eglSwapBuffers"), (void *)swapbuffers_hook, (void **)&o_swapbuffers, "eglSwapBuffers");
        if (!o_swap_damage)
        {
            void *d = resolveSym(lib, "eglSwapBuffersWithDamageKHR");
            if (!d)
                d = resolveSym(lib, "eglSwapBuffersWithDamageEXT");
            hookAddr(d, (void *)swap_damage_hook, (void **)&o_swap_damage, "eglSwapBuffersWithDamage");
        }
    }

    static const char *kInputLibs[] = {
        "libinput.so",
        "/system/lib64/libinput.so",
        "/system/lib/libinput.so",
    };
    static const char *kMotion =
        "_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE";
    static const char *kConsume =
        "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventE";
    static const char *kConsume2 =
        "_ZN7android13InputConsumer7ConsumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventE";
    // libinput.so 属于 Activity 输入栈的系统库，进程启动时即已加载；Android 16 上这些
    // InputConsumer 方法已不再是导出符号（dlopen 成功但 dlsym 必然失败），后台重试永远不会
    // 成功。这里只解析一次，避免后台线程每秒重新 dlopen/dlsym 并重复刷 symbol not found。
    static bool s_inputHooksSettled = false;
    if (!s_inputHooksSettled)
    {
        for (auto lib : kInputLibs)
        {
            if (!origInput)
                hookAddr(resolveSym(lib, kMotion), (void *)myInput, (void **)&origInput, "InputConsumer.initializeMotionEvent");
            if (!origConsume)
            {
                void *c = resolveSym(lib, kConsume);
                if (!c)
                    c = resolveSym(lib, kConsume2);
                hookAddr(c, (void *)myConsume, (void **)&origConsume, "InputConsumer.consume");
            }
        }
        s_inputHooksSettled = true;
        if (!origInput || !origConsume)
            LOGD("libinput touch injection unavailable on this Android version (symbols hidden) — menu touch still works via ImGui_ImplAndroid");
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
        int waited = 0;
        while (!isLibraryLoaded(OBFUSCATE("libEGL.so")) && !isLibraryLoaded(OBFUSCATE("libunity.so")) && waited < 45)
        {
            sleep(1);
            waited++;
        }
        installOverlayHooks();
        // Unity often loads after EGL — keep retrying in the background.
        std::thread([] {
            for (int i = 0; i < 60; i++)
            {
                installOverlayHooks();
                sleep(1);
            }
        }).detach();
    }
    LOGI("%s", (char *)OBFUSCATE("ImGUI Hooks initialized"));
    return nullptr;
}

static void applyRounding(ImGuiStyle &style, float windowR, float frameR)
{
    style.WindowRounding = windowR;
    style.ChildRounding = frameR;
    style.FrameRounding = frameR;
    style.PopupRounding = frameR;
    style.ScrollbarRounding = frameR;
    style.GrabRounding = frameR;
    style.TabRounding = frameR;
    style.WindowBorderSize = windowR > 8.f ? 0.f : 1.f;
    style.FrameBorderSize = 0.f;
}

const char *themeName(int theme)
{
    switch (theme)
    {
    case 1:
        return "theme.light";
    case 2:
        return "theme.teal";
    case 3:
        return "theme.md3dark";
    case 4:
        return "theme.md3light";
    case 5:
        return "theme.amoled";
    case 6:
        return "theme.dracula";
    case 7:
        return "theme.nord";
    default:
        return "theme.dark";
    }
}

void menuStyle(int theme)
{
    ImGuiStyle &style = ImGui::GetStyle();
    applyRounding(style, 8.f, 6.f);
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
    case 2: // Teal
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
    case 3: { // MD3 Dark — Material You
        applyRounding(style, 20.f, 14.f);
        const ImVec4 surface(0.11f, 0.11f, 0.12f, 0.97f);
        const ImVec4 container(0.17f, 0.16f, 0.18f, 0.96f);
        const ImVec4 primary(0.82f, 0.74f, 1.00f, 1.00f);
        const ImVec4 primaryC(0.31f, 0.27f, 0.45f, 1.00f);
        const ImVec4 onSurf(0.90f, 0.88f, 0.90f, 1.00f);
        style.Colors[ImGuiCol_WindowBg] = surface;
        style.Colors[ImGuiCol_ChildBg] = container;
        style.Colors[ImGuiCol_PopupBg] = ImVec4(0.20f, 0.19f, 0.22f, 0.98f);
        style.Colors[ImGuiCol_FrameBg] = ImVec4(0.22f, 0.21f, 0.25f, 0.95f);
        style.Colors[ImGuiCol_Text] = onSurf;
        style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.58f, 0.56f, 0.60f, 1.00f);
        style.Colors[ImGuiCol_Button] = primaryC;
        style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.40f, 0.35f, 0.58f, 1.00f);
        style.Colors[ImGuiCol_ButtonActive] = primary;
        style.Colors[ImGuiCol_Header] = primaryC;
        style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.40f, 0.35f, 0.58f, 1.00f);
        style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.48f, 0.42f, 0.66f, 1.00f);
        style.Colors[ImGuiCol_Separator] = ImVec4(0.58f, 0.56f, 0.60f, 0.50f);
        style.Colors[ImGuiCol_Border] = ImVec4(0.58f, 0.56f, 0.60f, 0.40f);
        style.Colors[ImGuiCol_TitleBg] = container;
        style.Colors[ImGuiCol_TitleBgActive] = primaryC;
        style.Colors[ImGuiCol_Tab] = container;
        style.Colors[ImGuiCol_TabHovered] = ImVec4(0.40f, 0.35f, 0.58f, 1.00f);
        style.Colors[ImGuiCol_TabActive] = primaryC;
        style.Colors[ImGuiCol_CheckMark] = primary;
        style.Colors[ImGuiCol_SliderGrab] = primary;
        style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.90f, 0.84f, 1.00f, 1.00f);
        style.Colors[ImGuiCol_ScrollbarBg] = surface;
        style.Colors[ImGuiCol_ScrollbarGrab] = primaryC;
        style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.40f, 0.35f, 0.58f, 1.00f);
        style.Colors[ImGuiCol_ScrollbarGrabActive] = primary;
        style.Colors[ImGuiCol_ResizeGrip] = primaryC;
        style.Colors[ImGuiCol_ResizeGripHovered] = primary;
        break;
    }
    case 4: { // MD3 Light
        applyRounding(style, 20.f, 14.f);
        style.Colors[ImGuiCol_WindowBg] = ImVec4(0.99f, 0.97f, 1.00f, 0.98f);
        style.Colors[ImGuiCol_ChildBg] = ImVec4(0.95f, 0.93f, 0.97f, 0.96f);
        style.Colors[ImGuiCol_PopupBg] = ImVec4(0.96f, 0.94f, 0.98f, 0.98f);
        style.Colors[ImGuiCol_FrameBg] = ImVec4(0.91f, 0.89f, 0.94f, 0.95f);
        style.Colors[ImGuiCol_Text] = ImVec4(0.11f, 0.11f, 0.12f, 1.00f);
        style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.45f, 0.44f, 0.48f, 1.00f);
        style.Colors[ImGuiCol_Button] = ImVec4(0.91f, 0.87f, 1.00f, 1.00f);
        style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.82f, 0.74f, 1.00f, 1.00f);
        style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.40f, 0.31f, 0.64f, 1.00f);
        style.Colors[ImGuiCol_Header] = ImVec4(0.91f, 0.87f, 1.00f, 1.00f);
        style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.82f, 0.74f, 1.00f, 1.00f);
        style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.67f, 0.58f, 0.90f, 1.00f);
        style.Colors[ImGuiCol_Separator] = ImVec4(0.48f, 0.45f, 0.51f, 0.40f);
        style.Colors[ImGuiCol_Border] = ImVec4(0.48f, 0.45f, 0.51f, 0.30f);
        style.Colors[ImGuiCol_TitleBg] = ImVec4(0.95f, 0.93f, 0.97f, 1.00f);
        style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.91f, 0.87f, 1.00f, 1.00f);
        style.Colors[ImGuiCol_Tab] = ImVec4(0.95f, 0.93f, 0.97f, 1.00f);
        style.Colors[ImGuiCol_TabHovered] = ImVec4(0.82f, 0.74f, 1.00f, 1.00f);
        style.Colors[ImGuiCol_TabActive] = ImVec4(0.91f, 0.87f, 1.00f, 1.00f);
        style.Colors[ImGuiCol_CheckMark] = ImVec4(0.40f, 0.31f, 0.64f, 1.00f);
        style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.40f, 0.31f, 0.64f, 1.00f);
        style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.31f, 0.22f, 0.54f, 1.00f);
        style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.99f, 0.97f, 1.00f, 0.90f);
        style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.82f, 0.74f, 1.00f, 0.90f);
        style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.67f, 0.58f, 0.90f, 1.00f);
        style.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.40f, 0.31f, 0.64f, 1.00f);
        break;
    }
    case 5: // AMOLED
        applyRounding(style, 12.f, 8.f);
        style.Colors[ImGuiCol_WindowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.94f);
        style.Colors[ImGuiCol_ChildBg] = ImVec4(0.05f, 0.05f, 0.05f, 0.90f);
        style.Colors[ImGuiCol_PopupBg] = ImVec4(0.06f, 0.06f, 0.06f, 0.96f);
        style.Colors[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.12f, 0.12f, 0.90f);
        style.Colors[ImGuiCol_Text] = ImVec4(0.95f, 0.95f, 0.95f, 1.00f);
        style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.45f, 0.45f, 0.45f, 1.00f);
        style.Colors[ImGuiCol_Button] = ImVec4(0.16f, 0.16f, 0.16f, 0.80f);
        style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.28f, 0.28f, 0.28f, 1.00f);
        style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.40f, 0.40f, 0.40f, 1.00f);
        style.Colors[ImGuiCol_Header] = ImVec4(0.16f, 0.16f, 0.16f, 0.80f);
        style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.28f, 0.28f, 0.28f, 1.00f);
        style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.36f, 0.36f, 0.36f, 1.00f);
        style.Colors[ImGuiCol_Separator] = ImVec4(0.22f, 0.22f, 0.22f, 0.80f);
        style.Colors[ImGuiCol_Border] = ImVec4(0.20f, 0.20f, 0.20f, 0.70f);
        style.Colors[ImGuiCol_TitleBg] = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
        style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);
        style.Colors[ImGuiCol_Tab] = ImVec4(0.08f, 0.08f, 0.08f, 0.90f);
        style.Colors[ImGuiCol_TabHovered] = ImVec4(0.28f, 0.28f, 0.28f, 1.00f);
        style.Colors[ImGuiCol_TabActive] = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
        style.Colors[ImGuiCol_CheckMark] = ImVec4(0.40f, 0.85f, 0.55f, 1.00f);
        style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.40f, 0.85f, 0.55f, 1.00f);
        style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.55f, 0.95f, 0.65f, 1.00f);
        style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.90f);
        style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.22f, 0.22f, 0.22f, 0.90f);
        style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.35f, 0.35f, 0.35f, 1.00f);
        style.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
        break;
    case 6: // Dracula
        applyRounding(style, 10.f, 7.f);
        style.Colors[ImGuiCol_WindowBg] = ImVec4(0.16f, 0.16f, 0.21f, 0.97f);
        style.Colors[ImGuiCol_ChildBg] = ImVec4(0.13f, 0.13f, 0.18f, 0.92f);
        style.Colors[ImGuiCol_PopupBg] = ImVec4(0.16f, 0.16f, 0.21f, 0.98f);
        style.Colors[ImGuiCol_FrameBg] = ImVec4(0.27f, 0.28f, 0.35f, 0.90f);
        style.Colors[ImGuiCol_Text] = ImVec4(0.97f, 0.97f, 0.95f, 1.00f);
        style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.38f, 0.42f, 0.52f, 1.00f);
        style.Colors[ImGuiCol_Button] = ImVec4(0.74f, 0.58f, 0.98f, 0.55f);
        style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.74f, 0.58f, 0.98f, 0.85f);
        style.Colors[ImGuiCol_ButtonActive] = ImVec4(1.00f, 0.47f, 0.78f, 1.00f);
        style.Colors[ImGuiCol_Header] = ImVec4(0.74f, 0.58f, 0.98f, 0.40f);
        style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.74f, 0.58f, 0.98f, 0.70f);
        style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.80f, 0.47f, 0.65f, 1.00f);
        style.Colors[ImGuiCol_Separator] = ImVec4(0.27f, 0.28f, 0.35f, 1.00f);
        style.Colors[ImGuiCol_Border] = ImVec4(0.27f, 0.28f, 0.35f, 0.80f);
        style.Colors[ImGuiCol_TitleBg] = ImVec4(0.13f, 0.13f, 0.18f, 1.00f);
        style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.74f, 0.58f, 0.98f, 0.45f);
        style.Colors[ImGuiCol_Tab] = ImVec4(0.16f, 0.16f, 0.21f, 0.90f);
        style.Colors[ImGuiCol_TabHovered] = ImVec4(0.74f, 0.58f, 0.98f, 0.70f);
        style.Colors[ImGuiCol_TabActive] = ImVec4(0.27f, 0.28f, 0.35f, 1.00f);
        style.Colors[ImGuiCol_CheckMark] = ImVec4(0.31f, 0.98f, 0.48f, 1.00f);
        style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.74f, 0.58f, 0.98f, 1.00f);
        style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(1.00f, 0.47f, 0.78f, 1.00f);
        style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.13f, 0.13f, 0.18f, 0.90f);
        style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.27f, 0.28f, 0.35f, 0.90f);
        style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.74f, 0.58f, 0.98f, 0.70f);
        style.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.74f, 0.58f, 0.98f, 1.00f);
        break;
    case 7: // Nord
        applyRounding(style, 10.f, 7.f);
        style.Colors[ImGuiCol_WindowBg] = ImVec4(0.18f, 0.20f, 0.25f, 0.97f);
        style.Colors[ImGuiCol_ChildBg] = ImVec4(0.23f, 0.26f, 0.32f, 0.92f);
        style.Colors[ImGuiCol_PopupBg] = ImVec4(0.18f, 0.20f, 0.25f, 0.98f);
        style.Colors[ImGuiCol_FrameBg] = ImVec4(0.26f, 0.30f, 0.37f, 0.90f);
        style.Colors[ImGuiCol_Text] = ImVec4(0.93f, 0.94f, 0.96f, 1.00f);
        style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.60f, 0.64f, 0.69f, 1.00f);
        style.Colors[ImGuiCol_Button] = ImVec4(0.37f, 0.51f, 0.67f, 0.70f);
        style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.53f, 0.75f, 0.81f, 1.00f);
        style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.64f, 0.75f, 0.55f, 1.00f);
        style.Colors[ImGuiCol_Header] = ImVec4(0.37f, 0.51f, 0.67f, 0.55f);
        style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.53f, 0.75f, 0.81f, 0.80f);
        style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.37f, 0.51f, 0.67f, 1.00f);
        style.Colors[ImGuiCol_Separator] = ImVec4(0.30f, 0.34f, 0.42f, 0.80f);
        style.Colors[ImGuiCol_Border] = ImVec4(0.30f, 0.34f, 0.42f, 0.70f);
        style.Colors[ImGuiCol_TitleBg] = ImVec4(0.18f, 0.20f, 0.25f, 1.00f);
        style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.37f, 0.51f, 0.67f, 0.70f);
        style.Colors[ImGuiCol_Tab] = ImVec4(0.23f, 0.26f, 0.32f, 0.90f);
        style.Colors[ImGuiCol_TabHovered] = ImVec4(0.53f, 0.75f, 0.81f, 0.80f);
        style.Colors[ImGuiCol_TabActive] = ImVec4(0.37f, 0.51f, 0.67f, 1.00f);
        style.Colors[ImGuiCol_CheckMark] = ImVec4(0.64f, 0.75f, 0.55f, 1.00f);
        style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.53f, 0.75f, 0.81f, 1.00f);
        style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.64f, 0.75f, 0.55f, 1.00f);
        style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.18f, 0.20f, 0.25f, 0.90f);
        style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.30f, 0.34f, 0.42f, 0.90f);
        style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.37f, 0.51f, 0.67f, 1.00f);
        style.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.53f, 0.75f, 0.81f, 1.00f);
        break;
    default: // Dark
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

    // 霓虹边框（可选开关）：自定义颜色发光边框
    extern bool neonBorder;
    extern float neonBorderColor[3];
    if (neonBorder)
    {
        style.WindowBorderSize = 2.0f;
        style.ChildBorderSize = 1.5f;
        style.PopupBorderSize = 1.5f;
        style.Colors[ImGuiCol_Border] = ImVec4(neonBorderColor[0], neonBorderColor[1], neonBorderColor[2], 0.90f);
        style.Colors[ImGuiCol_BorderShadow] = ImVec4(neonBorderColor[0], neonBorderColor[1], neonBorderColor[2], 0.50f);
    }

    // 未选中 tab 统一调暗，增强选中/未选中的层级区分
    ImVec4 &tabUnselected = style.Colors[ImGuiCol_Tab];
    tabUnselected.x *= 0.65f;
    tabUnselected.y *= 0.65f;
    tabUnselected.z *= 0.65f;
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
    // 启用 Docking（单视口内窗口停靠/分割/tab）。不启用 Multi-Viewports：
    // Android overlay 单 GL 上下文，imgui_impl_android 不支持独立 OS 窗口。
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    ImGui_ImplAndroid_Init(nullptr);
    ImGui_ImplOpenGL3_Init("#version 300 es");

    loadOverlayFonts();

    ImGui::GetStyle().ScaleAllSizes(2);
    ImGui::GetStyle().ScrollbarSize *= 2.5f;
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
    if (menuAddress)
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

static int g_overlayWidth = 0;
static int g_overlayHeight = 0;

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
    // 重入保护：内层嵌套调用只转发原始函数，不重复绘制
    if (g_inSwap)
    {
        if (o_swapbuffers)
            return o_swapbuffers(dpy, surf);
        return EGL_TRUE;
    }
    g_inSwap = true;

    EGLint w, h;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    // 首次回调日志：证明游戏确实走到了被 hook 的 eglSwapBuffers（即 overlay 真正上屏）
    static bool s_firstSwapLogged = false;
    if (!s_firstSwapLogged)
    {
        s_firstSwapLogged = true;
        LOGI("eglSwapBuffers called first time (%dx%d)", w, h);
    }
    glWidth = w;
    glHeight = h;

    if (g_overlayWidth <= 0 || g_overlayHeight <= 0)
    {
        g_overlayWidth = w;
        g_overlayHeight = h;
        initialScreenSize.x = (float)w;
        initialScreenSize.y = (float)h;
    }
    else if (isOverlaySizeChanged(w, h))
    {
        g_overlayWidth = w;
        g_overlayHeight = h;
        initialScreenSize.x = (float)w;
        initialScreenSize.y = (float)h;
        LOGI("Overlay size changed to %dx%d, request window layout reset", w, h);
        resetWindow = true;
    }

    setupMenu();
    internalDrawMenu(w, h);

    EGLBoolean ret = o_swapbuffers ? o_swapbuffers(dpy, surf) : EGL_TRUE;
    g_inSwap = false;
    return ret;
}
