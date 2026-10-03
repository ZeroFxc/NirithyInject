#include "zygisk.hpp"
#include "Tool/Targets.h"
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cstdint>
#include <string>
#include <vector>
#include <jni.h>
#include <android/log.h>

using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;

extern void *hack_thread(void *);

// Always log to logcat even in release — injection failures are otherwise silent.
#define ZLOG(...) __android_log_print(ANDROID_LOG_INFO, "il2cpp_tool", __VA_ARGS__)

static bool g_enableHack = false;

static bool writeExact(int fd, const void *data, size_t n)
{
    auto p = static_cast<const uint8_t *>(data);
    size_t off = 0;
    while (off < n)
    {
        ssize_t w = write(fd, p + off, n - off);
        if (w <= 0)
            return false;
        off += static_cast<size_t>(w);
    }
    return true;
}

static bool readExact(int fd, void *data, size_t n)
{
    auto p = static_cast<uint8_t *>(data);
    size_t off = 0;
    while (off < n)
    {
        ssize_t r = read(fd, p + off, n - off);
        if (r <= 0)
            return false;
        off += static_cast<size_t>(r);
    }
    return true;
}

// 会话日志帧：uint32 长度 + 内容，companion 侧按同样协议读取
static bool writeFrame(int fd, const std::string &s)
{
    uint32_t n = static_cast<uint32_t>(s.size());
    return writeExact(fd, &n, sizeof(n)) && (n == 0 || writeExact(fd, s.data(), n));
}

// 向 companion 日志通道发送一行（通道不可用时静默忽略）
static void sendLogFrame(int fd, const std::string &line)
{
    if (fd >= 0)
        (void)writeFrame(fd, line + "\n");
}

static std::string jstringToStd(JNIEnv *env, jstring s)
{
    if (!s)
        return "";
    const char *c = env->GetStringUTFChars(s, nullptr);
    std::string out = c ? c : "";
    env->ReleaseStringUTFChars(s, c);
    return out;
}

// Root companion: zygote (and the app after specialize) cannot read /data/adb
// because of SELinux. The companion runs as magiskd/zygiskd and CAN.
static void companion_handler(int fd)
{
    uint32_t len = 0;
    if (!readExact(fd, &len, sizeof(len)) || len > 512)
    {
        uint8_t no = 0;
        writeExact(fd, &no, 1);
        close(fd);
        return;
    }
    std::string pkg(len, '\0');
    if (len && !readExact(fd, pkg.data(), len))
    {
        uint8_t no = 0;
        writeExact(fd, &no, 1);
        close(fd);
        return;
    }
    pkg = Targets::NormalizePackage(pkg);
    auto list = Targets::Load();
    uint8_t matched = Targets::Matches(pkg, list) ? 1 : 0;
    writeExact(fd, &matched, 1);

    char line[384];
    if (matched)
        snprintf(line, sizeof(line), "INJECT %s (%zu targets)", pkg.c_str(), list.size());
    else
        snprintf(line, sizeof(line), "SKIP %s (%zu targets)", pkg.c_str(), list.size());
    Targets::AppendInjectLog(line);
    ZLOG("%s", line);

    if (!matched)
    {
        close(fd);
        return;
    }

    // 注入命中：本次会话日志落到 /data/adb/NirithyInject/<包名>/<日期>.log。
    // 之后保持这条 socket 作为日志通道，逐帧接收目标进程内的日志，
    // 直到客户端关闭连接（即目标进程退出）。
    Targets::AppendPackageLog(pkg, line);
    for (;;)
    {
        uint32_t n = 0;
        if (!readExact(fd, &n, sizeof(n)) || n == 0 || n > 64 * 1024)
            break;
        std::string msg(n, '\0');
        if (!readExact(fd, msg.data(), n))
            break;
        if (!msg.empty() && msg.back() == '\n')
            msg.pop_back();
        Targets::AppendPackageLog(pkg, msg.c_str());
    }
    Targets::AppendPackageLog(pkg, "process exited, session log closed");
    close(fd);
}

class Il2cppToolModule : public zygisk::ModuleBase
{
  public:
    void onLoad(Api *api, JNIEnv *env) override
    {
        api_ = api;
        env_ = env;
        // 模块被 zygisk 成功装载的凭据：若日志里连这一行都没有，说明模块根本没被加载
        ZLOG("module loaded (api=%p, env=%p)", (void *)api, (void *)env);
    }

    void preAppSpecialize(AppSpecializeArgs *args) override
    {
        if (!args)
        {
            ZLOG("preAppSpecialize: null args");
            return;
        }

        std::string pkg;
        if (args->nice_name)
            pkg = Targets::NormalizePackage(jstringToStd(env_, args->nice_name));
        if (pkg.empty() && args->app_data_dir)
            pkg = Targets::ParsePackageFromDataDir(jstringToStd(env_, args->app_data_dir));
        // 关键诊断点：zygote 派生出的每个应用进程都会走到这里，打印进程包名
        ZLOG("preAppSpecialize: pkg='%s'", pkg.c_str());

        // Prefer the root companion so we can actually read the WebUI allow-list.
        bool decided = false;
        int cfd = api_ ? api_->connectCompanion() : -1;
        if (cfd < 0)
            ZLOG("companion connect failed (cfd=%d), falling back to local list", cfd);
        if (cfd >= 0)
        {
            uint32_t n = static_cast<uint32_t>(pkg.size());
            if (writeExact(cfd, &n, sizeof(n)) && (n == 0 || writeExact(cfd, pkg.data(), n)))
            {
                uint8_t r = 0;
                if (readExact(cfd, &r, 1))
                {
                    g_enableHack = r != 0;
                    decided = true;
                }
            }
            if (decided && g_enableHack)
            {
                // 注入命中：不关闭这条连接，交给 hack_thread 作为会话日志通道，
                // 由 companion 写入 /data/adb/NirithyInject/<包名>/<日期>.log
                logFd_ = cfd;
                sendLogFrame(logFd_, std::string("preAppSpecialize: pkg='") + pkg + "' companion matched");
            }
            else
            {
                close(cfd);
            }
        }

        if (!decided)
        {
            // Fallback: unsandboxed zygote sometimes can still read the file.
            auto list = Targets::Load();
            g_enableHack = Targets::Matches(pkg, list);
            ZLOG("companion unavailable, local list=%zu %s %s", list.size(), pkg.c_str(),
                 g_enableHack ? "INJECT" : "skip");
        }

        if (!g_enableHack)
        {
            ZLOG("skip %s: not in target list", pkg.c_str());
            if (api_)
                api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
        }
        else
            ZLOG("will inject overlay into %s", pkg.c_str());
    }

    void postAppSpecialize(const AppSpecializeArgs *) override
    {
        ZLOG("postAppSpecialize: enableHack=%d", (int)g_enableHack);
        if (!g_enableHack)
            return;
        sendLogFrame(logFd_, "postAppSpecialize: starting hack_thread");
        pthread_t tid;
        // 把会话日志通道 fd 一并交给 hack_thread（-1 表示无通道，落盘将回退到应用私有目录）
        if (pthread_create(&tid, nullptr, hack_thread, (void *)(intptr_t)logFd_) != 0)
        {
            ZLOG("failed to start hack_thread");
        }
        else
        {
            pthread_detach(tid);
            ZLOG("hack_thread started");
        }
    }

  private:
    Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;
    // 会话日志通道 fd：preAppSpecialize 建立，postAppSpecialize 交给 hack_thread
    int logFd_ = -1;
};

REGISTER_ZYGISK_MODULE(Il2cppToolModule)
REGISTER_ZYGISK_COMPANION(companion_handler)
