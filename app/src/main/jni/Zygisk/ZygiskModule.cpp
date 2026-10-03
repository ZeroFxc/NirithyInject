#include "zygisk.hpp"
#include "Tool/Targets.h"
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
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
    close(fd);
}

class Il2cppToolModule : public zygisk::ModuleBase
{
  public:
    void onLoad(Api *api, JNIEnv *env) override
    {
        api_ = api;
        env_ = env;
    }

    void preAppSpecialize(AppSpecializeArgs *args) override
    {
        if (!args)
            return;

        std::string pkg;
        if (args->nice_name)
            pkg = Targets::NormalizePackage(jstringToStd(env_, args->nice_name));
        if (pkg.empty() && args->app_data_dir)
            pkg = Targets::ParsePackageFromDataDir(jstringToStd(env_, args->app_data_dir));

        // Prefer the root companion so we can actually read the WebUI allow-list.
        bool decided = false;
        int cfd = api_ ? api_->connectCompanion() : -1;
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
            close(cfd);
        }

        if (!decided)
        {
            // Fallback: unsandboxed zygote sometimes can still read the file.
            auto list = Targets::Load();
            g_enableHack = Targets::Matches(pkg, list);
            ZLOG("companion unavailable, local list=%zu %s %s", list.size(), pkg.c_str(),
                 g_enableHack ? "INJECT" : "skip");
        }

        if (!g_enableHack && api_)
            api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
        else
            ZLOG("will inject overlay into %s", pkg.c_str());
    }

    void postAppSpecialize(const AppSpecializeArgs *) override
    {
        if (!g_enableHack)
            return;
        pthread_t tid;
        if (pthread_create(&tid, nullptr, hack_thread, nullptr) != 0)
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
};

REGISTER_ZYGISK_MODULE(Il2cppToolModule)
REGISTER_ZYGISK_COMPANION(companion_handler)
