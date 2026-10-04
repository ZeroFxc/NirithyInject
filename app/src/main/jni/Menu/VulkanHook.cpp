// Native Vulkan overlay: hook vkQueuePresentKHR and draw ImGui on the swapchain.
// Loaded via Dobby from libvulkan.so. Does not use Java SurfaceView.

#include "VulkanHook.h"
#include "ImGui.h"
#include "get_device_api_level_inlines.h"
#include "Includes/Logger.h"
#include "Includes/obfuscate.h"
#include "dobby.h"
#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_android.h"
#include "imgui/backends/imgui_impl_vulkan.h"
#include "Includes/Roboto-Regular.h"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <dlfcn.h>
#include <link.h>
#include <elf.h>
#include <sys/mman.h>
#include <vector>
#include <unordered_map>
#include <unordered_set>

extern void (*menuAddress)();
extern void (*onInitAddr)();
extern bool isInitialized;
extern int glWidth;
extern int glHeight;
extern ImVec2 initialScreenSize;
void setupMenu();
void internalDrawMenu(int width, int height);

static void *g_vulkanLib = nullptr;
static std::atomic<bool> g_vkActive{false};
static std::atomic<bool> g_hooksInstalled{false};

#define VK_FN(ret, name, args) static PFN_##name p##name = nullptr;
VK_FN(VkResult, vkCreateInstance, )
VK_FN(VkResult, vkCreateDevice, )
VK_FN(void, vkDestroyDevice, )
VK_FN(VkResult, vkCreateSwapchainKHR, )
VK_FN(void, vkDestroySwapchainKHR, )
VK_FN(VkResult, vkGetSwapchainImagesKHR, )
VK_FN(VkResult, vkQueuePresentKHR, )
VK_FN(PFN_vkVoidFunction, vkGetInstanceProcAddr, )
VK_FN(PFN_vkVoidFunction, vkGetDeviceProcAddr, )
VK_FN(VkResult, vkQueueSubmit, )
VK_FN(void, vkGetDeviceQueue, )
VK_FN(VkResult, vkCreateCommandPool, )
VK_FN(void, vkDestroyCommandPool, )
VK_FN(VkResult, vkAllocateCommandBuffers, )
VK_FN(VkResult, vkBeginCommandBuffer, )
VK_FN(VkResult, vkEndCommandBuffer, )
VK_FN(VkResult, vkResetCommandBuffer, )
VK_FN(VkResult, vkCreateFence, )
VK_FN(void, vkDestroyFence, )
VK_FN(VkResult, vkWaitForFences, )
VK_FN(VkResult, vkResetFences, )
VK_FN(VkResult, vkCreateRenderPass, )
VK_FN(void, vkDestroyRenderPass, )
VK_FN(VkResult, vkCreateImageView, )
VK_FN(void, vkDestroyImageView, )
VK_FN(VkResult, vkCreateFramebuffer, )
VK_FN(void, vkDestroyFramebuffer, )
VK_FN(VkResult, vkCreateSemaphore, )
VK_FN(void, vkDestroySemaphore, )
VK_FN(void, vkCmdBeginRenderPass, )
VK_FN(void, vkCmdEndRenderPass, )
VK_FN(VkResult, vkDeviceWaitIdle, )
VK_FN(void, vkDestroyInstance, )
VK_FN(void, vkGetPhysicalDeviceQueueFamilyProperties, )
VK_FN(void, vkGetPhysicalDeviceMemoryProperties, )
#undef VK_FN

static PFN_vkCreateDevice orig_vkCreateDevice = nullptr;
static PFN_vkCreateSwapchainKHR orig_vkCreateSwapchainKHR = nullptr;
static PFN_vkDestroySwapchainKHR orig_vkDestroySwapchainKHR = nullptr;
static PFN_vkQueuePresentKHR orig_vkQueuePresentKHR = nullptr;
static PFN_vkGetInstanceProcAddr orig_vkGetInstanceProcAddr = nullptr;
static PFN_vkGetDeviceProcAddr orig_vkGetDeviceProcAddr = nullptr;

static PFN_vkVoidFunction hooked_vkGetInstanceProcAddr(VkInstance instance, const char *pName);

// dlsym GOT hook：Unity/UE 等引擎通过 dlsym(libvulkan.so, "vkGetInstanceProcAddr")
// 获取函数指针再直接调用，GOT hook GIPA 拦不到（其 GOT 表无此符号）。
// 拦截 dlsym 后对该符号返回我们的包装，进而拦截 GDPA 和 device-level 函数获取。
typedef void *(*dlsym_fn)(void *, const char *);
static dlsym_fn orig_dlsym = nullptr;

static void *hooked_dlsym(void *handle, const char *symbol)
{
    void *fn = orig_dlsym(handle, symbol);
    if (symbol && strcmp(symbol, "vkGetInstanceProcAddr") == 0)
        return (void *)hooked_vkGetInstanceProcAddr;
    return fn;
}

struct OverlayFrame
{
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore complete = VK_NULL_HANDLE;
};

struct OverlayState
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{0, 0};
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    std::vector<OverlayFrame> frames;
    bool imguiVulkan = false;
    uint32_t minImageCount = 2;
};

static OverlayState g_ov;
static std::mutex g_ovMtx;

static void loadVulkanProc(void *lib)
{
    pvkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    pvkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)dlsym(lib, "vkGetDeviceProcAddr");
}

// === GOT hook (PLT hook) ===
// 修改调用方 .so 的 GOT 表，不触碰 libvulkan.so 代码段，规避 Android 16 PAC/BTI。
#ifdef __LP64__
#define ELFW_R_SYM ELF64_R_SYM
#else
#define ELFW_R_SYM ELF32_R_SYM
#endif

struct GotHookEntry
{
    const char *symbol;
    void *replace;
    void **orig;
    bool found;
};

static std::atomic<bool> g_gotHookDone{false};

static int gotHookPhdrCallback(struct dl_phdr_info *info, size_t size, void *data)
{
    auto *entries = (std::vector<GotHookEntry> *)data;
    if (!info->dlpi_name || info->dlpi_name[0] == '\0')
        return 0;
    // 只 hook 调用方，不碰 libvulkan.so 自身和我们自己的库
    if (strstr(info->dlpi_name, "libvulkan.so"))
        return 0;
    if (strstr(info->dlpi_name, "libTool.so"))
        return 0;
    if (!info->dlpi_phdr || info->dlpi_phnum == 0)
        return 0;

    uintptr_t base = info->dlpi_addr;

    // 用 dlpi_phdr 直接找 PT_DYNAMIC，不解析 ELF header
    const ElfW(Phdr) *dynPhdr = nullptr;
    for (int i = 0; i < (int)info->dlpi_phnum; i++)
    {
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC)
        {
            dynPhdr = &info->dlpi_phdr[i];
            break;
        }
    }
    if (!dynPhdr)
        return 0;

    const ElfW(Dyn) *dyn = (const ElfW(Dyn) *)(base + dynPhdr->p_vaddr);
    const char *strtab = nullptr;
    const ElfW(Sym) *symtab = nullptr;
    const uint8_t *jmprel = nullptr;
    size_t pltrelsz = 0;
    int pltrel = DT_RELA;

    for (; dyn->d_tag != DT_NULL; dyn++)
    {
        switch (dyn->d_tag)
        {
        case DT_STRTAB: strtab = (const char *)(base + dyn->d_un.d_ptr); break;
        case DT_SYMTAB: symtab = (const ElfW(Sym) *)(base + dyn->d_un.d_ptr); break;
        case DT_JMPREL: jmprel = (const uint8_t *)(base + dyn->d_un.d_ptr); break;
        case DT_PLTRELSZ: pltrelsz = dyn->d_un.d_val; break;
        case DT_PLTREL: pltrel = (int)dyn->d_un.d_val; break;
        }
    }
    if (!strtab || !symtab || !jmprel || pltrelsz == 0)
        return 0;

    size_t entsize = (pltrel == DT_RELA) ? sizeof(ElfW(Rela)) : sizeof(ElfW(Rel));
    size_t count = pltrelsz / entsize;
    long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0)
        pageSize = 4096;

    for (size_t i = 0; i < count; i++)
    {
        const char *symName = nullptr;
        uintptr_t *gotEntry = nullptr;
        if (pltrel == DT_RELA)
        {
            const ElfW(Rela) *rel = (const ElfW(Rela) *)(jmprel + i * entsize);
            symName = strtab + symtab[ELFW_R_SYM(rel->r_info)].st_name;
            gotEntry = (uintptr_t *)(base + rel->r_offset);
        }
        else
        {
            const ElfW(Rel) *rel = (const ElfW(Rel) *)(jmprel + i * entsize);
            symName = strtab + symtab[ELFW_R_SYM(rel->r_info)].st_name;
            gotEntry = (uintptr_t *)(base + rel->r_offset);
        }
        if (!symName)
            continue;
        for (auto &e : *entries)
        {
            if (strcmp(symName, e.symbol) != 0)
                continue;
            // 已经是我们的值则跳过（轮询时重复扫描的幂等保护）
            if (*gotEntry == (uintptr_t)e.replace)
                continue;
            // Android FULL RELRO: GOT 表只读，写入前必须 mprotect 设为可写
            uintptr_t pageStart = (uintptr_t)gotEntry & ~((uintptr_t)pageSize - 1);
            if (mprotect((void *)pageStart, (size_t)pageSize, PROT_READ | PROT_WRITE) != 0)
                continue;
            *e.orig = (void *)*gotEntry;
            *gotEntry = (uintptr_t)e.replace;
            e.found = true;
            LOGI("GOT hook %s in %s", e.symbol, info->dlpi_name);
        }
    }
    return 0;
}

static void installGotHooks(std::vector<GotHookEntry> &entries)
{
    dl_iterate_phdr(gotHookPhdrCallback, &entries);
}

// 轮询线程：游戏库（libunity.so/libil2cpp.so等）可能在 initVulkanHooks 之后才加载，
// 因此定期重新枚举已加载库，对新出现的库补做 GOT hook。
static void gotHookPollThread(std::vector<GotHookEntry> entries)
{
    for (int i = 0; i < 30; i++)
    {
        sleep(1);
        if (g_gotHookDone.load())
            break;
        bool anyFound = false;
        for (auto &e : entries)
            e.found = false;
        installGotHooks(entries);
        for (auto &e : entries)
            if (e.found)
                anyFound = true;
        if (anyFound)
            LOGI("GOT hook poll #%d matched new modules", i);
    }
}

static PFN_vkVoidFunction loader(const char *name, void *ud)
{
    auto inst = (VkInstance)ud;
    PFN_vkVoidFunction fn = nullptr;
    if (pvkGetDeviceProcAddr && g_ov.device)
        fn = pvkGetDeviceProcAddr(g_ov.device, name);
    if (!fn && pvkGetInstanceProcAddr)
        fn = pvkGetInstanceProcAddr(inst, name);
    if (!fn && g_vulkanLib)
        fn = (PFN_vkVoidFunction)dlsym(g_vulkanLib, name);
    return fn;
}

static void bindDeviceFns(VkDevice device)
{
#define BIND(name) p##name = (PFN_##name)pvkGetDeviceProcAddr(device, #name);
    BIND(vkCreateSwapchainKHR)
    BIND(vkDestroySwapchainKHR)
    BIND(vkGetSwapchainImagesKHR)
    BIND(vkQueuePresentKHR)
    BIND(vkQueueSubmit)
    BIND(vkGetDeviceQueue)
    BIND(vkCreateCommandPool)
    BIND(vkDestroyCommandPool)
    BIND(vkAllocateCommandBuffers)
    BIND(vkBeginCommandBuffer)
    BIND(vkEndCommandBuffer)
    BIND(vkResetCommandBuffer)
    BIND(vkCreateFence)
    BIND(vkDestroyFence)
    BIND(vkWaitForFences)
    BIND(vkResetFences)
    BIND(vkCreateRenderPass)
    BIND(vkDestroyRenderPass)
    BIND(vkCreateImageView)
    BIND(vkDestroyImageView)
    BIND(vkCreateFramebuffer)
    BIND(vkDestroyFramebuffer)
    BIND(vkCreateSemaphore)
    BIND(vkDestroySemaphore)
    BIND(vkCmdBeginRenderPass)
    BIND(vkCmdEndRenderPass)
    BIND(vkDeviceWaitIdle)
    BIND(vkDestroyDevice)
#undef BIND
}

static void destroyOverlayLocked()
{
    if (!g_ov.device)
        return;
    if (pvkDeviceWaitIdle)
        pvkDeviceWaitIdle(g_ov.device);
    if (g_ov.imguiVulkan)
    {
        ImGui_ImplVulkan_Shutdown();
        g_ov.imguiVulkan = false;
    }
    for (auto &f : g_ov.frames)
    {
        if (f.fb && pvkDestroyFramebuffer)
            pvkDestroyFramebuffer(g_ov.device, f.fb, nullptr);
        if (f.view && pvkDestroyImageView)
            pvkDestroyImageView(g_ov.device, f.view, nullptr);
        if (f.fence && pvkDestroyFence)
            pvkDestroyFence(g_ov.device, f.fence, nullptr);
        if (f.complete && pvkDestroySemaphore)
            pvkDestroySemaphore(g_ov.device, f.complete, nullptr);
    }
    g_ov.frames.clear();
    if (g_ov.renderPass && pvkDestroyRenderPass)
        pvkDestroyRenderPass(g_ov.device, g_ov.renderPass, nullptr);
    g_ov.renderPass = VK_NULL_HANDLE;
    if (g_ov.cmdPool && pvkDestroyCommandPool)
        pvkDestroyCommandPool(g_ov.device, g_ov.cmdPool, nullptr);
    g_ov.cmdPool = VK_NULL_HANDLE;
    g_ov.swapchain = VK_NULL_HANDLE;
}

static bool createRenderPassLocked()
{
    VkAttachmentDescription att{};
    att.format = g_ov.format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &colorRef;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = 1;
    rp.pAttachments = &att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    rp.dependencyCount = 1;
    rp.pDependencies = &dep;
    return pvkCreateRenderPass(g_ov.device, &rp, nullptr, &g_ov.renderPass) == VK_SUCCESS;
}

static bool rebuildSwapchainLocked(VkSwapchainKHR sc, const VkSwapchainCreateInfoKHR *ci)
{
    destroyOverlayLocked();
    g_ov.swapchain = sc;
    if (ci)
    {
        g_ov.format = ci->imageFormat;
        g_ov.extent = ci->imageExtent;
        g_ov.minImageCount = ci->minImageCount;
    }
    if (g_ov.extent.width == 0 || g_ov.extent.height == 0)
        return false;
    if (!createRenderPassLocked())
        return false;

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = g_ov.queueFamily;
    if (pvkCreateCommandPool(g_ov.device, &poolInfo, nullptr, &g_ov.cmdPool) != VK_SUCCESS)
        return false;

    uint32_t count = 0;
    pvkGetSwapchainImagesKHR(g_ov.device, sc, &count, nullptr);
    if (count == 0)
        return false;
    std::vector<VkImage> images(count);
    pvkGetSwapchainImagesKHR(g_ov.device, sc, &count, images.data());
    g_ov.frames.resize(count);

    std::vector<VkCommandBuffer> cmds(count);
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = g_ov.cmdPool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = count;
    if (pvkAllocateCommandBuffers(g_ov.device, &alloc, cmds.data()) != VK_SUCCESS)
        return false;

    for (uint32_t i = 0; i < count; i++)
    {
        auto &f = g_ov.frames[i];
        f.image = images[i];
        f.cmd = cmds[i];
        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = f.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = g_ov.format;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 1;
        if (pvkCreateImageView(g_ov.device, &view, nullptr, &f.view) != VK_SUCCESS)
            return false;
        VkFramebufferCreateInfo fb{};
        fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass = g_ov.renderPass;
        fb.attachmentCount = 1;
        fb.pAttachments = &f.view;
        fb.width = g_ov.extent.width;
        fb.height = g_ov.extent.height;
        fb.layers = 1;
        if (pvkCreateFramebuffer(g_ov.device, &fb, nullptr, &f.fb) != VK_SUCCESS)
            return false;
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        pvkCreateFence(g_ov.device, &fi, nullptr, &f.fence);
        VkSemaphoreCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        pvkCreateSemaphore(g_ov.device, &si, nullptr, &f.complete);
    }

    if (!g_ov.imguiVulkan)
    {
        ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_0, loader, g_ov.instance);
        ImGui_ImplVulkan_InitInfo init{};
        memset(&init, 0, sizeof(init));
        init.ApiVersion = VK_API_VERSION_1_0;
        init.Instance = g_ov.instance;
        init.PhysicalDevice = g_ov.phys;
        init.Device = g_ov.device;
        init.QueueFamily = g_ov.queueFamily;
        init.Queue = g_ov.queue;
        init.DescriptorPoolSize = 16;
        init.MinImageCount = g_ov.minImageCount < 2 ? 2 : g_ov.minImageCount;
        init.ImageCount = (uint32_t)g_ov.frames.size();
        init.PipelineInfoMain.RenderPass = g_ov.renderPass;
        init.PipelineInfoMain.Subpass = 0;
        init.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        init.CheckVkResultFn = [](VkResult err) {
            if (err != VK_SUCCESS)
                LOGE("Vulkan overlay vk error %d", (int)err);
        };
        if (ImGui_ImplVulkan_Init(&init))
        {
            g_ov.imguiVulkan = true;
            LOGI("ImGui Vulkan backend initialized");
        }
        else
        {
            LOGE("ImGui_ImplVulkan_Init failed");
            return false;
        }
    }
    else
    {
        ImGui_ImplVulkan_SetMinImageCount(g_ov.minImageCount < 2 ? 2 : g_ov.minImageCount);
        ImGui_ImplVulkan_PipelineInfo pinfo{};
        pinfo.RenderPass = g_ov.renderPass;
        pinfo.Subpass = 0;
        pinfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        ImGui_ImplVulkan_CreateMainPipeline(&pinfo);
    }
    return true;
}

static void ensureImguiContext(uint32_t w, uint32_t h)
{
    if (isInitialized)
        return;
    glWidth = (int)w;
    glHeight = (int)h;
    if (!ImGui::GetCurrentContext())
    {
        auto ctx = ImGui::CreateContext();
        if (!ctx)
            return;
        ImGuiIO &io = ImGui::GetIO();
        io.DisplaySize = ImVec2((float)w, (float)h);
        io.IniFilename = nullptr;
        ImGui_ImplAndroid_Init(nullptr);
        io.Fonts->AddFontFromMemoryTTF(Roboto_Regular, sizeof(Roboto_Regular), 40.0f);
        ImGui::GetStyle().ScaleAllSizes(2);
        ImGui::GetStyle().ScrollbarSize *= 2.5f;
        if (onInitAddr)
            onInitAddr();
    }
    isInitialized = true;
}

static VkResult hooked_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *info)
{
    g_vkActive = true;
    if (!info || info->swapchainCount == 0 || !orig_vkQueuePresentKHR)
        return orig_vkQueuePresentKHR ? orig_vkQueuePresentKHR(queue, info) : VK_ERROR_DEVICE_LOST;

    std::lock_guard<std::mutex> lock(g_ovMtx);
    if (!g_ov.device || !g_ov.imguiVulkan || g_ov.frames.empty() || !menuAddress)
        return orig_vkQueuePresentKHR(queue, info);

    uint32_t idx = info->pImageIndices[0];
    if (idx >= g_ov.frames.size())
        return orig_vkQueuePresentKHR(queue, info);

    auto &f = g_ov.frames[idx];
    uint32_t w = g_ov.extent.width;
    uint32_t h = g_ov.extent.height;
    glWidth = (int)w;
    glHeight = (int)h;
    static bool initialScreenSet = false;
    if (!initialScreenSet)
    {
        initialScreenSize.x = (float)w;
        initialScreenSize.y = (float)h;
        initialScreenSet = true;
    }
    ensureImguiContext(w, h);

    pvkWaitForFences(g_ov.device, 1, &f.fence, VK_TRUE, UINT64_MAX);
    pvkResetFences(g_ov.device, 1, &f.fence);
    pvkResetCommandBuffer(f.cmd, 0);

    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplAndroid_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowSize(ImVec2((float)w / 2, (float)h / 2), ImGuiCond_Once);
    menuAddress();
    ImGui::Render();

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    pvkBeginCommandBuffer(f.cmd, &begin);

    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = g_ov.renderPass;
    rp.framebuffer = f.fb;
    rp.renderArea.extent = g_ov.extent;
    pvkCmdBeginRenderPass(f.cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), f.cmd);
    pvkCmdEndRenderPass(f.cmd);
    pvkEndCommandBuffer(f.cmd);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &f.cmd;
    // Same-queue in-order submit: overlay finishes before the original present.
    pvkQueueSubmit(queue, 1, &submit, f.fence);

    return orig_vkQueuePresentKHR(queue, info);
}

static VkResult hooked_vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *ci, const VkAllocationCallbacks *a,
                                            VkSwapchainKHR *out)
{
    VkResult r = orig_vkCreateSwapchainKHR(device, ci, a, out);
    if (r == VK_SUCCESS && out && *out)
    {
        std::lock_guard<std::mutex> lock(g_ovMtx);
        g_ov.device = device;
        rebuildSwapchainLocked(*out, ci);
        LOGI("Vulkan swapchain overlay rebuilt %ux%u", ci->imageExtent.width, ci->imageExtent.height);
    }
    return r;
}

static void hooked_vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR sc, const VkAllocationCallbacks *a)
{
    {
        std::lock_guard<std::mutex> lock(g_ovMtx);
        if (sc == g_ov.swapchain)
            destroyOverlayLocked();
    }
    if (orig_vkDestroySwapchainKHR)
        orig_vkDestroySwapchainKHR(device, sc, a);
}

static VkResult hooked_vkCreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a,
                                      VkDevice *out)
{
    VkResult r = orig_vkCreateDevice(phys, ci, a, out);
    if (r == VK_SUCCESS && out && *out)
    {
        std::lock_guard<std::mutex> lock(g_ovMtx);
        g_ov.phys = phys;
        g_ov.device = *out;
        bindDeviceFns(*out);
        g_ov.queueFamily = 0;
        if (ci && ci->queueCreateInfoCount > 0)
            g_ov.queueFamily = ci->pQueueCreateInfos[0].queueFamilyIndex;
        pvkGetDeviceQueue = pvkGetDeviceQueue ? pvkGetDeviceQueue
                                              : (PFN_vkGetDeviceQueue)pvkGetDeviceProcAddr(*out, "vkGetDeviceQueue");
        if (pvkGetDeviceQueue)
            pvkGetDeviceQueue(*out, g_ov.queueFamily, 0, &g_ov.queue);
        LOGI("Vulkan device captured queueFamily=%u", g_ov.queueFamily);
    }
    return r;
}

// GOT hook 层：游戏通过 GOT 调用 vkGetInstanceProcAddr 时进入这里。
// 对 vkGetDeviceProcAddr 返回我们的包装，从而拦截 device-level 函数获取。
static PFN_vkVoidFunction hooked_vkGetDeviceProcAddr(VkDevice device, const char *pName)
{
    if (!orig_vkGetDeviceProcAddr)
        return nullptr;
    PFN_vkVoidFunction fn = orig_vkGetDeviceProcAddr(device, pName);
    if (!pName)
        return fn;
    if (strcmp(pName, "vkQueuePresentKHR") == 0)
        return (PFN_vkVoidFunction)hooked_vkQueuePresentKHR;
    if (strcmp(pName, "vkCreateSwapchainKHR") == 0)
        return (PFN_vkVoidFunction)hooked_vkCreateSwapchainKHR;
    if (strcmp(pName, "vkDestroySwapchainKHR") == 0)
        return (PFN_vkVoidFunction)hooked_vkDestroySwapchainKHR;
    return fn;
}

static PFN_vkVoidFunction hooked_vkGetInstanceProcAddr(VkInstance instance, const char *pName)
{
    if (!orig_vkGetInstanceProcAddr)
        return nullptr;
    PFN_vkVoidFunction fn = orig_vkGetInstanceProcAddr(instance, pName);
    if (!pName)
        return fn;
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0)
        return (PFN_vkVoidFunction)hooked_vkGetDeviceProcAddr;
    return fn;
}

// 注意：这里刻意【不使用】Dobby inline hook vkGetInstanceProcAddr / vkGetDeviceProcAddr。
// Android 16（该设备 Build fingerprint 为 OnePlus/OP60EBL1:16）启用了 PAC/BTI，libvulkan.so
// 中这两个导出函数的入口首条指令是 BTI landing pad（bti c，编码 0xd503245f）。Dobby 覆盖入口
// 后首条不再是 BTI 指令，而系统 libhwui 的 RenderThread::requireVkContext ->
// VulkanManager::initialize() 也会用 blr 直接调用它们，跳入时 BTI 校验失败，报
// SIGILL/ILL_ILLOPC，fault addr 恰为函数入口。
// 已在 04_10-04-00-31_865.log 确认：pc == 0x7c9b160440 == libvulkan.so vkGetInstanceProcAddr
// （vaddr 0x1e440），调用栈 #01 为 libhwui VulkanManager::initialize()。
// device 级函数（vkCreateDevice / vkQueuePresentKHR / vkCreateSwapchainKHR 等）已由下方
// 直接 DobbyHook 拦截，无需再劫持 GIPA/GDPA。

void initVulkanHooks()
{
    if (g_hooksInstalled.exchange(true))
        return;

    // API < 26 的设备上 Vulkan loader/驱动不完善，加载并 hook libvulkan.so 容易崩溃，
    // 因此直接跳过 Vulkan 叠加层，仅保留 GLES 路径。读取不到等级（-1）时按可用来处理。
    const int apiLevel = api_level();
    if (apiLevel >= 0 && apiLevel < 26)
    {
        LOGI("API level %d < 26 — skipping Vulkan overlay hooks", apiLevel);
        g_hooksInstalled = false;
        return;
    }

    g_vulkanLib = dlopen(OBFUSCATE("libvulkan.so"), RTLD_NOW | RTLD_NOLOAD);
    if (!g_vulkanLib)
        g_vulkanLib = dlopen(OBFUSCATE("libvulkan.so"), RTLD_NOW);
    if (!g_vulkanLib)
    {
        LOGI("libvulkan.so not loaded — skipping Vulkan overlay hooks");
        g_hooksInstalled = false;
        return;
    }
    loadVulkanProc(g_vulkanLib);
    auto createDev = dlsym(g_vulkanLib, OBFUSCATE("vkCreateDevice"));
    auto present = dlsym(g_vulkanLib, OBFUSCATE("vkQueuePresentKHR"));
    auto createSc = dlsym(g_vulkanLib, OBFUSCATE("vkCreateSwapchainKHR"));
    auto destroySc = dlsym(g_vulkanLib, OBFUSCATE("vkDestroySwapchainKHR"));

    if (!createDev || !present || !createSc || !destroySc)
        LOGI("Vulkan symbols missing: device=%d present=%d swapchain=%d destroy=%d",
             createDev != nullptr, present != nullptr, createSc != nullptr, destroySc != nullptr);

    if (createDev && DobbyHook(createDev, (void *)hooked_vkCreateDevice, (void **)&orig_vkCreateDevice) != 0)
        LOGI("DobbyHook vkCreateDevice failed");
    if (present && DobbyHook(present, (void *)hooked_vkQueuePresentKHR, (void **)&orig_vkQueuePresentKHR) != 0)
        LOGI("DobbyHook vkQueuePresentKHR failed");
    if (createSc && DobbyHook(createSc, (void *)hooked_vkCreateSwapchainKHR, (void **)&orig_vkCreateSwapchainKHR) != 0)
        LOGI("DobbyHook vkCreateSwapchainKHR failed");
    if (destroySc && DobbyHook(destroySc, (void *)hooked_vkDestroySwapchainKHR, (void **)&orig_vkDestroySwapchainKHR) != 0)
        LOGI("DobbyHook vkDestroySwapchainKHR failed");

    // GOT hook vkGetInstanceProcAddr：拦截游戏通过 GOT 调用的 GIPA，
    // 进而拦截 vkGetDeviceProcAddr 和 device-level 函数获取。
    // 对 Unity/UE 等通过 GetProcAddr 拿函数指针直接调用的引擎生效。
    // 只改调用方 GOT 表（数据段），不碰 libvulkan.so 代码，无 PAC/BTI 问题。
    orig_vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(g_vulkanLib, OBFUSCATE("vkGetInstanceProcAddr"));
    orig_vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)dlsym(g_vulkanLib, OBFUSCATE("vkGetDeviceProcAddr"));
    orig_dlsym = (dlsym_fn)dlsym(RTLD_DEFAULT, OBFUSCATE("dlsym"));
    std::vector<GotHookEntry> gotEntries = {
        {OBFUSCATE("vkGetInstanceProcAddr"), (void *)hooked_vkGetInstanceProcAddr, (void **)&orig_vkGetInstanceProcAddr, false},
        {OBFUSCATE("dlsym"), (void *)hooked_dlsym, (void **)&orig_dlsym, false},
    };
    installGotHooks(gotEntries);
    bool anyFound = false;
    for (auto &e : gotEntries)
        if (e.found)
            anyFound = true;
    if (!anyFound)
        LOGI("GOT hook: no module matched at init, starting poll thread for late-loaded libs");
    // 游戏主库（libunity.so/libil2cpp.so等）通常在 initVulkanHooks 之后才加载，
    // 启动轮询线程持续补 hook 新出现的库。
    std::thread pollThread(gotHookPollThread, gotEntries);
    pollThread.detach();

    LOGI("Vulkan overlay hooks installed");
}

bool isVulkanOverlayActive()
{
    return g_vkActive.load();
}
